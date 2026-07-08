/*
** Filename:  NonTerminalLevelStep.cpp
**
** Purpose:
**   Implements ProcessNonTerminalLevel declared in NonTerminalLevelStep.h.
*/

/* Includes */
#include "NonTerminalLevelStep.h"
#include "CalculatorFileName.h"
#include "CalculatorLookupSource.h"
#include "CalculatorScratchCounts.h"
#include "OutcomeTriple.h"
#include "TerminalClassify.h"
#include "RetrogradeKernels.h"
#include "RSFFileName.h"
#include "OthelloBasics.h"
#include "RingNestedIndex.h"
#include <vector>
#include <atomic>
#include <thread>
#include <windows.h>

/* Structures and Types */

/*
** Type:    PlayerLevelResult
** @brief   One color's outcome from a single (possibly aborted) attempt at
**          processing level: whether it overflowed, board/outcome
**          bookkeeping, and (on success) the scratch segments holding its
**          result, not yet joined to the permanent counts directory.
*/
struct PlayerLevelResult
{
    bool             overflowed      = false;
    uint64_t         boardsProcessed = 0;
    WinTieLossTriple totals          = {};   /* best-effort display approximation -- see file Notes */

    SegmentList                           scratchSegments;
    std::vector<std::pair<char, int64_t>> scratchPlan;
    int                                   scratchByteWidth = 1;
};

/* Internal Helpers */

/*
** Function: NextWiderTier
** @brief    Returns the next tier up from currentWidth in the agreed
**           progression (nibble -> 1 -> 2 -> 4 -> 8 -> then 1-byte
**           increments -- see project_adaptive_counter_width_design memory).
** @param    currentWidth - COUNTER_WIDTH_NIBBLE or a byte width
** @return   The next-wider byte width.
*/
static int NextWiderTier(int currentWidth)
{
    if (currentWidth == COUNTER_WIDTH_NIBBLE) return 1;
    if (currentWidth == 1) return 2;
    if (currentWidth == 2) return 4;
    if (currentWidth == 4) return 8;
    return currentWidth + 1;
}

/*
** Type:    ParentJobResult
** @brief   One parent's computed contribution, filled in by its
**          thread-pool job: either a terminal one-hot classification or
**          the sum of its children's already-computed triples. ok=false
**          means this parent's sum overflowed at the level's current width.
*/
struct ParentJobResult
{
    bool                ok         = true;
    bool                isTerminal = false;
    int                 outcome    = OUTCOME_TIE;   /* valid only when isTerminal -- see ClassifyTerminalOutcome */
    OutcomeTriple        wide   = {};
    NibbleOutcomeTriple  nibble = {};
};

