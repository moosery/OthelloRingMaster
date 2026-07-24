/*
** Filename:  RingNestedIndex.cpp
**
** Purpose:
**   Implements RingNestedIndexBuilder/RingNestedIndexReader (declared in
**   RingNestedIndex.h).
*/

/* Includes */
#include "RingNestedIndex.h"
#include <stdio.h>
#include <windows.h>

/* Functions */

/*
** Method: RingNestedIndexBuilder::Init
** @brief  Attaches the already-open outputs this builder writes to.
** @param  pCellsInUseWriterIn - already-open RSFWriter for the CellsInUse output
** @param  pRing1WriterIn      - already-open RSFWriter (RSF_SHAPE_RING_LEVEL) for the Ring_1 output, or nullptr if not applicable
** @param  pRing2WriterIn      - already-open RSFWriter (RSF_SHAPE_RING_LEVEL) for the Ring_2 output, or nullptr if not applicable
** @param  pRing34WriterIn     - already-open RSFWriter (RSF_SHAPE_LEAF16) for the Ring_3_4 output (always required)
*/
void RingNestedIndexBuilder::Init(RSFWriter* pCellsInUseWriterIn, RSFWriter* pRing1WriterIn,
                                   RSFWriter* pRing2WriterIn, RSFWriter* pRing34WriterIn)
{
    pCellsInUseWriter = pCellsInUseWriterIn;
    pRing1Writer      = pRing1WriterIn;
    pRing2Writer      = pRing2WriterIn;
    pRing34Writer     = pRing34WriterIn;
}

/*
** Method: RingNestedIndexBuilder::CloseRing34Group
** @brief  Writes the current Ring_3_4 group's record and closes it.
*/
void RingNestedIndexBuilder::CloseRing34Group()
{
    if (!haveRing34Group) return;

    /* Not storing ring34GroupCount on disk (Ring34Rec has no count field --
    ** see RingNestedIndex.h Notes) -- but it's still checked here every
    ** time, so a violation of the "always 1" invariant shows up in stats
    ** instead of silently losing data.
    */
    Ring34Rec rec{ curRing34Pattern };
    RSFWriterRecordShaped(pRing34Writer, &rec);
    if (ring34GroupCount != 1)
        stats.ring34GroupsWithCountNot1++;
    stats.ring34Records++;
    haveRing34Group = false;
}

/*
** Method: RingNestedIndexBuilder::CloseRing2Group
** @brief  Closes the current Ring_3_4 group, then writes the current
**         Ring_2 group's record and closes it -- unless pRing2Writer is
**         null (this board size never touches Ring_2, see file Notes), in
**         which case this only cascades down and haveRing2Group can never
**         have been set true in the first place. No count field written --
**         see RingNestedIndex.h Notes (span derived via lookahead on read).
*/
void RingNestedIndexBuilder::CloseRing2Group()
{
    CloseRing34Group();
    if (!pRing2Writer || !haveRing2Group) return;

    RingLevelRec rec{ curRing2Pattern, ring2GroupRing34Start };
    RSFWriterRecordShaped(pRing2Writer, &rec);
    stats.ring2Records++;
    haveRing2Group = false;
}

/*
** Method: RingNestedIndexBuilder::CloseRing1Group
** @brief  Closes the current Ring_2 group (which cascades to Ring_3_4),
**         then writes the current Ring_1 group's record and closes it --
**         unless pRing1Writer is null (see CloseRing2Group's comment; same
**         reasoning applies one level up). No count field written -- see
**         RingNestedIndex.h Notes (span derived via lookahead on read).
*/
void RingNestedIndexBuilder::CloseRing1Group()
{
    CloseRing2Group();
    if (!pRing1Writer || !haveRing1Group) return;

    RingLevelRec rec{ curRing1Pattern, ring1GroupRing2Start };
    RSFWriterRecordShaped(pRing1Writer, &rec);
    stats.ring1Records++;
    haveRing1Group = false;
}

