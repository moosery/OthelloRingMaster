/*
** Filename:  OthelloBasicsForCUDA.h
**
** Purpose:
**   Declares everything row-major-bit-dependent for Othello board
**   manipulation: the full working BOARD struct (occupancy/color/next-player/
**   possible-moves/win-tie-loss/state), every bit macro that addresses a
**   cell by row-major index, board-size mask setup, and the device
**   functions that actually generate moves, compute flips, rotate/mirror,
**   and canonicalize a board. All of this used to live in OthelloBasics.h;
**   it moved here because the CPU never touches row-major bit structure --
**   only the GPU does, per the CPU-organizes/GPU-solves boundary (see
**   project_ring_layout_implementation_plan memory). BOARD_KEY (the lean,
**   CPU-visible on-disk/organizing key) still lives in OthelloBasics.h.
**
** Notes:
**   A previous "_key" family of device functions here (dev_applyMove_key,
**   dev_canonicalize_key, etc.) operated directly on BOARD_KEY and read a
**   next-player bit from it. That bit no longer exists on BOARD_KEY --
**   next-player is tracked externally now (which file/batch a key belongs
**   to), matching the on-disk record format's own convention. That family
**   served a use case not part of this solution, so it was dropped rather
**   than reworked.
*/

#pragma once

/* Includes */
#include <OthelloBasics.h>

/* Structures and Types */

/*
** Type:    BOARD
** @brief   Full GPU working-set board: occupancy/color bitboards, the
**          next-player bit, cached possible-moves, win/tie/loss counters,
**          and play state. This is transient, GPU-internal representation
**          used during expansion/generation -- once a board is finalized
**          and filed, only its BOARD_KEY (the two bitboards) is stored.
*/
typedef struct _Board
{
    unsigned long long  ullCellsInUse;      /* 0-> Not used      1-> Used             */
    unsigned long long  ullCellColors;      /* 0-> White         1-> Black            */
    unsigned short      usBoardInfo;        /* 0b0000000X        Next Player 1->Black */
                                            /*                               0->White */
    unsigned short      _pad1[3];           /* explicit alignment padding             */
    unsigned long long  ullPossibleMoves;   /* 0-> No move       1-> Can play          */
    unsigned long long  ullBlackWins;       /* Number of potential black wins          */
    unsigned long long  ullWhiteWins;       /* Number of potential white wins          */
    unsigned long long  ullTies;            /* Number of tie boards                    */
    unsigned short      usBoardState;       /* 0=not played, 1=played/non-terminal,    */
                                            /* 2=played/terminal, 3=played/no moves    */
    unsigned short      _pad2[3];           /* explicit trailing padding               */
} BOARD, * PBOARD;

/* Board-size constants that device functions need in lieu of the host globals
** (g_boardMask, g_boardRightEdge, g_boardLeftEdge).
** Populate with OBCuda_GetBoardConsts() after calling SetBoardSizeForRun().
*/
struct DevBoardConsts {
    unsigned long long boardMask;
    unsigned long long boardRightEdge;
    unsigned long long boardLeftEdge;
};

/* Macros and Defines */

/* The bit we move all around */
#define FIRSTBIT                           ((unsigned long long) 0x8000000000000000)

/* BIT Index Macro */
#define GETINDEX(row,col)                  ((row * 8) + col)

/* Next Player Macros */
#define GETBOARDNEXTPLAYERSHORT(val)       (((val) & 0x01) ? BLACK : WHITE)
#define GETBOARDNEXTPLAYER(pBoard)         GETBOARDNEXTPLAYERSHORT((pBoard)->usBoardInfo)
#define SETBOARDNEXTPLAYERBLACKSHORT(val)  (val) = ((val) | 0x01)
#define SETBOARDNEXTPLAYERWHITESHORT(val)  (val) = ((val) & 0xFE)
#define SETBOARDNEXTPLAYERBLACK(pBoard)    SETBOARDNEXTPLAYERBLACKSHORT((pBoard)->usBoardInfo)
#define SETBOARDNEXTPLAYERWHITE(pBoard)    SETBOARDNEXTPLAYERWHITESHORT((pBoard)->usBoardInfo)
#define SETBOARDNEXTPLAYER(pBoard,color)   if(color == BLACK) { SETBOARDNEXTPLAYERBLACK(pBoard); } else { SETBOARDNEXTPLAYERWHITE(pBoard); }
#define SETBOARDNEXTPLAYERFLIP(pBoard)     if(GETBOARDNEXTPLAYER(pBoard) == WHITE) { SETBOARDNEXTPLAYERBLACK(pBoard); } else { SETBOARDNEXTPLAYERWHITE(pBoard); }

