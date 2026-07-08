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

/* Functions */

/*
** Method: RingNestedIndexBuilder::Init
** @brief  Attaches the four already-open output files this builder writes to.
** @param  fpCellsInUseIn - CellsInUse output file
** @param  fpRing1In      - Ring_1 output file
** @param  fpRing2In      - Ring_2 output file
** @param  fpRing34In     - Ring_3_4 output file
*/
void RingNestedIndexBuilder::Init(FILE* fpCellsInUseIn, FILE* fpRing1In, FILE* fpRing2In, FILE* fpRing34In)
{
    fpCellsInUse = fpCellsInUseIn;
    fpRing1      = fpRing1In;
    fpRing2      = fpRing2In;
    fpRing34     = fpRing34In;
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
    fwrite(&rec, sizeof(rec), 1, fpRing34);
    if (ring34GroupCount != 1)
        stats.ring34GroupsWithCountNot1++;
    stats.ring34Records++;
    haveRing34Group = false;
}

/*
** Method: RingNestedIndexBuilder::CloseRing2Group
** @brief  Closes the current Ring_3_4 group, then writes the current
**         Ring_2 group's record and closes it.
*/
void RingNestedIndexBuilder::CloseRing2Group()
{
    CloseRing34Group();
    if (!haveRing2Group) return;

    uint64_t      count = stats.ring34Records - ring2GroupRing34Start;
    RingLevelRec  rec{ count, curRing2Pattern, ring2GroupRing34Start };
    fwrite(&rec, sizeof(rec), 1, fpRing2);
    stats.ring2Records++;
    haveRing2Group = false;
}

/*
** Method: RingNestedIndexBuilder::CloseRing1Group
** @brief  Closes the current Ring_2 group (which cascades to Ring_3_4),
**         then writes the current Ring_1 group's record and closes it.
*/
void RingNestedIndexBuilder::CloseRing1Group()
{
    CloseRing2Group();
    if (!haveRing1Group) return;

    uint64_t      count = stats.ring2Records - ring1GroupRing2Start;
    RingLevelRec  rec{ count, curRing1Pattern, ring1GroupRing2Start };
    fwrite(&rec, sizeof(rec), 1, fpRing1);
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

    /* A new occupancy pattern closes every open group below it (cascading
    ** through CloseRing1Group), then starts a new CellsInUse entry pointing
    ** at wherever the next Ring_1 record will land.
    */
    if (!havePattern || key.ullCellsInUse != curPattern)
    {
        CloseRing1Group();

        CellsInUseRec rec{ key.ullCellsInUse, stats.ring1Records };
        fwrite(&rec, sizeof(rec), 1, fpCellsInUse);
        stats.cellsInUseRecords++;

        curPattern  = key.ullCellsInUse;
        havePattern = true;
    }

    if (!haveRing1Group || ring1Pattern != curRing1Pattern)
    {
        CloseRing1Group();
        curRing1Pattern      = ring1Pattern;
        ring1GroupRing2Start = stats.ring2Records;
        haveRing1Group       = true;
    }

    if (!haveRing2Group || ring2Pattern != curRing2Pattern)
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
** Function: readWholeFile
** @brief    Reads path fully into pOut, resizing to the file's element count.
** @param    path - file path to read
** @param    pOut - out: filled with every record in the file
** @return   true if the file opened and read successfully.
*/
template <typename T>
static bool readWholeFile(const char* path, std::vector<T>* pOut)
{
    FILE* fp = fopen(path, "rb");
    if (!fp)
        return false;

    _fseeki64(fp, 0, SEEK_END);
    int64_t sizeBytes = _ftelli64(fp);
    _fseeki64(fp, 0, SEEK_SET);

    if (sizeBytes < 0 || (sizeBytes % (int64_t)sizeof(T)) != 0)
    {
        fclose(fp);
        return false;
    }

    pOut->resize((size_t)(sizeBytes / (int64_t)sizeof(T)));
    bool ok = pOut->empty() || fread(pOut->data(), sizeof(T), pOut->size(), fp) == pOut->size();

    fclose(fp);
    return ok;
}

/*
** Method: RingNestedIndexReader::Load
** @brief  Reads all four nested index files fully into memory.
** @param  cellsInUsePath - path to the CellsInUse file
** @param  ring1Path      - path to the Ring_1 file
** @param  ring2Path      - path to the Ring_2 file
** @param  ring34Path     - path to the Ring_3_4 file
** @return true if all four files were read successfully.
*/
bool RingNestedIndexReader::Load(const char* cellsInUsePath, const char* ring1Path, const char* ring2Path, const char* ring34Path)
{
    return readWholeFile(cellsInUsePath, &cellsInUse)
        && readWholeFile(ring1Path, &ring1)
        && readWholeFile(ring2Path, &ring2)
        && readWholeFile(ring34Path, &ring34);
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
** @param  onBoard - called once per board with the reconstructed ring-ordered BOARD_KEY
*/
void RingNestedIndexReader::ExpandAll(const std::function<void(const BOARD_KEY& key)>& onBoard) const
{
    size_t numCellsInUse = cellsInUse.size();

    for (size_t i = 0; i < numCellsInUse; i++)
    {
        /* CellsInUseRec has no count field -- a group's span runs until the
        ** next entry's offset (or the end of Ring_1, for the last entry).
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
