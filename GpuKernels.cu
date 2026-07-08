/*
** Filename:  GpuKernels.cu
**
** Purpose:
**   Implements the GPU accumulator declared in GpuKernels.h: ExpandKernel
**   (move generation/flip/canonicalize, plus the ring<->row-major boundary
**   conversion), the two-pass CUB radix-sort dedup pipeline
**   (SortAndDedupRegion and its helper kernels), and the accumulator's
**   create/destroy/process/flush API.
**
** Notes:
**   Adapted from an earlier solver implementation, renamed onto
**   BOARD_KEY_DISK -> UINT64_PAIR throughout (see Utility/RingStoreFile.h) --
**   every kernel except ExpandKernel treats boards as opaque 16-byte sort
**   keys and needed no logic change at all, only the type rename.
**
**   ExpandKernel is the one place in this whole file that interprets board
**   bits, and per the CPU-organizes/GPU-solves boundary and
**   project_gpu_reorder_integration_design memory, live board math (move
**   generation, flip computation, canonicalization) must stay row-major.
**   So ExpandKernel converts at its own boundary: incoming UINT64_PAIR
**   fields are ring-ordered (that is the store's on-disk format) and get
**   converted to row-major via dev_RingToRowMajor before any move-gen touches
**   them; each child's resulting fields get converted back to ring order via
**   dev_RowMajorToRing immediately before the scatter-write into d_accum, so
**   d_accum is ring-ordered from the moment it is written and everything
**   downstream (SortAndDedupRegion's CUB sort/dedup/compaction) needs no
**   awareness that ring order is involved at all -- a bijective permutation
**   preserves equality, so dedup correctness is unaffected by doing the
**   conversion first.
**
**   RingConversion.h's forward/inverse permutation tables are declared
**   `static` __constant__ memory, so this file gets its OWN independent copy
**   from OthelloBasicsForCUDA/RingConversion.cu's copy -- GpuKernels_
**   InitRingPermutationTables() (below) uploads this file's copy, and
**   GpuAccumulatorCreate calls it internally so no caller has to remember to.
*/

/* Includes */
#include "GpuKernels.h"
#include "OthelloBasicsForCUDA.h"    /* BOARD, dev_boardMoveCalculator/dev_playMove/dev_canonicalize */
#include "RingConversion.h"          /* dev_RowMajorToRing/dev_RingToRowMajor, this TU's constant tables */
#include "RingPermutation.h"         /* BuildRingPermutation/BuildInverseRingPermutation, for this TU's init */
#include "Error.h"
#include <cuda_runtime.h>
#include <cub/cub.cuh>
#include <string.h>

/* Macros and Defines */

/*
** CUDA error helper -- Fatals immediately rather than propagating an error
** code, since a CUDA runtime failure mid-solve leaves accumulator/stream
** state unrecoverable anyway.
*/
#define GPU_CHECK(call) \
    do { \
        cudaError_t _e = (call); \
        if (_e != cudaSuccess) \
            Fatal(FATAL_GPU_ERROR, "CUDA error %s:%d  %s", \
                  __FILE__, __LINE__, cudaGetErrorString(_e)); \
    } while (0)

/* Structures and Types */

/*
** Type:    __GpuAccumulator
** @brief   Concrete state behind the opaque GpuAccumulator handle.
** @details Memory layout (two-stack UINT64_PAIR edition):
**
**   Per-batch (device):
**     d_input          [batchSize] UINT64_PAIR    H2D staging
**     d_blackWritePos  uint32_t                   atomic counter for black stack
**     d_whiteWritePos  uint32_t                   atomic counter for white stack
**
**   Per-batch (pinned host):
**     h_input          [batchSize] UINT64_PAIR
**     h_blackWritePos  uint32_t
**     h_whiteWritePos  uint32_t
**
**   Large buffers (device) -- sized by accumCapacity:
**     d_accum    [accumCapacity] UINT64_PAIR  two-stack layout:
**                  black stack: indices [0 .. blackWriteOffset-1]  (grows up)
**                  white stack: indices [cap-1 .. cap-whiteWriteOffset] (grows down)
**     d_gather   [accumCapacity] UINT64_PAIR  sorted+deduped output:
**                  [0 .. blackUnique-1]                        black boards
**                  [blackUnique .. blackUnique+whiteUnique-1]  white boards
**     d_fieldA   [accumCapacity] uint64_t   sort key ping-pong A / not-flags temp
**     d_fieldB   [accumCapacity] uint64_t   sort key ping-pong B
**     d_indicesA [accumCapacity] uint32_t   sort value ping-pong A (final permutation)
**     d_indicesB [accumCapacity] uint32_t   sort value ping-pong B / prefix-sum output
**     d_flags    [accumCapacity] uint8_t    dup flags (1 = duplicate)
**
**   Memory budget: 80% of totalGpuBytes.
**     Expand overhead: batchSize*16 + 8 bytes (d_input + two atomic counters).
**     Per accumCapacity slot: 16+16+8+8+4+4+1 = 57 bytes.
*/
struct __GpuAccumulator
{
    /* Per-batch (device) */
    UINT64_PAIR*  d_input;
    uint32_t*     d_blackWritePos;
    uint32_t*     d_whiteWritePos;

