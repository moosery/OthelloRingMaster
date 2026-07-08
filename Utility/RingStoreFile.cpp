/*
** Filename:  RingStoreFile.cpp
**
** Purpose:
**   Implements the generic ring-store record file format declared in
**   RingStoreFile.h: RSFWrite for one-shot batch output, and the
**   RSFWriter/RSFReader streaming types covering all three format variants
**   (.rsf plain, .rsfz delta+varint compressed, .rsfzl delta+varint+LZ4).
*/

/* Includes */
#include "RingStoreFile.h"
#include "Error.h"
#include "Logger.h"
#include "Mem.h"
#include "FileAndDirUtils.h"
#include "lz4frame.h"
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <windows.h>

/* Functions */

/*
** ============================================================
** Delta + zigzag helpers (used by both writer and reader)
** ============================================================
*/

/*
** Function: ZZEnc
** @brief    Zigzag-encodes a signed delta into an unsigned value, so small
**           negative and positive deltas both varint-encode to few bytes.
** @param    v - the signed delta to encode
** @return   The zigzag-encoded unsigned value.
*/
static inline uint64_t ZZEnc(int64_t v) { return (v < 0) ? (uint64_t)(-v * 2 - 1) : (uint64_t)(v * 2); }

/*
** Function: ZZDec
** @brief    Reverses ZZEnc.
** @param    v - the zigzag-encoded unsigned value
** @return   The original signed delta.
*/
static inline int64_t ZZDec(uint64_t v) { return (v & 1) ? -(int64_t)((v >> 1) + 1) : (int64_t)(v >> 1); }

/*
** Function: VarIntPut
** @brief    Writes v to out as a LEB128-style variable-length integer.
** @param    v   - the value to encode
** @param    out - buffer to receive the encoded bytes (at least 10 bytes for a full uint64_t)
** @return   Number of bytes written.
*/
static inline size_t VarIntPut(uint64_t v, uint8_t* out)
{
    size_t n = 0;
    do {
        uint8_t b = (uint8_t)(v & 0x7F);
        v >>= 7;
        if (v) b |= 0x80;
        out[n++] = b;
    } while (v);
    return n;
}

/*
** ============================================================
** RSFWriter
** ============================================================
*/

/*
** Type:    __RSFWriter
** @brief   Concrete state behind the opaque RSFWriter handle. One struct
**          covers all three writer modes (plain/.rsfz/.rsfzl, file- or
**          memory-backed) via the compressed/isLZ4/memOut fields, rather
**          than separate types per mode.
*/
struct __RSFWriter
{
    FILE*         f;
    char          path[MAX_FULL_PATH_NAME];
    uint64_t      count;
    UINT64_PAIR   firstRec;
    UINT64_PAIR   lastRec;
    bool          hasFirst;
    bool          compressed;
    bool          isLZ4;                     /* true when LZ4 frame is active (file or memory mode) */

    /* compressed-only */
    uint8_t*  varBuf;
    size_t    varBufPos;
    uint64_t  compBytesTotal;
    uint64_t  prevHi;
    uint64_t  prevLo;

    /* LZ4 layer (.rsfzl) -- only when lz4Cctx != nullptr */
    LZ4F_cctx*  lz4Cctx;
    uint8_t*    lz4OutBuf;
    size_t      lz4OutBufSize;

    /* memory-backed output -- mutually exclusive with f (null f = memory mode) */
    uint8_t*  memOut;
    size_t    memOutPos;
    size_t    memOutMax;
};

/*
** Function: WriteOut
** @brief    Writes size bytes of data to a writer's destination, whichever
**           of file or memory mode it's in.
** @param    pw   - the writer to write to
** @param    data - the bytes to write
** @param    size - number of bytes in data
*/
static void WriteOut(RSFWriter* pw, const void* data, size_t size)
{
    if (pw->memOut)
    {
        if (pw->memOutPos + size > pw->memOutMax)
            Fatal(FATAL_ALLOCATION_FAILED,
                  "RSFWriter: memory output overflow (pos=%zu size=%zu cap=%zu)",
                  pw->memOutPos, size, pw->memOutMax);
        memcpy(pw->memOut + pw->memOutPos, data, size);
        pw->memOutPos += size;
    }
    else
    {
        if (fwrite(data, 1, size, pw->f) != size)
            Fatal(FATAL_FILE_OPEN, "RSFWriter: write failed on '%s'", pw->path);
    }
}