/*
** Method: RingNestedIndexBuilder::Process
** @brief  Feeds one ring-ordered BOARD_KEY into the builder.
** @param  key - the next ring-ordered BOARD_KEY in sorted order
*/
void RingNestedIndexBuilder::Process(const BOARD_KEY& key)
{
    uint32_t  ring1Pattern  = (uint32_t)((key.ullCellColors >> RING1_SHIFT)  & ((1u << RING1_BITS) - 1));
    uint32_t  ring2Pattern  = (uint32_t)((key.ullCellColors >> RING2_SHIFT)  & ((1u << RING2_BITS) - 1));
    uint16_t  ring34Pattern = (uint16_t)((key.ullCellColors >> RING34_SHIFT) & ((1u << RING34_BITS) - 1));

    /* A skipped level (pWriter == nullptr, this board size never touches
    ** that ring -- see file Notes) must genuinely never see a nonzero
    ** pattern. If it ever does, either the board-size assumption driving
    ** the skip is wrong or the data is corrupt -- either way, silently
    ** ignoring real color bits would be real, silent data loss, so fail
    ** loudly instead of trusting the assumption blindly.
    */
    if (!pRing1Writer && ring1Pattern != 0)
        Fatal(FATAL_MERGE_LOGIC_ERROR,
              "RingNestedIndexBuilder::Process: Ring_1 is skipped for this board size but "
              "ring1Pattern=0x%X is nonzero (ullCellsInUse=0x%llX ullCellColors=0x%llX)",
              ring1Pattern, (unsigned long long)key.ullCellsInUse, (unsigned long long)key.ullCellColors);
    if (!pRing2Writer && ring2Pattern != 0)
        Fatal(FATAL_MERGE_LOGIC_ERROR,
              "RingNestedIndexBuilder::Process: Ring_2 is skipped for this board size but "
              "ring2Pattern=0x%X is nonzero (ullCellsInUse=0x%llX ullCellColors=0x%llX)",
              ring2Pattern, (unsigned long long)key.ullCellsInUse, (unsigned long long)key.ullCellColors);

    /* A new occupancy pattern closes every open group below it (cascading
    ** through CloseRing1Group -- safe to call unconditionally, it no-ops
    ** at any level whose writer is null), then starts a new CellsInUse
    ** entry pointing at wherever the next record actually lands: Ring_1
    ** normally, or Ring_2/Ring_3_4 directly when the outer level(s) are
    ** skipped for this board size.
    */
    if (!havePattern || key.ullCellsInUse != curPattern)
    {
        CloseRing1Group();

        uint64_t nextOffset = pRing1Writer ? stats.ring1Records
                            : pRing2Writer ? stats.ring2Records
                                           : stats.ring34Records;

        /* CellsInUseRec's (pattern, offset) shape is bit-identical to
        ** UINT64_PAIR, so this goes through the same compressed RSFWriter
        ** the flat store format uses -- see file Notes.
        */
        UINT64_PAIR rec{ key.ullCellsInUse, nextOffset };
        RSFWriterRecord(pCellsInUseWriter, &rec);
        stats.cellsInUseRecords++;

        curPattern  = key.ullCellsInUse;
        havePattern = true;
    }

    if (pRing1Writer && (!haveRing1Group || ring1Pattern != curRing1Pattern))
    {
        CloseRing1Group();
        curRing1Pattern      = ring1Pattern;
        ring1GroupRing2Start = stats.ring2Records;
        haveRing1Group       = true;
    }

    if (pRing2Writer && (!haveRing2Group || ring2Pattern != curRing2Pattern))
    {
        CloseRing2Group();
        curRing2Pattern       = ring2Pattern;
        ring2GroupRing34Start = stats.ring34Records;
        haveRing2Group        = true;
    }

    if (!haveRing34Group || ring34Pattern != curRing34Pattern)
    {
        CloseRing34Group();
        curRing34Pattern = ring34Pattern;
        ring34GroupCount = 0;
        haveRing34Group  = true;
    }
    ring34GroupCount++;
    stats.totalBoards++;
}

/*
** Method: RingNestedIndexBuilder::Finish
** @brief  Flushes any still-open groups. Call once after the last Process().
*/
void RingNestedIndexBuilder::Finish()
{
    CloseRing1Group();
}

/*
** Function: readRingLevelViaRSF
** @brief    Reads a Ring_1/Ring_2 file (RSF_SHAPE_RING_LEVEL, via
**           RSFOpenShaped/RSFReadShaped -- see file Notes) fully into pOut.
** @param    path - path to the Ring_1 or Ring_2 file
** @param    pOut - out: filled with every RingLevelRec in the file
** @return   true if the file opened and read successfully.
*/
static bool readRingLevelViaRSF(const char* path, std::vector<RingLevelRec>* pOut)
{
    RSFReader* r = RSFOpenShaped(path, RSF_SHAPE_RING_LEVEL);
    if (!r) return false;

    pOut->resize((size_t)RSFReaderTrailer(r)->recordCount);

    size_t i = 0;
    while (i < pOut->size() && RSFReadShaped(r, &(*pOut)[i], 1) == 1)
        i++;
    RSFClose(&r);
    return i == pOut->size();
}

/*
** Function: readRing34ViaRSF
** @brief    Reads the Ring_3_4 file (RSF_SHAPE_LEAF16, via
**           RSFOpenShaped/RSFReadShaped -- see file Notes) fully into pOut.
** @param    path - path to the Ring_3_4 file
** @param    pOut - out: filled with every Ring34Rec in the file
** @return   true if the file opened and read successfully.
*/
static bool readRing34ViaRSF(const char* path, std::vector<Ring34Rec>* pOut)
{
    RSFReader* r = RSFOpenShaped(path, RSF_SHAPE_LEAF16);
    if (!r) return false;

    pOut->resize((size_t)RSFReaderTrailer(r)->recordCount);

    size_t i = 0;
    while (i < pOut->size() && RSFReadShaped(r, &(*pOut)[i], 1) == 1)
        i++;
    RSFClose(&r);
    return i == pOut->size();
}

