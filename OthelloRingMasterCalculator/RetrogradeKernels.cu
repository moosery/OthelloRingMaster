/*
** Filename:  RetrogradeKernels.cu
**
** Purpose:
**   Implements the GPU context declared in RetrogradeKernels.h:
**   RetrogradeExpandKernel (move generation/flip/canonicalize plus the
**   ring<->row-major boundary conversion -- the same logic GpuKernels.cu's
**   ExpandKernel already uses, see that file's header comment for the
**   full boundary-conversion rationale) and the context's create/destroy/
**   process/read API.
**
** Notes:
**   No sort, no dedup, no atomics: each thread (one per parent) owns a
**   fixed, private slot range in the output buffer, since retrograde
**   summing wants every (parent, child) edge preserved, not deduped (see
**   RetrogradeKernels.h's own Notes). This makes the whole pipeline much
**   smaller than GpuKernels.cu's two-stack accumulator.
**
**   This first implementation is synchronous (blocking cudaMemcpy, no
**   stream/ping-pong) -- correctness-focused for the 4x4 validation target
**   (Phase 6), not yet tuned for real 6x6 throughput. Parallelization
**   strategy is still an open question (see
**   project_retrograde_calculator_implementation_plan memory's Phase 3
**   section) to revisit once this is proven correct.
*/

/* Includes */
#include "RetrogradeKernels.h"
#include "OthelloBasicsForCUDA.h"    /* BOARD, dev_boardMoveCalculator/dev_playMove/dev_canonicalize */
#include "RingConversion.h"          /* dev_RowMajorToRing/dev_RingToRowMajor, this TU's constant tables */
#include "RingPermutation.h"         /* BuildRingPermutation/BuildInverseRingPermutation, for this TU's init */
#include "Error.h"
#include <cuda_runtime.h>
#include <vector>

/*
** CUDA error helper -- Fatals immediately, matching GpuKernels.cu's own
** GPU_CHECK (file-local there too; not worth sharing for one macro).
*/
#define RETRO_GPU_CHECK(call) \
    do { \
        cudaError_t _e = (call); \
        if (_e != cudaSuccess) \
            Fatal(FATAL_GPU_ERROR, "CUDA error %s:%d  %s", \
                  __FILE__, __LINE__, cudaGetErrorString(_e)); \
    } while (0)

/* Structures and Types */

/*
** Type:    __RetrogradeGpuContext
** @brief   Concrete state behind the opaque RetrogradeGpuContext handle.
*/
struct __RetrogradeGpuContext
{
    UINT64_PAIR*  d_input;
    UINT64_PAIR*  d_children;
    uint32_t*     d_childCount;
    uint8_t*      d_childPlayer;         /* per-child, not per-parent -- see file/header Notes */
    uint8_t*      d_childColorFlipped;   /* per-child -- see file/header Notes */
    uint32_t*     d_maxMovesStat;

    std::vector<UINT64_PAIR>  h_children;
    std::vector<uint32_t>     h_childCount;
    std::vector<uint8_t>      h_childPlayer;
    std::vector<uint8_t>      h_childColorFlipped;

    DevBoardConsts  boardConsts;
    int             batchSize;
    int             maxMovesPerBoard;
};

/* Functions */

/*
** Function: RetrogradeKernels_InitRingPermutationTables
** @brief    Builds the forward/inverse ring-permutation tables on the CPU
**           and uploads them into THIS FILE's GPU constant memory -- see
**           RingConversion.h's Notes on why every .cu file that includes
**           it needs its own init call.
*/
static void RetrogradeKernels_InitRingPermutationTables()
{
    std::vector<int>  forward = BuildRingPermutation(8, 0);
    std::vector<int>  inverse = BuildInverseRingPermutation(8, 0);
    int               forwardArr[64];
    int               inverseArr[64];

    for (int i = 0; i < 64; i++)
    {
        forwardArr[i] = forward[i];
        inverseArr[i] = inverse[i];
    }

    cudaMemcpyToSymbol(g_ringForwardPerm, forwardArr, sizeof(forwardArr));
    cudaMemcpyToSymbol(g_ringInversePerm, inverseArr, sizeof(inverseArr));
}