/*
** Function: FlushVarBuf
** @brief    Flushes a writer's pending varint buffer out, through the LZ4
**           compressor if one is active, otherwise straight to WriteOut.
** @param    pw - the writer whose varint buffer should be flushed
*/
static void FlushVarBuf(RSFWriter* pw)
{
    if (pw->varBufPos == 0) return;

    if (pw->lz4Cctx)
    {
        size_t compSize = LZ4F_compressUpdate(pw->lz4Cctx,
                                               pw->lz4OutBuf, pw->lz4OutBufSize,
                                               pw->varBuf,    pw->varBufPos, nullptr);
        if (LZ4F_isError(compSize))
            Fatal(FATAL_FILE_OPEN,
                  "FlushVarBuf: LZ4 compress failed on '%s': %s",
                  pw->path, LZ4F_getErrorName(compSize));
        if (compSize > 0)
        {
            WriteOut(pw, pw->lz4OutBuf, compSize);
            pw->compBytesTotal += compSize;
        }
    }
    else
    {
        WriteOut(pw, pw->varBuf, pw->varBufPos);
        pw->compBytesTotal += pw->varBufPos;
    }
    pw->varBufPos = 0;
}

/*
** Function: RSFWriterOpen
** @brief    Opens path for streaming, plain (uncompressed) .rsf output.
** @param    path - file path to create (overwritten if it exists)
** @return   A new RSFWriter. Fatals on failure (never returns nullptr).
*/
RSFWriter* RSFWriterOpen(const char* path)
{
    FILE* f = fopen(path, "wb");
    if (!f)
        Fatal(FATAL_FILE_OPEN, "RSFWriterOpen: cannot create '%s'", path);
    setvbuf(f, NULL, _IOFBF, RSF_WRITE_BUFFER_SIZE);

    RSFWriter* pw = (RSFWriter*)MemMalloc("RSFWriter", sizeof(RSFWriter));
    if (!pw) { fclose(f); Fatal(FATAL_ALLOCATION_FAILED, "RSFWriterOpen: cannot allocate writer"); }
    memset(pw, 0, sizeof(RSFWriter));
    pw->f = f;
    strncpy(pw->path, path, sizeof(pw->path) - 1);
    return pw;
}

/*
** Function: RSFWriterOpenZImpl
** @brief    Shared implementation for RSFWriterOpenZ/RSFWriterOpenZL: opens
**           path for streaming, delta+varint compressed output, adding an
**           LZ4 frame layer on top if forceLZ4 is set or path ends in ".rsfzl".
** @param    path     - file path to create (overwritten if it exists)
** @param    forceLZ4 - add the LZ4 layer regardless of path's extension
** @return   A new RSFWriter. Fatals on failure (never returns nullptr).
*/
static RSFWriter* RSFWriterOpenZImpl(const char* path, bool forceLZ4)
{
    FILE* f = fopen(path, "wb");
    if (!f)
        Fatal(FATAL_FILE_OPEN, "RSFWriterOpenZ: cannot create '%s'", path);
    setvbuf(f, NULL, _IOFBF, RSF_COMP_WRITE_BUFFER_SIZE);

    RSFWriter* pw = (RSFWriter*)MemMalloc("RSFWriterZ", sizeof(RSFWriter));
    if (!pw) { fclose(f); Fatal(FATAL_ALLOCATION_FAILED, "RSFWriterOpenZ: cannot allocate writer"); }
    memset(pw, 0, sizeof(RSFWriter));
    pw->f          = f;
    pw->compressed = true;
    strncpy(pw->path, path, sizeof(pw->path) - 1);

    pw->varBuf = (uint8_t*)MemMalloc("RSFWriterZBuf", RSF_COMP_WRITE_BUFFER_SIZE);
    if (!pw->varBuf)
    {
        fclose(f); MemFree(pw);
        Fatal(FATAL_ALLOCATION_FAILED, "RSFWriterOpenZ: cannot allocate write buffer");
    }

    /* .rsfzl (or forceLZ4): add LZ4 frame layer on top of varint */
    if (forceLZ4 || strstr(path, ".rsfzl"))
    {
        pw->isLZ4 = true;
        LZ4F_errorCode_t lz4Err = LZ4F_createCompressionContext(&pw->lz4Cctx, LZ4F_VERSION);
        if (LZ4F_isError(lz4Err))
        {
            fclose(f); MemFree(pw->varBuf); MemFree(pw);
            Fatal(FATAL_ALLOCATION_FAILED,
                  "RSFWriterOpenZ: LZ4 context create failed on '%s': %s",
                  path, LZ4F_getErrorName(lz4Err));
        }

        LZ4F_preferences_t prefs      = {};
        prefs.frameInfo.contentChecksumFlag = LZ4F_contentChecksumEnabled;

        pw->lz4OutBufSize = LZ4F_compressBound(RSF_COMP_WRITE_BUFFER_SIZE, nullptr)
                          + LZ4F_HEADER_SIZE_MAX + 32;
        pw->lz4OutBuf = (uint8_t*)MemMalloc("RSFWriterZLBuf", pw->lz4OutBufSize);
        if (!pw->lz4OutBuf)
        {
            LZ4F_freeCompressionContext(pw->lz4Cctx);
            fclose(f); MemFree(pw->varBuf); MemFree(pw);
            Fatal(FATAL_ALLOCATION_FAILED,
                  "RSFWriterOpenZ: cannot allocate LZ4 output buffer");
        }

        /* Write LZ4 frame header; enable content checksum for integrity verification */
        size_t headerSize = LZ4F_compressBegin(pw->lz4Cctx,
                                                pw->lz4OutBuf, pw->lz4OutBufSize, &prefs);
        if (LZ4F_isError(headerSize))
        {
            LZ4F_freeCompressionContext(pw->lz4Cctx);
            fclose(f); MemFree(pw->lz4OutBuf); MemFree(pw->varBuf); MemFree(pw);
            Fatal(FATAL_FILE_OPEN,
                  "RSFWriterOpenZ: LZ4 frame begin failed on '%s': %s",
                  path, LZ4F_getErrorName(headerSize));
        }
        WriteOut(pw, pw->lz4OutBuf, headerSize);
        pw->compBytesTotal += headerSize;
    }

    return pw;
}

