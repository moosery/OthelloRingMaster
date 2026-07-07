/*
** Filename:  BlasterFile.h
**
** Purpose:
**   Declares the Blaster on-disk board-key file format, shared between
**   OthelloLevelBlaster and offline tooling in this solution (e.g.
**   OthelloRingSplitAnalyzer) that reads its store files. A file is a
**   sequence of 16-byte BOARD_KEY_DISK records followed by a 64-byte
**   BlasterFileTrailer written last (a missing/corrupt trailer magic means
**   the file was never fully written and must be discarded). Records
**   contain only the two bitboard fields (ullCellsInUse + ullCellColors);
**   player turn and board size are encoded in the filename, not the record.
**   Records are sorted ascending (ullCellsInUse first, then ullCellColors)
**   and contain no duplicates within a player stream.
**
**   Three format variants share this same trailer layout, distinguished by
**   the trailer's magic value:
**     - .blf   (BLF_MAGIC)   -- plain, uncompressed records.
**     - .blfz  (BLFZ_MAGIC)  -- delta+varint compressed records.
**     - .blfzl (BLFZL_MAGIC) -- delta+varint, then LZ4-framed on top.
**   BLFWriter/BLFReader are opaque streaming types covering all three;
**   BLFWrite is a one-shot batch writer for an already-sorted/deduped
**   in-memory array (always uncompressed).
*/

#pragma once

/* Includes */
#include <stdint.h>

/* Structures and Types */

/*
** Type:    BOARD_KEY_DISK
** @brief   On-disk board key: 16 bytes, no player bit, no padding.
*/
#pragma pack(push, 1)
typedef struct _BoardKeyDisk
{
    uint64_t ullCellsInUse;
    uint64_t ullCellColors;
} BOARD_KEY_DISK, * PBOARD_KEY_DISK;
#pragma pack(pop)
static_assert(sizeof(BOARD_KEY_DISK) == 16, "BOARD_KEY_DISK must be 16 bytes");

/*
** Type:    BlasterFileTrailer
** @brief   Fixed 64-byte trailer written at the end of every Blaster file,
**          after all BOARD_KEY_DISK records. Its magic field is written
**          last, so a missing/wrong magic reliably signals an incomplete
**          or corrupt file rather than requiring per-record validation.
*/
#pragma pack(push, 1)
typedef struct __BlasterFileTrailer
{
    uint8_t   minKey[16];     /* first BOARD_KEY_DISK in sorted order                 */
    uint8_t   maxKey[16];     /* last  BOARD_KEY_DISK in sorted order                 */
    uint64_t  recordCount;    /* BOARD_KEY_DISK records preceding this trailer        */
    uint8_t   _reserved[16];  /* reserved, must be zero                               */
    uint64_t  magic;          /* BLF_MAGIC -- written last; absence = incomplete file */
} BlasterFileTrailer, * PBlasterFileTrailer;
#pragma pack(pop)
static_assert(sizeof(BlasterFileTrailer) == 64, "BlasterFileTrailer must be 64 bytes");

/*
** Type:    BLFWriter
** @brief   Opaque streaming writer. BLFWriterOpen produces plain 16-byte
**          records; BLFWriterOpenZ/BLFWriterOpenZMem produce delta+varint
**          (+ optional LZ4) compressed output. BLFWriterRecord and
**          BLFWriterClose work identically across all of them.
*/
typedef struct __BLFWriter BLFWriter;

/*
** Type:    BLFReader
** @brief   Opaque sequential reader, dispatching on the trailer's magic to
**          handle .blf/.blfz/.blfzl transparently.
*/
typedef struct __BLFReader BLFReader;

/* Constants */
#define BLF_MAGIC   0x424C535446494C45ULL   /* "BLSTFILE" in little-endian ASCII */
#define BLFZ_MAGIC  0x5A46494C54534C42ULL   /* "BLSTFILZ" in little-endian ASCII */
#define BLFZL_MAGIC 0x4C5A46494C54534CULL   /* "LSTFILZL" in little-endian ASCII */

#define BLF_WRITE_BUFFER_SIZE      (512  * 1024)
#define BLF_COMP_WRITE_BUFFER_SIZE (1024 * 1024)
#define BLF_COMP_READ_BUFFER_SIZE  (1024 * 1024)

/* Functions */