    /* Per-batch (pinned host) */
    UINT64_PAIR*  h_input;
    uint32_t*     h_blackWritePos;
    uint32_t*     h_whiteWritePos;

    /* Large device buffers */
    UINT64_PAIR*  d_accum;
    UINT64_PAIR*  d_gather;
    uint64_t*     d_fieldA;
    uint64_t*     d_fieldB;
    uint32_t*     d_indicesA;
    uint32_t*     d_indicesB;
    uint8_t*      d_flags;
    uint32_t*     d_batchStats;    /* [0]=pass, [1]=terminal, [2]=maxMoves */
    uint32_t      h_batchStats[3];

    /* Pre-allocated CUB temp storage */
    void*   d_sortTemp;
    size_t  sortTempBytes;
    void*   d_scanTemp;
    size_t  scanTempBytes;

    cudaStream_t    stream;
    DevBoardConsts  boardConsts;

    int     batchSize;
    int     maxMovesPerBoard;
    int     numRotations;
    size_t  accumCapacity;

    size_t  blackWriteOffset;   /* total black boards written since last reset */
    size_t  whiteWriteOffset;   /* total white boards written since last reset */
    int     uniqueBlackCount;   /* set by GpuFlushPrepare */
    int     uniqueWhiteCount;   /* set by GpuFlushPrepare */
    int     uniqueCount;        /* = uniqueBlackCount + uniqueWhiteCount */
};

/* Functions */

