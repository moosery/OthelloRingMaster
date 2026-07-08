/*
** Filename:  RingPermutation.h
**
** Purpose:
**   Declares the ring-order permutation table builders, promoted out of an
**   earlier offline analysis tool's original code: GenerateRingOrder
**   produces the (row,col) visiting order for an NxN ring walk (outermost
**   perimeter first, innermost last); BuildRingPermutation turns that into a
**   forward table (ring-position -> row-major absolute bit index);
**   BuildInverseRingPermutation produces the reverse mapping (row-major
**   absolute bit index -> ring-position), needed by BoardKeyPrint to know
**   which ring bit a given (row,col) displays. SelfCheckRingOrder validates
**   GenerateRingOrder(8) against a hand-derived cell order.
**
**   These functions only ever produce/validate plain int tables -- they never
**   touch an actual board's bits. Applying a table to transform real board
**   bits is GPU-only (see OthelloBasicsForCUDA), per the strict CPU-organizes/
**   GPU-solves boundary.
**
** Notes:
**   Ring geometry is always the full 8x8 depth (N=8, boardOffset=0)
**   regardless of actual board size -- a smaller board's active cells are
**   already centered within the 8x8 word by the production encoding, so the
**   same fixed permutation applies to every board size. See
**   project_ring_split_validated_findings memory for why.
*/

#pragma once

/* Includes */
#include <vector>
#include <utility>

/* Functions */

/*
** Function: GenerateRingOrder
** @brief    Produces the (row,col) visiting order for an NxN ring walk:
**           outermost ring first (clockwise from top-left corner), innermost last.
** @param    N - board dimension (always 8 in practice; see Notes)
** @return   Vector of N*N (row,col) pairs, 0-indexed within the NxN board, in ring order.
*/
std::vector<std::pair<int, int>> GenerateRingOrder(int N);

/*
** Function: BuildRingPermutation
** @brief    Maps ring-order position k (0 = MSB of a ring-gathered value) to
**           the original row-major bit index-from-MSB (row*8+col).
** @param    N           - board dimension (always 8 in practice; see Notes)
** @param    boardOffset - offset centering a ring frame smaller than 8 within the 8x8 word; always 0 in practice
** @return   Vector of N*N indices: perm[k] = row-major index for ring-position k.
*/
std::vector<int> BuildRingPermutation(int N, int boardOffset);

/*
** Function: BuildInverseRingPermutation
** @brief    Reverses BuildRingPermutation: maps a row-major bit index-from-MSB
**           to its ring-order position.
** @param    N           - board dimension (always 8 in practice; see Notes)
** @param    boardOffset - offset centering a ring frame smaller than 8 within the 8x8 word; always 0 in practice
** @return   Vector of N*N indices: invPerm[rowMajorIdx] = ring-position k.
*/
std::vector<int> BuildInverseRingPermutation(int N, int boardOffset);

/*
** Function: SelfCheckRingOrder
** @brief    Runs GenerateRingOrder(8) against a hand-derived cell order and
**           reports any mismatch.
** @return   true if every position matched the hand-derived order.
*/
bool SelfCheckRingOrder();