/*
** Function: readCellsInUseViaRSF
** @brief    Reads the CellsInUse file (compressed via RSFWriter/RSFReader --
**           see file Notes) fully into pOut.
** @param    path - path to the CellsInUse file
** @param    pOut - out: filled with every CellsInUseRec in the file
** @return   true if the file opened and read successfully.
*/
static bool readCellsInUseViaRSF(const char* path, std::vector<CellsInUseRec>* pOut)
{
    RSFReader* r = RSFOpen(path);
    if (!r) return false;

    pOut->resize((size_t)RSFReaderTrailer(r)->recordCount);

    UINT64_PAIR rec;
    size_t      i = 0;
    while (i < pOut->size() && RSFRead(r, &rec, 1) == 1)
    {
        (*pOut)[i].pattern = rec.hi;
        (*pOut)[i].offset  = rec.lo;
        i++;
    }
    RSFClose(&r);
    return i == pOut->size();
}

/*
** Method: RingNestedIndexReader::Load
** @brief  Reads the applicable nested index files fully into memory.
** @param  cellsInUsePath - path to the CellsInUse file
** @param  ring1Path      - path to the Ring_1 file, or nullptr if not applicable for this board size
** @param  ring2Path      - path to the Ring_2 file, or nullptr if not applicable for this board size
** @param  ring34Path     - path to the Ring_3_4 file
** @return true if every applicable file was read successfully.
*/
bool RingNestedIndexReader::Load(const char* cellsInUsePath, const char* ring1Path, const char* ring2Path, const char* ring34Path)
{
    hasRing1 = (ring1Path != nullptr);
    hasRing2 = (ring2Path != nullptr);

    if (!readCellsInUseViaRSF(cellsInUsePath, &cellsInUse)) return false;
    if (hasRing1 && !readRingLevelViaRSF(ring1Path, &ring1)) return false;
    if (hasRing2 && !readRingLevelViaRSF(ring2Path, &ring2)) return false;
    return readRing34ViaRSF(ring34Path, &ring34);
}

/*
** Method: RingNestedIndexReader::GetBoardCount
** @brief  Returns the total number of boards represented by the loaded index.
** @return ring34.size() (one entry per board -- see RingNestedIndex.h Notes).
*/
uint64_t RingNestedIndexReader::GetBoardCount() const
{
    return (uint64_t)ring34.size();
}

