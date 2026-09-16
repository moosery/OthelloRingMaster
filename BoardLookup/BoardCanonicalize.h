/*
** Filename:  BoardCanonicalize.h
**
** Purpose:
**   CPU-only port of the GPU's dev_canonicalize/dev_RowMajorToRing family
**   (OthelloBasicsForCUDA.h / RingConversion.h), scoped for lookup-only use
**   cases -- a human-typed or otherwise externally-sourced board, not the
**   live solver's own bulk pipeline. Deliberately a NEW, standalone module,
**   not an addition to OthelloBasics.h (whose own documented scope rule is
**   "nothing... ever interprets a board key's bits by cell position -- no
**   row-major indexing, no move generation, no canonicalization" -- see
**   project_othellobasics_scope_rule memory) or to any CUDA-only file --
**   this keeps the live solver/calculator's real GPU pipeline completely
**   untouched while still giving any CPU-only tool (this session's own
**   interactive board-lookup CLI, a future web-UI lookup backend, etc.)
**   the one real piece it was missing.
**
**   Every function here is a direct, bit-for-bit port of its GPU
**   counterpart, in the SAME order of operations the real production
**   kernel uses (confirmed via RetrogradeKernels.cu's own real usage):
**   canonicalize (rotate/mirror/flip/compare) happens entirely in ROW-
**   MAJOR bit space, and ring conversion happens AFTERWARD, only on the
**   winning transform. Comparing in the wrong bit space would silently
**   pick a different "canonical" board than the one actually in the
**   store -- see BoardCanonicalize.cpp's own self-check against a real,
**   hand-verified production constant for how this is validated on every
**   process that links this library, not just once by hand.
**
**   Move generation/flip computation is deliberately NOT ported here --
**   canonicalize only needs it (on the GPU) to populate a transient
**   ullPossibleMoves field that isn't part of the stored BOARD_KEY and
**   isn't needed to find a board's location in the store.
**
** Notes:
**   Ring geometry is always the full 8x8 depth regardless of actual board
**   size, same as RingPermutation.h/RingConversion.h -- a smaller board's
**   active cells are already centered within the 8x8 word by the
**   production encoding (see project_ring_split_validated_findings memory).
*/

#pragma once

/* Includes */
#include <cstdint>

/* Structures and Types */

/*
** Type:    CanonicalizeResult
** @brief   The winning transform's ring-ordered occupancy/color pattern
**          (directly comparable to a real BOARD_KEY / CellsInUseRec.pattern),
**          plus which color is to move in that canonical orientation --
**          this determines which player's store files to search, since a
**          canonical position's "whose turn" isn't fixed by the physical
**          stone arrangement alone.
*/
struct CanonicalizeResult
{
    uint64_t ringCellsInUse = 0;   /* ring-ordered occupancy -- matches CellsInUseRec.pattern / BOARD_KEY::ullCellsInUse */
    uint64_t ringCellColors = 0;   /* ring-ordered color pattern -- caller splits into Ring_1/Ring_2/Ring_3_4 sub-fields (see RingNestedIndex.h's RING*_BITS/RING*_SHIFT) */
    bool     blackToMove    = false;
};

/* Functions */

/*
** Function: RowMajorToRing
** @brief    Converts a row-major-ordered 64-bit board value (occupancy or
**           color) to ring order, via the same permutation table
**           RingPermutation.h/RingConversion.h build for the GPU -- built
**           once, cached, and reused across calls.
** @param    rowMajorValue - the row-major-ordered value
** @return   The same 64 bits, reordered into ring order.
*/
uint64_t RowMajorToRing(uint64_t rowMajorValue);

/*
** Function: RingToRowMajor
** @brief    Inverse of RowMajorToRing.
** @param    ringValue - the ring-ordered value
** @return   The same 64 bits, reordered into row-major order.
*/
uint64_t RingToRowMajor(uint64_t ringValue);

/*
** Function: CanonicalizeBoard
** @brief    Ports dev_canonicalize to the CPU: generates up to numRotations
**           symmetric variants of a RAW ROW-MAJOR board (4 rotations, their
**           vertical mirrors, and -- for the full 16 -- each of those with
**           colors swapped and the to-move player flipped), keeps the
**           minimum under the same ordering dev_boardLT uses (cellsInUse,
**           then cellColors, then to-move player as a tiebreaker), and
**           returns that winner already converted to ring order.
** @param    rowMajorCellsInUse - raw board occupancy, row-major bit order
**                                (bit (63 - (row*8+col)) set if occupied,
**                                row/col 0-indexed within the 8x8 word --
**                                a smaller board's cells are centered, see
**                                this file's own Notes)
** @param    rowMajorCellColors - raw board colors, row-major bit order
**                                (1 = black, 0 = white, only meaningful
**                                where cellsInUse is set)
** @param    blackToMove        - whose turn it is at the board as typed in
** @param    numRotations       - 1, 4, 8, or 16 (16 is the real, full
**                                symmetry group this store's canonical
**                                form is built on; smaller values are for
**                                testing/diagnostics only)
** @return   The canonical ring-ordered pattern and resulting to-move color.
*/
CanonicalizeResult CanonicalizeBoard(uint64_t rowMajorCellsInUse, uint64_t rowMajorCellColors,
                                      bool blackToMove, int numRotations = 16);
