/*
** Filename:  StoreCompareLevel.h
**
** Purpose:
**   Declares the per-level, per-player board-set comparison this tool is
**   built around: confirms two stores (e.g. the old pre-RMA Intel run and
**   the from-scratch AMD rerun, see project_store_set_equality_tool_plan
**   memory) hold the EXACT SAME SET of boards at a level, not just a
**   matching count. Streams both sides via RingNestedIndexPullReader (the
**   same reader the live solver's own cascade k-way merge uses, so
**   whatever on-disk ring-store shape either side actually has -- however
**   old -- is read the same way the project already trusts elsewhere; no
**   separate "legacy format" path needed here).
**
** Notes:
**   A sorted-merge walk, not a hash/set build: both sides' ring nested-
**   index files are already globally sorted by construction (that's what
**   the merge that wrote them guarantees), so comparing is a classic
**   two-pointer merge -- O(1) memory regardless of level size, never
**   holding a whole level resident (see feedback_never_load_level_
**   wholesale memory), the same guarantee PullReader itself already makes.
*/

#pragma once

/* Includes */
#include "OthelloBasics.h"
#include <cstdint>
#include <vector>

/* Constants */

/* Cap on how many individual mismatched boards get kept for the printed
** report -- a badly diverged level could otherwise produce millions of
** entries; the exact count is always reported even when the sample is
** capped.
*/
constexpr uint64_t STORE_COMPARE_MAX_SAMPLE_MISMATCHES = 20;

/* Structures and Types */

/*
** Type:    StoreCompareMismatch
** @brief   One board present on exactly one side.
*/
struct StoreCompareMismatch
{
    BOARD_KEY key;
    bool      onlyInOld;   /* true: old has it, new doesn't. false: the reverse. */
};

/*
** Type:    StoreCompareResult
** @brief   Outcome of comparing one level/player's full board set between
**          two stores.
*/
struct StoreCompareResult
{
    uint64_t matched               = 0;   /* boards present, identical, on both sides */
    uint64_t onlyInOldCount        = 0;   /* total boards found only in the old store */
    uint64_t onlyInNewCount        = 0;   /* total boards found only in the new store */
    bool     oldCorrupted          = false;   /* PullReader::IsCorrupted() on the old side */
    bool     newCorrupted          = false;   /* PullReader::IsCorrupted() on the new side */
    bool     oldOpenFailed         = false;   /* old side's files wouldn't open at all */
    bool     newOpenFailed         = false;   /* new side's files wouldn't open at all */
    std::vector<StoreCompareMismatch> sample;   /* first STORE_COMPARE_MAX_SAMPLE_MISMATCHES mismatches, in the order found */

    /*
    ** Method: IsClean
    ** @brief  True if every board matched and nothing went wrong opening/reading either side.
    ** @details Othello's strict black/white alternation means an early level can
    **          legitimately have zero boards for one color (level 0 is black-only,
    **          level 1 white-only, level 2 black-only, ...) -- RingNestedIndexBuilder
    **          never writes files for a color with no boards, so BOTH sides failing to
    **          open identically is that expected case, not a divergence. Only an
    **          ASYMMETRIC open failure (one side has the files, the other doesn't) is
    **          a real problem -- that's the only open-failure shape this treats as unclean.
    */
    bool IsClean() const
    {
        if (oldOpenFailed != newOpenFailed) return false;
        return onlyInOldCount == 0 && onlyInNewCount == 0 && !oldCorrupted && !newCorrupted;
    }
};

/* Functions */

/*
** Function: StoreCompareLevelPlayer
** @brief    Compares one level/player's complete board set between two
**           store directories via a streaming two-pointer merge.
** @param    oldStoreDir - old store's directory (e.g. Y:\OthelloRingMaster.save\Store)
** @param    newStoreDir - new store's directory (e.g. Y:\OthelloRingMaster\Store)
** @param    boardSize   - board size (4, 6, or 8) -- selects which rings apply
** @param    level       - level number to compare
** @param    player      - RSF_PLAYER_BLACK or RSF_PLAYER_WHITE
** @return   Result of the comparison. A side's files failing to open at all
**           (oldOpenFailed/newOpenFailed) is reported in the result, not
**           thrown as Fatal() -- this tool surveys and reports, it never
**           halts the whole run over one bad level, the same read-only-
**           and-keep-going posture OthelloRingMasterStoreStats already has.
*/
StoreCompareResult StoreCompareLevelPlayer(const char* oldStoreDir, const char* newStoreDir,
                                            int boardSize, int level, int player);