/*
** Function: ProcessNonTerminalLevelForPlayer
** @brief    Processes one color of level at byteWidth: streams this
**           color's boards through the GPU in batches, dispatching each
**           batch's parents to pState->pLookupThreadPool (one job per
**           parent -- lookups now involve real disk seeks against
**           segmented scratch, so serializing them would squander the
**           whole point of spreading data across drives; each parent's
**           own accumulator is thread-local, so no cross-thread
**           synchronization is needed beyond waiting for the batch's
**           jobs to finish). Writes results to SCRATCH (not the
**           permanent counts directory -- the caller joins scratch to
**           final once both colors of level succeed). Aborts
**           (result.overflowed = true) at the first overflow; caller is
**           responsible for retrying the WHOLE level (both colors) at a
**           wider width, and for discarding this attempt's scratch.
** @param    pConfig          - run configuration (boardSize)
** @param    pState           - calculator state (storeDirectory, driveInfo, driveLedger, pLookupThreadPool)
** @param    pGpu             - GPU context to expand batches through
** @param    lookup           - level+1's staged lookup source
** @param    level            - the level being processed
** @param    player           - RSF_PLAYER_BLACK or RSF_PLAYER_WHITE
** @param    byteWidth        - level's tier width for this attempt
** @param    maxMovesPerBoard - GetMaxMovesForBoardSize(boardSize)
** @param    gpuBatchSize     - parents per GPU round trip
** @return   This color's result (overflowed flag, board count, best-effort
**           totals, and -- on success -- the scratch segments holding the result).
*/
static PlayerLevelResult ProcessNonTerminalLevelForPlayer(
    POthelloRingMasterCalculatorConfig pConfig, POthelloRingMasterCalculatorState pState,
    RetrogradeGpuContext* pGpu, const LookupSource& lookup,
    int level, int player, int byteWidth, int maxMovesPerBoard, int gpuBatchSize)
{
    PlayerLevelResult result;
    result.scratchByteWidth = (byteWidth == COUNTER_WIDTH_NIBBLE) ? 1 : byteWidth;

    int  boardSize = (int)pConfig->boardSize;
    bool hasRing1  = RingNestedIndexHasRing1(boardSize);
    bool hasRing2  = RingNestedIndexHasRing2(boardSize);

    char cellsInUsePath[MAX_FULL_PATH_NAME];
    char ring1PathBuf[MAX_FULL_PATH_NAME];
    char ring2PathBuf[MAX_FULL_PATH_NAME];
    char ring34Path[MAX_FULL_PATH_NAME];
    RSFNameCellsInUseFile(cellsInUsePath, sizeof(cellsInUsePath), pState->storeDirectory, boardSize, level, player, 0);
    if (hasRing1)
        RSFNameRing1File(ring1PathBuf, sizeof(ring1PathBuf), pState->storeDirectory, boardSize, level, player, 0);
    if (hasRing2)
        RSFNameRing2File(ring2PathBuf, sizeof(ring2PathBuf), pState->storeDirectory, boardSize, level, player, 0);
    RSFNameRing34File(ring34Path, sizeof(ring34Path), pState->storeDirectory, boardSize, level, player, 0);

    const char* ring1Path     = hasRing1 ? ring1PathBuf : nullptr;
    const char* ring2Path     = hasRing2 ? ring2PathBuf : nullptr;
    int         expectedCount = 2 + (hasRing1 ? 1 : 0) + (hasRing2 ? 1 : 0);

    int foundCount = RingNestedIndexFileCount(cellsInUsePath, ring1Path, ring2Path, ring34Path);
    if (foundCount == 0)
    {
        LoggerLog("ProcessNonTerminalLevel: level %d has no %s-to-move boards, skipping\n", level, RSFPlayerStr(player));
        return result;
    }

    RingNestedIndexReader reader;
    if (foundCount != expectedCount || !reader.Load(cellsInUsePath, ring1Path, ring2Path, ring34Path))
        Fatal(FATAL_MERGE_LOGIC_ERROR,
              "ProcessNonTerminalLevel: level %d %s-to-move nested-index files are corrupt/partial (found %d of %d expected files)",
              level, RSFPlayerStr(player), foundCount, expectedCount);

    char baseName[MAX_FULL_PATH_NAME];
    snprintf(baseName, sizeof(baseName), "L%04d_%s_out", level, RSFPlayerStr(player));

    ScratchCountsWriter writer;
    writer.Init(pState, pConfig->storeDrive, pConfig->countsDrive, reader.GetBoardCount(), byteWidth,
                pConfig->scratchDirNameNoDrive, baseName);

    std::vector<UINT64_PAIR> batch;
    batch.reserve(gpuBatchSize);
    uint8_t playerBit = (uint8_t)player;

    /*
    ** Function: flushBatch
    ** @brief  Runs the buffered batch through the GPU, then dispatches
    **         one thread-pool job per parent to do that parent's
    **         (possibly disk-seeking) lookups+sum, waits for all of
    **         them, and writes results out in original order. Returns
    **         false the instant any parent's sum overflows -- the level
    **         attempt is abandoned at that point (see caller).
    */
    auto flushBatch = [&]() -> bool
    {
        if (batch.empty()) return true;

        RetrogradeExpandBatch(pGpu, batch.data(), (int)batch.size(), playerBit);

        int parentCount = (int)batch.size();
        std::vector<ParentJobResult> jobResults(parentCount);
        std::atomic<int> remaining{ parentCount };

        for (int p = 0; p < parentCount; p++)
        {
            pState->pLookupThreadPool->QueueJob([&, p](uint32_t /*thdIdx*/)
            {
                ParentJobResult& jr = jobResults[p];

                uint32_t blackCount = RetrogradeGetChildCount(pGpu, p, RSF_PLAYER_BLACK);
                uint32_t whiteCount = RetrogradeGetChildCount(pGpu, p, RSF_PLAYER_WHITE);

                if (byteWidth == COUNTER_WIDTH_NIBBLE) NibbleOutcomeTripleSetZero(&jr.nibble);
                else                                    OutcomeTripleSetZero(&jr.wide, byteWidth);

                if (blackCount == 0 && whiteCount == 0)
                {
                    BOARD_KEY parentKey{ batch[p].hi, batch[p].lo };
                    jr.isTerminal = true;
                    jr.outcome    = ClassifyTerminalOutcome(parentKey);

                    if (byteWidth == COUNTER_WIDTH_NIBBLE) NibbleOutcomeTripleSetOneHot(&jr.nibble, jr.outcome);
                    else                                    OutcomeTripleSetOneHot(&jr.wide, byteWidth, jr.outcome);
                }
                else
                {
                    for (int childPlayer = RSF_PLAYER_WHITE; jr.ok && childPlayer <= RSF_PLAYER_BLACK; childPlayer++)
                    {
                        uint32_t thisColorCount = (childPlayer == RSF_PLAYER_BLACK) ? blackCount : whiteCount;

                        for (uint32_t c = 0; jr.ok && c < thisColorCount; c++)
                        {
                            UINT64_PAIR childRec;
                            bool        colorFlipped;
                            RetrogradeGetChild(pGpu, p, childPlayer, (int)c, &childRec, &colorFlipped);
                            BOARD_KEY childKey{ childRec.hi, childRec.lo };

                            OutcomeTriple childTriple;
                            if (!LookupChildTriple(lookup, (uint8_t)childPlayer, childKey, &childTriple))
                                Fatal(FATAL_MERGE_LOGIC_ERROR,
                                      "ProcessNonTerminalLevel: level %d child (cellsInUse=0x%llX cellColors=0x%llX, %s-to-move) not found in level %d's store",
                                      level, (unsigned long long)childKey.ullCellsInUse, (unsigned long long)childKey.ullCellColors,
                                      RSFPlayerStr(childPlayer), level + 1);

                            /* Canonicalization's color-flip symmetry means this
                            ** child's stored blackWins/whiteWins can be swapped
                            ** relative to the real, played-out continuation --
                            ** un-swap before folding into the parent (tie is
                            ** unaffected either way). See RetrogradeKernels.h Notes.
                            */
                            if (colorFlipped)
                            {
                                for (int b = 0; b < WIDE_COUNTER_MAX_BYTES; b++)
                                {
                                    uint8_t tmp          = childTriple.black[b];
                                    childTriple.black[b] = childTriple.white[b];
                                    childTriple.white[b] = tmp;
                                }
                            }

                            if (byteWidth == COUNTER_WIDTH_NIBBLE)
                            {
                                NibbleOutcomeTriple childNibble{ childTriple.black[0], childTriple.white[0], childTriple.tie[0] };
                                jr.ok = NibbleOutcomeTripleAdd(&jr.nibble, &childNibble);
                            }
                            else
                            {
                                jr.ok = OutcomeTripleAdd(&jr.wide, &childTriple, byteWidth);
                            }
                        }
                    }
                }

                remaining.fetch_sub(1, std::memory_order_release);
            });
        }

        /* Wait for this batch's jobs -- cheap spin, batches are frequent
        ** and short-lived, not worth a full condition-variable barrier here.
        */
        while (remaining.load(std::memory_order_acquire) > 0)
            std::this_thread::yield();

        for (int p = 0; p < parentCount; p++)
        {
            const ParentJobResult& jr = jobResults[p];
            if (!jr.ok)
            {
                result.overflowed = true;
                return false;
            }

            if (byteWidth == COUNTER_WIDTH_NIBBLE) writer.WriteNibbleTriple(jr.nibble);
            else                                    writer.WriteTriple(jr.wide);

            /* Matches Phase 3's original scope exactly: totals is a count
            ** of TERMINAL boards' one-hot classification, not a running
            ** sum of the wide census (see file Notes on best-effort display).
            */
            if (jr.isTerminal)
            {
                if (jr.outcome == OUTCOME_BLACK_WIN)      result.totals.blackWins++;
                else if (jr.outcome == OUTCOME_WHITE_WIN) result.totals.whiteWins++;
                else                                       result.totals.ties++;
            }

            result.boardsProcessed++;
        }

        batch.clear();
        return true;
    };

    bool aborted = false;
    reader.ExpandAll([&](const BOARD_KEY& key)
    {
        if (aborted) return;
        batch.push_back(UINT64_PAIR{ key.ullCellsInUse, key.ullCellColors });
        if ((int)batch.size() >= gpuBatchSize && !flushBatch())
            aborted = true;
    });
    if (!aborted)
        flushBatch();

    writer.Finish();

    if (result.overflowed)
    {
        DeleteSegments(pState, writer.store.segments, writer.store.plan);
    }
    else
    {
        result.scratchSegments = writer.store.segments;
        result.scratchPlan     = writer.store.plan;
        LoggerLog("ProcessNonTerminalLevel: level %d %s-to-move: %llu boards\n",
                  level, RSFPlayerStr(player), (unsigned long long)result.boardsProcessed);
    }

    return result;
}