/*
** Method: RingNestedIndexReader::ExpandAll
** @brief  Walks the nested index and calls onBoard once per board, in the
**         same sorted order the index was built from.
** @details Picks one of three walk shapes up front based on hasRing1/
**          hasRing2 (set by Load()), rather than branching per board --
**          CellsInUse[i].offset indexes directly into whichever level is
**          actually the next one stored for this board size (Ring_1
**          normally, Ring_2 or Ring_3_4 directly when the outer level(s)
**          are skipped -- see RingNestedIndexHasRing1/HasRing2).
** @param  onBoard - called once per board with the reconstructed ring-ordered BOARD_KEY
*/
void RingNestedIndexReader::ExpandAll(const std::function<void(const BOARD_KEY& key)>& onBoard) const
{
    size_t numCellsInUse = cellsInUse.size();

    if (hasRing1)
    {
        /* Full 4-level walk: CellsInUse -> Ring_1 -> Ring_2 -> Ring_3_4 (8x8).
        ** None of CellsInUse/Ring_1/Ring_2 carry a count field -- each
        ** group's span runs until the next record's offset in that SAME
        ** flat stream (offsets are monotonic across the whole stream, not
        ** just within one parent group), or to the end of the next stored
        ** level for the very last record -- see file Notes.
        */
        for (size_t i = 0; i < numCellsInUse; i++)
        {
            uint64_t ring1Begin = cellsInUse[i].offset;
            uint64_t ring1End   = (i + 1 < numCellsInUse) ? cellsInUse[i + 1].offset : (uint64_t)ring1.size();

            for (uint64_t j = ring1Begin; j < ring1End; j++)
            {
                uint64_t ring2Begin = ring1[j].offset;
                uint64_t ring2End   = (j + 1 < ring1.size()) ? ring1[j + 1].offset : (uint64_t)ring2.size();

                for (uint64_t k = ring2Begin; k < ring2End; k++)
                {
                    uint64_t ring34Begin = ring2[k].offset;
                    uint64_t ring34End   = (k + 1 < ring2.size()) ? ring2[k + 1].offset : (uint64_t)ring34.size();

                    for (uint64_t m = ring34Begin; m < ring34End; m++)
                    {
                        BOARD_KEY key;
                        key.ullCellsInUse = cellsInUse[i].pattern;
                        key.ullCellColors = ((uint64_t)ring1[j].pattern  << RING1_SHIFT)
                                          | ((uint64_t)ring2[k].pattern  << RING2_SHIFT)
                                          | ((uint64_t)ring34[m].pattern << RING34_SHIFT);
                        onBoard(key);
                    }
                }
            }
        }
    }
    else if (hasRing2)
    {
        /* 3-level walk: CellsInUse -> Ring_2 -> Ring_3_4 (6x6 -- Ring_1 is
        ** always empty at this board size, see file Notes, so it isn't
        ** stored at all and CellsInUse's offset indexes Ring_2 directly).
        */
        for (size_t i = 0; i < numCellsInUse; i++)
        {
            uint64_t ring2Begin = cellsInUse[i].offset;
            uint64_t ring2End   = (i + 1 < numCellsInUse) ? cellsInUse[i + 1].offset : (uint64_t)ring2.size();

            for (uint64_t k = ring2Begin; k < ring2End; k++)
            {
                uint64_t ring34Begin = ring2[k].offset;
                uint64_t ring34End   = (k + 1 < ring2.size()) ? ring2[k + 1].offset : (uint64_t)ring34.size();

                for (uint64_t m = ring34Begin; m < ring34End; m++)
                {
                    BOARD_KEY key;
                    key.ullCellsInUse = cellsInUse[i].pattern;
                    key.ullCellColors = ((uint64_t)ring2[k].pattern  << RING2_SHIFT)
                                      | ((uint64_t)ring34[m].pattern << RING34_SHIFT);
                    onBoard(key);
                }
            }
        }
    }
    else
    {
        /* 2-level walk: CellsInUse -> Ring_3_4 directly (4x4 -- both Ring_1
        ** and Ring_2 are always empty at this board size).
        */
        for (size_t i = 0; i < numCellsInUse; i++)
        {
            uint64_t ring34Begin = cellsInUse[i].offset;
            uint64_t ring34End   = (i + 1 < numCellsInUse) ? cellsInUse[i + 1].offset : (uint64_t)ring34.size();

            for (uint64_t m = ring34Begin; m < ring34End; m++)
            {
                BOARD_KEY key;
                key.ullCellsInUse = cellsInUse[i].pattern;
                key.ullCellColors = (uint64_t)ring34[m].pattern << RING34_SHIFT;
                onBoard(key);
            }
        }
    }
}

