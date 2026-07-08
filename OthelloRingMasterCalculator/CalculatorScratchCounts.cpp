/*
** Filename:  CalculatorScratchCounts.cpp
**
** Purpose:
**   Implements ScratchCountsWriter and JoinScratchCountsToFinal, declared
**   in CalculatorScratchCounts.h.
*/

/* Includes */
#include "CalculatorScratchCounts.h"
#include "CalculatorCountsFile.h"
#include <stdio.h>
#include <string.h>

/* Functions */

/*
** Method: ScratchCountsWriter::Init
** @brief  See CalculatorScratchCounts.h.
*/
void ScratchCountsWriter::Init(POthelloRingMasterCalculatorState pState, char excludeDrive1, char excludeDrive2,
                                uint64_t count, int officialByteWidthIn,
                                const char* scratchDirNoDrive, const char* baseName)
{
    officialByteWidth = officialByteWidthIn;
    scratchByteWidth  = (officialByteWidth == COUNTER_WIDTH_NIBBLE) ? 1 : officialByteWidth;

    int      recordSize = 3 * scratchByteWidth;
    int64_t  totalBytes = (int64_t)count * (int64_t)recordSize;

    std::vector<std::pair<char, int64_t>> plan;
    PlanScratchDrives(pState, excludeDrive1, excludeDrive2, totalBytes, baseName, &plan);

    store.Init(std::move(plan), recordSize, /*isKeySorted=*/false, scratchDirNoDrive, baseName);
}

/*
** Method: ScratchCountsWriter::WriteTriple
** @brief  See CalculatorScratchCounts.h.
*/
void ScratchCountsWriter::WriteTriple(const OutcomeTriple& triple)
{
    uint8_t buf[3 * WIDE_COUNTER_MAX_BYTES];
    memcpy(buf,                        triple.black, scratchByteWidth);
    memcpy(buf + scratchByteWidth,      triple.white, scratchByteWidth);
    memcpy(buf + 2 * scratchByteWidth,  triple.tie,   scratchByteWidth);
    store.Write(buf);
}

/*
** Method: ScratchCountsWriter::WriteNibbleTriple
** @brief  See CalculatorScratchCounts.h.
*/
void ScratchCountsWriter::WriteNibbleTriple(const NibbleOutcomeTriple& triple)
{
    uint8_t buf[3] = { triple.black, triple.white, triple.tie };
    store.Write(buf);
}

/*
** Method: ScratchCountsWriter::Finish
** @brief  See CalculatorScratchCounts.h.
*/
void ScratchCountsWriter::Finish()
{
    store.Finish();
}

/*
** Function: JoinScratchCountsToFinal
** @brief    See CalculatorScratchCounts.h.
*/
void JoinScratchCountsToFinal(const SegmentList& segments, int scratchByteWidth, int officialByteWidth,
                              const char* finalPath)
{
    int recordSize = 3 * scratchByteWidth;

    NibbleCountsWriter*     pNibbleWriter = nullptr;
    CalculatorCountsWriter* pWideWriter   = nullptr;
    if (officialByteWidth == COUNTER_WIDTH_NIBBLE) pNibbleWriter = NibbleCountsWriterOpen(finalPath);
    else                                            pWideWriter   = CalculatorCountsWriterOpen(finalPath, officialByteWidth);

    uint8_t buf[3 * WIDE_COUNTER_MAX_BYTES];

    for (const SegmentInfo& seg : segments)
    {
        FILE* f = fopen(seg.path, "rb");
        if (!f)
            Fatal(FATAL_FILE_OPEN, "JoinScratchCountsToFinal: cannot open segment '%s'", seg.path);

        for (uint64_t i = 0; i < seg.recordCount; i++)
        {
            if (fread(buf, (size_t)recordSize, 1, f) != 1)
                Fatal(FATAL_READ_FAILED, "JoinScratchCountsToFinal: truncated segment '%s' (record %llu of %llu)",
                      seg.path, (unsigned long long)i, (unsigned long long)seg.recordCount);

            if (officialByteWidth == COUNTER_WIDTH_NIBBLE)
            {
                NibbleOutcomeTriple t{ buf[0], buf[1], buf[2] };
                NibbleCountsWriterWrite(pNibbleWriter, &t);
            }
            else
            {
                OutcomeTriple t;
                OutcomeTripleSetZero(&t, officialByteWidth);
                memcpy(t.black, buf,                        scratchByteWidth);
                memcpy(t.white, buf + scratchByteWidth,      scratchByteWidth);
                memcpy(t.tie,   buf + 2 * scratchByteWidth,  scratchByteWidth);
                CalculatorCountsWriterWrite(pWideWriter, &t);
            }
        }

        fclose(f);
    }

    if (pNibbleWriter) NibbleCountsWriterClose(pNibbleWriter);
    if (pWideWriter)   CalculatorCountsWriterClose(pWideWriter);
}