/*
** Function: GpuKernels_InitRingPermutationTables
** @brief    Builds the forward/inverse ring-permutation tables on the CPU
**           and uploads them into THIS FILE's GPU constant memory.
** @details  RingConversion.h's `static __constant__` tables give every .cu
**           file that includes it its own independent copy -- this file is
**           the second one (RingConversion.cu is the first), so it needs its
**           own init call targeting its own copy. OthelloRingMaster.cpp's
**           existing call to OBCuda_InitRingPermutationTables() only
**           populates RingConversion.cu's copy; it does not reach this one.
*/
static void GpuKernels_InitRingPermutationTables()
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
** Function: ExpandKernel
** @brief    One thread per input board: converts the ring-ordered input to
**           row-major, generates children (handling the pass/terminal
**           cases), canonicalizes each child, converts back to ring order,
**           and scatters directly into the two-stack d_accum via atomic
**           counters -- black children at d_accum[atomicAdd(d_blackWritePos)],
**           white children at d_accum[cap-1-atomicAdd(d_whiteWritePos)].
** @details  No separate scatter kernel is needed; the intermediate results
**           array is eliminated entirely.
** @param    input            - batch of ring-ordered board records
** @param    batchSize        - number of records in input
** @param    inputPlayerBit   - next player to move for this whole batch (1=black, 0=white)
** @param    consts           - board-size masks for move generation
** @param    numRotations     - canonicalization symmetry count (1, 4, 8, or 16)
** @param    d_batchStats     - out: [0]+=pass, [1]+=terminal, [2]=max(children per board)
** @param    d_accum          - two-stack output buffer (ring-ordered)
** @param    accumCapacity    - capacity of d_accum
** @param    d_blackWritePos  - atomic write cursor for the black (grows-up) stack
** @param    d_whiteWritePos  - atomic write cursor for the white (grows-down) stack
*/
__global__ void ExpandKernel(
    const UINT64_PAIR* __restrict__ input,
    int                              batchSize,
    uint8_t                          inputPlayerBit,
    DevBoardConsts                   consts,
    int                              numRotations,
    uint32_t*                        d_batchStats,
    UINT64_PAIR*                     d_accum,
    uint32_t                         accumCapacity,
    uint32_t*                        d_blackWritePos,
    uint32_t*                        d_whiteWritePos)
{
    int i = (int)(blockIdx.x * blockDim.x + threadIdx.x);
    if (i >= batchSize) return;

    /* Reconstruct a full row-major BOARD from the ring-ordered input record
    ** plus the batch's external player bit -- this is the "on the way in"
    ** half of the ring<->row-major boundary conversion.
    */
    const UINT64_PAIR srcRec = input[i];
    BOARD src = {};
    src.ullCellsInUse = dev_RingToRowMajor(srcRec.hi);
    src.ullCellColors = dev_RingToRowMajor(srcRec.lo);
    src.usBoardInfo   = inputPlayerBit;

    dev_boardMoveCalculator(&src, consts);
    unsigned long long moves = src.ullPossibleMoves;

    if (moves == 0)
    {
        /* Current player can't move -- try a pass. */
        BOARD passBoard = src;
        SETBOARDNEXTPLAYERFLIP(&passBoard);
        passBoard.ullPossibleMoves = 0;
        dev_boardMoveCalculator(&passBoard, consts);
        unsigned long long oppMoves = passBoard.ullPossibleMoves;

        if (oppMoves == 0)
        {
            atomicAdd(&d_batchStats[1], 1u);   /* terminal: both players stuck */
            return;
        }

        atomicAdd(&d_batchStats[0], 1u);   /* pass: opponent moves */
        moves = oppMoves;
        src   = passBoard;   /* expand opponent's moves as next-level children */
    }

    int count = 0;
    while (moves)
    {
        int moveIdx = __clzll(moves);
        moves &= ~(0x8000000000000000ULL >> moveIdx);

        BOARD child = {};
        dev_playMove(&src, &child, moveIdx);
        dev_canonicalize(&child, numRotations, consts);

        /* "On the way out" half of the boundary conversion: convert the
        ** canonicalized child's fields back to ring order before it ever
        ** reaches d_accum, so everything downstream (sort/dedup/compaction)
        ** operates on already-ring-ordered opaque keys.
        */
        UINT64_PAIR childRec;
        childRec.hi = dev_RowMajorToRing(child.ullCellsInUse);
        childRec.lo = dev_RowMajorToRing(child.ullCellColors);
        int childPlayer = child.usBoardInfo & 0x01;

        if (childPlayer == 1)   /* black */
        {
            uint32_t pos = atomicAdd(d_blackWritePos, 1u);
            if (pos < accumCapacity)
                d_accum[pos] = childRec;
        }
        else   /* white */
        {
            uint32_t pos = atomicAdd(d_whiteWritePos, 1u);
            if (pos < accumCapacity)
                d_accum[accumCapacity - 1u - pos] = childRec;
        }
        count++;
    }

    __threadfence_block();   /* ensure prior atomicAdds are visible before atomicMax */
    atomicMax(&d_batchStats[2], (uint32_t)count);
}

/*
** ============================================================
** Sort helpers -- 2-pass CUB DeviceRadixSort::SortPairs on UINT64_PAIR
** fields. LSB-first: lo then hi -> ascending by (hi, lo). Each kernel takes
** a base pointer so it can operate on either the black or white region.
** These never interpret board bits -- hi/lo are opaque sort keys.
** ============================================================
*/

/*
** Function: InitIndicesKernel
** @brief    Fills indices[i] = i, the identity permutation CUB's radix sort
**           starts from.
*/
__global__ void InitIndicesKernel(uint32_t* indices, uint32_t count)
{
    uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < count) indices[i] = i;
}

/*
** Function: ExtractFieldKernel
** @brief    Extracts one field (hi or lo) from each board into a flat sort-key array.
** @param    fieldIdx - 0=hi  1=lo
*/
__global__ void ExtractFieldKernel(
    const UINT64_PAIR* __restrict__ boards,
    uint32_t                        count,
    int                              fieldIdx,
    uint64_t*                       out)
{
    uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= count) return;
    out[i] = (fieldIdx == 0) ? boards[i].hi : boards[i].lo;
}