/*
** Function: RingNestedIndexStreamAll
** @brief    See RingNestedIndex.h.
*/
bool RingNestedIndexStreamAll(const char* cellsInUsePath, const char* ring1Path, const char* ring2Path,
                              const char* ring34Path, const std::function<void(const BOARD_KEY& key)>& onBoard,
                              const volatile bool* pTerminate)
{
    bool hasRing1 = (ring1Path != nullptr);
    bool hasRing2 = (ring2Path != nullptr);

    RSFReader* pCellsInUse = RSFOpen(cellsInUsePath);
    if (!pCellsInUse) return false;

    RSFReader* pRing1  = hasRing1 ? RSFOpenShaped(ring1Path, RSF_SHAPE_RING_LEVEL) : nullptr;
    RSFReader* pRing2  = hasRing2 ? RSFOpenShaped(ring2Path, RSF_SHAPE_RING_LEVEL) : nullptr;
    RSFReader* pRing34 = RSFOpenShaped(ring34Path, RSF_SHAPE_LEAF16);

    if ((hasRing1 && !pRing1) || (hasRing2 && !pRing2) || !pRing34)
    {
        RSFClose(&pCellsInUse);
        if (pRing1)  RSFClose(&pRing1);
        if (pRing2)  RSFClose(&pRing2);
        if (pRing34) RSFClose(&pRing34);
        return false;
    }

    bool ok = true;

    /* Separate from 'ok': ok means "clean data, no truncation/corruption";
    ** terminated means "caller asked us to stop early via pTerminate," a
    ** completely different, non-error reason to unwind. Kept as its own
    ** flag (checked alongside ok in every one of the four onBoard loops
    ** below, all of which already share 'ok' as a single cascading
    ** continue-condition) so a caller-requested stop still returns true
    ** (not corrupted) -- see the header comment for the full contract.
    */
    bool terminated = false;

    /* One-record lookahead on CellsInUse, and (since neither carries a
    ** count field any more -- see file Notes) on Ring_1/Ring_2 too. Each
    ** is a single flat stream spanning the WHOLE level, not per-parent-
    ** group arrays, so these lookahead buffers are hoisted here rather
    ** than reset per parent group -- they must survive across CellsInUse/
    ** Ring_1 group boundaries exactly like CellsInUse's own lookahead
    ** already survives across levels.
    */
    UINT64_PAIR curCells{}, nextCells{};
    bool haveCurCells  = (RSFRead(pCellsInUse, &curCells, 1) == 1);
    bool haveNextCells = haveCurCells && (RSFRead(pCellsInUse, &nextCells, 1) == 1);

    RingLevelRec curRing1{}, nextRing1{};
    bool haveCurRing1  = hasRing1 && (RSFReadShaped(pRing1, &curRing1, 1) == 1);
    bool haveNextRing1 = haveCurRing1 && (RSFReadShaped(pRing1, &nextRing1, 1) == 1);

    RingLevelRec curRing2{}, nextRing2{};
    bool haveCurRing2  = hasRing2 && (RSFReadShaped(pRing2, &curRing2, 1) == 1);
    bool haveNextRing2 = haveCurRing2 && (RSFReadShaped(pRing2, &nextRing2, 1) == 1);

    while (haveCurCells && ok && !terminated)
    {
        uint64_t pattern = curCells.hi;

        /* This group's span in the next stored level: an exact count when
        ** a next CellsInUse record exists (nextCells.lo - curCells.lo);
        ** for the LAST group, "consume until that level's stream hits
        ** EOF" -- UINT64_MAX as a loop bound lets the EOF check do the work.
        */
        uint64_t groupSpan = haveNextCells ? (nextCells.lo - curCells.lo) : UINT64_MAX;

        if (hasRing1)
        {
            for (uint64_t r1 = 0; ok && !terminated && r1 < groupSpan; r1++)
            {
                if (!haveCurRing1) { if (haveNextCells) ok = false; break; }

                uint32_t ring1Pattern = curRing1.pattern;
                uint64_t ring2Span    = haveNextRing1 ? (nextRing1.offset - curRing1.offset) : UINT64_MAX;

                if (hasRing2)
                {
                    for (uint64_t r2 = 0; ok && !terminated && r2 < ring2Span; r2++)
                    {
                        if (!haveCurRing2) { if (haveNextRing1) ok = false; break; }

                        uint32_t ring2Pattern = curRing2.pattern;
                        uint64_t ring34Span   = haveNextRing2 ? (nextRing2.offset - curRing2.offset) : UINT64_MAX;

                        for (uint64_t r34 = 0; ok && !terminated && r34 < ring34Span; r34++)
                        {
                            if (pTerminate && *pTerminate) { terminated = true; break; }

                            Ring34Rec ring34Rec;
                            if (RSFReadShaped(pRing34, &ring34Rec, 1) != 1) { if (haveNextRing2) ok = false; break; }

                            BOARD_KEY key;
                            key.ullCellsInUse = pattern;
                            key.ullCellColors = ((uint64_t)ring1Pattern << RING1_SHIFT)
                                              | ((uint64_t)ring2Pattern << RING2_SHIFT)
                                              | ((uint64_t)ring34Rec.pattern << RING34_SHIFT);
                            onBoard(key);
                        }

                        curRing2      = nextRing2;
                        haveCurRing2  = haveNextRing2;
                        haveNextRing2 = haveCurRing2 && (RSFReadShaped(pRing2, &nextRing2, 1) == 1);
                    }
                }
                else
                {
                    /* hasRing1 true but hasRing2 false never occurs for
                    ** any board size this project supports (8x8 implies
                    ** 6x6's Ring_2 too), but handled correctly anyway:
                    ** Ring_1's own offset points directly into Ring_3_4, so
                    ** ring2Span here is really "how many Ring_3_4 records
                    ** belong to this Ring_1 group."
                    */
                    for (uint64_t r34 = 0; ok && !terminated && r34 < ring2Span; r34++)
                    {
                        if (pTerminate && *pTerminate) { terminated = true; break; }

                        Ring34Rec ring34Rec;
                        if (RSFReadShaped(pRing34, &ring34Rec, 1) != 1) { if (haveNextRing1) ok = false; break; }

                        BOARD_KEY key;
                        key.ullCellsInUse = pattern;
                        key.ullCellColors = ((uint64_t)ring1Pattern << RING1_SHIFT)
                                          | ((uint64_t)ring34Rec.pattern << RING34_SHIFT);
                        onBoard(key);
                    }
                }

                curRing1      = nextRing1;
                haveCurRing1  = haveNextRing1;
                haveNextRing1 = haveCurRing1 && (RSFReadShaped(pRing1, &nextRing1, 1) == 1);
            }
        }
        else if (hasRing2)
        {
            for (uint64_t r2 = 0; ok && !terminated && r2 < groupSpan; r2++)
            {
                if (!haveCurRing2) { if (haveNextCells) ok = false; break; }

                uint32_t ring2Pattern = curRing2.pattern;
                uint64_t ring34Span   = haveNextRing2 ? (nextRing2.offset - curRing2.offset) : UINT64_MAX;

                for (uint64_t r34 = 0; ok && !terminated && r34 < ring34Span; r34++)
                {
                    if (pTerminate && *pTerminate) { terminated = true; break; }

                    Ring34Rec ring34Rec;
                    if (RSFReadShaped(pRing34, &ring34Rec, 1) != 1) { if (haveNextRing2) ok = false; break; }

                    BOARD_KEY key;
                    key.ullCellsInUse = pattern;
                    key.ullCellColors = ((uint64_t)ring2Pattern << RING2_SHIFT)
                                      | ((uint64_t)ring34Rec.pattern << RING34_SHIFT);
                    onBoard(key);
                }

                curRing2      = nextRing2;
                haveCurRing2  = haveNextRing2;
                haveNextRing2 = haveCurRing2 && (RSFReadShaped(pRing2, &nextRing2, 1) == 1);
            }
        }
        else
        {
            for (uint64_t r34 = 0; ok && !terminated && r34 < groupSpan; r34++)
            {
                if (pTerminate && *pTerminate) { terminated = true; break; }

                Ring34Rec ring34Rec;
                if (RSFReadShaped(pRing34, &ring34Rec, 1) != 1) { if (haveNextCells) ok = false; break; }

                BOARD_KEY key;
                key.ullCellsInUse = pattern;
                key.ullCellColors = (uint64_t)ring34Rec.pattern << RING34_SHIFT;
                onBoard(key);
            }
        }

        curCells      = nextCells;
        haveCurCells  = haveNextCells;
        haveNextCells = haveCurCells && (RSFRead(pCellsInUse, &nextCells, 1) == 1);
    }

    RSFClose(&pCellsInUse);
    if (pRing1) RSFClose(&pRing1);
    if (pRing2) RSFClose(&pRing2);
    RSFClose(&pRing34);

    return ok;
}