/* Globals */

/* Board-size globals -- call SetBoardSizeForRun(boardSize) once before any
** GPU run. Masks are precomputed here so device code avoids rebuilding them
** on every call (which can be billions of times for a full solve). Defaults
** match boardSize=4 so a SetBoardSizeForRun call is not strictly required
** when the board size is 4.
*/
inline int                g_boardSize      = 4;
inline int                g_boardSi        = 2;                    /* (8-4)/2 */
inline int                g_boardEi        = 6;                    /* 8-2     */
inline unsigned long long g_boardLeftEdge  = 0x0000202020200000ULL;
inline unsigned long long g_boardRightEdge = 0x0000040404040000ULL;
inline unsigned long long g_boardMask      = 0x00003C3C3C3C0000ULL;

/* Functions */

/*
** Function: SetBoardSizeForRun
** @brief    Rebuilds the board-size masks (g_boardMask/g_boardLeftEdge/
**           g_boardRightEdge) for a new board size.
** @param    boardSize - the board size to configure for (4, 6, or 8)
*/
inline void SetBoardSizeForRun(int boardSize)
{
    g_boardSize      = boardSize;
    g_boardSi        = (8 - boardSize) / 2;
    g_boardEi        = 8 - g_boardSi;
    g_boardLeftEdge  = 0;
    g_boardRightEdge = 0;
    g_boardMask      = 0;
    for (int r = g_boardSi; r < g_boardEi; r++)
    {
        g_boardLeftEdge  |= (FIRSTBIT >> GETINDEX(r, g_boardSi));
        g_boardRightEdge |= (FIRSTBIT >> GETINDEX(r, g_boardEi - 1));
        for (int c = g_boardSi; c < g_boardEi; c++)
            g_boardMask |= (FIRSTBIT >> GETINDEX(r, c));
    }
}

/*
** Function: OBCuda_GetBoardConsts
** @brief    Captures the current g_board* globals into a DevBoardConsts for
**           device code to use.
** @return   The current board-size constants.
*/
DevBoardConsts OBCuda_GetBoardConsts();

/* ─────────────────────────────────────────────────────────────────────────────
** Device functions -- available only in CUDA compilation units (.cu files).
** ─────────────────────────────────────────────────────────────────────────────
*/
#ifdef __CUDACC__

// Manual byte-swap (replaces _byteswap_uint64 which is an MSVC-only intrinsic).
__device__ __forceinline__
unsigned long long dev_bswap64(unsigned long long x)
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

// Transpose an 8x8 bit matrix stored in a uint64 along the A1-H8 diagonal.
__device__ __forceinline__
unsigned long long dev_flipDiagA1H8(unsigned long long x)
{
    unsigned long long t;
    const unsigned long long k1 = 0x5500550055005500ULL;
    const unsigned long long k2 = 0x3333000033330000ULL;
    const unsigned long long k4 = 0x0F0F0F0F00000000ULL;
    t  = k4 & (x ^ (x << 28)); x ^= t ^ (t >> 28);
    t  = k2 & (x ^ (x << 14)); x ^= t ^ (t >> 14);
    t  = k1 & (x ^ (x <<  7)); x ^= t ^ (t >>  7);
    return x;
}

// 90-degree clockwise rotation: byteswap then diagonal transpose.
__device__ __forceinline__
void dev_rotate90Right(const BOARD* src, BOARD* dst)
{
    dst->usBoardInfo      = src->usBoardInfo;
    dst->ullPossibleMoves = 0;
    dst->ullCellsInUse    = dev_flipDiagA1H8(dev_bswap64(src->ullCellsInUse));
    dst->ullCellColors    = dev_flipDiagA1H8(dev_bswap64(src->ullCellColors));
}

// Reverse bits within each byte independently (mirrors column order per row).
__device__ __forceinline__
unsigned long long dev_mirrorBytewise(unsigned long long x)
{
    x = ((x & 0xF0F0F0F0F0F0F0F0ULL) >> 4) | ((x & 0x0F0F0F0F0F0F0F0FULL) << 4);
    x = ((x & 0xCCCCCCCCCCCCCCCCULL) >> 2) | ((x & 0x3333333333333333ULL) << 2);
    x = ((x & 0xAAAAAAAAAAAAAAAAULL) >> 1) | ((x & 0x5555555555555555ULL) << 1);
    return x;
}

