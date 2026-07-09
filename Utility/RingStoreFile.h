/*
** Filename:  RingStoreFile.h
**
** Purpose:
**   Declares a generic on-disk record file format: a sequence of 16-byte
**   UINT64_PAIR records followed by a 64-byte RSFTrailer written last (a
**   missing/corrupt trailer magic means the file was never fully written
**   and must be discarded). This module is not Othello-aware -- it only
**   knows about two-uint64_t records, sorted ascending (hi first, then lo)
**   with no duplicates within a stream. Any caller's own key type that is
**   binary-compatible with UINT64_PAIR (same two-uint64_t layout) can be
**   cast to/from it at the call site.
**
**   Three format variants share this same trailer layout, distinguished by
**   the trailer's magic value:
**     - .rsf   (RSF_MAGIC)   -- plain, uncompressed records.
**     - .rsfz  (RSFZ_MAGIC)  -- delta+varint compressed records.
**     - .rsfzl (RSFZL_MAGIC) -- delta+varint, then LZ4-framed on top.
**   RSFWriter/RSFReader are opaque streaming types covering all three;
**   RSFWrite is a one-shot batch writer for an already-sorted/deduped
**   in-memory array (always uncompressed).
**
** Notes:
**   Promoted from an earlier analysis-only version of this format (now
**   deleted -- its job of proving the ring-split theory is done) and
**   genericized: no more Othello-specific naming or types, so any project
**   in this solution can depend on it without pulling in board semantics.
*/

#pragma once

/* Includes */
#include <stdint.h>

/* Structures and Types */

/*
** Type:    UINT64_PAIR
** @brief   Generic 16-byte record: two uint64_t fields, no padding, no
**          semantic meaning attached at this layer.
*/
#pragma pack(push, 1)
typedef struct _Uint64Pair
{
    uint64_t hi;
    uint64_t lo;
} UINT64_PAIR, * PUINT64_PAIR;
#pragma pack(pop)
static_assert(sizeof(UINT64_PAIR) == 16, "UINT64_PAIR must be 16 bytes");

/*
** Type:    RSFRecordShape
** @brief   Names a record's field layout for the generic (Shape-suffixed)
**          writer/reader entry points -- everything below RSF_SHAPE_PAIR64
**          is new, added so OthelloBasics/RingNestedIndex.h's ring-nested-
**          index files (CellsInUse/Ring_1/Ring_2/Ring_3_4) can go through
**          this same delta+varint+LZ4 machinery instead of the separate,
**          simpler Lz4Stream framing they used before. Every shape's
**          fields are still delta+zigzag+varint encoded per field, exactly
**          like RSF_SHAPE_PAIR64's hi/lo always have been -- only the
**          field count and per-field byte width vary.
*/
typedef enum
{
    RSF_SHAPE_PAIR64 = 0,   /* 2 fields, 8+8 bytes  -- today's UINT64_PAIR (unchanged, default for every existing entry point) */
    RSF_SHAPE_RING_LEVEL,   /* 2 fields, 4+8 bytes  -- Ring_1/Ring_2: {pattern (u32), offset (u64)} */
    RSF_SHAPE_LEAF16,       /* 1 field,  2 bytes    -- Ring_3_4: {pattern (u16)} */
} RSFRecordShape;

/* Widest shape currently defined -- bounds the fixed-size prevField[]
** arrays in __RSFWriter/__RSFReader so no shape needs a heap allocation
** just to track its own previous-record state.
*/
#define RSF_SHAPE_MAX_FIELDS 2

/*
** Type:    RSFTrailer
** @brief   Fixed 64-byte trailer written at the end of every ring-store
**          file, after all UINT64_PAIR records. Its magic field is written
**          last, so a missing/wrong magic reliably signals an incomplete
**          or corrupt file rather than requiring per-record validation.
*/
#pragma pack(push, 1)
typedef struct _RSFTrailer
{
    uint8_t   minKey[16];     /* first UINT64_PAIR in sorted order                    */
    uint8_t   maxKey[16];     /* last  UINT64_PAIR in sorted order                    */
    uint64_t  recordCount;    /* UINT64_PAIR records preceding this trailer           */
    uint8_t   _reserved[16];  /* reserved, must be zero                               */
    uint64_t  magic;          /* RSF_MAGIC -- written last; absence = incomplete file */
} RSFTrailer, * PRSFTrailer;
#pragma pack(pop)
static_assert(sizeof(RSFTrailer) == 64, "RSFTrailer must be 64 bytes");

