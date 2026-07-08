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
**   Within each parent's slot range, black children are packed from the
**   front and white children from the back, growing toward each other --
**   color is positional, never a stored per-child tag (see header Notes).
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
#include "RSFFileName.h"             /* RSF_PLAYER_BLACK/RSF_PLAYER_WHITE */
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
    UINT64_PAIR*  d_children;           /* per-parent two-stack: black from front, white from back */
    uint8_t*      d_childColorFlipped;  /* same packing as d_children */
    uint32_t*     d_blackChildCount;
    uint32_t*     d_whiteChildCount;
    uint32_t*     d_maxMovesStat;

    std::vector<UINT64_PAIR>  h_children;
    std::vector<uint8_t>      h_childColorFlipped;
    std::vector<uint32_t>     h_blackChildCount;
    std::vector<uint32_t>     h_whiteChildCount;

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
**           packs them into this thread's own private slot range -- black
**           children from the front, white from the back, growing toward
**           each other (no atomics, no dedup, no stored color tag -- see
**           file/header Notes).
** @param    input               - batch of ring-ordered parent board records
** @param    batchSize           - number of records in input
** @param    inputPlayerBit      - next player to move for this whole batch (1=black, 0=white)
** @param    consts              - board-size masks for move generation
** @param    numRotations        - canonicalization symmetry count (always 16 here, matching
**                                 the forward solver's own canonicalization so lookups
**                                 against already-stored canonical representatives succeed)
** @param    maxMovesPerBoard    - capacity of each thread's private slot range
** @param    d_children          - out: [batchSize * maxMovesPerBoard], this thread's
**                                 children packed black-from-front/white-from-back
** @param    d_childColorFlipped - out: same packing as d_children -- see header Notes
** @param    d_blackChildCount   - out: [batchSize], this parent's black child count
** @param    d_whiteChildCount   - out: [batchSize], this parent's white child count
**                                 (both 0 means terminal)
** @param    d_maxMovesStat      - out: running max (black+white) child count seen,
**                                 for a host-side sanity check against maxMovesPerBoard
*/
__global__ void RetrogradeExpandKernel(
    const UINT64_PAIR* __restrict__ input,
    int                              batchSize,
    uint8_t                          inputPlayerBit,
    DevBoardConsts                   consts,
    int                              numRotations,
    int                              maxMovesPerBoard,
    UINT64_PAIR*                     d_children,
    uint8_t*                         d_childColorFlipped,
    uint32_t*                        d_blackChildCount,
    uint32_t*                        d_whiteChildCount,
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
            d_blackChildCount[i] = 0;
            d_whiteChildCount[i] = 0;
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

    uint32_t blackCount = 0, whiteCount = 0;
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
        uint8_t childPlayer  = child.usBoardInfo & 0x01;
        uint8_t colorFlipped = (childPlayer != expectedChildPlayer) ? 1 : 0;

        if ((int)(blackCount + whiteCount) < maxMovesPerBoard)
        {
            size_t base = (size_t)i * (size_t)maxMovesPerBoard;
            size_t slot = (childPlayer == RSF_PLAYER_BLACK)
                        ? base + blackCount
                        : base + (size_t)(maxMovesPerBoard - 1) - whiteCount;
            d_children[slot]          = childRec;
            d_childColorFlipped[slot] = colorFlipped;
        }

        if (childPlayer == RSF_PLAYER_BLACK) blackCount++;
        else                                 whiteCount++;
    }

    d_blackChildCount[i] = blackCount;
    d_whiteChildCount[i] = whiteCount;
    atomicMax(d_maxMovesStat, blackCount + whiteCount);
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
    p->maxMovesPerBoard = maxMovesPerBoard;
    p->boardConsts      = OBCuda_GetBoardConsts();

    size_t childCapacity = (size_t)batchSize * (size_t)maxMovesPerBoard;

    RETRO_GPU_CHECK(cudaMalloc(&p->d_input,             (size_t)batchSize * sizeof(UINT64_PAIR)));
    RETRO_GPU_CHECK(cudaMalloc(&p->d_children,           childCapacity     * sizeof(UINT64_PAIR)));
    RETRO_GPU_CHECK(cudaMalloc(&p->d_childColorFlipped,  childCapacity     * sizeof(uint8_t)));
    RETRO_GPU_CHECK(cudaMalloc(&p->d_blackChildCount,   (size_t)batchSize * sizeof(uint32_t)));
    RETRO_GPU_CHECK(cudaMalloc(&p->d_whiteChildCount,   (size_t)batchSize * sizeof(uint32_t)));
    RETRO_GPU_CHECK(cudaMalloc(&p->d_maxMovesStat, sizeof(uint32_t)));

    p->h_children.resize(childCapacity);
    p->h_childColorFlipped.resize(childCapacity);
    p->h_blackChildCount.resize(batchSize);
    p->h_whiteChildCount.resize(batchSize);

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
    cudaFree(pCtx->d_childColorFlipped);
    cudaFree(pCtx->d_blackChildCount);
    cudaFree(pCtx->d_whiteChildCount);
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
        pCtx->d_children, pCtx->d_childColorFlipped,
        pCtx->d_blackChildCount, pCtx->d_whiteChildCount, pCtx->d_maxMovesStat);
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
    RETRO_GPU_CHECK(cudaMemcpy(pCtx->h_childColorFlipped.data(), pCtx->d_childColorFlipped, childCapacity * sizeof(uint8_t), cudaMemcpyDeviceToHost));
    RETRO_GPU_CHECK(cudaMemcpy(pCtx->h_blackChildCount.data(), pCtx->d_blackChildCount, (size_t)count * sizeof(uint32_t), cudaMemcpyDeviceToHost));
    RETRO_GPU_CHECK(cudaMemcpy(pCtx->h_whiteChildCount.data(), pCtx->d_whiteChildCount, (size_t)count * sizeof(uint32_t), cudaMemcpyDeviceToHost));
}

/*
** Function: RetrogradeGetChildCount
** @brief    See RetrogradeKernels.h.
*/
uint32_t RetrogradeGetChildCount(const RetrogradeGpuContext* pCtx, int parentIdx, int player)
{
    return (player == RSF_PLAYER_BLACK) ? pCtx->h_blackChildCount[parentIdx] : pCtx->h_whiteChildCount[parentIdx];
}

/*
** Function: RetrogradeGetChild
** @brief    See RetrogradeKernels.h.
*/
void RetrogradeGetChild(const RetrogradeGpuContext* pCtx, int parentIdx, int player, int childIdx,
                         UINT64_PAIR* pOutBoard, bool* pOutColorFlipped)
{
    size_t base = (size_t)parentIdx * (size_t)pCtx->maxMovesPerBoard;
    size_t slot = (player == RSF_PLAYER_BLACK)
                ? base + (size_t)childIdx
                : base + (size_t)(pCtx->maxMovesPerBoard - 1) - (size_t)childIdx;

    *pOutBoard        = pCtx->h_children[slot];
    *pOutColorFlipped = pCtx->h_childColorFlipped[slot] != 0;
}
