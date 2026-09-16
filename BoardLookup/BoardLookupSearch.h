/*
** Filename:  BoardLookupSearch.h
**
** Purpose:
**   Real random-access search against an already-indexed level (see
**   OthelloRingMasterLevelIndexer) -- given a canonical, ring-ordered board
**   key, finds its level, which player's files it lives in, and its exact
**   position (segment file + local offset) in each of CellsInUse, Ring_2,
**   and Ring_3_4, without ever decoding more than one segment per ring.
**
**   Deliberately separate from BoardCanonicalize.h/.cpp: this module only
**   ever deals with already-canonical ring-ordered patterns, never row-
**   major bits or symmetry search. A caller that already has a canonical
**   key from somewhere else (a future live-solver integration, say) can
**   use this directly without going through canonicalization at all --
**   two independently reusable pieces, not one monolithic function. See
**   BoardCanonicalize.h for the other half (raw board -> canonical key).
**
**   No level in this level's whole hierarchy is ever loaded wholesale --
**   at most one segment per ring is decoded per lookup (see
**   [[feedback_never_load_level_wholesale]]).
**
** Notes:
**   Relies on group-boundary alignment already guaranteed by the indexer:
**   one CellsInUse group's children in Ring_2 never cross a Ring_2 segment
**   boundary, and one Ring_2 group's children in Ring_3_4 never cross a
**   Ring_3_4 segment boundary (that's the whole point of aligning cuts to
**   the next ring up's own `.offset` field -- see
**   OthelloRingMasterLevelIndexer.cpp's own Purpose). So once a group is
**   found in one ring, its children are guaranteed to live in exactly one
**   segment of the next ring down -- multi-segment-spanning search is only
**   ever needed for CellsInUse's own top-level, unbounded search (which
**   uses each segment's manifest-recorded [minPattern,maxPattern] to pick
**   the one right segment directly, rather than scanning).
*/

#pragma once

/* Includes */
#include <cstdint>

/* Constants */

/* Matches Utility/FileAndDirUtils.h's own MAX_FULL_PATH_NAME exactly (not
** included here, so this public header stays a plain, minimal dependency
** for a caller that only wants the struct shapes) -- kept in sync
** deliberately, since a smaller buffer here would risk silently truncating
** a real path FileAndDirUtils.h itself considers valid.
*/
constexpr int BOARD_LOOKUP_MAX_PATH = 4000;

/* Structures and Types */

/*
** Type:    RingLocation
** @brief   Where one ring's matching record actually lives.
*/
struct RingLocation
{
    bool     found                = false;
    uint64_t globalOrdinal        = 0;                        /* this ring's own 0-based position among ALL its records */
    char     segmentPath[BOARD_LOOKUP_MAX_PATH] = {};
    uint64_t segmentStartOrdinal  = 0;
    uint64_t localIndexInSegment  = 0;
    uint64_t childOffset          = 0;                        /* this record's own `.offset` field -- where ITS children start in the next ring down (0 / unused for Ring_3_4, which has none) */
    double   elapsedSeconds       = 0.0;                      /* real time spent resolving THIS ring alone -- loading its manifest/segment listing plus the search itself, not the other rings */
};

/*
** Type:    BoardLookupResult
** @brief   Everything a lookup found (or didn't) for one canonical board.
*/
struct BoardLookupResult
{
    bool     found  = false;
    int      level  = -1;
    int      player = 0;      /* RSF_PLAYER_BLACK / RSF_PLAYER_WHITE (RSFFileName.h) */

    RingLocation cellsInUseLoc;
    RingLocation ring2Loc;     /* .found stays false for a board size with no Ring_2 (4x4) */
    RingLocation ring34Loc;

    double   elapsedSeconds = 0.0;
    char     notFoundReason[256] = {};   /* set when found is false, explaining where the descent stopped */
};

/* Functions */

/*
** Function: FindBoardInStore
** @brief    Descends CellsInUse -> Ring_2 -> Ring_3_4 for a canonical,
**           ring-ordered board key, using each ring's real segmented index
**           (must already exist -- see OthelloRingMasterLevelIndexer).
**           Level is derived directly from the board itself (popcount of
**           ringCellsInUse, minus the 4 starting discs -- see
**           project_level_numbering_offset_by_one memory), never guessed
**           or passed in separately.
** @param    ringCellsInUse - canonical, ring-ordered occupancy (e.g. from
**                            CanonicalizeBoard's result)
** @param    ringCellColors - canonical, ring-ordered color pattern
** @param    player         - RSF_PLAYER_BLACK or RSF_PLAYER_WHITE -- which
**                            player's files to search (the canonical
**                            form's own resulting to-move color)
** @param    boardSize      - board size (4, 6, or 8)
** @param    levelIndexDir  - the segmented index area (see
**                            OthelloRingMasterLevelIndexer's own
**                            --levelindex-dir) -- the only thing this ever
**                            reads. The original flat storeDir files are
**                            never touched by a lookup: every ring is
**                            always segmented now (v1.3.1), so there is no
**                            "fall back to the flat file" case left.
** @return   Where the board was found (or wasn't, with notFoundReason set).
*/
BoardLookupResult FindBoardInStore(uint64_t ringCellsInUse, uint64_t ringCellColors, int player,
                                    int boardSize, const char* levelIndexDir);