/* Functions */

/*
** Function: ProcessNonTerminalLevel
** @brief    See NonTerminalLevelStep.h.
*/
void ProcessNonTerminalLevel(POthelloRingMasterCalculatorConfig pConfig, POthelloRingMasterCalculatorState pState,
                              CounterWidthConfig* pWidthConfig, int level)
{
    if (!CreateFullPath(pState->countsDirectory))
        Fatal(FATAL_CREATE_DIR_FAILED, "ProcessNonTerminalLevel: cannot create counts directory '%s'", pState->countsDirectory);

    ClockTick startTick;
    ClockStart(&startTick);

    int boardSize = (int)pConfig->boardSize;
    int nextLevel = level + 1;

    LookupSource lookup;
    LoadLookupSource(pConfig, pState, pWidthConfig, nextLevel, &lookup);

    int       maxMovesPerBoard = GetMaxMovesForBoardSize(boardSize);
    const int kGpuBatchSize    = 65536;   /* first-slice constant -- not yet tuned, see RetrogradeKernels.cu Notes */

    RetrogradeGpuContext* pGpu = RetrogradeGpuContextCreate(boardSize, kGpuBatchSize, maxMovesPerBoard);

    pState->currentLevel = (uint8_t)level;

    PlayerLevelResult blackResult, whiteResult;
    for (;;)
    {
        int byteWidth = CounterWidthConfigGet(pWidthConfig, level);

        pState->currentPlayer = RSF_PLAYER_BLACK;
        blackResult = ProcessNonTerminalLevelForPlayer(pConfig, pState, pGpu, lookup, level, RSF_PLAYER_BLACK,
                                                        byteWidth, maxMovesPerBoard, kGpuBatchSize);
        if (blackResult.overflowed)
        {
            CounterWidthConfigBumpLevel(pWidthConfig, level, NextWiderTier(byteWidth));
            CounterWidthConfigSave(pWidthConfig, pState->cacheDirectory);
            continue;
        }

        pState->currentPlayer = RSF_PLAYER_WHITE;
        whiteResult = ProcessNonTerminalLevelForPlayer(pConfig, pState, pGpu, lookup, level, RSF_PLAYER_WHITE,
                                                        byteWidth, maxMovesPerBoard, kGpuBatchSize);
        if (whiteResult.overflowed)
        {
            /* Black already succeeded at this (now-too-narrow) width -- its
            ** scratch is stale too, since the whole level retries together.
            */
            if (!blackResult.scratchSegments.empty())
                DeleteSegments(pState, blackResult.scratchSegments, blackResult.scratchPlan);

            CounterWidthConfigBumpLevel(pWidthConfig, level, NextWiderTier(byteWidth));
            CounterWidthConfigSave(pWidthConfig, pState->cacheDirectory);
            continue;
        }

        break;   /* both colors succeeded at this width */
    }

    RetrogradeGpuContextDestroy(pGpu);

    /* This level is now confirmed to need (at least) its final width --
    ** bump propagates that floor to shallower, not-yet-processed levels
    ** (a no-op for this level's own entry, since it's already set).
    */
    int finalWidth = CounterWidthConfigGet(pWidthConfig, level);
    CounterWidthConfigBumpLevel(pWidthConfig, level, finalWidth);
    CounterWidthConfigSave(pWidthConfig, pState->cacheDirectory);

    /* Join scratch to the permanent counts directory -- "read the first
    ** drive, write it out, read the next drive, etc.," no sort needed,
    ** since each color's segments are already contiguous slices of one
    ** sorted stream. A color with no boards at this level (empty
    ** scratchSegments) gets no file at all, matching Phase 2/3's existing
    ** absent-color behavior.
    */
    for (int player = RSF_PLAYER_WHITE; player <= RSF_PLAYER_BLACK; player++)
    {
        PlayerLevelResult& r = (player == RSF_PLAYER_BLACK) ? blackResult : whiteResult;
        if (r.scratchSegments.empty()) continue;

        char finalPath[MAX_FULL_PATH_NAME];
        CalcNameCountsFile(finalPath, sizeof(finalPath), pState->countsDirectory, boardSize, level, player);
        JoinScratchCountsToFinal(r.scratchSegments, r.scratchByteWidth, finalWidth, finalPath);
        DeleteSegments(pState, r.scratchSegments, r.scratchPlan);
    }

    ReleaseLookupSource(pState, &lookup);

    CalculatorLevelStats* pStats = &pState->levelStats[level];
    pStats->boardsProcessedBlack     = blackResult.boardsProcessed;
    pStats->boardsProcessedWhite     = whiteResult.boardsProcessed;
    pStats->blackToMoveTotals        = blackResult.totals;
    pStats->whiteToMoveTotals        = whiteResult.totals;
    pStats->combinedTotals.blackWins = blackResult.totals.blackWins + whiteResult.totals.blackWins;
    pStats->combinedTotals.whiteWins = blackResult.totals.whiteWins + whiteResult.totals.whiteWins;
    pStats->combinedTotals.ties      = blackResult.totals.ties      + whiteResult.totals.ties;
    pStats->startTick                = startTick;
    pStats->totalNanos               = ClockNanosSinceStart(&startTick);

    SYSTEMTIME st = {};
    GetLocalTime(&st);
    snprintf(pStats->completedAt, sizeof(pStats->completedAt), "%04d-%02d-%02d %02d:%02d:%02d",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

    LoggerLog("ProcessNonTerminalLevel: level %d complete at width %d, %llu boards total, %lld ns\n",
              level, finalWidth,
              (unsigned long long)(pStats->boardsProcessedBlack + pStats->boardsProcessedWhite),
              (long long)pStats->totalNanos);
}
