/*
** Filename:  OthelloRingMasterBoardLookup.cpp
**
** Purpose:
**   Real, permanent tool: interactive CLI that reads a human-typed 6x6
**   board (6 lines of 6 characters, space/b/w -- see PrintUsage) and whose
**   turn it is, canonicalizes it (BoardLookup/BoardCanonicalize.h), and
**   reports where it lives in the real, already-segmented store (Board
**   Lookup/BoardLookupSearch.h) -- level, player, and for each of
**   CellsInUse/Ring_2/Ring_3_4: which segment file and what offset within
**   it, plus real lookup timing.
**
**   All the real work lives in the BoardLookup library (deliberately
**   built as its own reusable module, not tangled into this CLI's own
**   .cpp) -- this file is just the human-facing shell: read input, call
**   the library, print results.
**
** Notes:
**   Requires the level actually already be indexed -- see
**   OthelloRingMasterLevelIndexer. A board from an unindexed level (or one
**   that was never actually reached during the real solve) is a normal,
**   expected "not found" outcome, not a crash.
*/

/* Includes */
#include "BoardCanonicalize.h"
#include "BoardLookupSearch.h"
#include "RSFFileName.h"
#include "FileAndDirUtils.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>

/* Functions */

/*
** Function: PrintUsage
** @brief    Prints command-line usage help.
*/
static void PrintUsage(const char* prog)
{
    printf("Usage: %s [options]\n\n", prog);
    printf("  --board-size N        Board size (6 is the only real, indexed size today)  [default: 6]\n");
    printf("  --levelindex-drive L  Drive letter the segmented index lives on             [default: Y]\n");
    printf("  --levelindex-dir P    Sub-path on that drive (no drive letter)                [default: \\OthelloRingMaster\\Store\\levelIndexDir]\n");
    printf("  --help                Show this help\n\n");
    printf("Then interactively prompts for the board: %d lines of %d characters each\n", 6, 6);
    printf("(no leading/trailing spaces trimmed -- type exactly %d characters per row),\n", 6);
    printf("space = empty, b/B = black, w/W = white, then whose turn it is (b/w).\n\n");
}

/*
** Function: ReadBoardLine
** @brief    Reads one board row from stdin, validates it's exactly
**           boardSize real board characters (space/b/B/w/W).
** @param    boardSize - expected line length
** @param    out       - out: boardSize characters, normalized to lowercase
**                       (or ' ')
** @return   true on a valid line; false on EOF or a malformed line.
*/
static bool ReadBoardLine(int boardSize, char* out)
{
    char raw[256];
    if (!fgets(raw, sizeof(raw), stdin))
        return false;

    /* Strip trailing CR/LF. */
    size_t len = strlen(raw);
    while (len > 0 && (raw[len - 1] == '\n' || raw[len - 1] == '\r')) { raw[len - 1] = '\0'; len--; }

    if ((int)len != boardSize)
    {
        printf("ERROR: expected exactly %d characters, got %d -- try again.\n", boardSize, (int)len);
        return false;
    }

    for (int i = 0; i < boardSize; i++)
    {
        char c = raw[i];
        if (c == ' ')      out[i] = ' ';
        else if (c == 'b' || c == 'B') out[i] = 'b';
        else if (c == 'w' || c == 'W') out[i] = 'w';
        else
        {
            printf("ERROR: character %d is '%c' -- only space, b/B, w/W are valid.\n", i + 1, c);
            return false;
        }
    }
    return true;
}

int main(int argc, char* argv[])
{
    int  boardSize  = 6;
    char levelIndexDrive = 'Y';
    char levelIndexDirNoDrive[MAX_FULL_PATH_NAME] = "\\OthelloRingMaster\\Store\\levelIndexDir";

    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) { PrintUsage(argv[0]); return 0; }
#define REQUIRE_NEXT(flag) if (++i >= argc) { printf("ERROR: %s requires a value\n", flag); return 1; }
        if (strcmp(argv[i], "--board-size") == 0)            { REQUIRE_NEXT("--board-size")        boardSize = atoi(argv[i]); }
        else if (strcmp(argv[i], "--levelindex-drive") == 0) { REQUIRE_NEXT("--levelindex-drive")  levelIndexDrive = (char)toupper((unsigned char)argv[i][0]); }
        else if (strcmp(argv[i], "--levelindex-dir") == 0)   { REQUIRE_NEXT("--levelindex-dir")    strncpy(levelIndexDirNoDrive, argv[i], sizeof(levelIndexDirNoDrive) - 1); }
        else { printf("ERROR: unknown argument '%s'\n\n", argv[i]); PrintUsage(argv[0]); return 1; }
