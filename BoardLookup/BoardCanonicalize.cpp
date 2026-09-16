/*
** Filename:  BoardCanonicalize.cpp
**
** Purpose:
**   Implements BoardCanonicalize.h -- see that file's own Purpose for why
**   this exists as a standalone CPU port instead of touching OthelloBasics.h/
**   OthelloBasicsForCUDA.h/RingConversion.h.
**
** Notes:
**   VALIDATION: the ring-conversion half of this file is checked once per
**   process, at first use, against a real, hand-verified production
**   constant -- the Othello starting position's row-major layout (center
**   2x2: (3,3)/(4,4) White, (3,4)/(4,3) Black) must convert to ring-ordered
**   CellsInUse=0x0F, CellColors=0x05 (OthelloBasics/BoardKeyAllocate.cpp's
**   kStartingCellsInUseRingOrdered/kStartingCellColorsRingOrdered -- the
**   one real value already hand-verified against the GPU's own ring
**   permutation, so this is checked against production truth, not just
**   re-derived independently). Fatals immediately on any mismatch --
**   every other result this library could produce would be equally wrong,
**   and a silent wrong-canonical-form bug here would look like "board not
**   found" for every real lookup, not a loud, obvious failure.
*/

/* Includes */
#include "BoardCanonicalize.h"
#include "RingPermutation.h"
#include "Error.h"
#include <vector>
#include <cstring>

/* Constants */

static constexpr uint64_t kStartingCellsInUseRingOrdered = 0x000000000000000FULL;
static constexpr uint64_t kStartingCellColorsRingOrdered = 0x0000000000000005ULL;

/* Functions */

/*
** Function: StartingRowMajorCellsInUse
** @brief    The Othello starting position's occupancy, row-major bit order
**           -- (3,3)/(3,4)/(4,3)/(4,4) occupied, 0-indexed row/col within
**           the 8x8 word, bit (63-(row*8+col)) per cell. Same layout
**           OthelloBasics/BoardKeyAllocate.cpp's own comment describes.
*/
static uint64_t StartingRowMajorCellsInUse()
{
    uint64_t v = 0;
    v |= (1ULL << (63 - (3 * 8 + 3)));
    v |= (1ULL << (63 - (3 * 8 + 4)));
    v |= (1ULL << (63 - (4 * 8 + 3)));
    v |= (1ULL << (63 - (4 * 8 + 4)));
    return v;
}

/*
** Function: StartingRowMajorCellColors
** @brief    The Othello starting position's colors, row-major bit order --
**           Black at (3,4) and (4,3), White at (3,3) and (4,4).
*/
static uint64_t StartingRowMajorCellColors()
{
    uint64_t v = 0;
    v |= (1ULL << (63 - (3 * 8 + 4)));
    v |= (1ULL << (63 - (4 * 8 + 3)));
    return v;
}

/*
** Function: GetRingPermTables
** @brief    Builds (once, cached) the forward/inverse ring permutation
**           tables via RingPermutation.h's existing CPU builders -- the
**           same tables OBCuda_InitRingPermutationTables uploads to the
**           GPU, just consumed directly here instead of via GPU constant
**           memory.
*/
static void GetRingPermTables(const std::vector<int>** ppForward, const std::vector<int>** ppInverse)
{
    static std::vector<int> forward = BuildRingPermutation(8, 0);
    static std::vector<int> inverse = BuildInverseRingPermutation(8, 0);
    *ppForward = &forward;
    *ppInverse = &inverse;
}

/*
** Function: GatherByPermutation
** @brief    CPU port of dev_GatherByRingPermutation (RingConversion.h) --
**           output bit k comes from input bit perm[k].
*/
static uint64_t GatherByPermutation(uint64_t value, const std::vector<int>& perm)
{
    uint64_t result = 0;
    for (int k = 0; k < 64; k++)
    {
        uint64_t bit = (value >> (63 - perm[k])) & 1ULL;
        result |= (bit << (63 - k));
    }
    return result;
}