/*
** Function: GatherFieldKernel
** @brief    Extracts one field, permuted by perm (the previous sort pass's result).
** @param    fieldIdx - 0=hi  1=lo
*/
__global__ void GatherFieldKernel(
    const UINT64_PAIR* __restrict__ boards,
    const uint32_t*     __restrict__ perm,
    uint32_t                         count,
    int                               fieldIdx,
    uint64_t*                        out)
{
    uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= count) return;
    const UINT64_PAIR& b = boards[perm[i]];
    out[i] = (fieldIdx == 0) ? b.hi : b.lo;
}

/*
** ============================================================
** Dedup kernels
** ============================================================
*/

/*
** Function: MarkDupFlagsKernel
** @brief    Flags boards[perm[i]] as a duplicate if it equals boards[perm[i-1]]
**           (adjacent-compare in already-sorted order).
*/
__global__ void MarkDupFlagsKernel(
    const UINT64_PAIR* __restrict__ boards,
    const uint32_t*     __restrict__ perm,
    uint8_t*                         flags,
    uint32_t                         count)
{
    uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= count) return;
    if (i == 0) { flags[0] = 0; return; }
    const UINT64_PAIR& a = boards[perm[i - 1]];
    const UINT64_PAIR& b = boards[perm[i]];
    flags[i] = (a.hi == b.hi && a.lo == b.lo) ? 1u : 0u;
}

/*
** Function: InvertFlagsKernel
** @brief    notFlags[i] = 1 if flags[i] == 0. Written as uint32_t for cub::DeviceScan.
*/
__global__ void InvertFlagsKernel(
    const uint8_t* __restrict__ flags,
    uint32_t*                   notFlags,
    uint32_t                    count)
{
    uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < count) notFlags[i] = flags[i] ? 0u : 1u;
}

/*
** Function: CompactKernel
** @brief    Scatters unique boards (in sorted order) into out at gatherOffset + outPos[i].
*/
__global__ void CompactKernel(
    const UINT64_PAIR* __restrict__ accum,
    const uint32_t*     __restrict__ perm,
    const uint8_t*      __restrict__ flags,
    const uint32_t*     __restrict__ outPos,
    uint32_t                         count,
    UINT64_PAIR*                     out,
    uint32_t                         gatherOffset)
{
    uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= count || flags[i]) return;
    out[gatherOffset + outPos[i]] = accum[perm[i]];
}

/*
** Function: SortAndDedupRegion
** @brief    Sorts region [base .. base+N-1] by (hi, lo), deduplicates, and
**           compacts unique boards into out[gatherOffset..].
** @param    p            - the accumulator whose pre-allocated scratch buffers to use
** @param    base         - start of the region to sort (either the black or white stack)
** @param    N             - number of records in the region
** @param    out          - destination for compacted unique records (d_gather)
** @param    gatherOffset - offset into out where this region's unique records start
** @return   The unique count.
*/
static int SortAndDedupRegion(GpuAccumulator* p,
                               UINT64_PAIR* base, uint32_t N,
                               UINT64_PAIR* out, uint32_t gatherOffset)
{
    if (N == 0) return 0;

    int threads = 256;
    int blocks  = ((int)N + threads - 1) / threads;

    void*   d_temp    = p->d_sortTemp;
    size_t  tempBytes = p->sortTempBytes;

    /* Pass 1: sort by lo (LSB field) */
    ExtractFieldKernel<<<blocks, threads>>>(base, N, 1, p->d_fieldA);
    InitIndicesKernel <<<blocks, threads>>>(p->d_indicesA, N);
    {
        cub::DoubleBuffer<uint64_t> kDb(p->d_fieldA, p->d_fieldB);
        cub::DoubleBuffer<uint32_t> vDb(p->d_indicesA, p->d_indicesB);
        GPU_CHECK((cub::DeviceRadixSort::SortPairs(d_temp, tempBytes, kDb, vDb, (int)N)));
        if (vDb.selector != 0)
            GPU_CHECK(cudaMemcpy(p->d_indicesA, p->d_indicesB,
                                 N * sizeof(uint32_t), cudaMemcpyDeviceToDevice));
    }

    /* Pass 2: sort by hi (MSB field) */
    GatherFieldKernel<<<blocks, threads>>>(base, p->d_indicesA, N, 0, p->d_fieldA);
    {
        cub::DoubleBuffer<uint64_t> kDb(p->d_fieldA, p->d_fieldB);
        cub::DoubleBuffer<uint32_t> vDb(p->d_indicesA, p->d_indicesB);
        GPU_CHECK((cub::DeviceRadixSort::SortPairs(d_temp, tempBytes, kDb, vDb, (int)N)));
        if (vDb.selector != 0)
            GPU_CHECK(cudaMemcpy(p->d_indicesA, p->d_indicesB,
                                 N * sizeof(uint32_t), cudaMemcpyDeviceToDevice));
    }

    /* Mark adjacent duplicates in sorted order */
    MarkDupFlagsKernel<<<blocks, threads>>>(base, p->d_indicesA, p->d_flags, N);

    /* Compact: invert flags, prefix-sum for output positions, scatter unique boards */
    uint32_t* d_notFlags = reinterpret_cast<uint32_t*>(p->d_fieldA);
    InvertFlagsKernel<<<blocks, threads>>>(p->d_flags, d_notFlags, N);

    size_t scanBytes = p->scanTempBytes;
    GPU_CHECK((cub::DeviceScan::ExclusiveSum(p->d_scanTemp, scanBytes,
                                              d_notFlags, p->d_indicesB, (int)N)));

    CompactKernel<<<blocks, threads>>>(base, p->d_indicesA, p->d_flags,
                                       p->d_indicesB, N, out, gatherOffset);
    GPU_CHECK(cudaDeviceSynchronize());

    /* uniqueCount = last prefix-sum value + last not-flag value */
    uint32_t h_lastPos, h_lastNotFlag;
    GPU_CHECK(cudaMemcpy(&h_lastPos,     p->d_indicesB + (N - 1), sizeof(uint32_t),
                         cudaMemcpyDeviceToHost));
    GPU_CHECK(cudaMemcpy(&h_lastNotFlag, d_notFlags    + (N - 1), sizeof(uint32_t),
                         cudaMemcpyDeviceToHost));
    return (int)(h_lastPos + h_lastNotFlag);
}

