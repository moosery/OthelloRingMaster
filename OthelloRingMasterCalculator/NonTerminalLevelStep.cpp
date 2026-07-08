/*
** Filename:  NonTerminalLevelStep.cpp
**
** Purpose:
**   Implements ProcessNonTerminalLevel declared in NonTerminalLevelStep.h.
*/

/* Includes */
#include "NonTerminalLevelStep.h"
#include "CalculatorFileName.h"
#include "CalculatorCountsFile.h"
#include "OutcomeTriple.h"
#include "TerminalClassify.h"
#include "RetrogradeKernels.h"
#include "RSFFileName.h"
#include "OthelloBasics.h"
#include "RingNestedIndex.h"
#include <vector>
#include <windows.h>

/* Structures and Types */

/*
** Type:    NextLevelLookupData
** @brief   level+1's board-position lookup structures and already-computed
**          counts, for both colors -- everything ProcessNonTerminalLevel
**          needs to resolve a generated child to its already-computed
**          OutcomeTriple. Loaded once per ProcessNonTerminalLevel call and
**          reused across both of level's own colors.
*/
struct NextLevelLookupData
{
    RingNestedIndexReader readerBlack, readerWhite;
    bool hasBlack = false, hasWhite = false;
    int  byteWidth = COUNTER_WIDTH_NIBBLE;

    std::vector<OutcomeTriple>       countsBlack, countsWhite;              /* used iff byteWidth != nibble */
    std::vector<NibbleOutcomeTriple> nibbleCountsBlack, nibbleCountsWhite;  /* used iff byteWidth == nibble */
};

