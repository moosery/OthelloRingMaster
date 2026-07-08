/*
** Filename:  CalculatorCountsFile.cpp
**
** Purpose:
**   Implements the streamed writer/reader pairs declared in
**   CalculatorCountsFile.h, on top of Utility/Lz4Stream.h.
*/

/* Includes */
#include "CalculatorCountsFile.h"
#include <string.h>

/* Structures and Types */

struct __CalculatorCountsWriter
{
    Lz4StreamWriter* pStream;
    int              byteWidth;
};

struct __CalculatorCountsReader
{
    Lz4StreamReader* pStream;
    int              byteWidth;
};

struct __NibbleCountsWriter
{
    Lz4StreamWriter*    pStream;
    bool                havePending;
    NibbleOutcomeTriple pending;
};

struct __NibbleCountsReader
{
    Lz4StreamReader*    pStream;
    uint64_t            recordCount;
    uint64_t            recordsRead;
    bool                havePending;
    NibbleOutcomeTriple pending;
};

/* Internal Helpers */

/*
** Function: PackNibblePair
** @brief    Packs two boards' nibble triples into 3 bytes: board a's
**           (black,white,tie) nibbles fill byte0 and the low nibble of
**           byte1; board b's fill the high nibble of byte1 and byte2.
** @param    pA  - first board's triple
** @param    pB  - second board's triple
** @param    out - out: exactly 3 packed bytes
*/
static void PackNibblePair(const NibbleOutcomeTriple* pA, const NibbleOutcomeTriple* pB, uint8_t out[3])
{
    out[0] = (uint8_t)((pA->black & 0xF) | ((pA->white & 0xF) << 4));
    out[1] = (uint8_t)((pA->tie   & 0xF) | ((pB->black & 0xF) << 4));
    out[2] = (uint8_t)((pB->white & 0xF) | ((pB->tie   & 0xF) << 4));
}

/*
** Function: UnpackNibblePair
** @brief    Reverses PackNibblePair.
** @param    in  - exactly 3 packed bytes
** @param    pA  - out: first board's triple
** @param    pB  - out: second board's triple (may be zero-padding -- see NibbleCountsReaderRead)
*/
static void UnpackNibblePair(const uint8_t in[3], NibbleOutcomeTriple* pA, NibbleOutcomeTriple* pB)
{
    pA->black = (uint8_t)(in[0] & 0xF);
    pA->white = (uint8_t)((in[0] >> 4) & 0xF);
    pA->tie   = (uint8_t)(in[1] & 0xF);
    pB->black = (uint8_t)((in[1] >> 4) & 0xF);
    pB->white = (uint8_t)(in[2] & 0xF);
    pB->tie   = (uint8_t)((in[2] >> 4) & 0xF);
}

/* Functions: Byte-and-wider tier */

/*
** Function: CalculatorCountsWriterOpen
** @brief    See CalculatorCountsFile.h.
*/
CalculatorCountsWriter* CalculatorCountsWriterOpen(const char* path, int byteWidth)
{
    CalculatorCountsWriter* pWriter = (CalculatorCountsWriter*)MemMalloc("CalculatorCountsWriter", sizeof(CalculatorCountsWriter));
    if (pWriter == nullptr)
        Fatal(FATAL_ALLOCATION_FAILED, "CalculatorCountsWriterOpen: cannot allocate writer for '%s'", path);

    pWriter->pStream   = Lz4StreamWriterOpen(path);
    pWriter->byteWidth = byteWidth;
    return pWriter;
}

/*
** Function: CalculatorCountsWriterWrite
** @brief    See CalculatorCountsFile.h.
*/
void CalculatorCountsWriterWrite(CalculatorCountsWriter* pWriter, const OutcomeTriple* pTriple)
{
    Lz4StreamWriterWrite(pWriter->pStream, pTriple->black, pWriter->byteWidth);
    Lz4StreamWriterWrite(pWriter->pStream, pTriple->white, pWriter->byteWidth);
    Lz4StreamWriterWrite(pWriter->pStream, pTriple->tie,   pWriter->byteWidth);
}