// Mirror the board around the vertical axis.
__device__ __forceinline__
void dev_mirrorVerticalAxis(const BOARD* src, BOARD* dst)
{
    dst->usBoardInfo      = src->usBoardInfo;
    dst->ullCellsInUse    = dev_mirrorBytewise(src->ullCellsInUse);
    dst->ullCellColors    = dev_mirrorBytewise(src->ullCellColors);
    dst->ullPossibleMoves = 0;
}

// Returns true if board a sorts before board b (mirrors BoardCompare < 0).
// Comparison order: ullCellsInUse, ullCellColors, next-player (Black='B'=66 < White='W'=87).
__device__ __forceinline__
bool dev_boardLT(const BOARD* a, const BOARD* b)
{
    if (a->ullCellsInUse != b->ullCellsInUse)
        return a->ullCellsInUse < b->ullCellsInUse;
    if (a->ullCellColors != b->ullCellColors)
        return a->ullCellColors < b->ullCellColors;
    // bit=1 → Black='B'=66; bit=0 → White='W'=87.  Black sorts first → higher bit = less.
    return (a->usBoardInfo & 0x01u) > (b->usBoardInfo & 0x01u);
}

// Compute all pieces flipped by placing moveBit for player against opponent.
// Uses full-grid 8x8 column masks — correct for all board sizes.
__device__ __forceinline__
unsigned long long dev_computeFlips(unsigned long long moveBit,
                                    unsigned long long player,
                                    unsigned long long opponent)
{
    const unsigned long long NLC = ~0x8080808080808080ULL; // not left  col
    const unsigned long long NRC = ~0x0101010101010101ULL; // not right col
    unsigned long long flips = 0, x;

    x  = (moveBit << 8) & opponent;
    x |= (x      << 8) & opponent; x |= (x << 8) & opponent;
    x |= (x      << 8) & opponent; x |= (x << 8) & opponent;
    x |= (x      << 8) & opponent;
    if ((x << 8) & player) flips |= x;

    x  = (moveBit >> 8) & opponent;
    x |= (x      >> 8) & opponent; x |= (x >> 8) & opponent;
    x |= (x      >> 8) & opponent; x |= (x >> 8) & opponent;
    x |= (x      >> 8) & opponent;
    if ((x >> 8) & player) flips |= x;

    x  = ((moveBit & NRC) >> 1) & opponent;
    x |= ((x & NRC) >> 1) & opponent; x |= ((x & NRC) >> 1) & opponent;
    x |= ((x & NRC) >> 1) & opponent; x |= ((x & NRC) >> 1) & opponent;
    x |= ((x & NRC) >> 1) & opponent;
    if  ((x & NRC) >> 1  & player) flips |= x;

    x  = ((moveBit & NLC) << 1) & opponent;
    x |= ((x & NLC) << 1) & opponent; x |= ((x & NLC) << 1) & opponent;
    x |= ((x & NLC) << 1) & opponent; x |= ((x & NLC) << 1) & opponent;
    x |= ((x & NLC) << 1) & opponent;
    if  ((x & NLC) << 1  & player) flips |= x;

    x  = ((moveBit & NRC) >> 9) & opponent;
    x |= ((x & NRC) >> 9) & opponent; x |= ((x & NRC) >> 9) & opponent;
    x |= ((x & NRC) >> 9) & opponent; x |= ((x & NRC) >> 9) & opponent;
    x |= ((x & NRC) >> 9) & opponent;
    if  ((x & NRC) >> 9  & player) flips |= x;

    x  = ((moveBit & NLC) >> 7) & opponent;
    x |= ((x & NLC) >> 7) & opponent; x |= ((x & NLC) >> 7) & opponent;
    x |= ((x & NLC) >> 7) & opponent; x |= ((x & NLC) >> 7) & opponent;
    x |= ((x & NLC) >> 7) & opponent;
    if  ((x & NLC) >> 7  & player) flips |= x;

    x  = ((moveBit & NRC) << 7) & opponent;
    x |= ((x & NRC) << 7) & opponent; x |= ((x & NRC) << 7) & opponent;
    x |= ((x & NRC) << 7) & opponent; x |= ((x & NRC) << 7) & opponent;
    x |= ((x & NRC) << 7) & opponent;
    if  ((x & NRC) << 7  & player) flips |= x;

    x  = ((moveBit & NLC) << 9) & opponent;
    x |= ((x & NLC) << 9) & opponent; x |= ((x & NLC) << 9) & opponent;
    x |= ((x & NLC) << 9) & opponent; x |= ((x & NLC) << 9) & opponent;
    x |= ((x & NLC) << 9) & opponent;
    if  ((x & NLC) << 9  & player) flips |= x;

    return flips;
}