/*
** Function: GpuAccumulatorCreate
** @brief    Allocates device memory (80% of totalGpuBytes) and creates a new accumulator.
** @param    batchSize        - boards per H2D batch
** @param    maxMovesPerBoard - worst-case children per board, for capacity checks
** @param    totalGpuBytes    - total GPU memory budget to size the accumulator from
** @return   A new GpuAccumulator.
*/
GpuAccumulator* GpuAccumulatorCreate(int batchSize, int maxMovesPerBoard, size_t totalGpuBytes)
{
    /* This file's own copy of the ring-permutation constant tables --
    ** required before ExpandKernel's first launch, see file Notes.
    */
    GpuKernels_InitRingPermutationTables();

    GpuAccumulator* p = new GpuAccumulator();
    memset(p, 0, sizeof(*p));

    p->batchSize        = batchSize;
    p->maxMovesPerBoard = maxMovesPerBoard;
    p->numRotations     = 16;
    p->boardConsts      = OBCuda_GetBoardConsts();

    /* Per-slot device cost: 16(d_accum)+16(d_gather)+8(fieldA)+8(fieldB)+4(indA)+4(indB)+1(flags)=57
    ** Expand overhead: batchSize*16 (d_input) + 2*4 (two atomic counters)
    */
    size_t expandBytes = (size_t)batchSize * sizeof(UINT64_PAIR) + 2 * sizeof(uint32_t);
    size_t budget      = totalGpuBytes * 8 / 10;
    if (budget > expandBytes) budget -= expandBytes;
    p->accumCapacity   = budget / 57;

    GPU_CHECK(cudaStreamCreate(&p->stream));

    /* Per-batch device allocations */
    GPU_CHECK(cudaMalloc(&p->d_input,         (size_t)batchSize * sizeof(UINT64_PAIR)));
    GPU_CHECK(cudaMalloc(&p->d_blackWritePos, sizeof(uint32_t)));
    GPU_CHECK(cudaMalloc(&p->d_whiteWritePos, sizeof(uint32_t)));

    /* Pinned host buffers */
    GPU_CHECK(cudaMallocHost(&p->h_input,         (size_t)batchSize * sizeof(UINT64_PAIR)));
    GPU_CHECK(cudaMallocHost(&p->h_blackWritePos, sizeof(uint32_t)));
    GPU_CHECK(cudaMallocHost(&p->h_whiteWritePos, sizeof(uint32_t)));

    /* Large device buffers */
    GPU_CHECK(cudaMalloc(&p->d_accum,    p->accumCapacity * sizeof(UINT64_PAIR)));
    GPU_CHECK(cudaMalloc(&p->d_gather,   p->accumCapacity * sizeof(UINT64_PAIR)));
    GPU_CHECK(cudaMalloc(&p->d_fieldA,   p->accumCapacity * sizeof(uint64_t)));
    GPU_CHECK(cudaMalloc(&p->d_fieldB,   p->accumCapacity * sizeof(uint64_t)));
    GPU_CHECK(cudaMalloc(&p->d_indicesA, p->accumCapacity * sizeof(uint32_t)));
    GPU_CHECK(cudaMalloc(&p->d_indicesB, p->accumCapacity * sizeof(uint32_t)));
    GPU_CHECK(cudaMalloc(&p->d_flags,      p->accumCapacity * sizeof(uint8_t)));
    GPU_CHECK(cudaMalloc(&p->d_batchStats, 3 * sizeof(uint32_t)));
    GPU_CHECK(cudaMemset( p->d_batchStats, 0, 3 * sizeof(uint32_t)));

    /* Pre-allocate CUB temp storage for sort and prefix-sum (sized for worst-case = accumCapacity) */
    {
        cub::DoubleBuffer<uint64_t> kq(p->d_fieldA, p->d_fieldB);
        cub::DoubleBuffer<uint32_t> vq(p->d_indicesA, p->d_indicesB);
        p->sortTempBytes = 0;
        cub::DeviceRadixSort::SortPairs(nullptr, p->sortTempBytes, kq, vq, (int)p->accumCapacity);
        GPU_CHECK(cudaMalloc(&p->d_sortTemp, p->sortTempBytes));
    }
    {
        uint32_t* dummy = reinterpret_cast<uint32_t*>(p->d_fieldA);
        p->scanTempBytes = 0;
        cub::DeviceScan::ExclusiveSum(nullptr, p->scanTempBytes, dummy, p->d_indicesB, (int)p->accumCapacity);
        GPU_CHECK(cudaMalloc(&p->d_scanTemp, p->scanTempBytes));
    }

    GPU_CHECK(cudaMemset(p->d_blackWritePos, 0, sizeof(uint32_t)));
    GPU_CHECK(cudaMemset(p->d_whiteWritePos, 0, sizeof(uint32_t)));
    return p;
}

