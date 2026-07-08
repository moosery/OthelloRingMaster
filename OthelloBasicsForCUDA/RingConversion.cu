/*
** Filename:  RingConversion.cu
**
** Purpose:
**   Implements OBCuda_InitRingPermutationTables and OBCuda_TestRingRoundTrip
**   (declared in RingConversion.h): uploading the CPU-built ring permutation
**   tables to GPU constant memory, and a validation kernel proving
**   dev_RowMajorToRing/dev_RingToRowMajor are correct before anything else
**   in the solution depends on them.
*/

/* Includes */
#include "RingConversion.h"
#include "RingPermutation.h"
#include <cuda_runtime.h>
#include <stdio.h>

/* Constants */

/* The Othello starting position, row-major and ring-ordered forms -- the
** ring-ordered constants here must match BoardKeyAllocate.cpp's hardcoded
** values exactly, since both were derived from the same source computation.
** Testing the forward conversion against a value hand-verified on the CPU
** side (not just a self-consistent round trip) catches a permutation table
** that is internally consistent but simply wrong.
*/
static const unsigned long long kStartRowMajorCellsInUse = 0x0000001818000000ULL;
static const unsigned long long kStartRowMajorCellColors = 0x0000000810000000ULL;
static const unsigned long long kStartRingCellsInUse     = 0x000000000000000FULL;
static const unsigned long long kStartRingCellColors     = 0x0000000000000005ULL;

/* Functions */

/*
** Function: OBCuda_InitRingPermutationTables
** @brief    Builds the forward and inverse ring-permutation tables on the
**           CPU and uploads them into this file's GPU constant memory.
*/
void OBCuda_InitRingPermutationTables()
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
** Function: RingRoundTripTestKernel
** @brief    Per-thread: converts one test value row-major->ring->row-major
**           and flags a mismatch. Thread 0 additionally checks the known
**           starting position's forward conversion against its
**           hand-verified expected ring-ordered constant.
** @param    pTestValues - array of row-major test values, one per thread
** @param    count       - number of entries in pTestValues
** @param    pFailCount  - device counter, incremented (atomically) on any failed check
*/
__global__ void RingRoundTripTestKernel(const unsigned long long* pTestValues, int count, int* pFailCount)
{
    int idx = blockIdx.x * blockDim.x + threadIdx.x;

    if (idx < count)
    {
        unsigned long long original  = pTestValues[idx];
        unsigned long long ring      = dev_RowMajorToRing(original);
        unsigned long long roundTrip = dev_RingToRowMajor(ring);

        /* A mismatch here means the forward/inverse tables are not true
        ** inverses of each other for this bit pattern.
        */
        if (roundTrip != original)
            atomicAdd(pFailCount, 1);
    }

    if (idx == 0)
    {
        /* A mismatch here means the tables are self-consistent but simply
        ** wrong -- the round-trip check alone can't catch that.
        */
        if (dev_RowMajorToRing(kStartRowMajorCellsInUse) != kStartRingCellsInUse)
            atomicAdd(pFailCount, 1);
        if (dev_RowMajorToRing(kStartRowMajorCellColors) != kStartRingCellColors)
            atomicAdd(pFailCount, 1);
    }
}

/*
** Function: OBCuda_TestRingRoundTrip
** @brief    Launches RingRoundTripTestKernel over a handful of edge-case and
**           known-value test inputs and reports whether every check passed.
** @return   true if every check passed.
*/
bool OBCuda_TestRingRoundTrip()
{
    const unsigned long long testValues[] = {
        0x0000000000000000ULL,
        0xFFFFFFFFFFFFFFFFULL,
        0x8000000000000000ULL,
        0x0000000000000001ULL,
        0xA5A5A5A5A5A5A5A5ULL,
        kStartRowMajorCellsInUse,
        kStartRowMajorCellColors,
    };
    const int  count = (int)(sizeof(testValues) / sizeof(testValues[0]));

    unsigned long long*  pDeviceValues    = nullptr;
    int*                 pDeviceFailCount = nullptr;
    int                  hostFailCount    = 0;
    bool                 ok               = false;

    if (cudaMalloc(&pDeviceValues, sizeof(testValues)) != cudaSuccess)
    {
        fprintf(stderr, "OBCuda_TestRingRoundTrip: cudaMalloc(pDeviceValues) failed\n");
        return false;
    }
    if (cudaMalloc(&pDeviceFailCount, sizeof(int)) != cudaSuccess)
    {
        fprintf(stderr, "OBCuda_TestRingRoundTrip: cudaMalloc(pDeviceFailCount) failed\n");
        cudaFree(pDeviceValues);
        return false;
    }

    cudaMemcpy(pDeviceValues, testValues, sizeof(testValues), cudaMemcpyHostToDevice);
    cudaMemcpy(pDeviceFailCount, &hostFailCount, sizeof(int), cudaMemcpyHostToDevice);

    RingRoundTripTestKernel<<<1, count>>>(pDeviceValues, count, pDeviceFailCount);

    if (cudaDeviceSynchronize() != cudaSuccess)
    {
        fprintf(stderr, "OBCuda_TestRingRoundTrip: kernel launch/execution failed: %s\n",
                cudaGetErrorString(cudaGetLastError()));
    }
    else
    {
        cudaMemcpy(&hostFailCount, pDeviceFailCount, sizeof(int), cudaMemcpyDeviceToHost);
        ok = (hostFailCount == 0);
        if (!ok)
            fprintf(stderr, "OBCuda_TestRingRoundTrip: %d check(s) failed\n", hostFailCount);
    }

    cudaFree(pDeviceValues);
    cudaFree(pDeviceFailCount);

    return ok;
}