/*
** Type:    PlayerLevelResult
** @brief   One color's outcome from a single (possibly aborted) attempt at
**          processing level.
*/
struct PlayerLevelResult
{
    bool             overflowed      = false;
    uint64_t         boardsProcessed = 0;
    WinTieLossTriple totals          = {};   /* best-effort display approximation -- see file Notes */
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
** Function: LoadNextLevelLookupData
** @brief    Loads nextLevel's board-position lookup structures and
**           already-computed counts (both colors) fully into memory.
** @param    pConfig      - run configuration (boardSize)
** @param    pState       - calculator state (storeDirectory, countsDirectory)
** @param    pWidthConfig - this board size's width table (nextLevel's width is read)
** @param    nextLevel    - the already-fully-processed level to load as a lookup source
** @param    pOut         - out: filled lookup data
*/
static void LoadNextLevelLookupData(POthelloRingMasterCalculatorConfig pConfig, POthelloRingMasterCalculatorState pState,
                                     CounterWidthConfig* pWidthConfig, int nextLevel, NextLevelLookupData* pOut)
{
    int boardSize   = (int)pConfig->boardSize;
    pOut->byteWidth = CounterWidthConfigGet(pWidthConfig, nextLevel);

    bool hasRing1 = RingNestedIndexHasRing1(boardSize);
    bool hasRing2 = RingNestedIndexHasRing2(boardSize);

    for (int player = RSF_PLAYER_WHITE; player <= RSF_PLAYER_BLACK; player++)
    {
        char cellsInUsePath[MAX_FULL_PATH_NAME];
        char ring1PathBuf[MAX_FULL_PATH_NAME];
        char ring2PathBuf[MAX_FULL_PATH_NAME];
        char ring34Path[MAX_FULL_PATH_NAME];
        RSFNameCellsInUseFile(cellsInUsePath, sizeof(cellsInUsePath), pState->storeDirectory, boardSize, nextLevel, player, 0);
        if (hasRing1)
            RSFNameRing1File(ring1PathBuf, sizeof(ring1PathBuf), pState->storeDirectory, boardSize, nextLevel, player, 0);
        if (hasRing2)
            RSFNameRing2File(ring2PathBuf, sizeof(ring2PathBuf), pState->storeDirectory, boardSize, nextLevel, player, 0);
        RSFNameRing34File(ring34Path, sizeof(ring34Path), pState->storeDirectory, boardSize, nextLevel, player, 0);

        const char* ring1Path      = hasRing1 ? ring1PathBuf : nullptr;
        const char* ring2Path      = hasRing2 ? ring2PathBuf : nullptr;
        int         expectedCount  = 2 + (hasRing1 ? 1 : 0) + (hasRing2 ? 1 : 0);

        int foundCount = RingNestedIndexFileCount(cellsInUsePath, ring1Path, ring2Path, ring34Path);
        if (foundCount == 0)
        {
            /* Genuinely no boards of this color at nextLevel -- legitimate, nothing to load. */
            continue;
        }

        RingNestedIndexReader& reader = (player == RSF_PLAYER_BLACK) ? pOut->readerBlack : pOut->readerWhite;
        if (foundCount != expectedCount || !reader.Load(cellsInUsePath, ring1Path, ring2Path, ring34Path))
            Fatal(FATAL_MERGE_LOGIC_ERROR,
                  "ProcessNonTerminalLevel: level %d %s-to-move nested-index files are corrupt/partial (found %d of %d expected files)",
                  nextLevel, RSFPlayerStr(player), foundCount, expectedCount);

        if (player == RSF_PLAYER_BLACK) pOut->hasBlack = true;
        else                            pOut->hasWhite = true;

        char countsPath[MAX_FULL_PATH_NAME];
        CalcNameCountsFile(countsPath, sizeof(countsPath), pState->countsDirectory, boardSize, nextLevel, player);
        uint64_t boardCount = reader.GetBoardCount();

        if (pOut->byteWidth == COUNTER_WIDTH_NIBBLE)
        {
            std::vector<NibbleOutcomeTriple>& vec = (player == RSF_PLAYER_BLACK) ? pOut->nibbleCountsBlack : pOut->nibbleCountsWhite;
            NibbleCountsReader* pReader = NibbleCountsReaderOpen(countsPath, boardCount);
            if (!pReader)
                Fatal(FATAL_MERGE_LOGIC_ERROR, "ProcessNonTerminalLevel: level %d %s-to-move counts file missing at '%s'",
                      nextLevel, RSFPlayerStr(player), countsPath);

            vec.reserve((size_t)boardCount);
            NibbleOutcomeTriple t;
            while (NibbleCountsReaderRead(pReader, &t))
                vec.push_back(t);
            NibbleCountsReaderClose(&pReader);

            if (vec.size() != boardCount)
                Fatal(FATAL_MERGE_LOGIC_ERROR, "ProcessNonTerminalLevel: level %d %s-to-move counts file has %zu records, expected %llu",
                      nextLevel, RSFPlayerStr(player), vec.size(), (unsigned long long)boardCount);
        }
        else
        {
            std::vector<OutcomeTriple>& vec = (player == RSF_PLAYER_BLACK) ? pOut->countsBlack : pOut->countsWhite;
            CalculatorCountsReader* pReader = CalculatorCountsReaderOpen(countsPath, pOut->byteWidth);
            if (!pReader)
                Fatal(FATAL_MERGE_LOGIC_ERROR, "ProcessNonTerminalLevel: level %d %s-to-move counts file missing at '%s'",
                      nextLevel, RSFPlayerStr(player), countsPath);

            vec.reserve((size_t)boardCount);
            OutcomeTriple t;
            while (CalculatorCountsReaderRead(pReader, &t))
                vec.push_back(t);
            CalculatorCountsReaderClose(&pReader);

            if (vec.size() != boardCount)
                Fatal(FATAL_MERGE_LOGIC_ERROR, "ProcessNonTerminalLevel: level %d %s-to-move counts file has %zu records, expected %llu",
                      nextLevel, RSFPlayerStr(player), vec.size(), (unsigned long long)boardCount);
        }
    }
}