/*
** Function: CalculatorCountsWriterClose
** @brief    See CalculatorCountsFile.h.
*/
void CalculatorCountsWriterClose(CalculatorCountsWriter* pWriter)
{
    Lz4StreamWriterClose(pWriter->pStream);
    MemFree(pWriter);
}

/*
** Function: CalculatorCountsReaderOpen
** @brief    See CalculatorCountsFile.h.
*/
CalculatorCountsReader* CalculatorCountsReaderOpen(const char* path, int byteWidth)
{
    Lz4StreamReader* pStream = Lz4StreamReaderOpen(path);
    if (pStream == nullptr)
        return nullptr;

    CalculatorCountsReader* pReader = (CalculatorCountsReader*)MemMalloc("CalculatorCountsReader", sizeof(CalculatorCountsReader));
    if (pReader == nullptr)
        Fatal(FATAL_ALLOCATION_FAILED, "CalculatorCountsReaderOpen: cannot allocate reader for '%s'", path);

    pReader->pStream   = pStream;
    pReader->byteWidth = byteWidth;
    return pReader;
}

/*
** Function: CalculatorCountsReaderRead
** @brief    See CalculatorCountsFile.h. Each of the three fields is read
**           and checked individually so a truncation partway through a
**           record (e.g. black and white present but tie missing) is
**           distinguished from a clean end-of-stream at a record
**           boundary -- per the project's standing "never silently
**           drop/truncate" rule, the former is fatal, not a quiet stop.
*/
bool CalculatorCountsReaderRead(CalculatorCountsReader* pReader, OutcomeTriple* pOutTriple)
{
    OutcomeTripleSetZero(pOutTriple, pReader->byteWidth);

    size_t gotBlack = Lz4StreamReaderRead(pReader->pStream, pOutTriple->black, pReader->byteWidth);
    if (gotBlack == 0)
        return false;
    if (gotBlack != (size_t)pReader->byteWidth)
        Fatal(FATAL_MERGE_LOGIC_ERROR, "CalculatorCountsReaderRead: truncated record (black: got %zu of %d bytes)", gotBlack, pReader->byteWidth);

    size_t gotWhite = Lz4StreamReaderRead(pReader->pStream, pOutTriple->white, pReader->byteWidth);
    if (gotWhite != (size_t)pReader->byteWidth)
        Fatal(FATAL_MERGE_LOGIC_ERROR, "CalculatorCountsReaderRead: truncated record (white: got %zu of %d bytes)", gotWhite, pReader->byteWidth);

    size_t gotTie = Lz4StreamReaderRead(pReader->pStream, pOutTriple->tie, pReader->byteWidth);
    if (gotTie != (size_t)pReader->byteWidth)
        Fatal(FATAL_MERGE_LOGIC_ERROR, "CalculatorCountsReaderRead: truncated record (tie: got %zu of %d bytes)", gotTie, pReader->byteWidth);

    return true;
}

/*
** Function: CalculatorCountsReaderClose
** @brief    See CalculatorCountsFile.h.
*/
void CalculatorCountsReaderClose(CalculatorCountsReader** ppReader)
{
    if (ppReader == nullptr || *ppReader == nullptr)
        return;

    Lz4StreamReaderClose(&(*ppReader)->pStream);
    MemFree(*ppReader);
    *ppReader = nullptr;
}

/* Functions: Nibble tier */

/*
** Function: NibbleCountsWriterOpen
** @brief    See CalculatorCountsFile.h.
*/
NibbleCountsWriter* NibbleCountsWriterOpen(const char* path)
{
    NibbleCountsWriter* pWriter = (NibbleCountsWriter*)MemMalloc("NibbleCountsWriter", sizeof(NibbleCountsWriter));
    if (pWriter == nullptr)
        Fatal(FATAL_ALLOCATION_FAILED, "NibbleCountsWriterOpen: cannot allocate writer for '%s'", path);

    pWriter->pStream     = Lz4StreamWriterOpen(path);
    pWriter->havePending = false;
    return pWriter;
}