/*
** ============================================================
** RingNestedIndexPullReader
** ============================================================
*/

/*
** Method: RingNestedIndexPullReader::Open
** @brief  See RingNestedIndex.h.
*/
bool RingNestedIndexPullReader::Open(const char* cellsInUsePath, const char* ring1Path,
                                      const char* ring2Path, const char* ring34Path)
{
    hasRing1 = (ring1Path != nullptr);
    hasRing2 = (ring2Path != nullptr);

    pCellsInUse = RSFOpen(cellsInUsePath);
    if (!pCellsInUse) return false;

    pRing1  = hasRing1 ? RSFOpenShaped(ring1Path, RSF_SHAPE_RING_LEVEL) : nullptr;
    pRing2  = hasRing2 ? RSFOpenShaped(ring2Path, RSF_SHAPE_RING_LEVEL) : nullptr;
    pRing34 = RSFOpenShaped(ring34Path, RSF_SHAPE_LEAF16);

    if ((hasRing1 && !pRing1) || (hasRing2 && !pRing2) || !pRing34)
    {
        Close();
        return false;
    }

    haveCurCells  = (RSFRead(pCellsInUse, &curCells, 1) == 1);
    haveNextCells = haveCurCells && (RSFRead(pCellsInUse, &nextCells, 1) == 1);

    haveCurRing1  = hasRing1 && (RSFReadShaped(pRing1, &curRing1, 1) == 1);
    haveNextRing1 = haveCurRing1 && (RSFReadShaped(pRing1, &nextRing1, 1) == 1);

    haveCurRing2  = hasRing2 && (RSFReadShaped(pRing2, &curRing2, 1) == 1);
    haveNextRing2 = haveCurRing2 && (RSFReadShaped(pRing2, &nextRing2, 1) == 1);

    haveCurRing34 = (RSFReadShaped(pRing34, &curRing34, 1) == 1);

    FillCurrent();
    return true;
}

/*
** Method: RingNestedIndexPullReader::FillCurrent
** @brief  Computes currentBoard from the current cursor position
**         (curCells/curRing1/curRing2/curRing34), or clears haveCurrent if
**         nothing is available. Private helper shared by Open()/Advance().
** @return haveCurrent's new value, for the callers' convenience.
*/
bool RingNestedIndexPullReader::FillCurrent()
{
    if (!haveCurCells || !haveCurRing34) { haveCurrent = false; return false; }

    currentBoard.ullCellsInUse = curCells.hi;
    uint64_t colors = (uint64_t)curRing34.pattern << RING34_SHIFT;
    if (hasRing1) colors |= (uint64_t)curRing1.pattern << RING1_SHIFT;
    if (hasRing2) colors |= (uint64_t)curRing2.pattern << RING2_SHIFT;
    currentBoard.ullCellColors = colors;

    haveCurrent = true;
    return true;
}