#undef REQUIRE_NEXT
    }

    if (boardSize != 6)
        printf("WARNING: board size %d has no real segmented data yet (only 6x6 is indexed today) -- this will likely just report 'not found'.\n\n", boardSize);

    char levelIndexDir[MAX_FULL_PATH_NAME];
    snprintf(levelIndexDir, sizeof(levelIndexDir), "%c:%s", levelIndexDrive, levelIndexDirNoDrive);

    printf("Enter the board: %d rows of %d characters (space/b/w), one row per line.\n", boardSize, boardSize);

    char board[8][8];
    for (int r = 0; r < boardSize; r++)
    {
        char line[9];
        while (!ReadBoardLine(boardSize, line)) { /* reprompt on a malformed line */ }
        memcpy(board[r], line, boardSize);
    }

    printf("Whose turn is it (b/w)? ");
    fflush(stdout);
    char turnLine[16];
    bool blackToMove = true;
    if (fgets(turnLine, sizeof(turnLine), stdin))
    {
        char c = turnLine[0];
        if (c == 'w' || c == 'W') blackToMove = false;
        else if (c == 'b' || c == 'B') blackToMove = true;
        else printf("(unrecognized -- defaulting to black to move)\n");
    }

    /* Row-major bit conversion -- a smaller board's cells are centered
    ** within the 8x8 word (offset = (8-boardSize)/2, confirmed against
    ** SetBoardSizeForRun's own g_boardSi -- OthelloBasicsForCUDA.h), same
    ** encoding the whole store is built on.
    */
    int offset = (8 - boardSize) / 2;
    uint64_t rowMajorCellsInUse = 0, rowMajorCellColors = 0;
    for (int r = 0; r < boardSize; r++)
    {
        for (int c = 0; c < boardSize; c++)
        {
            char ch = board[r][c];
            if (ch == ' ') continue;
            int bitIdx = (r + offset) * 8 + (c + offset);
            uint64_t bit = 1ULL << (63 - bitIdx);
            rowMajorCellsInUse |= bit;
            if (ch == 'b') rowMajorCellColors |= bit;
        }
    }

    printf("\nRaw (row-major) cellsInUse=0x%016llx cellColors=0x%016llx, %s to move\n",
           (unsigned long long)rowMajorCellsInUse, (unsigned long long)rowMajorCellColors,
           blackToMove ? "black" : "white");

    CanonicalizeResult canon = CanonicalizeBoard(rowMajorCellsInUse, rowMajorCellColors, blackToMove);
    printf("Canonical (ring-ordered) cellsInUse=0x%016llx cellColors=0x%016llx, %s to move\n\n",
           (unsigned long long)canon.ringCellsInUse, (unsigned long long)canon.ringCellColors,
           canon.blackToMove ? "black" : "white");

    int player = canon.blackToMove ? RSF_PLAYER_BLACK : RSF_PLAYER_WHITE;
    BoardLookupResult result = FindBoardInStore(canon.ringCellsInUse, canon.ringCellColors, player,
                                                 boardSize, levelIndexDir);

    printf("Level: %d   Player to move: %s\n", result.level, RSFPlayerStr(player));

    if (!result.found)
    {
        printf("NOT FOUND -- %s\n", result.notFoundReason);
        printf("(lookup took %.6f seconds)\n", result.elapsedSeconds);
        return 0;
    }

    printf("\nFOUND. (lookup took %.6f seconds)\n\n", result.elapsedSeconds);
    printf("  CellsInUse: global ordinal %llu -- segment '%s'  (starts at %llu, local index %llu)\n",
           (unsigned long long)result.cellsInUseLoc.globalOrdinal, result.cellsInUseLoc.segmentPath,
           (unsigned long long)result.cellsInUseLoc.segmentStartOrdinal, (unsigned long long)result.cellsInUseLoc.localIndexInSegment);
    printf("  Ring_2:     global ordinal %llu -- segment '%s'  (starts at %llu, local index %llu)\n",
           (unsigned long long)result.ring2Loc.globalOrdinal, result.ring2Loc.segmentPath,
           (unsigned long long)result.ring2Loc.segmentStartOrdinal, (unsigned long long)result.ring2Loc.localIndexInSegment);
    printf("  Ring_3_4:   global ordinal %llu -- segment '%s'  (starts at %llu, local index %llu)\n",
           (unsigned long long)result.ring34Loc.globalOrdinal, result.ring34Loc.segmentPath,
           (unsigned long long)result.ring34Loc.segmentStartOrdinal, (unsigned long long)result.ring34Loc.localIndexInSegment);

    return 0;
}