/*
** Function: GpuAccumulatorDestroy
** @brief    Frees all device/pinned-host memory owned by pAccum.
** @param    pAccum - the accumulator to destroy
*/
void GpuAccumulatorDestroy(GpuAccumulator* pAccum)
{
    cudaStreamSynchronize(pAccum->stream);
    cudaStreamDestroy(pAccum->stream);
    cudaFree(pAccum->d_input);
    cudaFree(pAccum->d_blackWritePos);
    cudaFree(pAccum->d_whiteWritePos);
    cudaFreeHost(pAccum->h_input);
    cudaFreeHost(pAccum->h_blackWritePos);
    cudaFreeHost(pAccum->h_whiteWritePos);
    cudaFree(pAccum->d_accum);
    cudaFree(pAccum->d_gather);
    cudaFree(pAccum->d_fieldA);
    cudaFree(pAccum->d_fieldB);
    cudaFree(pAccum->d_indicesA);
    cudaFree(pAccum->d_indicesB);
    cudaFree(pAccum->d_flags);
    cudaFree(pAccum->d_batchStats);
    cudaFree(pAccum->d_sortTemp);
    cudaFree(pAccum->d_scanTemp);
    delete pAccum;
}

/*
** Function: GpuAccumulatorHasRoom
** @brief    Checks (pessimistically) whether pAccum has room for another batch.
** @param    pAccum         - the accumulator to check
** @param    nextBatchCount - number of boards in the next batch
** @return   true if the batch is safe to process without overflowing the accumulator.
*/
bool GpuAccumulatorHasRoom(const GpuAccumulator* pAccum, int nextBatchCount)
{
    size_t worstCase = (size_t)nextBatchCount * (size_t)pAccum->maxMovesPerBoard;
    size_t used      = pAccum->blackWriteOffset + pAccum->whiteWriteOffset;
    return (used + worstCase) <= pAccum->accumCapacity;
}