uint64_t RowMajorToRing(uint64_t rowMajorValue)
{
    const std::vector<int>*pForward, *pInverse;
    GetRingPermTables(&pForward, &pInverse);
    return GatherByPermutation(rowMajorValue, *pForward);
}

uint64_t RingToRowMajor(uint64_t ringValue)
{
    const std::vector<int>*pForward, *pInverse;
    GetRingPermTables(&pForward, &pInverse);
    return GatherByPermutation(ringValue, *pInverse);
}

/*
** Function: SelfCheckStartingPosition
** @brief    Verifies RowMajorToRing reproduces the real, hand-verified
**           production constant for the Othello starting position --
**           Fatals immediately if not (see this file's own top-of-file
**           Notes for why that's the right failure mode here).
*/
static void SelfCheckStartingPosition()
{
    uint64_t gotCellsInUse = RowMajorToRing(StartingRowMajorCellsInUse());
    uint64_t gotCellColors = RowMajorToRing(StartingRowMajorCellColors());
    if (gotCellsInUse != kStartingCellsInUseRingOrdered || gotCellColors != kStartingCellColorsRingOrdered)
        Fatal(FATAL_MERGE_LOGIC_ERROR,
              "BoardCanonicalize: ring-conversion self-check FAILED -- got cellsInUse=0x%016llx cellColors=0x%016llx, "
              "expected 0x%016llx / 0x%016llx (OthelloBasics/BoardKeyAllocate.cpp's hand-verified production "
              "constants). Refusing to trust any further canonicalization/lookup result.",
              (unsigned long long)gotCellsInUse, (unsigned long long)gotCellColors,
              (unsigned long long)kStartingCellsInUseRingOrdered, (unsigned long long)kStartingCellColorsRingOrdered);
}

namespace
{
    struct SelfCheckRunner { SelfCheckRunner() { SelfCheckStartingPosition(); } };
    static SelfCheckRunner g_selfCheckRunner;
}

/* Bit-manipulation primitives -- direct ports of OthelloBasicsForCUDA.h's
** dev_bswap64/dev_flipDiagA1H8/dev_mirrorBytewise. Pure integer bit
** arithmetic in the GPU originals too -- __device__ there was a matter of
** file location (row-major bit interpretation is GPU-exclusive by policy),
** not any real CUDA dependency.
*/
static uint64_t Bswap64(uint64_t x)
{
    return ((x & 0x00000000000000FFULL) << 56) |
           ((x & 0x000000000000FF00ULL) << 40) |
           ((x & 0x0000000000FF0000ULL) << 24) |
           ((x & 0x00000000FF000000ULL) <<  8) |
           ((x & 0x000000FF00000000ULL) >>  8) |
           ((x & 0x0000FF0000000000ULL) >> 24) |
           ((x & 0x00FF000000000000ULL) >> 40) |
           ((x & 0xFF00000000000000ULL) >> 56);
}

static uint64_t FlipDiagA1H8(uint64_t x)
{
    uint64_t t;
    const uint64_t k1 = 0x5500550055005500ULL;
    const uint64_t k2 = 0x3333000033330000ULL;
    const uint64_t k4 = 0x0F0F0F0F00000000ULL;
    t = k4 & (x ^ (x << 28)); x ^= t ^ (t >> 28);
    t = k2 & (x ^ (x << 14)); x ^= t ^ (t >> 14);
    t = k1 & (x ^ (x <<  7)); x ^= t ^ (t >>  7);
    return x;
}

static uint64_t MirrorBytewise(uint64_t x)
{
    x = ((x & 0xF0F0F0F0F0F0F0F0ULL) >> 4) | ((x & 0x0F0F0F0F0F0F0F0FULL) << 4);
    x = ((x & 0xCCCCCCCCCCCCCCCCULL) >> 2) | ((x & 0x3333333333333333ULL) << 2);
    x = ((x & 0xAAAAAAAAAAAAAAAAULL) >> 1) | ((x & 0x5555555555555555ULL) << 1);
    return x;
}