// Apply a move at moveIdx (0=top-left, 63=bottom-right) for color onto board.
__device__ __forceinline__
void dev_applyMove(BOARD* board, char color, int moveIdx)
{
    unsigned long long moveBit  = FIRSTBIT >> moveIdx;
    unsigned long long occupied = board->ullCellsInUse;
    unsigned long long colors   = board->ullCellColors;

    unsigned long long player, opponent;
    if (color == BLACK) {
        player   = occupied &  colors;
        opponent = occupied & ~colors;
    } else {
        player   = occupied & ~colors;
        opponent = occupied &  colors;
    }

    unsigned long long flips = dev_computeFlips(moveBit, player, opponent);

    board->ullCellsInUse |= moveBit;
    if (color == BLACK)
        board->ullCellColors |= (moveBit | flips);
    else
        board->ullCellColors &= ~(moveBit | flips);
}

// Produce the result board after playing moveIdx on src; result written to dst.
// Mirrors MovePlayAndSetResultBoard.
__device__ __forceinline__
void dev_playMove(const BOARD* src, BOARD* dst, int moveIdx)
{
    dst->ullCellsInUse    = src->ullCellsInUse;
    dst->ullCellColors    = src->ullCellColors;
    dst->usBoardInfo      = src->usBoardInfo;
    dst->ullPossibleMoves = 0;
    dst->ullBlackWins     = 0;
    dst->ullWhiteWins     = 0;
    dst->ullTies          = 0;
    dst->usBoardState     = 0;

    char color = GETBOARDNEXTPLAYER(src);
    SETBOARDNEXTPLAYERFLIP(dst);
    dev_applyMove(dst, color, moveIdx);
}

// Compute ullPossibleMoves for the current player using 8-direction bitboard fill.
__device__ __forceinline__
void dev_boardMoveCalculator(BOARD* board, const DevBoardConsts& c)
{
    char color = GETBOARDNEXTPLAYER(board);

    unsigned long long myPieces, oppPieces;
    if (color == BLACK) {
        myPieces  = board->ullCellsInUse &  board->ullCellColors;
        oppPieces = board->ullCellsInUse & ~board->ullCellColors;
    } else {
        myPieces  = board->ullCellsInUse & ~board->ullCellColors;
        oppPieces = board->ullCellsInUse &  board->ullCellColors;
    }

    const unsigned long long notRight = ~c.boardRightEdge;
    const unsigned long long notLeft  = ~c.boardLeftEdge;
    unsigned long long empty      = c.boardMask & ~(myPieces | oppPieces);
    unsigned long long validMoves = 0;
    unsigned long long gen, candidates;

    candidates = oppPieces & notRight;
    gen  = (myPieces & notRight) >> 1; gen &= candidates;
    gen |= ((gen & notRight) >> 1) & candidates; gen |= ((gen & notRight) >> 1) & candidates;
    gen |= ((gen & notRight) >> 1) & candidates; gen |= ((gen & notRight) >> 1) & candidates;
    gen |= ((gen & notRight) >> 1) & candidates;
    validMoves |= ((gen & notRight) >> 1) & empty;

    candidates = oppPieces & notLeft;
    gen  = (myPieces & notLeft) << 1; gen &= candidates;
    gen |= ((gen & notLeft) << 1) & candidates; gen |= ((gen & notLeft) << 1) & candidates;
    gen |= ((gen & notLeft) << 1) & candidates; gen |= ((gen & notLeft) << 1) & candidates;
    gen |= ((gen & notLeft) << 1) & candidates;
    validMoves |= ((gen & notLeft) << 1) & empty;

    candidates = oppPieces;
    gen  = (myPieces >> 8) & candidates;
    gen |= ((gen >> 8) & candidates); gen |= ((gen >> 8) & candidates);
    gen |= ((gen >> 8) & candidates); gen |= ((gen >> 8) & candidates);
    gen |= ((gen >> 8) & candidates);
    validMoves |= (gen >> 8) & empty;

    candidates = oppPieces;
    gen  = (myPieces << 8) & candidates;
    gen |= ((gen << 8) & candidates); gen |= ((gen << 8) & candidates);
    gen |= ((gen << 8) & candidates); gen |= ((gen << 8) & candidates);
    gen |= ((gen << 8) & candidates);
    validMoves |= (gen << 8) & empty;

    candidates = oppPieces & notRight;
    gen  = (myPieces & notRight) >> 9; gen &= candidates;
    gen |= ((gen & notRight) >> 9) & candidates; gen |= ((gen & notRight) >> 9) & candidates;
    gen |= ((gen & notRight) >> 9) & candidates; gen |= ((gen & notRight) >> 9) & candidates;
    gen |= ((gen & notRight) >> 9) & candidates;
    validMoves |= ((gen & notRight) >> 9) & empty;

    candidates = oppPieces & notLeft;
    gen  = (myPieces & notLeft) >> 7; gen &= candidates;
    gen |= ((gen & notLeft) >> 7) & candidates; gen |= ((gen & notLeft) >> 7) & candidates;
    gen |= ((gen & notLeft) >> 7) & candidates; gen |= ((gen & notLeft) >> 7) & candidates;
    gen |= ((gen & notLeft) >> 7) & candidates;
    validMoves |= ((gen & notLeft) >> 7) & empty;

    candidates = oppPieces & notRight;
    gen  = (myPieces & notRight) << 7; gen &= candidates;
    gen |= ((gen & notRight) << 7) & candidates; gen |= ((gen & notRight) << 7) & candidates;
    gen |= ((gen & notRight) << 7) & candidates; gen |= ((gen & notRight) << 7) & candidates;
    gen |= ((gen & notRight) << 7) & candidates;
    validMoves |= ((gen & notRight) << 7) & empty;

    candidates = oppPieces & notLeft;
    gen  = (myPieces & notLeft) << 9; gen &= candidates;
    gen |= ((gen & notLeft) << 9) & candidates; gen |= ((gen & notLeft) << 9) & candidates;
    gen |= ((gen & notLeft) << 9) & candidates; gen |= ((gen & notLeft) << 9) & candidates;
    gen |= ((gen & notLeft) << 9) & candidates;
    validMoves |= ((gen & notLeft) << 9) & empty;

    board->ullPossibleMoves = validMoves;
}

