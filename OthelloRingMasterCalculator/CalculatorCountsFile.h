/*
** Filename:  CalculatorCountsFile.h
**
** Purpose:
**   Declares the streamed writer/reader for one level's outcome-triple
**   records, positionally aligned to that level's board store (record i
**   here corresponds to record i in the board store -- see
**   project_adaptive_counter_width_design memory for why no key is
**   stored). Two variants: CalculatorCountsWriter/Reader for the
**   byte-and-wider tiers (each record is 3*byteWidth contiguous bytes --
**   black, then white, then tie), and NibbleCountsWriter/Reader for the
**   narrowest tier (2 boards packed into 3 bytes, since 3 counters at 4
**   bits each is 12 bits/board, not byte-aligned on its own).
**
** Notes:
**   Both ride Utility/Lz4Stream.h's Lz4StreamWriter/Reader underneath --
**   same fully-streaming, bounded-memory discipline as
**   OthelloBasics/RingNestedIndex.h's Ring_1/Ring_2/Ring_3_4 files, no raw
**   intermediate file ever touches disk.
*/

#pragma once

/* Includes */
#include "OutcomeTriple.h"

/* Structures and Types */

typedef struct __CalculatorCountsWriter CalculatorCountsWriter;
typedef struct __CalculatorCountsReader CalculatorCountsReader;
typedef struct __NibbleCountsWriter     NibbleCountsWriter;
typedef struct __NibbleCountsReader     NibbleCountsReader;

/* Functions */

/*
** Function: CalculatorCountsWriterOpen
** @brief    Opens path for streaming output at the given byte-and-wider tier.
** @param    path       - file path to create (overwritten if it exists)
** @param    byteWidth  - this level's tier width in bytes (1, 2, 4, 8, or 9+ -- never 0/nibble, see NibbleCountsWriterOpen for that tier)
** @return   A new CalculatorCountsWriter. Fatals on failure (never returns nullptr).
*/
CalculatorCountsWriter* CalculatorCountsWriterOpen(const char* path, int byteWidth);

/*
** Function: CalculatorCountsWriterWrite
** @brief    Appends one record.
** @param    pWriter - the writer to append to
** @param    pTriple - the record to write (only the first byteWidth bytes
**                     of each counter are written)
*/
void CalculatorCountsWriterWrite(CalculatorCountsWriter* pWriter, const OutcomeTriple* pTriple);

/*
** Function: CalculatorCountsWriterClose
** @brief    Flushes and closes the writer.
** @param    pWriter - the writer to close (no longer valid after this call)
*/
void CalculatorCountsWriterClose(CalculatorCountsWriter* pWriter);

/*
** Function: CalculatorCountsReaderOpen
** @brief    Opens path for sequential reading at the given byte-and-wider tier.
** @param    path       - file path to open for reading
** @param    byteWidth  - this level's tier width in bytes (must match what it was written with)
** @return   A new CalculatorCountsReader, or nullptr if the file is missing. Does NOT fatal on a missing file.
*/
CalculatorCountsReader* CalculatorCountsReaderOpen(const char* path, int byteWidth);

/*
** Function: CalculatorCountsReaderRead
** @brief    Reads the next record.
** @param    pReader     - the reader to read from
** @param    pOutTriple  - out: filled with the next record (bytes beyond
**                         byteWidth in each counter are zeroed)
** @return   true if a record was read; false at a clean end of stream.
**           Fatals (does not return) on a truncated mid-record read.
*/
bool CalculatorCountsReaderRead(CalculatorCountsReader* pReader, OutcomeTriple* pOutTriple);

/*
** Function: CalculatorCountsReaderClose
** @brief    Closes and frees a reader, and nulls the caller's pointer to it.
** @param    ppReader - address of the reader pointer to close
*/
void CalculatorCountsReaderClose(CalculatorCountsReader** ppReader);

/*
** Function: NibbleCountsWriterOpen
** @brief    Opens path for streaming output at the nibble tier.
** @param    path - file path to create (overwritten if it exists)
** @return   A new NibbleCountsWriter. Fatals on failure (never returns nullptr).
*/
NibbleCountsWriter* NibbleCountsWriterOpen(const char* path);

/*
** Function: NibbleCountsWriterWrite
** @brief    Appends one record. Buffers one pending board internally --
**           every second call actually writes 3 packed bytes to disk.
** @param    pWriter - the writer to append to
** @param    pTriple - the record to write
*/
void NibbleCountsWriterWrite(NibbleCountsWriter* pWriter, const NibbleOutcomeTriple* pTriple);

/*
** Function: NibbleCountsWriterClose
** @brief    Flushes (padding a trailing odd board with a zero second slot
**           if one is pending) and closes the writer.
** @param    pWriter - the writer to close (no longer valid after this call)
*/
void NibbleCountsWriterClose(NibbleCountsWriter* pWriter);

/*
** Function: NibbleCountsReaderOpen
** @brief    Opens path for sequential reading at the nibble tier.
** @param    path        - file path to open for reading
** @param    recordCount - the true number of records (needed because a
**                         packed pair can't self-describe a trailing odd
**                         board's zero-padding -- the reader relies on
**                         this count to know when to stop, discarding any
**                         padding rather than returning it)
** @return   A new NibbleCountsReader, or nullptr if the file is missing. Does NOT fatal on a missing file.
*/
NibbleCountsReader* NibbleCountsReaderOpen(const char* path, uint64_t recordCount);

/*
** Function: NibbleCountsReaderRead
** @brief    Reads the next record.
** @param    pReader    - the reader to read from
** @param    pOutTriple - out: filled with the next record
** @return   true if a record was read; false once recordCount records
**           have all been returned. Fatals (does not return) on a
**           truncated mid-pair read.
*/
bool NibbleCountsReaderRead(NibbleCountsReader* pReader, NibbleOutcomeTriple* pOutTriple);

/*
** Function: NibbleCountsReaderClose
** @brief    Closes and frees a reader, and nulls the caller's pointer to it.
** @param    ppReader - address of the reader pointer to close
*/
void NibbleCountsReaderClose(NibbleCountsReader** ppReader);