/*
** Function: RSFWriterOpenZ
** @brief    Opens path for streaming, delta+varint compressed output.
**           Adds an LZ4 frame layer on top automatically if path ends in ".rsfzl".
** @param    path - file path to create (overwritten if it exists)
** @return   A new RSFWriter. Fatals on failure (never returns nullptr).
*/
RSFWriter* RSFWriterOpenZ(const char* path)
{
    return RSFWriterOpenZImpl(path, false);
}

/*
** Function: RSFWriterOpenZL
** @brief    Opens path for streaming, delta+varint+LZ4 compressed output,
**           regardless of path's extension -- for callers whose naming
**           convention doesn't use ".rsfzl" but still want the full
**           compression tier (e.g. OthelloBasics/RingNestedIndex.h's
**           CellsInUse output).
** @param    path - file path to create (overwritten if it exists)
** @return   A new RSFWriter. Fatals on failure (never returns nullptr).
*/
RSFWriter* RSFWriterOpenZL(const char* path)
{
    return RSFWriterOpenZImpl(path, true);
}

/*
** Function: RSFWriterOpenZMem
** @brief    Opens a memory-backed writer producing delta+varint+LZ4
**           compressed output directly into buf, instead of a file.
** @param    buf      - destination buffer for compressed output
** @param    maxBytes - capacity of buf; RSFWriterRecord/Close fatal if exceeded
** @return   A new RSFWriter. Fatals on failure (never returns nullptr).
*/
RSFWriter* RSFWriterOpenZMem(uint8_t* buf, size_t maxBytes)
{
    RSFWriter* pw = (RSFWriter*)MemMalloc("RSFWriterZMem", sizeof(RSFWriter));
    if (!pw) Fatal(FATAL_ALLOCATION_FAILED, "RSFWriterOpenZMem: cannot allocate writer");
    memset(pw, 0, sizeof(RSFWriter));
    pw->compressed = true;
    pw->isLZ4      = true;
    pw->memOut     = buf;
    pw->memOutMax  = maxBytes;
    strncpy(pw->path, "(memory)", sizeof(pw->path) - 1);

    pw->varBuf = (uint8_t*)MemMalloc("RSFWriterZMemVarBuf", RSF_COMP_WRITE_BUFFER_SIZE);
    if (!pw->varBuf)
    {
        MemFree(pw);
        Fatal(FATAL_ALLOCATION_FAILED, "RSFWriterOpenZMem: cannot allocate varint buffer");
    }

    LZ4F_errorCode_t lz4Err = LZ4F_createCompressionContext(&pw->lz4Cctx, LZ4F_VERSION);
    if (LZ4F_isError(lz4Err))
    {
        MemFree(pw->varBuf); MemFree(pw);
        Fatal(FATAL_ALLOCATION_FAILED,
              "RSFWriterOpenZMem: LZ4 context create failed: %s", LZ4F_getErrorName(lz4Err));
    }

    LZ4F_preferences_t prefs = {};
    prefs.frameInfo.contentChecksumFlag = LZ4F_contentChecksumEnabled;

    pw->lz4OutBufSize = LZ4F_compressBound(RSF_COMP_WRITE_BUFFER_SIZE, nullptr)
                      + LZ4F_HEADER_SIZE_MAX + 32;
    pw->lz4OutBuf = (uint8_t*)MemMalloc("RSFWriterZMemLZ4Buf", pw->lz4OutBufSize);
    if (!pw->lz4OutBuf)
    {
        LZ4F_freeCompressionContext(pw->lz4Cctx);
        MemFree(pw->varBuf); MemFree(pw);
        Fatal(FATAL_ALLOCATION_FAILED, "RSFWriterOpenZMem: cannot allocate LZ4 output buffer");
    }

    size_t headerSize = LZ4F_compressBegin(pw->lz4Cctx, pw->lz4OutBuf, pw->lz4OutBufSize, &prefs);
    if (LZ4F_isError(headerSize))
    {
        LZ4F_freeCompressionContext(pw->lz4Cctx);
        MemFree(pw->lz4OutBuf); MemFree(pw->varBuf); MemFree(pw);
        Fatal(FATAL_ALLOCATION_FAILED,
              "RSFWriterOpenZMem: LZ4 compressBegin failed: %s", LZ4F_getErrorName(headerSize));
    }
    WriteOut(pw, pw->lz4OutBuf, headerSize);
    pw->compBytesTotal += headerSize;

    return pw;
}