/*
** Function: NibbleCountsWriterWrite
** @brief    See CalculatorCountsFile.h.
*/
void NibbleCountsWriterWrite(NibbleCountsWriter* pWriter, const NibbleOutcomeTriple* pTriple)
{
    if (!pWriter->havePending)
    {
        pWriter->pending     = *pTriple;
        pWriter->havePending = true;
        return;
    }

    uint8_t packed[3];
    PackNibblePair(&pWriter->pending, pTriple, packed);
    Lz4StreamWriterWrite(pWriter->pStream, packed, 3);
    pWriter->havePending = false;
}

/*
** Function: NibbleCountsWriterClose
** @brief    See CalculatorCountsFile.h. A pending board with no partner
**           is padded with a zero triple so the pair still packs into a
**           whole 3-byte unit -- the reader is told the true record
**           count separately and discards this padding rather than
**           returning it (see NibbleCountsReaderRead).
*/
void NibbleCountsWriterClose(NibbleCountsWriter* pWriter)
{
    if (pWriter->havePending)
    {
        NibbleOutcomeTriple pad;
        NibbleOutcomeTripleSetZero(&pad);

        uint8_t packed[3];
        PackNibblePair(&pWriter->pending, &pad, packed);
        Lz4StreamWriterWrite(pWriter->pStream, packed, 3);
    }

    Lz4StreamWriterClose(pWriter->pStream);
    MemFree(pWriter);
}

/*
** Function: NibbleCountsReaderOpen
** @brief    See CalculatorCountsFile.h.
*/
NibbleCountsReader* NibbleCountsReaderOpen(const char* path, uint64_t recordCount)
{
    Lz4StreamReader* pStream = Lz4StreamReaderOpen(path);
    if (pStream == nullptr)
        return nullptr;

    NibbleCountsReader* pReader = (NibbleCountsReader*)MemMalloc("NibbleCountsReader", sizeof(NibbleCountsReader));
    if (pReader == nullptr)
        Fatal(FATAL_ALLOCATION_FAILED, "NibbleCountsReaderOpen: cannot allocate reader for '%s'", path);

    pReader->pStream     = pStream;
    pReader->recordCount = recordCount;
    pReader->recordsRead = 0;
    pReader->havePending = false;
    return pReader;
}

/*
** Function: NibbleCountsReaderRead
** @brief    See CalculatorCountsFile.h. Unpacks one board at a time from
**           each 3-byte pair, holding the pair's second board until the
**           next call; on the final odd board of an odd-count stream,
**           the padding second slot is recognized by recordCount having
**           already been fully satisfied and is never handed back to the
**           caller.
*/
bool NibbleCountsReaderRead(NibbleCountsReader* pReader, NibbleOutcomeTriple* pOutTriple)
{
    if (pReader->recordsRead >= pReader->recordCount)
        return false;

    if (pReader->havePending)
    {
        *pOutTriple = pReader->pending;
        pReader->havePending = false;
        pReader->recordsRead++;
        return true;
    }

    uint8_t packed[3];
    size_t got = Lz4StreamReaderRead(pReader->pStream, packed, 3);
    if (got != 3)
        Fatal(FATAL_MERGE_LOGIC_ERROR, "NibbleCountsReaderRead: truncated pair (got %zu of 3 bytes, %llu of %llu records read so far)",
              got, (unsigned long long)pReader->recordsRead, (unsigned long long)pReader->recordCount);

    NibbleOutcomeTriple a, b;
    UnpackNibblePair(packed, &a, &b);

    *pOutTriple = a;
    pReader->recordsRead++;

    if (pReader->recordsRead < pReader->recordCount)
    {
        pReader->pending     = b;
        pReader->havePending = true;
    }
    /* else b is the trailing zero-pad written by NibbleCountsWriterClose -- discarded, never returned. */

    return true;
}

/*
** Function: NibbleCountsReaderClose
** @brief    See CalculatorCountsFile.h.
*/
void NibbleCountsReaderClose(NibbleCountsReader** ppReader)
{
    if (ppReader == nullptr || *ppReader == nullptr)
        return;

    Lz4StreamReaderClose(&(*ppReader)->pStream);
    MemFree(*ppReader);
    *ppReader = nullptr;
}