/*
** Function: GpuProcessBatch
** @brief    H2D-copies count boards, expands each, and scatters unique-so-far
**           children directly into the accumulator's two-stack layout.
** @param    pAccum    - the accumulator to expand into
** @param    pBoards   - ring-ordered board records to expand
** @param    count     - number of records in pBoards
** @param    playerBit - 1=black, 0=white -- next player to move for this whole batch
*/
void GpuProcessBatch(GpuAccumulator* pAccum, const UINT64_PAIR* pBoards,
                     int count, uint8_t playerBit)
{
    cudaStream_t s = pAccum->stream;

    memcpy(pAccum->h_input, pBoards, (size_t)count * sizeof(UINT64_PAIR));
    GPU_CHECK(cudaMemcpyAsync(pAccum->d_input, pAccum->h_input,
                              (size_t)count * sizeof(UINT64_PAIR),
                              cudaMemcpyHostToDevice, s));

    /* Set atomic counters to current accumulated offsets before expand */
    uint32_t bwo = (uint32_t)pAccum->blackWriteOffset;
    uint32_t wwo = (uint32_t)pAccum->whiteWriteOffset;
    GPU_CHECK(cudaMemcpyAsync(pAccum->d_blackWritePos, &bwo, sizeof(uint32_t),
                              cudaMemcpyHostToDevice, s));
    GPU_CHECK(cudaMemcpyAsync(pAccum->d_whiteWritePos, &wwo, sizeof(uint32_t),
                              cudaMemcpyHostToDevice, s));

    /* Expand + direct two-stack scatter (one thread per input board) */
    {
        int threads = 256;
        int blocks  = (count + threads - 1) / threads;
        ExpandKernel<<<blocks, threads, 0, s>>>(
            pAccum->d_input, count, playerBit,
            pAccum->boardConsts, pAccum->numRotations,
            pAccum->d_batchStats,
            pAccum->d_accum, (uint32_t)pAccum->accumCapacity,
            pAccum->d_blackWritePos, pAccum->d_whiteWritePos);
    }

    /* D2H updated counters to track stack sizes on the host */
    GPU_CHECK(cudaMemcpyAsync(pAccum->h_blackWritePos, pAccum->d_blackWritePos,
                              sizeof(uint32_t), cudaMemcpyDeviceToHost, s));
    GPU_CHECK(cudaMemcpyAsync(pAccum->h_whiteWritePos, pAccum->d_whiteWritePos,
                              sizeof(uint32_t), cudaMemcpyDeviceToHost, s));
    GPU_CHECK(cudaStreamSynchronize(s));

    pAccum->blackWriteOffset = *pAccum->h_blackWritePos;
    pAccum->whiteWriteOffset = *pAccum->h_whiteWritePos;
}

/*
** Function: GpuFlushPrepare
** @brief    Syncs the stream and sorts+dedups both stack regions on device,
**           independently: black region [0..B-1], white region [cap-W..cap-1].
**           Output in d_gather: black first, then white.
** @param    pAccum - the accumulator to flush
** @return   Total unique board count (black + white); 0 if nothing to flush.
*/
int GpuFlushPrepare(GpuAccumulator* pAccum)
{
    uint32_t B = (uint32_t)pAccum->blackWriteOffset;
    if (B > (uint32_t)pAccum->accumCapacity) B = (uint32_t)pAccum->accumCapacity;

    uint32_t wCap = (uint32_t)pAccum->accumCapacity - B;
    uint32_t W    = (uint32_t)pAccum->whiteWriteOffset;
    if (W > wCap) W = wCap;

    if (B == 0 && W == 0)
    {
        GPU_CHECK(cudaMemcpy(pAccum->h_batchStats, pAccum->d_batchStats,
                             3 * sizeof(uint32_t), cudaMemcpyDeviceToHost));
        pAccum->uniqueBlackCount = 0;
        pAccum->uniqueWhiteCount = 0;
        pAccum->uniqueCount      = 0;
        return 0;
    }

    /* Black region: d_accum[0..B-1] */
    pAccum->uniqueBlackCount = SortAndDedupRegion(
        pAccum, pAccum->d_accum, B, pAccum->d_gather, 0);

    /* White region: d_accum[cap-W..cap-1] (stored reverse-chronologically; sort fixes order) */
    UINT64_PAIR* whiteBase = pAccum->d_accum + pAccum->accumCapacity - W;
    pAccum->uniqueWhiteCount = SortAndDedupRegion(
        pAccum, whiteBase, W, pAccum->d_gather, (uint32_t)pAccum->uniqueBlackCount);

    pAccum->uniqueCount = pAccum->uniqueBlackCount + pAccum->uniqueWhiteCount;

    GPU_CHECK(cudaMemcpy(pAccum->h_batchStats, pAccum->d_batchStats,
                         3 * sizeof(uint32_t), cudaMemcpyDeviceToHost));
    return pAccum->uniqueCount;
}

