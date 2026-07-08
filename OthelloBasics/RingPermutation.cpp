/*
** Filename:  RingPermutation.cpp
**
** Purpose:
**   Implements the ring-order permutation table builders declared in
**   RingPermutation.h, promoted out of an earlier offline analysis tool's
**   original static functions so both CPU-organizing code and GPU
**   boundary-conversion kernels can share one validated source of truth
**   for the ring layout.
*/

/* Includes */
#include "RingPermutation.h"
#include <stdio.h>
#include <string.h>

/* Constants */

/* Literal cell order given by hand for N=8 (outer ring first, inner last),
** used only to self-check GenerateRingOrder(8) at startup.
*/
static const char* kLiteralN8Order[64] = {
    "A1","A2","A3","A4","A5","A6","A7","A8","B8","C8","D8","E8","F8","G8","H8","H7",
    "H6","H5","H4","H3","H2","H1","G1","F1","E1","D1","C1","B1","B2","B3","B4","B5",
    "B6","B7","C7","D7","E7","F7","G7","G6","G5","G4","G3","G2","F2","E2","D2","C2",
    "C3","C4","C5","C6","D6","E6","F6","F5","F4","F3","E3","D3","D4","D5","E5","E4"
};

/* Functions */

/*
** Function: GenerateRingOrder
** @brief    Produces the (row,col) visiting order for an NxN ring walk:
**           outermost ring first (clockwise from top-left corner), innermost last.
** @param    N - board dimension (always 8 in practice; see RingPermutation.h Notes)
** @return   Vector of N*N (row,col) pairs, 0-indexed within the NxN board, in ring order.
*/
std::vector<std::pair<int, int>> GenerateRingOrder(int N)
{
    std::vector<std::pair<int, int>> order;
    order.reserve((size_t)N * N);

    for (int d = 0; d < N / 2; d++)
    {
        int lo = d, hi = N - 1 - d;

        for (int c = lo; c <= hi; c++)               order.push_back({ lo, c });   /* top row, L->R          */
        for (int r = lo + 1; r <= hi; r++)           order.push_back({ r, hi });   /* right col, top->bottom */
        if (hi > lo)
            for (int c = hi - 1; c >= lo; c--)       order.push_back({ hi, c });   /* bottom row, R->L       */
        for (int r = hi - 1; r >= lo + 1; r--)       order.push_back({ r, lo });   /* left col, bottom->top  */
    }
    return order;
}

/*
** Function: BuildRingPermutation
** @brief    Maps ring-order position k (0 = MSB of a ring-gathered value) to
**           the original row-major bit index-from-MSB (row*8+col).
** @param    N           - board dimension (always 8 in practice)
** @param    boardOffset - offset centering a ring frame smaller than 8 within the 8x8 word; always 0 in practice
** @return   Vector of N*N indices: perm[k] = row-major index for ring-position k.
*/
std::vector<int> BuildRingPermutation(int N, int boardOffset)
{
    auto              order = GenerateRingOrder(N);
    std::vector<int>  perm(order.size());

    for (size_t k = 0; k < order.size(); k++)
    {
        int absRow = order[k].first + boardOffset;
        int absCol = order[k].second + boardOffset;
        perm[k] = absRow * 8 + absCol;   /* index-from-MSB, matches OthelloBasics.h's GETINDEX convention */
    }
    return perm;
}

/*
** Function: BuildInverseRingPermutation
** @brief    Reverses BuildRingPermutation: maps a row-major bit index-from-MSB
**           to its ring-order position.
** @param    N           - board dimension (always 8 in practice)
** @param    boardOffset - offset centering a ring frame smaller than 8 within the 8x8 word; always 0 in practice
** @return   Vector of N*N indices: invPerm[rowMajorIdx] = ring-position k.
*/
std::vector<int> BuildInverseRingPermutation(int N, int boardOffset)
{
    std::vector<int>  forward = BuildRingPermutation(N, boardOffset);
    std::vector<int>  inverse(forward.size());

    for (size_t k = 0; k < forward.size(); k++)
        inverse[forward[k]] = (int)k;

    return inverse;
}

/*
** Function: SelfCheckRingOrder
** @brief    Runs GenerateRingOrder(8) against a hand-derived cell order and
**           reports any mismatch.
** @return   true if every position matched the hand-derived order.
*/
bool SelfCheckRingOrder()
{
    auto order = GenerateRingOrder(8);
    bool ok    = true;

    for (int i = 0; i < 64; i++)
    {
        int   row = order[i].first, col = order[i].second;
        char  buf[4];

        snprintf(buf, sizeof(buf), "%c%d", 'A' + row, col + 1);

        /* A mismatch here means the ring-walk geometry itself is wrong --
        ** fail loudly rather than silently trusting an unverified table.
        */
        if (strcmp(buf, kLiteralN8Order[i]) != 0)
        {
            fprintf(stderr, "SelfCheckRingOrder MISMATCH at index %d: generated '%s', expected '%s'\n",
                    i, buf, kLiteralN8Order[i]);
            ok = false;
        }
    }
    return ok;
}