/*
** Function: RSFWriterRecord
** @brief    Appends one record to a streaming writer.
** @param    pw   - the writer to append to
** @param    pRec - the record to write
*/
void RSFWriterRecord(RSFWriter* pw, const UINT64_PAIR* pRec)
{
    if (!pw->compressed)
    {
        if (fwrite(pRec, sizeof(UINT64_PAIR), 1, pw->f) != 1)
            Fatal(FATAL_FILE_OPEN, "RSFWriterRecord: write failed");
    }
    else
    {
        /* Ensure room for two max-length varints (10 bytes each) */
        if (pw->varBufPos + 20 > RSF_COMP_WRITE_BUFFER_SIZE)
            FlushVarBuf(pw);
        uint64_t dHi = ZZEnc((int64_t)pRec->hi - (int64_t)pw->prevHi);
        uint64_t dLo = ZZEnc((int64_t)pRec->lo - (int64_t)pw->prevLo);
        pw->varBufPos += VarIntPut(dHi, pw->varBuf + pw->varBufPos);
        pw->varBufPos += VarIntPut(dLo, pw->varBuf + pw->varBufPos);
        pw->prevHi = pRec->hi;
        pw->prevLo = pRec->lo;
    }

    if (!pw->hasFirst) { pw->firstRec = *pRec; pw->hasFirst = true; }
    pw->lastRec = *pRec;
    pw->count++;
}

/*
** Function: RSFWriterClose
** @brief    Flushes any pending output, writes the trailer, closes the
**           writer, and frees it.
** @param    pw         - the writer to close (no longer valid after this call)
** @param    pFileBytes - out: total bytes written (compressed payload + trailer for compressed writers; buffer bytes written for memory mode), or nullptr to skip
** @return   The number of records written.
*/
uint64_t RSFWriterClose(RSFWriter* pw, uint64_t* pFileBytes)
{
    if (pw->compressed)
        FlushVarBuf(pw);

    /* End the LZ4 frame (writes end mark + content checksum) */
    if (pw->lz4Cctx)
    {
        size_t endSize = LZ4F_compressEnd(pw->lz4Cctx,
                                           pw->lz4OutBuf, pw->lz4OutBufSize, nullptr);
        if (LZ4F_isError(endSize))
            Fatal(FATAL_FILE_OPEN,
                  "RSFWriterClose: LZ4 frame end failed on '%s': %s",
                  pw->path, LZ4F_getErrorName(endSize));
        if (endSize > 0)
        {
            WriteOut(pw, pw->lz4OutBuf, endSize);
            pw->compBytesTotal += endSize;
        }
        LZ4F_freeCompressionContext(pw->lz4Cctx);
        MemFree(pw->lz4OutBuf);
    }

    RSFTrailer trailer = {};
    trailer.recordCount = pw->count;
    if (pw->hasFirst)
    {
        memcpy(trailer.minKey, &pw->firstRec, sizeof(UINT64_PAIR));
        memcpy(trailer.maxKey, &pw->lastRec,  sizeof(UINT64_PAIR));
    }

    uint64_t fileBytes;
    if (pw->compressed)
    {
        memcpy(trailer._reserved, &pw->compBytesTotal, sizeof(uint64_t));
        trailer.magic = pw->isLZ4 ? RSFZL_MAGIC : RSFZ_MAGIC;
        fileBytes     = pw->compBytesTotal + sizeof(RSFTrailer);
        MemFree(pw->varBuf);
    }
    else
    {
        trailer.magic = RSF_MAGIC;
        fileBytes     = pw->count * sizeof(UINT64_PAIR) + sizeof(RSFTrailer);
    }

    WriteOut(pw, &trailer, sizeof(trailer));
    if (pw->f) fclose(pw->f);

    uint64_t count = pw->count;
    MemFree(pw);
    if (pFileBytes) *pFileBytes = fileBytes;
    return count;
}

