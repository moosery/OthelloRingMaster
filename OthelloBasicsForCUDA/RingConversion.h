/*
** Filename:  RingConversion.h
**
** Purpose:
**   Declares/defines the GPU boundary conversion between row-major and
**   ring-ordered board bits: dev_RowMajorToRing/dev_RingToRowMajor are the
**   only place in the whole solution allowed to know both bit orderings
**   exist. Everything else GPU-side (move generation, flip computation,
**   canonicalization in OthelloBasicsForCUDA.h) stays permanently
**   row-major-only; everything CPU-side (OthelloBasics.h) stays permanently
**   ring-order-only. These two functions are the bridge, and nothing else
**   touches it.
**
**   OBCuda_InitRingPermutationTables must be called once, after
**   SetBoardSizeForRun-equivalent setup, before any kernel calls
**   dev_RowMajorToRing/dev_RingToRowMajor -- it uploads the forward/inverse
**   permutation tables (built on the CPU by OthelloBasics/RingPermutation.h,
**   which never touches an actual board's bits) into GPU constant memory.
**
** Notes:
**   The permutation tables are always the full 8x8 depth (N=8, offset=0)
**   regardless of actual board size -- see RingPermutation.h Notes for why.
**
**   The constant-memory tables below are declared `static`, so each .cu
**   file that includes this header gets its OWN independent copy (CUDA
**   device-scope globals default to external linkage; `static` avoids a
**   duplicate-symbol link error if more than one .cu file ever includes
**   this header). That also means OBCuda_InitRingPermutationTables must be
**   called once per .cu file that uses these functions, not just once
**   solution-wide, if this header is ever included by more than one .cu
**   file. Today only RingConversion.cu does.
*/

#pragma once

/* Includes */
#include "OthelloBasicsForCUDA.h"

/* Functions */

/*
** Function: OBCuda_InitRingPermutationTables
** @brief    Builds the forward and inverse ring-permutation tables on the
**           CPU and uploads them into this file's GPU constant memory.
**           Must be called once before any kernel in this compilation unit
**           calls dev_RowMajorToRing/dev_RingToRowMajor.
*/
void OBCuda_InitRingPermutationTables();

/*
** Function: OBCuda_TestRingRoundTrip
** @brief    Launches a validation kernel checking that dev_RowMajorToRing
**           and dev_RingToRowMajor are true inverses of each other across a
**           set of test values, and that the known Othello starting
**           position converts to the same ring-ordered constant already
**           hand-verified on the CPU side (see BoardKeyAllocate.cpp).
** @return   true if every check passed.
*/
bool OBCuda_TestRingRoundTrip();

#ifdef __CUDACC__

/* Forward/inverse ring-position permutation tables, uploaded by
** OBCuda_InitRingPermutationTables. See file Notes on `static` linkage.
*/
static __constant__ int g_ringForwardPerm[64];
static __constant__ int g_ringInversePerm[64];

/*
** Function: dev_GatherByRingPermutation
** @brief    Reorders value's 64 bits according to perm: output bit k comes
**           from input bit perm[k]. The same primitive works for both
**           directions -- pass g_ringForwardPerm for row-major->ring, or
**           g_ringInversePerm for ring->row-major.
** @param    value - the 64-bit value to reorder
** @param    perm  - the 64-entry permutation table (output position -> input bit position)
** @return   value with its bits reordered according to perm.
*/
__device__ __forceinline__
unsigned long long dev_GatherByRingPermutation(unsigned long long value, const int* perm)
{
    unsigned long long result = 0;

    for (int k = 0; k < 64; k++)
    {
        unsigned long long bit = (value >> (63 - perm[k])) & 1ULL;
        result |= (bit << (63 - k));
    }
    return result;
}

/*
** Function: dev_RowMajorToRing
** @brief    Converts a row-major-ordered 64-bit board value to ring order.
** @param    rowMajorValue - the row-major-ordered value (e.g. BOARD::ullCellsInUse)
** @return   The same 64 bits, reordered into ring order.
*/
__device__ __forceinline__
unsigned long long dev_RowMajorToRing(unsigned long long rowMajorValue)
{
    return dev_GatherByRingPermutation(rowMajorValue, g_ringForwardPerm);
}

/*
** Function: dev_RingToRowMajor
** @brief    Converts a ring-ordered 64-bit board value back to row-major order.
** @param    ringValue - the ring-ordered value (e.g. BOARD_KEY::ullCellsInUse)
** @return   The same 64 bits, reordered into row-major order.
*/
__device__ __forceinline__
unsigned long long dev_RingToRowMajor(unsigned long long ringValue)
{
    return dev_GatherByRingPermutation(ringValue, g_ringInversePerm);
}

#endif // __CUDACC__