/* A minimal board-symmetry struct -- just what canonicalize actually
** needs (occupancy, color, to-move player). No ullPossibleMoves -- see
** BoardCanonicalize.h's own Purpose on why that's deliberately not ported.
*/
struct SymBoard
{
    uint64_t cellsInUse  = 0;
    uint64_t cellColors  = 0;
    bool     blackToMove = false;
};

static void Rotate90Right(const SymBoard& src, SymBoard* dst)
{
    dst->blackToMove = src.blackToMove;
    dst->cellsInUse   = FlipDiagA1H8(Bswap64(src.cellsInUse));
    dst->cellColors   = FlipDiagA1H8(Bswap64(src.cellColors));
}

static void MirrorVerticalAxis(const SymBoard& src, SymBoard* dst)
{
    dst->blackToMove = src.blackToMove;
    dst->cellsInUse   = MirrorBytewise(src.cellsInUse);
    dst->cellColors   = MirrorBytewise(src.cellColors);
}

static void BoardFlip(const SymBoard& src, SymBoard* dst)
{
    dst->blackToMove = !src.blackToMove;
    dst->cellsInUse   = src.cellsInUse;
    dst->cellColors   = ~src.cellColors & src.cellsInUse;
}

/*
** Function: BoardLT
** @brief    Direct port of dev_boardLT: cellsInUse, then cellColors, then
**           to-move player as a tiebreaker (Black sorts first, matching
**           "bit=1 -> Black='B'=66 < White='W'=87" in the GPU original).
*/
static bool BoardLT(const SymBoard& a, const SymBoard& b)
{
    if (a.cellsInUse != b.cellsInUse) return a.cellsInUse < b.cellsInUse;
    if (a.cellColors != b.cellColors) return a.cellColors < b.cellColors;
    return a.blackToMove && !b.blackToMove;
}

CanonicalizeResult CanonicalizeBoard(uint64_t rowMajorCellsInUse, uint64_t rowMajorCellColors,
                                      bool blackToMove, int numRotations)
{
    SymBoard arr[16];
    arr[0].cellsInUse  = rowMajorCellsInUse;
    arr[0].cellColors  = rowMajorCellColors;
    arr[0].blackToMove = blackToMove;

    if (numRotations >= 4)
    {
        Rotate90Right(arr[0], &arr[1]);
        Rotate90Right(arr[1], &arr[2]);
        Rotate90Right(arr[2], &arr[3]);
    }
    if (numRotations >= 8)
    {
        MirrorVerticalAxis(arr[0], &arr[4]);
        Rotate90Right(arr[4], &arr[5]);
        Rotate90Right(arr[5], &arr[6]);
        Rotate90Right(arr[6], &arr[7]);
    }
    if (numRotations >= 16)
    {
        BoardFlip(arr[0], &arr[8]);
        Rotate90Right(arr[8],  &arr[9]);
        Rotate90Right(arr[9],  &arr[10]);
        Rotate90Right(arr[10], &arr[11]);
        MirrorVerticalAxis(arr[8], &arr[12]);
        Rotate90Right(arr[12], &arr[13]);
        Rotate90Right(arr[13], &arr[14]);
        Rotate90Right(arr[14], &arr[15]);
    }

    int n = (numRotations >= 16) ? 16 : (numRotations >= 8) ? 8 : (numRotations >= 4) ? 4 : 1;
    int minIdx = 0;
    for (int i = 1; i < n; i++)
        if (BoardLT(arr[i], arr[minIdx])) minIdx = i;

    CanonicalizeResult result;
    result.ringCellsInUse = RowMajorToRing(arr[minIdx].cellsInUse);
    result.ringCellColors = RowMajorToRing(arr[minIdx].cellColors);
    result.blackToMove    = arr[minIdx].blackToMove;
    return result;
}