/*
** Function: LookupChildTriple
** @brief    Finds child's position in lookup's board store for childPlayer
**           and returns its already-computed OutcomeTriple, uniformly
**           zero-extended regardless of whether nextLevel was stored at
**           the nibble tier or a byte-and-wider tier.
** @param    lookup      - level+1's loaded lookup data
** @param    childPlayer - which color's store to search
** @param    childKey    - the child board to find
** @param    pOut        - out: child's OutcomeTriple, valid up to at least
**                         lookup.byteWidth bytes (zero beyond that)
** @return   true if found. false is always a real data-integrity problem
**           at the caller (level+1's store must be complete), not a
**           legitimate case to handle quietly.
*/
static bool LookupChildTriple(const NextLevelLookupData& lookup, uint8_t childPlayer, const BOARD_KEY& childKey, OutcomeTriple* pOut)
{
    bool hasReader = (childPlayer == RSF_PLAYER_BLACK) ? lookup.hasBlack : lookup.hasWhite;
    if (!hasReader) return false;

    const RingNestedIndexReader& reader = (childPlayer == RSF_PLAYER_BLACK) ? lookup.readerBlack : lookup.readerWhite;

    uint64_t pos;
    if (!reader.FindBoardPosition(childKey, &pos))
        return false;

    if (lookup.byteWidth == COUNTER_WIDTH_NIBBLE)
    {
        const std::vector<NibbleOutcomeTriple>& vec = (childPlayer == RSF_PLAYER_BLACK) ? lookup.nibbleCountsBlack : lookup.nibbleCountsWhite;
        if (pos >= vec.size()) return false;

        OutcomeTripleSetZero(pOut, 1);
        pOut->black[0] = vec[pos].black;
        pOut->white[0] = vec[pos].white;
        pOut->tie[0]   = vec[pos].tie;
    }
    else
    {
        const std::vector<OutcomeTriple>& vec = (childPlayer == RSF_PLAYER_BLACK) ? lookup.countsBlack : lookup.countsWhite;
        if (pos >= vec.size()) return false;

        /* Already zero-extended to the full 32-byte representation by
        ** OutcomeTripleSetZero at read time (see CalculatorCountsFile.cpp),
        ** so this is safe to add at any byteWidth >= lookup.byteWidth
        ** (guaranteed by the monotonic width-propagation invariant --
        ** level's own width is never narrower than level+1's).
        */
        *pOut = vec[pos];
    }
    return true;
}