/*
** Function: RetrogradeExpandKernel
** @brief    One thread per input (parent) board: converts the ring-ordered
**           input to row-major, generates children (handling the pass/
**           terminal cases exactly like GpuKernels.cu's ExpandKernel),
**           canonicalizes each child, converts back to ring order, and
**           writes them into this thread's own private slot range --
**           no atomics, no dedup (see file Notes).
** @param    input             - batch of ring-ordered parent board records
** @param    batchSize         - number of records in input
** @param    inputPlayerBit    - next player to move for this whole batch (1=black, 0=white)
** @param    consts            - board-size masks for move generation
** @param    numRotations      - canonicalization symmetry count (always 16 here, matching
**                               the forward solver's own canonicalization so lookups
**                               against already-stored canonical representatives succeed)
** @param    maxMovesPerBoard  - capacity of each thread's private slot range
** @param    d_children        - out: [batchSize * maxMovesPerBoard], this thread's
**                                children at [i*maxMovesPerBoard, i*maxMovesPerBoard+count)
** @param    d_childCount      - out: [batchSize], this parent's child count (0 = terminal)
** @param    d_childPlayer     - out: [batchSize * maxMovesPerBoard], each child's own
**                                next-player tag (per-child -- see file/header Notes)
** @param    d_childColorFlipped - out: [batchSize * maxMovesPerBoard], each child's own
**                                flag for whether its canonical form came from
**                                dev_canonicalize's color-swap family (see file/header Notes)
** @param    d_maxMovesStat    - out: running max child count seen, for a host-side sanity
**                                check against maxMovesPerBoard
*/
__global__ void RetrogradeExpandKernel(
    const UINT64_PAIR* __restrict__ input,
    int                              batchSize,
    uint8_t                          inputPlayerBit,
    DevBoardConsts                   consts,
    int                              numRotations,
    int                              maxMovesPerBoard,
    UINT64_PAIR*                     d_children,
    uint32_t*                        d_childCount,
    uint8_t*                         d_childPlayer,
    uint8_t*                         d_childColorFlipped,
    uint32_t*                        d_maxMovesStat)
{
    int i = (int)(blockIdx.x * blockDim.x + threadIdx.x);
    if (i >= batchSize) return;

    const UINT64_PAIR srcRec = input[i];
    BOARD src = {};
    src.ullCellsInUse = dev_RingToRowMajor(srcRec.hi);
    src.ullCellColors = dev_RingToRowMajor(srcRec.lo);
    src.usBoardInfo   = inputPlayerBit;

    dev_boardMoveCalculator(&src, consts);
    unsigned long long moves = src.ullPossibleMoves;

    if (moves == 0)
    {
        /* Current player can't move -- try a pass, exactly like ExpandKernel. */
        BOARD passBoard = src;
        SETBOARDNEXTPLAYERFLIP(&passBoard);
        passBoard.ullPossibleMoves = 0;
        dev_boardMoveCalculator(&passBoard, consts);
        unsigned long long oppMoves = passBoard.ullPossibleMoves;

        if (oppMoves == 0)
        {
            /* Terminal: both players stuck. The CPU side classifies this
            ** parent directly from its own piece count (input[i]) --
            ** no children to look up.
            */
            d_childCount[i] = 0;
            return;
        }

        moves = oppMoves;
        src   = passBoard;
    }

    /* Every child's pre-canonicalization next-player is the flip of src's
    ** mover (see dev_playMove) -- fixed for this whole loop since src
    ** doesn't change per-child. If a specific child's FINAL (post-
    ** canonicalize) player bit differs from this expected value, its
    ** canonical form came from the color-swap symmetry family, so its
    ** stored colors (and therefore its already-computed black/white win
    ** counts) are swapped relative to the real continuation.
    */
    uint8_t expectedChildPlayer = (uint8_t)((src.usBoardInfo & 0x01) ^ 0x01);

    int count = 0;
    while (moves)
    {
        int moveIdx = __clzll(moves);
        moves &= ~(0x8000000000000000ULL >> moveIdx);

        BOARD child = {};
        dev_playMove(&src, &child, moveIdx);
        dev_canonicalize(&child, numRotations, consts);

        UINT64_PAIR childRec;
        childRec.hi = dev_RowMajorToRing(child.ullCellsInUse);
        childRec.lo = dev_RowMajorToRing(child.ullCellColors);
        uint8_t childPlayer   = child.usBoardInfo & 0x01;
        uint8_t colorFlipped  = (childPlayer != expectedChildPlayer) ? 1 : 0;

        if (count < maxMovesPerBoard)
        {
            size_t slot = (size_t)i * (size_t)maxMovesPerBoard + count;
            d_children[slot]          = childRec;
            d_childPlayer[slot]       = childPlayer;
            d_childColorFlipped[slot] = colorFlipped;
        }
        count++;
    }

    d_childCount[i] = (uint32_t)count;
    atomicMax(d_maxMovesStat, (uint32_t)count);
}