/*
** Method: RingNestedIndexPullReader::Peek
** @brief  See RingNestedIndex.h.
*/
bool RingNestedIndexPullReader::Peek(BOARD_KEY* pOutKey) const
{
    if (!haveCurrent) return false;
    *pOutKey = currentBoard;
    return true;
}

/*
** Method: RingNestedIndexPullReader::Advance
** @brief  See RingNestedIndex.h.
** @details Consumes the current Ring_3_4 record (always -- it's the leaf,
**          one per board), then cascades upward exactly like the builder's
**          own CloseRing1Group/CloseRing2Group cascade, but in reverse:
**          reaching a level's next-record offset (the same boundary value
**          the builder wrote) advances that level's cursor and, if that
**          level itself just crossed ITS OWN parent's boundary, cascades
**          one level further up. A level with no next record (haveNextX
**          false -- the last group at that level) never reaches its
**          (infinite) boundary via count comparison; it only ends when the
**          underlying file's own EOF is hit, exactly like StreamAll.
*/
bool RingNestedIndexPullReader::Advance()
{
    if (!haveCurrent) return false;

    ring34Count++;
    haveCurRing34 = (RSFReadShaped(pRing34, &curRing34, 1) == 1);

    if (!haveCurRing34)
    {
        /* Ring_3_4 (the leaf, read every Advance() call) has run out.
        ** Legitimate only if this is genuinely the last group at every
        ** applicable level -- nothing anywhere still promises more boards.
        ** Otherwise the data is truncated/corrupt (same reasoning
        ** RingNestedIndexStreamAll's own "if (haveNext) ok = false" checks
        ** apply, just consolidated to the one place it can actually happen
        ** here: every higher-level cascade below only ever fires when its
        ** own haveNextX is already true, so it can never itself be the
        ** source of a truncation -- only running out of leaf records can).
        */
        bool trulyDone = !haveNextCells
                       && (!hasRing1 || !haveNextRing1)
                       && (!hasRing2 || !haveNextRing2);
        if (!trulyDone) corrupted = true;
        haveCurrent = false;
        return false;
    }

    /* Cascade the read cursors upward wherever ring34Count has now reached
    ** the next-record boundary the builder itself wrote at that level --
    ** mirrors CloseRing1Group/CloseRing2Group's own cascade, in reverse.
    ** Each check only ever fires when that level's own haveNextX is
    ** already true, so it can never spuriously trigger past real data.
    */
    if (hasRing2 && haveNextRing2 && ring34Count >= nextRing2.offset)
    {
        ring2Count++;
        curRing2      = nextRing2;
        haveCurRing2  = haveNextRing2;
        haveNextRing2 = haveCurRing2 && (RSFReadShaped(pRing2, &nextRing2, 1) == 1);

        if (hasRing1)
        {
            if (haveNextRing1 && ring2Count >= nextRing1.offset)
            {
                ring1Count++;
                curRing1      = nextRing1;
                haveCurRing1  = haveNextRing1;
                haveNextRing1 = haveCurRing1 && (RSFReadShaped(pRing1, &nextRing1, 1) == 1);

                if (haveNextCells && ring1Count >= nextCells.lo)
                {
                    curCells      = nextCells;
                    haveCurCells  = haveNextCells;
                    haveNextCells = haveCurCells && (RSFRead(pCellsInUse, &nextCells, 1) == 1);
                }
            }
        }
        else if (haveNextCells && ring2Count >= nextCells.lo)
        {
            curCells      = nextCells;
            haveCurCells  = haveNextCells;
            haveNextCells = haveCurCells && (RSFRead(pCellsInUse, &nextCells, 1) == 1);
        }
    }
    else if (!hasRing2 && hasRing1 && haveNextRing1 && ring34Count >= nextRing1.offset)
    {
        ring1Count++;
        curRing1      = nextRing1;
        haveCurRing1  = haveNextRing1;
        haveNextRing1 = haveCurRing1 && (RSFReadShaped(pRing1, &nextRing1, 1) == 1);

        if (haveNextCells && ring1Count >= nextCells.lo)
        {
            curCells      = nextCells;
            haveCurCells  = haveNextCells;
            haveNextCells = haveCurCells && (RSFRead(pCellsInUse, &nextCells, 1) == 1);
        }
    }
    else if (!hasRing2 && !hasRing1 && haveNextCells && ring34Count >= nextCells.lo)
    {
        curCells      = nextCells;
        haveCurCells  = haveNextCells;
        haveNextCells = haveCurCells && (RSFRead(pCellsInUse, &nextCells, 1) == 1);
    }

    return FillCurrent();
}

