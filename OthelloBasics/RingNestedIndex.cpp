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
** @param  pRing1WriterIn      - already-open Lz4StreamWriter for the Ring_1 output, or nullptr if not applicable
** @param  pRing2WriterIn      - already-open Lz4StreamWriter for the Ring_2 output, or nullptr if not applicable
** @param  pRing34WriterIn     - already-open Lz4StreamWriter for the Ring_3_4 output (always required)
*/
void RingNestedIndexBuilder::Init(RSFWriter* pCellsInUseWriterIn, Lz4StreamWriter* pRing1WriterIn,
                                   Lz4StreamWriter* pRing2WriterIn, Lz4StreamWriter* pRing34WriterIn)
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
    Lz4StreamWriterWrite(pRing34Writer, &rec, sizeof(rec));
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
**         have been set true in the first place.
*/
void RingNestedIndexBuilder::CloseRing2Group()
{
    CloseRing34Group();
    if (!pRing2Writer || !haveRing2Group) return;

    uint64_t      count = stats.ring34Records - ring2GroupRing34Start;
    RingLevelRec  rec{ count, curRing2Pattern, ring2GroupRing34Start };
    Lz4StreamWriterWrite(pRing2Writer, &rec, sizeof(rec));
    stats.ring2Records++;
    haveRing2Group = false;
}

/*
** Method: RingNestedIndexBuilder::CloseRing1Group
** @brief  Closes the current Ring_2 group (which cascades to Ring_3_4),
**         then writes the current Ring_1 group's record and closes it --
**         unless pRing1Writer is null (see CloseRing2Group's comment; same
**         reasoning applies one level up).
*/
void RingNestedIndexBuilder::CloseRing1Group()
{
    CloseRing2Group();
    if (!pRing1Writer || !haveRing1Group) return;

    uint64_t      count = stats.ring2Records - ring1GroupRing2Start;
    RingLevelRec  rec{ count, curRing1Pattern, ring1GroupRing2Start };
    Lz4StreamWriterWrite(pRing1Writer, &rec, sizeof(rec));
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
** Function: readStreamedRecords
** @brief    Reads every fixed-size T record from an Lz4Stream-compressed
**           file into pOut, appending until end of stream.
** @param    path - file path to read
** @param    pOut - out: filled with every record in the file
** @return   true if the file opened and every record was whole (no
**           truncated/corrupt trailing partial record).
*/
template <typename T>
static bool readStreamedRecords(const char* path, std::vector<T>* pOut)
{
    Lz4StreamReader* r = Lz4StreamReaderOpen(path);
    if (!r) return false;

    bool ok = true;
    for (;;)
    {
        T      rec;
        size_t got = Lz4StreamReaderRead(r, &rec, sizeof(rec));
        if (got == 0) break;                       /* clean end of stream */
        if (got != sizeof(rec)) { ok = false; break; }   /* truncated mid-record */
        pOut->push_back(rec);
    }

    Lz4StreamReaderClose(&r);
    return ok;
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
    if (hasRing1 && !readStreamedRecords(ring1Path, &ring1)) return false;
    if (hasRing2 && !readStreamedRecords(ring2Path, &ring2)) return false;
    return readStreamedRecords(ring34Path, &ring34);
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
        /* Full 4-level walk: CellsInUse -> Ring_1 -> Ring_2 -> Ring_3_4 (8x8). */
        for (size_t i = 0; i < numCellsInUse; i++)
        {
            /* CellsInUseRec has no count field -- a group's span runs until
            ** the next entry's offset (or the end of Ring_1, for the last entry).
            */
            uint64_t ring1Begin = cellsInUse[i].offset;
            uint64_t ring1End   = (i + 1 < numCellsInUse) ? cellsInUse[i + 1].offset : (uint64_t)ring1.size();

            for (uint64_t j = ring1Begin; j < ring1End; j++)
            {
                uint64_t ring2Begin = ring1[j].offset;
                uint64_t ring2End   = ring2Begin + ring1[j].count;

                for (uint64_t k = ring2Begin; k < ring2End; k++)
                {
                    uint64_t ring34Begin = ring2[k].offset;
                    uint64_t ring34End   = ring34Begin + ring2[k].count;

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
                uint64_t ring34End   = ring34Begin + ring2[k].count;

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