// Swap Black↔White by complementing ullCellColors within occupied cells and
// flipping the next-player bit.  Used to generate the color-mirror symmetry
// for 16-rotation canonicalization.
__device__ __forceinline__
void dev_boardFlip(const BOARD* src, BOARD* dst)
{
    dst->usBoardInfo      = src->usBoardInfo ^ 0x01u;           // flip current player
    dst->ullCellsInUse    = src->ullCellsInUse;
    dst->ullCellColors    = ~src->ullCellColors & src->ullCellsInUse; // swap Black↔White
    dst->ullPossibleMoves = 0;
}

// Canonicalize board in-place: generate up to numRotations symmetries, keep the
// minimum under BoardCompare ordering, then compute ullPossibleMoves on the winner.
// numRotations: 1, 4, 8, or 16.  16 includes the color-swap (BoardFlip) symmetry.
__device__ __forceinline__
void dev_canonicalize(BOARD* board, int numRotations, const DevBoardConsts& c)
{
    BOARD arr[16];

    arr[0].ullCellsInUse    = board->ullCellsInUse;
    arr[0].ullCellColors    = board->ullCellColors;
    arr[0].usBoardInfo      = board->usBoardInfo;
    arr[0].ullPossibleMoves = 0;

    if (numRotations >= 4) {
        dev_rotate90Right(&arr[0], &arr[1]);
        dev_rotate90Right(&arr[1], &arr[2]);
        dev_rotate90Right(&arr[2], &arr[3]);
    }
    if (numRotations >= 8) {
        dev_mirrorVerticalAxis(&arr[0], &arr[4]);
        dev_rotate90Right(&arr[4], &arr[5]);
        dev_rotate90Right(&arr[5], &arr[6]);
        dev_rotate90Right(&arr[6], &arr[7]);
    }
    if (numRotations >= 16) {
        dev_boardFlip(&arr[0], &arr[8]);
        dev_rotate90Right(&arr[8],  &arr[9]);
        dev_rotate90Right(&arr[9],  &arr[10]);
        dev_rotate90Right(&arr[10], &arr[11]);
        dev_mirrorVerticalAxis(&arr[8], &arr[12]);
        dev_rotate90Right(&arr[12], &arr[13]);
        dev_rotate90Right(&arr[13], &arr[14]);
        dev_rotate90Right(&arr[14], &arr[15]);
    }

    int n = (numRotations >= 16) ? 16
          : (numRotations >=  8) ?  8
          : (numRotations >=  4) ?  4 : 1;
    int minIdx = 0;
    for (int i = 1; i < n; i++) {
        if (dev_boardLT(&arr[i], &arr[minIdx]))
            minIdx = i;
    }

    board->ullCellsInUse    = arr[minIdx].ullCellsInUse;
    board->ullCellColors    = arr[minIdx].ullCellColors;
    board->usBoardInfo      = arr[minIdx].usBoardInfo;
    board->ullPossibleMoves = 0;

    dev_boardMoveCalculator(board, c);
}

#endif // __CUDACC__