/*
** ============================================================
** RSFWrite (batch, always uncompressed)
** ============================================================
*/

/*
** Function: RSFWrite
** @brief    Writes count already-sorted-and-deduped UINT64_PAIR records
**           to path as a plain (uncompressed) .rsf file, followed by the trailer.
** @param    path     - file path to create (overwritten if it exists)
** @param    pRecords - sorted, deduped array of records to write
** @param    count    - number of records in pRecords
*/
void RSFWrite(const char* path, const UINT64_PAIR* pRecords, uint64_t count)
{
    FILE* f = fopen(path, "wb");
    if (!f)
        Fatal(FATAL_FILE_OPEN, "RSFWrite: cannot create '%s'", path);

    if (count > 0 && fwrite(pRecords, sizeof(UINT64_PAIR), (size_t)count, f) != (size_t)count)
    {
        fclose(f);
        Fatal(FATAL_FILE_OPEN, "RSFWrite: record write failed for '%s'", path);
    }

    RSFTrailer trailer = {};
    trailer.recordCount = count;
    if (count > 0)
    {
        memcpy(trailer.minKey, &pRecords[0],         sizeof(UINT64_PAIR));
        memcpy(trailer.maxKey, &pRecords[count - 1], sizeof(UINT64_PAIR));
    }
    trailer.magic = RSF_MAGIC;

    if (fwrite(&trailer, sizeof(trailer), 1, f) != 1)
    {
        fclose(f);
        Fatal(FATAL_FILE_OPEN, "RSFWrite: trailer write failed for '%s'", path);
    }

    fclose(f);
}

/*
** ============================================================
** RSFReader (handles .rsf, .rsfz, and .rsfzl via magic dispatch)
** ============================================================
*/

/*
** Type:    __RSFReader
** @brief   Concrete state behind the opaque RSFReader handle. One struct
**          covers all three formats (plain/.rsfz/.rsfzl, file- or
**          memory-backed) via the compressed/lz4Dctx/memMode fields.
*/
struct __RSFReader
{
    FILE*       f;
    RSFTrailer  trailer;
    uint64_t    recordsRead;
    bool        compressed;
    bool        memMode;      /* true if compBuf is caller-owned; do not fread or free it */

    /* compressed-only (.rsfz and .rsfzl) */
    uint8_t*  compBuf;
    size_t    compBufSize;
    size_t    compBufPos;
    size_t    compBufFilled;
    uint64_t  compBytesTotal;
    uint64_t  compBytesConsumed;
    uint64_t  prevHi;
    uint64_t  prevLo;

    /* LZ4 decompression layer (.rsfzl only) -- nullptr for .rsfz */
    LZ4F_dctx*  lz4Dctx;
    uint8_t*    lz4DecBuf;        /* decompressed varint bytes             */
    size_t      lz4DecBufSize;
    size_t      lz4DecBufPos;     /* read position in lz4DecBuf            */
    size_t      lz4DecBufFilled;  /* valid bytes in lz4DecBuf              */
    bool        lz4FrameDone;     /* true after LZ4F_decompress returned 0 */
};