/*
** Function: GpuFlushRead
** @brief    D2H-copies a chunk of the sorted+deduped result for one player.
** @param    pAccum   - the accumulator to read from
** @param    player   - 1=black, 0=white
** @param    offset   - starting index within that player's unique results
** @param    pOut     - out: destination buffer, at least maxCount records
** @param    maxCount - maximum number of records to read
** @return   Count actually copied.
*/
int GpuFlushRead(GpuAccumulator* pAccum, int player, size_t offset,
                 UINT64_PAIR* pOut, int maxCount)
{
    int total      = (player == 1) ? pAccum->uniqueBlackCount : pAccum->uniqueWhiteCount;
    int baseOffset = (player == 1) ? 0                        : pAccum->uniqueBlackCount;
    int avail = total - (int)offset;
    if (avail <= 0) return 0;
    int got = (avail < maxCount) ? avail : maxCount;
    GPU_CHECK(cudaMemcpy(pOut, pAccum->d_gather + baseOffset + (int)offset,
                         (size_t)got * sizeof(UINT64_PAIR), cudaMemcpyDeviceToHost));
    return got;
}

/*
** Function: GpuFlushReset
** @brief    Resets the accumulator for the next accumulation window.
** @param    pAccum - the accumulator to reset
*/
void GpuFlushReset(GpuAccumulator* pAccum)
{
    GPU_CHECK(cudaMemsetAsync(pAccum->d_blackWritePos, 0, sizeof(uint32_t), pAccum->stream));
    GPU_CHECK(cudaMemsetAsync(pAccum->d_whiteWritePos, 0, sizeof(uint32_t), pAccum->stream));
    GPU_CHECK(cudaMemsetAsync(pAccum->d_batchStats,    0, 3 * sizeof(uint32_t), pAccum->stream));
    pAccum->blackWriteOffset = 0;
    pAccum->whiteWriteOffset = 0;
    pAccum->uniqueBlackCount = 0;
    pAccum->uniqueWhiteCount = 0;
    pAccum->uniqueCount      = 0;
    pAccum->h_batchStats[0]  = 0;
    pAccum->h_batchStats[1]  = 0;
    pAccum->h_batchStats[2]  = 0;
}

/*
** Function: GpuAccumulatorWriteOffset
** @brief    Returns the total raw boards accumulated since the last GpuFlushReset.
** @param    pAccum - the accumulator to query
** @return   Total raw board count (pre-dedup; both stacks).
*/
size_t GpuAccumulatorWriteOffset(const GpuAccumulator* pAccum)
{
    return pAccum->blackWriteOffset + pAccum->whiteWriteOffset;
}

/*
** Function: GpuFlushBlackCount
** @brief    Returns the unique black board count from the last GpuFlushPrepare.
*/
int GpuFlushBlackCount(const GpuAccumulator* pAccum) { return pAccum->uniqueBlackCount; }

/*
** Function: GpuFlushWhiteCount
** @brief    Returns the unique white board count from the last GpuFlushPrepare.
*/
int GpuFlushWhiteCount(const GpuAccumulator* pAccum) { return pAccum->uniqueWhiteCount; }

/*
** Function: GpuFlushPassBoards
** @brief    Returns the pass-board count accumulated in the current flush window.
*/
uint32_t GpuFlushPassBoards(const GpuAccumulator* pAccum) { return pAccum->h_batchStats[0]; }

/*
** Function: GpuFlushTermBoards
** @brief    Returns the terminal-board count accumulated in the current flush window.
*/
uint32_t GpuFlushTermBoards(const GpuAccumulator* pAccum) { return pAccum->h_batchStats[1]; }

/*
** Function: GpuFlushMaxMoves
** @brief    Returns the maximum child count produced by any single board in the current flush window.
*/
uint32_t GpuFlushMaxMoves(const GpuAccumulator* pAccum) { return pAccum->h_batchStats[2]; }