/*
** Function: BLFWrite
** @brief    Writes count already-sorted-and-deduped BOARD_KEY_DISK records
**           to path as a plain (uncompressed) .blf file, followed by the trailer.
** @param    path  - file path to create (overwritten if it exists)
** @param    pKeys - sorted, deduped array of records to write
** @param    count - number of records in pKeys
*/
void BLFWrite(const char* path, const BOARD_KEY_DISK* pKeys, uint64_t count);

/*
** Function: BLFWriterOpen
** @brief    Opens path for streaming, plain (uncompressed) .blf output.
** @param    path - file path to create (overwritten if it exists)
** @return   A new BLFWriter. Fatals on failure (never returns nullptr).
*/
BLFWriter* BLFWriterOpen(const char* path);

/*
** Function: BLFWriterOpenZ
** @brief    Opens path for streaming, delta+varint compressed output.
**           Adds an LZ4 frame layer on top automatically if path ends in ".blfzl".
** @param    path - file path to create (overwritten if it exists)
** @return   A new BLFWriter. Fatals on failure (never returns nullptr).
*/
BLFWriter* BLFWriterOpenZ(const char* path);

/*
** Function: BLFWriterOpenZMem
** @brief    Opens a memory-backed writer producing delta+varint+LZ4
**           compressed output directly into buf, instead of a file.
** @param    buf      - destination buffer for compressed output
** @param    maxBytes - capacity of buf; BLFWriterRecord/Close fatal if exceeded
** @return   A new BLFWriter. Fatals on failure (never returns nullptr).
*/
BLFWriter* BLFWriterOpenZMem(uint8_t* buf, size_t maxBytes);

/*
** Function: BLFWriterRecord
** @brief    Appends one record to a streaming writer.
** @param    pw   - the writer to append to
** @param    pKey - the record to write
*/
void BLFWriterRecord(BLFWriter* pw, const BOARD_KEY_DISK* pKey);

/*
** Function: BLFWriterClose
** @brief    Flushes any pending output, writes the trailer, closes the
**           writer, and frees it.
** @param    pw         - the writer to close (no longer valid after this call)
** @param    pFileBytes - out: total bytes written (compressed payload + trailer for compressed writers; buffer bytes written for memory mode), or nullptr to skip
** @return   The number of records written.
*/
uint64_t BLFWriterClose(BLFWriter* pw, uint64_t* pFileBytes = nullptr);

/*
** Function: BLFOpen
** @brief    Opens a Blaster file (.blf/.blfz/.blfzl, auto-detected via the
**           trailer magic) for sequential reading.
** @details  Validates the trailer (magic + size sanity) before returning,
**           so a caller never has to separately check file integrity.
** @param    path - file path to open for reading
** @return   A new BLFReader, or nullptr if the file is missing, incomplete, or corrupt. Does NOT fatal.
*/
BLFReader* BLFOpen(const char* path);

/*
** Function: BLFReaderOpenZMem
** @brief    Opens a memory-backed reader over a single compressed (LZ4-framed) pool segment.
** @param    compBuf    - buffer holding the compressed data; must remain valid until BLFClose. Not owned/freed by the reader.
** @param    compBytes  - number of valid bytes in compBuf
** @param    boardCount - number of BOARD_KEY_DISK records the segment decompresses to
** @return   A new BLFReader.
*/
BLFReader* BLFReaderOpenZMem(const uint8_t* compBuf, uint64_t compBytes, uint64_t boardCount);

/*
** Function: BLFRead
** @brief    Reads up to maxCount records from r into pOut.
** @param    r        - the reader to read from
** @param    pOut     - destination buffer, at least maxCount records
** @param    maxCount - maximum number of records to read
** @return   Number of records actually read; 0 means EOF.
*/
int BLFRead(BLFReader* r, BOARD_KEY_DISK* pOut, int maxCount);

/*
** Function: BLFTrailer
** @brief    Returns the trailer belonging to an open reader.
** @param    r - the reader to query
** @return   Pointer to the trailer, valid until BLFClose(r).
*/
const BlasterFileTrailer* BLFTrailer(const BLFReader* r);

/*
** Function: BLFClose
** @brief    Closes and frees a reader, and nulls the caller's pointer to it.
** @param    ppReader - address of the reader pointer to close
*/
void BLFClose(BLFReader** ppReader);