/*
** Function: RSFZReadByte
** @brief    Returns the next decoded varint-stream byte from a reader,
**           transparently refilling from disk/LZ4 as needed.
** @param    r - the reader to read from
** @return   The next byte of the (decompressed, if applicable) varint stream.
*/
static uint8_t RSFZReadByte(RSFReader* r)
{
    if (r->lz4Dctx)
    {
        /* LZ4 path: serve bytes from the decompressed varint buffer.
        ** Refill by decompressing from compBuf (which holds raw LZ4 frame data).
        */
        while (r->lz4DecBufPos >= r->lz4DecBufFilled)
        {
            if (r->lz4FrameDone)
                Fatal(FATAL_FILE_OPEN, "RSFZReadByte: read past end of LZ4 frame");

            /* Refill compBuf from disk if empty */
            if (r->compBufPos >= r->compBufFilled)
            {
                uint64_t remaining = r->compBytesTotal - r->compBytesConsumed;
                if (remaining == 0 || r->memMode)
                    Fatal(FATAL_FILE_OPEN,
                          "RSFZReadByte: LZ4 compressed stream exhausted before frame end");
                size_t toRead = (remaining < (uint64_t)r->compBufSize)
                                ? (size_t)remaining : r->compBufSize;
                r->compBufFilled = fread(r->compBuf, 1, toRead, r->f);
                r->compBufPos    = 0;
                if (r->compBufFilled == 0)
                    Fatal(FATAL_FILE_OPEN, "RSFZReadByte: LZ4 read failed");
            }

            size_t srcSize = r->compBufFilled - r->compBufPos;
            size_t dstSize = r->lz4DecBufSize;
            size_t ret = LZ4F_decompress(r->lz4Dctx,
                                          r->lz4DecBuf, &dstSize,
                                          r->compBuf + r->compBufPos, &srcSize,
                                          nullptr);
            if (LZ4F_isError(ret))
                Fatal(FATAL_FILE_OPEN,
                      "RSFZReadByte: LZ4 decompress error: %s",
                      LZ4F_getErrorName(ret));
            r->compBufPos        += srcSize;
            r->compBytesConsumed += srcSize;
            r->lz4DecBufPos       = 0;
            r->lz4DecBufFilled    = dstSize;
            if (ret == 0)
                r->lz4FrameDone = true;
            /* dstSize may be 0 when consuming LZ4 overhead (end mark,
            ** checksum); loop again to get the next decompressed chunk.
            */
        }
        return r->lz4DecBuf[r->lz4DecBufPos++];
    }

    /* Original varint-only path (.rsfz) */
    if (r->compBufPos >= r->compBufFilled)
    {
        uint64_t remaining = r->compBytesTotal - r->compBytesConsumed;
        if (remaining == 0 || r->memMode)
            Fatal(FATAL_FILE_OPEN, "RSFZReadByte: varint stream exhausted before record count reached");
        size_t toRead = (remaining < (uint64_t)r->compBufSize) ? (size_t)remaining : r->compBufSize;
        r->compBufFilled = fread(r->compBuf, 1, toRead, r->f);
        r->compBufPos    = 0;
        if (r->compBufFilled == 0)
            Fatal(FATAL_FILE_OPEN, "RSFZReadByte: varint read failed (short read on compressed stream)");
    }
    r->compBytesConsumed++;
    return r->compBuf[r->compBufPos++];
}

/*
** Function: RSFZReadVarInt
** @brief    Reads one LEB128-style variable-length integer from a reader.
** @param    r - the reader to read from
** @return   The decoded value.
*/
static uint64_t RSFZReadVarInt(RSFReader* r)
{
    uint64_t v  = 0;
    int      sh = 0;
    for (;;) {
        uint8_t b = RSFZReadByte(r);
        v |= (uint64_t)(b & 0x7F) << sh;
        sh += 7;
        if (!(b & 0x80)) break;
    }
    return v;
}