/*
** Type:    RSFWriter
** @brief   Opaque streaming writer. RSFWriterOpen produces plain 16-byte
**          records; RSFWriterOpenZ/RSFWriterOpenZMem produce delta+varint
**          (+ optional LZ4) compressed output. RSFWriterRecord and
**          RSFWriterClose work identically across all of them.
*/
typedef struct __RSFWriter RSFWriter;

/*
** Type:    RSFReader
** @brief   Opaque sequential reader, dispatching on the trailer's magic to
**          handle .rsf/.rsfz/.rsfzl transparently.
*/
typedef struct __RSFReader RSFReader;

/* Constants */
#define RSF_MAGIC   0x52534653544F5245ULL   /* "RSFSTORE" in ASCII byte order */
#define RSFZ_MAGIC  0x52534653544F525AULL   /* "RSFSTORZ" in ASCII byte order */
#define RSFZL_MAGIC 0x52534653544F524CULL   /* "RSFSTORL" in ASCII byte order */

#define RSF_WRITE_BUFFER_SIZE      (512  * 1024)
#define RSF_COMP_WRITE_BUFFER_SIZE (1024 * 1024)
#define RSF_COMP_READ_BUFFER_SIZE  (1024 * 1024)

/* Functions */

/*
** Function: RSFWrite
** @brief    Writes count already-sorted-and-deduped UINT64_PAIR records
**           to path as a plain (uncompressed) .rsf file, followed by the trailer.
** @param    path     - file path to create (overwritten if it exists)
** @param    pRecords - sorted, deduped array of records to write
** @param    count    - number of records in pRecords
*/
void RSFWrite(const char* path, const UINT64_PAIR* pRecords, uint64_t count);

/*
** Function: RSFWriterOpen
** @brief    Opens path for streaming, plain (uncompressed) .rsf output.
** @param    path - file path to create (overwritten if it exists)
** @return   A new RSFWriter. Fatals on failure (never returns nullptr).
*/
RSFWriter* RSFWriterOpen(const char* path);

/*
** Function: RSFWriterOpenZ
** @brief    Opens path for streaming, delta+varint compressed output.
**           Adds an LZ4 frame layer on top automatically if path ends in ".rsfzl".
** @param    path - file path to create (overwritten if it exists)
** @return   A new RSFWriter. Fatals on failure (never returns nullptr).
*/
RSFWriter* RSFWriterOpenZ(const char* path);

/*
** Function: RSFWriterOpenZL
** @brief    Opens path for streaming, delta+varint+LZ4 compressed output,
**           regardless of path's extension -- for callers whose naming
**           convention doesn't use ".rsfzl" but still want the full
**           compression tier.
** @param    path - file path to create (overwritten if it exists)
** @return   A new RSFWriter. Fatals on failure (never returns nullptr).
*/
RSFWriter* RSFWriterOpenZL(const char* path);

/*
** Function: RSFWriterOpenZMem
** @brief    Opens a memory-backed writer producing delta+varint+LZ4
**           compressed output directly into buf, instead of a file.
** @param    buf      - destination buffer for compressed output
** @param    maxBytes - capacity of buf; RSFWriterRecord/Close fatal if exceeded
** @return   A new RSFWriter. Fatals on failure (never returns nullptr).
*/
RSFWriter* RSFWriterOpenZMem(uint8_t* buf, size_t maxBytes);

/*
** Function: RSFWriterRecord
** @brief    Appends one record to a streaming writer.
** @param    pw  - the writer to append to
** @param    pRec - the record to write
*/
void RSFWriterRecord(RSFWriter* pw, const UINT64_PAIR* pRec);

/*
** Function: RSFWriterClose
** @brief    Flushes any pending output, writes the trailer, closes the
**           writer, and frees it.
** @param    pw         - the writer to close (no longer valid after this call)
** @param    pFileBytes - out: total bytes written (compressed payload + trailer for compressed writers; buffer bytes written for memory mode), or nullptr to skip
** @return   The number of records written.
*/
uint64_t RSFWriterClose(RSFWriter* pw, uint64_t* pFileBytes = nullptr);

/*
** Function: RSFOpen
** @brief    Opens a ring-store file (.rsf/.rsfz/.rsfzl, auto-detected via
**           the trailer magic) for sequential reading.
** @details  Validates the trailer (magic + size sanity) before returning,
**           so a caller never has to separately check file integrity.
** @param    path - file path to open for reading
** @return   A new RSFReader, or nullptr if the file is missing, incomplete, or corrupt. Does NOT fatal.
*/
RSFReader* RSFOpen(const char* path);