/*
** Method: RingNestedIndexPullReader::Close
** @brief  See RingNestedIndex.h.
*/
void RingNestedIndexPullReader::Close()
{
    if (pCellsInUse) RSFClose(&pCellsInUse);
    if (pRing1)      RSFClose(&pRing1);
    if (pRing2)      RSFClose(&pRing2);
    if (pRing34)     RSFClose(&pRing34);
    haveCurrent = false;
}

/*
** Function: BinarySearchPattern
** @brief    Binary searches vec[lo, hi) for an element whose pattern field
**           equals target.
** @param    vec    - vector to search (elements must expose a `pattern` field)
** @param    lo     - inclusive start of the sub-range to search
** @param    hi     - exclusive end of the sub-range to search
** @param    target - pattern value to find
** @param    pOutIdx - out: the matching index, if found
** @return   true if a match was found within [lo, hi).
*/
template <typename T>
static bool BinarySearchPattern(const std::vector<T>& vec, uint64_t lo, uint64_t hi,
                                 decltype(T::pattern) target, uint64_t* pOutIdx)
{
    uint64_t searchEnd = hi;

    while (lo < hi)
    {
        uint64_t mid = lo + (hi - lo) / 2;
        if (vec[mid].pattern < target) lo = mid + 1;
        else                           hi = mid;
    }

    if (lo < searchEnd && vec[lo].pattern == target)
    {
        *pOutIdx = lo;
        return true;
    }
    return false;
}

/*
** Method: RingNestedIndexReader::FindBoardPosition
** @brief  See RingNestedIndex.h.
*/
bool RingNestedIndexReader::FindBoardPosition(const BOARD_KEY& key, uint64_t* pOutPosition) const
{
    uint32_t ring1Pattern  = (uint32_t)((key.ullCellColors >> RING1_SHIFT)  & ((1u << RING1_BITS) - 1));
    uint32_t ring2Pattern  = (uint32_t)((key.ullCellColors >> RING2_SHIFT)  & ((1u << RING2_BITS) - 1));
    uint16_t ring34Pattern = (uint16_t)((key.ullCellColors >> RING34_SHIFT) & ((1u << RING34_BITS) - 1));

    size_t numCellsInUse = cellsInUse.size();

    uint64_t i;
    if (!BinarySearchPattern(cellsInUse, 0, numCellsInUse, key.ullCellsInUse, &i))
        return false;

    uint64_t begin, end;

    if (hasRing1)
    {
        begin = cellsInUse[i].offset;
        end   = (i + 1 < numCellsInUse) ? cellsInUse[i + 1].offset : (uint64_t)ring1.size();

        uint64_t j;
        if (!BinarySearchPattern(ring1, begin, end, ring1Pattern, &j))
            return false;

        /* No count field -- span runs to the next Ring_1 record's offset
        ** in this same flat stream, or to the end of Ring_2 for the last one.
        */
        begin = ring1[j].offset;
        end   = (j + 1 < ring1.size()) ? ring1[j + 1].offset : (uint64_t)ring2.size();
    }
    else
    {
        begin = cellsInUse[i].offset;
        end   = (i + 1 < numCellsInUse) ? cellsInUse[i + 1].offset
              : hasRing2               ? (uint64_t)ring2.size()
                                        : (uint64_t)ring34.size();
    }

    if (hasRing2)
    {
        uint64_t k;
        if (!BinarySearchPattern(ring2, begin, end, ring2Pattern, &k))
            return false;

        /* No count field -- span runs to the next Ring_2 record's offset
        ** in this same flat stream, or to the end of Ring_3_4 for the last one.
        */
        begin = ring2[k].offset;
        end   = (k + 1 < ring2.size()) ? ring2[k + 1].offset : (uint64_t)ring34.size();
    }

    uint64_t m;
    if (!BinarySearchPattern(ring34, begin, end, ring34Pattern, &m))
        return false;

    *pOutPosition = m;
    return true;
}

/*
** Function: RingNestedIndexFileCount
** @brief    Counts how many of the applicable nested-index files exist on disk.
** @param    cellsInUsePath - path to the CellsInUse file
** @param    ring1Path      - path to the Ring_1 file, or nullptr if not applicable for this board size
** @param    ring2Path      - path to the Ring_2 file, or nullptr if not applicable for this board size
** @param    ring34Path     - path to the Ring_3_4 file
** @return   0 if none of the applicable files exist; up to the number of non-null paths otherwise.
*/
int RingNestedIndexFileCount(const char* cellsInUsePath, const char* ring1Path,
                              const char* ring2Path, const char* ring34Path)
{
    const char* paths[4] = { cellsInUsePath, ring1Path, ring2Path, ring34Path };
    int         count    = 0;

    for (int i = 0; i < 4; i++)
        if (paths[i] && GetFileAttributesA(paths[i]) != INVALID_FILE_ATTRIBUTES)
            count++;

    return count;
}