/*
** Function: RSFOpen
** @brief    Opens a ring-store file (.rsf/.rsfz/.rsfzl, auto-detected via
**           the trailer magic) for sequential reading.
** @details  Validates the trailer (magic + size sanity) before returning,
**           so a caller never has to separately check file integrity.
** @param    path - file path to open for reading
** @return   A new RSFReader, or nullptr if the file is missing, incomplete, or corrupt. Does NOT fatal.
*/
RSFReader* RSFOpen(const char* path)
{
    FILE* f = fopen(path, "rb");
    if (!f) return nullptr;

    if (_fseeki64(f, -(int64_t)sizeof(RSFTrailer), SEEK_END) != 0)
    {
        fclose(f);
        LoggerLog("RSFOpen: cannot seek to trailer in '%s'\n", path);
        return nullptr;
    }

    RSFTrailer trailer = {};
    if (fread(&trailer, sizeof(trailer), 1, f) != 1)
    {
        fclose(f);
        LoggerLog("RSFOpen: cannot read trailer in '%s'\n", path);
        return nullptr;
    }

    bool compressed = false;
    bool lz4        = false;
    if (trailer.magic == RSF_MAGIC)
        compressed = false;
    else if (trailer.magic == RSFZ_MAGIC)
        compressed = true;
    else if (trailer.magic == RSFZL_MAGIC)
        { compressed = true; lz4 = true; }
    else
    {
        fclose(f);
        LoggerLog("RSFOpen: bad magic in '%s' (corrupt or incomplete)\n", path);
        return nullptr;
    }

    _fseeki64(f, 0, SEEK_END);
    int64_t actualSize = _ftelli64(f);

    if (!compressed)
    {
        int64_t expectedSize = (int64_t)trailer.recordCount * (int64_t)sizeof(UINT64_PAIR)
                             + (int64_t)sizeof(RSFTrailer);
        if (actualSize != expectedSize)
        {
            fclose(f);
            LoggerLog("RSFOpen: size mismatch in '%s' (expected %lld, got %lld)\n",
                      path, expectedSize, actualSize);
            return nullptr;
        }
    }
    else
    {
        uint64_t compressedBytes = 0;
        memcpy(&compressedBytes, trailer._reserved, sizeof(uint64_t));
        int64_t expectedSize = (int64_t)compressedBytes + (int64_t)sizeof(RSFTrailer);
        if (actualSize != expectedSize)
        {
            fclose(f);
            LoggerLog("RSFOpen: compressed size mismatch in '%s' (expected %lld, got %lld)\n",
                      path, expectedSize, actualSize);
            return nullptr;
        }
    }

    if (_fseeki64(f, 0, SEEK_SET) != 0)
    {
        fclose(f);
        return nullptr;
    }

    RSFReader* r = (RSFReader*)MemMalloc("RSFReader", sizeof(RSFReader));
    if (!r)
    {
        fclose(f);
        Fatal(FATAL_ALLOCATION_FAILED, "RSFOpen: cannot allocate reader");
        return nullptr;
    }
    memset(r, 0, sizeof(RSFReader));
    r->f          = f;
    r->trailer    = trailer;
    r->compressed = compressed;

    if (!compressed)
        setvbuf(f, NULL, _IOFBF, RSF_COMP_READ_BUFFER_SIZE);

    if (compressed)
    {
        uint64_t compressedBytes = 0;
        memcpy(&compressedBytes, trailer._reserved, sizeof(uint64_t));
        r->compBuf        = (uint8_t*)MemMalloc("RSFReaderZBuf", RSF_COMP_READ_BUFFER_SIZE);
        r->compBufSize    = RSF_COMP_READ_BUFFER_SIZE;
        r->compBytesTotal = compressedBytes;
        if (!r->compBuf)
        {
            fclose(f); MemFree(r);
            Fatal(FATAL_ALLOCATION_FAILED, "RSFOpen: cannot allocate read buffer");
            return nullptr;
        }

        if (lz4)
        {
            LZ4F_errorCode_t lz4Err =
                LZ4F_createDecompressionContext(&r->lz4Dctx, LZ4F_VERSION);
            if (LZ4F_isError(lz4Err))
            {
                fclose(f); MemFree(r->compBuf); MemFree(r);
                Fatal(FATAL_ALLOCATION_FAILED,
                      "RSFOpen: LZ4 decomp context failed: %s",
                      LZ4F_getErrorName(lz4Err));
                return nullptr;
            }
            r->lz4DecBufSize = RSF_COMP_READ_BUFFER_SIZE;
            r->lz4DecBuf = (uint8_t*)MemMalloc("RSFReaderZLBuf", r->lz4DecBufSize);
            if (!r->lz4DecBuf)
            {
                LZ4F_freeDecompressionContext(r->lz4Dctx);
                fclose(f); MemFree(r->compBuf); MemFree(r);
                Fatal(FATAL_ALLOCATION_FAILED,
                      "RSFOpen: cannot allocate LZ4 decomp buffer");
                return nullptr;
            }
        }
    }

    return r;
}