/*
** Function: RetrogradeGpuContextCreate
** @brief    See RetrogradeKernels.h.
*/
RetrogradeGpuContext* RetrogradeGpuContextCreate(int boardSize, int batchSize, int maxMovesPerBoard)
{
    RetrogradeKernels_InitRingPermutationTables();
    SetBoardSizeForRun(boardSize);

    __RetrogradeGpuContext* p = new __RetrogradeGpuContext();
    p->batchSize        = batchSize;
    p->maxMovesPerBoard  = maxMovesPerBoard;
    p->boardConsts       = OBCuda_GetBoardConsts();

    size_t childCapacity = (size_t)batchSize * (size_t)maxMovesPerBoard;

    RETRO_GPU_CHECK(cudaMalloc(&p->d_input,              (size_t)batchSize * sizeof(UINT64_PAIR)));
    RETRO_GPU_CHECK(cudaMalloc(&p->d_children,            childCapacity     * sizeof(UINT64_PAIR)));
    RETRO_GPU_CHECK(cudaMalloc(&p->d_childCount,         (size_t)batchSize * sizeof(uint32_t)));
    RETRO_GPU_CHECK(cudaMalloc(&p->d_childPlayer,         childCapacity     * sizeof(uint8_t)));
    RETRO_GPU_CHECK(cudaMalloc(&p->d_childColorFlipped,   childCapacity     * sizeof(uint8_t)));
    RETRO_GPU_CHECK(cudaMalloc(&p->d_maxMovesStat, sizeof(uint32_t)));

    p->h_children.resize(childCapacity);
    p->h_childCount.resize(batchSize);
    p->h_childPlayer.resize(childCapacity);
    p->h_childColorFlipped.resize(childCapacity);

    return p;
}

/*
** Function: RetrogradeGpuContextDestroy
** @brief    See RetrogradeKernels.h.
*/
void RetrogradeGpuContextDestroy(RetrogradeGpuContext* pCtx)
{
    if (!pCtx) return;
    cudaFree(pCtx->d_input);
    cudaFree(pCtx->d_children);
    cudaFree(pCtx->d_childCount);
    cudaFree(pCtx->d_childPlayer);
    cudaFree(pCtx->d_childColorFlipped);
    cudaFree(pCtx->d_maxMovesStat);
    delete pCtx;
}