/*
** Function: ProcessNonTerminalLevelForPlayer
** @brief    Processes one color of level at byteWidth: streams this
**           color's boards through the GPU in batches, classifies
**           terminal boards directly, sums non-terminal boards' children
**           (swapping blackWins/whiteWins for any child whose canonical
**           form came from the color-flip symmetry -- see
**           RetrogradeKernels.h's own Notes), and writes results out in
**           original board order. Aborts (result.overflowed = true) at
**           the first overflow; caller is responsible for retrying the
**           WHOLE level (both colors) at a wider width.
** @param    pConfig          - run configuration (boardSize)
** @param    pState           - calculator state (storeDirectory, countsDirectory)
** @param    pGpu             - GPU context to expand batches through
** @param    lookup           - level+1's loaded lookup data
** @param    level            - the level being processed
** @param    player           - RSF_PLAYER_BLACK or RSF_PLAYER_WHITE
** @param    byteWidth        - level's tier width for this attempt
** @param    maxMovesPerBoard - GetMaxMovesForBoardSize(boardSize)
** @param    gpuBatchSize     - parents per GPU round trip
** @return   This color's result (overflowed flag, board count, best-effort totals).
*/
static PlayerLevelResult ProcessNonTerminalLevelForPlayer(
    POthelloRingMasterCalculatorConfig pConfig, POthelloRingMasterCalculatorState pState,
    RetrogradeGpuContext* pGpu, const NextLevelLookupData& lookup,
    int level, int player, int byteWidth, int maxMovesPerBoard, int gpuBatchSize)
{
    PlayerLevelResult result;

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

    char countsPath[MAX_FULL_PATH_NAME];
    CalcNameCountsFile(countsPath, sizeof(countsPath), pState->countsDirectory, boardSize, level, player);

    NibbleCountsWriter*     pNibbleWriter = nullptr;
    CalculatorCountsWriter* pWideWriter   = nullptr;
    if (byteWidth == COUNTER_WIDTH_NIBBLE) pNibbleWriter = NibbleCountsWriterOpen(countsPath);
    else                                    pWideWriter   = CalculatorCountsWriterOpen(countsPath, byteWidth);

    std::vector<UINT64_PAIR> batch;
    batch.reserve(gpuBatchSize);
    uint8_t playerBit = (uint8_t)player;

    /*
    ** Function: flushBatch
    ** @brief  Runs the buffered batch through the GPU and, for each
    **         parent, either classifies it directly (terminal) or sums
    **         its children's already-computed triples, then writes the
    **         result. Returns false the instant an overflow is hit --
    **         the level attempt is abandoned at that point (see caller).
    */
    auto flushBatch = [&]() -> bool
    {
        if (batch.empty()) return true;

        RetrogradeExpandBatch(pGpu, batch.data(), (int)batch.size(), playerBit);

        for (int p = 0; p < (int)batch.size(); p++)
        {
            uint32_t childCount = RetrogradeGetChildCount(pGpu, p);

            OutcomeTriple       accumWide   = {};
            NibbleOutcomeTriple accumNibble = {};
            if (byteWidth == COUNTER_WIDTH_NIBBLE) NibbleOutcomeTripleSetZero(&accumNibble);
            else                                    OutcomeTripleSetZero(&accumWide, byteWidth);

            if (childCount == 0)
            {
                BOARD_KEY parentKey{ batch[p].hi, batch[p].lo };
                int outcome = ClassifyTerminalOutcome(parentKey);

                if (byteWidth == COUNTER_WIDTH_NIBBLE) NibbleOutcomeTripleSetOneHot(&accumNibble, outcome);
                else                                    OutcomeTripleSetOneHot(&accumWide, byteWidth, outcome);

                if (outcome == OUTCOME_BLACK_WIN)      result.totals.blackWins++;
                else if (outcome == OUTCOME_WHITE_WIN) result.totals.whiteWins++;
                else                                    result.totals.ties++;
            }
            else
            {
                const UINT64_PAIR* children = RetrogradeGetChildren(pGpu, p);
                bool ok = true;

                for (uint32_t c = 0; ok && c < childCount; c++)
                {
                    BOARD_KEY childKey{ children[c].hi, children[c].lo };
                    uint8_t   childPlayer  = RetrogradeGetChildPlayer(pGpu, p, (int)c);
                    bool      colorFlipped = RetrogradeGetChildColorFlipped(pGpu, p, (int)c);

                    OutcomeTriple childTriple;
                    if (!LookupChildTriple(lookup, childPlayer, childKey, &childTriple))
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
                        ok = NibbleOutcomeTripleAdd(&accumNibble, &childNibble);
                    }
                    else
                    {
                        ok = OutcomeTripleAdd(&accumWide, &childTriple, byteWidth);
                    }
                }

                if (!ok)
                {
                    result.overflowed = true;
                    return false;
                }
            }

            if (byteWidth == COUNTER_WIDTH_NIBBLE) NibbleCountsWriterWrite(pNibbleWriter, &accumNibble);
            else                                    CalculatorCountsWriterWrite(pWideWriter, &accumWide);

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

    if (pNibbleWriter) NibbleCountsWriterClose(pNibbleWriter);
    if (pWideWriter)   CalculatorCountsWriterClose(pWideWriter);

    if (!result.overflowed)
        LoggerLog("ProcessNonTerminalLevel: level %d %s-to-move: %llu boards\n",
                  level, RSFPlayerStr(player), (unsigned long long)result.boardsProcessed);

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

    NextLevelLookupData lookup;
    LoadNextLevelLookupData(pConfig, pState, pWidthConfig, nextLevel, &lookup);

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