/*
** Function: RSFReaderOpenZMem
** @brief    Opens a memory-backed reader over a single compressed (LZ4-framed) pool segment.
** @param    compBuf     - buffer holding the compressed data; must remain valid until RSFClose. Not owned/freed by the reader.
** @param    compBytes   - number of valid bytes in compBuf
** @param    recordCount - number of UINT64_PAIR records the segment decompresses to
** @return   A new RSFReader.
*/
RSFReader* RSFReaderOpenZMem(const uint8_t* compBuf, uint64_t compBytes, uint64_t recordCount);

/*
** Function: RSFRead
** @brief    Reads up to maxCount records from r into pOut.
** @param    r        - the reader to read from
** @param    pOut     - destination buffer, at least maxCount records
** @param    maxCount - maximum number of records to read
** @return   Number of records actually read; 0 means EOF.
*/
int RSFRead(RSFReader* r, UINT64_PAIR* pOut, int maxCount);

/*
** Function: RSFReaderTrailer
** @brief    Returns the trailer belonging to an open reader.
** @param    r - the reader to query
** @return   Pointer to the trailer, valid until RSFClose(r).
*/
const RSFTrailer* RSFReaderTrailer(const RSFReader* r);

/*
** Function: RSFClose
** @brief    Closes and frees a reader, and nulls the caller's pointer to it.
** @param    ppReader - address of the reader pointer to close
*/
void RSFClose(RSFReader** ppReader);

/* ------------------------------------------------------------------------
** Shaped API -- generalizes the writer/reader to any RSFRecordShape, always
** through the delta+varint+LZ4 (.rsfzl) tier. Added for
** OthelloBasics/RingNestedIndex.h's four ring-nested-index files
** (CellsInUse/Ring_1/Ring_2/Ring_3_4), which previously mixed this format
** (CellsInUse only, via RSFWriterOpenZL) with a separate, simpler Lz4Stream
** framing (Ring_1/Ring_2/Ring_3_4) -- now all four go through one format.
** None of the functions above are affected; they keep operating on
** UINT64_PAIR/RSF_SHAPE_PAIR64 exactly as before.
** ------------------------------------------------------------------------ */

/*
** Function: RSFWriterOpenZLShaped
** @brief    Opens path for streaming, delta+varint+LZ4 compressed output of
**           shape-typed records instead of UINT64_PAIR.
** @param    path  - file path to create (overwritten if it exists)
** @param    shape - the record layout this writer will accept
** @return   A new RSFWriter. Fatals on failure (never returns nullptr).
*/
RSFWriter* RSFWriterOpenZLShaped(const char* path, RSFRecordShape shape);

/*
** Function: RSFWriterRecordShaped
** @brief    Appends one shape-typed record to a shaped streaming writer.
** @param    pw        - the writer to append to (opened via a *Shaped entry point)
** @param    pRecord   - pointer to a tightly-packed (#pragma pack(push,1)) record matching pw's shape
*/
void RSFWriterRecordShaped(RSFWriter* pw, const void* pRecord);

/*
** Function: RSFOpenShaped
** @brief    Opens a .rsfzl ring-store file of shape-typed records for
**           sequential reading. Unlike RSFOpen, the shape is supplied by
**           the caller rather than auto-detected -- the trailer alone
**           can't distinguish e.g. RSF_SHAPE_RING_LEVEL from a narrower
**           reinterpretation of RSF_SHAPE_PAIR64, so the caller (which
**           already knows which ring file this is) must say so.
** @param    path  - file path to open for reading
** @param    shape - the record layout stored in this file
** @return   A new RSFReader, or nullptr if the file is missing, incomplete, or corrupt. Does NOT fatal.
*/
RSFReader* RSFOpenShaped(const char* path, RSFRecordShape shape);

/*
** Function: RSFReadShaped
** @brief    Reads up to maxCount shape-typed records from r into pOut.
** @param    r        - the reader to read from (opened via RSFOpenShaped)
** @param    pOut     - destination buffer, at least maxCount records of r's shape
** @param    maxCount - maximum number of records to read
** @return   Number of records actually read; 0 means EOF.
*/
int RSFReadShaped(RSFReader* r, void* pOut, int maxCount);