/*
** Function: RetrogradeExpandBatch
** @brief    See RetrogradeKernels.h.
*/
void RetrogradeExpandBatch(RetrogradeGpuContext* pCtx, const UINT64_PAIR* pParents,
                            int count, uint8_t playerBit)
{
    if (count > pCtx->batchSize)
        Fatal(FATAL_GPU_ERROR, "RetrogradeExpandBatch: count %d exceeds context batchSize %d", count, pCtx->batchSize);

    RETRO_GPU_CHECK(cudaMemcpy(pCtx->d_input, pParents, (size_t)count * sizeof(UINT64_PAIR), cudaMemcpyHostToDevice));
    RETRO_GPU_CHECK(cudaMemset(pCtx->d_maxMovesStat, 0, sizeof(uint32_t)));

    int threadsPerBlock = 256;
    int blocks          = (count + threadsPerBlock - 1) / threadsPerBlock;
    RetrogradeExpandKernel<<<blocks, threadsPerBlock>>>(
        pCtx->d_input, count, playerBit, pCtx->boardConsts, 16, pCtx->maxMovesPerBoard,
        pCtx->d_children, pCtx->d_childCount, pCtx->d_childPlayer, pCtx->d_childColorFlipped, pCtx->d_maxMovesStat);
    RETRO_GPU_CHECK(cudaGetLastError());
    RETRO_GPU_CHECK(cudaDeviceSynchronize());

    uint32_t maxMovesSeen = 0;
    RETRO_GPU_CHECK(cudaMemcpy(&maxMovesSeen, pCtx->d_maxMovesStat, sizeof(uint32_t), cudaMemcpyDeviceToHost));
    if ((int)maxMovesSeen > pCtx->maxMovesPerBoard)
        Fatal(FATAL_MAX_MOVES_EXCEEDED,
              "RetrogradeExpandBatch: a board produced %u children, exceeding maxMovesPerBoard %d",
              maxMovesSeen, pCtx->maxMovesPerBoard);

    size_t childCapacity = (size_t)count * (size_t)pCtx->maxMovesPerBoard;
    RETRO_GPU_CHECK(cudaMemcpy(pCtx->h_children.data(), pCtx->d_children, childCapacity * sizeof(UINT64_PAIR), cudaMemcpyDeviceToHost));
    RETRO_GPU_CHECK(cudaMemcpy(pCtx->h_childCount.data(), pCtx->d_childCount, (size_t)count * sizeof(uint32_t), cudaMemcpyDeviceToHost));
    RETRO_GPU_CHECK(cudaMemcpy(pCtx->h_childPlayer.data(), pCtx->d_childPlayer, childCapacity * sizeof(uint8_t), cudaMemcpyDeviceToHost));
    RETRO_GPU_CHECK(cudaMemcpy(pCtx->h_childColorFlipped.data(), pCtx->d_childColorFlipped, childCapacity * sizeof(uint8_t), cudaMemcpyDeviceToHost));
}

/*
** Function: RetrogradeGetChildCount
** @brief    See RetrogradeKernels.h.
*/
uint32_t RetrogradeGetChildCount(const RetrogradeGpuContext* pCtx, int parentIdx)
{
    return pCtx->h_childCount[parentIdx];
}

/*
** Function: RetrogradeGetChildPlayer
** @brief    See RetrogradeKernels.h.
*/
uint8_t RetrogradeGetChildPlayer(const RetrogradeGpuContext* pCtx, int parentIdx, int childIdx)
{
    return pCtx->h_childPlayer[(size_t)parentIdx * (size_t)pCtx->maxMovesPerBoard + childIdx];
}

/*
** Function: RetrogradeGetChildColorFlipped
** @brief    See RetrogradeKernels.h.
*/
bool RetrogradeGetChildColorFlipped(const RetrogradeGpuContext* pCtx, int parentIdx, int childIdx)
{
    return pCtx->h_childColorFlipped[(size_t)parentIdx * (size_t)pCtx->maxMovesPerBoard + childIdx] != 0;
}

/*
** Function: RetrogradeGetChildren
** @brief    See RetrogradeKernels.h.
*/
const UINT64_PAIR* RetrogradeGetChildren(const RetrogradeGpuContext* pCtx, int parentIdx)
{
    return &pCtx->h_children[(size_t)parentIdx * (size_t)pCtx->maxMovesPerBoard];
}