/*
** Function: RSFReaderOpenZMem
** @brief    Opens a memory-backed reader over a single compressed (LZ4-framed) pool segment.
** @param    compBuf     - buffer holding the compressed data; must remain valid until RSFClose. Not owned/freed by the reader.
** @param    compBytes   - number of valid bytes in compBuf
** @param    recordCount - number of UINT64_PAIR records the segment decompresses to
** @return   A new RSFReader.
*/
RSFReader* RSFReaderOpenZMem(const uint8_t* compBuf, uint64_t compBytes, uint64_t recordCount)
{
    RSFReader* r = (RSFReader*)MemMalloc("RSFReaderMem", sizeof(RSFReader));
    if (!r) Fatal(FATAL_ALLOCATION_FAILED, "RSFReaderOpenZMem: cannot allocate reader");
    memset(r, 0, sizeof(RSFReader));

    r->compressed          = true;
    r->memMode             = true;
    r->trailer.recordCount = recordCount;
    r->trailer.magic       = RSFZL_MAGIC;
    memcpy(r->trailer._reserved, &compBytes, sizeof(uint64_t));

    /* Point directly at caller's buffer -- no copy, no ownership */
    r->compBuf           = (uint8_t*)compBuf;
    r->compBufSize       = (size_t)compBytes;
    r->compBufFilled     = (size_t)compBytes;
    r->compBufPos        = 0;
    r->compBytesTotal    = compBytes;
    r->compBytesConsumed = 0;

    LZ4F_errorCode_t lz4Err = LZ4F_createDecompressionContext(&r->lz4Dctx, LZ4F_VERSION);
    if (LZ4F_isError(lz4Err))
    {
        MemFree(r);
        Fatal(FATAL_ALLOCATION_FAILED,
              "RSFReaderOpenZMem: LZ4 decomp context failed: %s", LZ4F_getErrorName(lz4Err));
    }

    r->lz4DecBufSize = RSF_COMP_READ_BUFFER_SIZE;
    r->lz4DecBuf = (uint8_t*)MemMalloc("RSFReaderMemDecBuf", r->lz4DecBufSize);
    if (!r->lz4DecBuf)
    {
        LZ4F_freeDecompressionContext(r->lz4Dctx);
        MemFree(r);
        Fatal(FATAL_ALLOCATION_FAILED, "RSFReaderOpenZMem: cannot allocate decomp buffer");
    }

    return r;
}

/*
** Function: RSFRead
** @brief    Reads up to maxCount records from r into pOut.
** @param    r        - the reader to read from
** @param    pOut     - destination buffer, at least maxCount records
** @param    maxCount - maximum number of records to read
** @return   Number of records actually read; 0 means EOF.
*/
int RSFRead(RSFReader* r, UINT64_PAIR* pOut, int maxCount)
{
    uint64_t remaining = r->trailer.recordCount - r->recordsRead;
    if (remaining == 0 || maxCount <= 0) return 0;
    int want = (remaining < (uint64_t)maxCount) ? (int)remaining : maxCount;

    if (!r->compressed)
    {
        int got = (int)fread(pOut, sizeof(UINT64_PAIR), (size_t)want, r->f);
        r->recordsRead += (uint64_t)got;
        return got;
    }

    int got = 0;
    while (got < want)
    {
        uint64_t dHi = RSFZReadVarInt(r);
        uint64_t dLo = RSFZReadVarInt(r);
        r->prevHi = (uint64_t)((int64_t)r->prevHi + ZZDec(dHi));
        r->prevLo = (uint64_t)((int64_t)r->prevLo + ZZDec(dLo));
        pOut[got].hi = r->prevHi;
        pOut[got].lo = r->prevLo;
        got++;
        r->recordsRead++;
    }
    return got;
}

/*
** Function: RSFReaderTrailer
** @brief    Returns the trailer belonging to an open reader.
** @param    r - the reader to query
** @return   Pointer to the trailer, valid until RSFClose(r).
*/
const RSFTrailer* RSFReaderTrailer(const RSFReader* r)
{
    return &r->trailer;
}

/*
** Function: RSFClose
** @brief    Closes and frees a reader, and nulls the caller's pointer to it.
** @param    ppReader - address of the reader pointer to close
*/
void RSFClose(RSFReader** ppReader)
{
    if (!ppReader || !*ppReader) return;
    RSFReader* r = *ppReader;
    if (r->f) fclose(r->f);
    if (r->lz4Dctx)    LZ4F_freeDecompressionContext(r->lz4Dctx);
    if (r->lz4DecBuf)  MemFree(r->lz4DecBuf);
    if (r->compressed && r->compBuf && !r->memMode) MemFree(r->compBuf);
    MemFree(r);
    *ppReader = nullptr;
}
