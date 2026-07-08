/*
** Filename:  Lz4Stream.cpp
**
** Purpose:
**   Implements Lz4StreamWriter/Lz4StreamReader declared in Lz4Stream.h.
*/

/* Includes */
#include "Lz4Stream.h"
#include "Error.h"
#include "Mem.h"
#include "lz4frame.h"
#include <stdio.h>
#include <string.h>

/* Functions */

/*
** ============================================================
** Lz4StreamWriter
** ============================================================
*/

/*
** Type:    __Lz4StreamWriter
** @brief   Concrete state behind the opaque Lz4StreamWriter handle.
**          Accumulates raw bytes into inBuf; whenever it fills, compresses
**          that chunk via LZ4F_compressUpdate and writes the result out --
**          never holds more than one chunk's worth of raw or compressed
**          data at a time, regardless of total stream length.
*/
struct __Lz4StreamWriter
{
    FILE*       f;
    LZ4F_cctx*  cctx;
    uint8_t*    inBuf;
    size_t      inBufPos;
    uint8_t*    outBuf;
    size_t      outBufSize;
};

/*
** Function: FlushChunk
** @brief    Compresses and writes out whatever raw bytes are currently
**           buffered in pw->inBuf, then resets the buffer.
** @param    pw - the writer whose buffered chunk to flush
*/
static void FlushChunk(Lz4StreamWriter* pw)
{
    if (pw->inBufPos == 0) return;

    size_t compSize = LZ4F_compressUpdate(pw->cctx, pw->outBuf, pw->outBufSize,
                                           pw->inBuf, pw->inBufPos, nullptr);
    if (LZ4F_isError(compSize))
        Fatal(FATAL_FILE_OPEN, "Lz4Stream: LZ4F_compressUpdate failed: %s", LZ4F_getErrorName(compSize));
    if (compSize > 0 && fwrite(pw->outBuf, 1, compSize, pw->f) != compSize)
        Fatal(FATAL_FILE_OPEN, "Lz4Stream: write failed");

    pw->inBufPos = 0;
}

/*
** Function: Lz4StreamWriterOpen
** @brief    Opens path for streaming LZ4-frame-compressed output.
** @param    path - file path to create (overwritten if it exists)
** @return   A new Lz4StreamWriter. Fatals on failure (never returns nullptr).
*/
Lz4StreamWriter* Lz4StreamWriterOpen(const char* path)
{
    FILE* f = fopen(path, "wb");
    if (!f)
        Fatal(FATAL_FILE_OPEN, "Lz4StreamWriterOpen: cannot create '%s'", path);

    Lz4StreamWriter* pw = (Lz4StreamWriter*)MemMalloc("Lz4StreamWriter", sizeof(Lz4StreamWriter));
    if (!pw) { fclose(f); Fatal(FATAL_ALLOCATION_FAILED, "Lz4StreamWriterOpen: cannot allocate writer"); }
    memset(pw, 0, sizeof(*pw));
    pw->f = f;

    if (LZ4F_isError(LZ4F_createCompressionContext(&pw->cctx, LZ4F_VERSION)))
        Fatal(FATAL_ALLOCATION_FAILED, "Lz4StreamWriterOpen: LZ4 context create failed");

    pw->inBuf = (uint8_t*)MemMalloc("Lz4StreamWriterIn", LZ4_STREAM_CHUNK_SIZE);
    if (!pw->inBuf)
        Fatal(FATAL_ALLOCATION_FAILED, "Lz4StreamWriterOpen: cannot allocate input buffer");

    pw->outBufSize = LZ4F_compressBound(LZ4_STREAM_CHUNK_SIZE, nullptr) + LZ4F_HEADER_SIZE_MAX + 32;
    pw->outBuf     = (uint8_t*)MemMalloc("Lz4StreamWriterOut", pw->outBufSize);
    if (!pw->outBuf)
        Fatal(FATAL_ALLOCATION_FAILED, "Lz4StreamWriterOpen: cannot allocate output buffer");

    LZ4F_preferences_t prefs = {};
    prefs.frameInfo.contentChecksumFlag = LZ4F_contentChecksumEnabled;
    size_t headerSize = LZ4F_compressBegin(pw->cctx, pw->outBuf, pw->outBufSize, &prefs);
    if (LZ4F_isError(headerSize))
        Fatal(FATAL_FILE_OPEN, "Lz4StreamWriterOpen: LZ4 frame begin failed: %s", LZ4F_getErrorName(headerSize));
    if (fwrite(pw->outBuf, 1, headerSize, f) != headerSize)
        Fatal(FATAL_FILE_OPEN, "Lz4StreamWriterOpen: header write failed on '%s'", path);

    return pw;
}

/*
** Function: Lz4StreamWriterWrite
** @brief    Appends size bytes of data to the stream, compressing and
**           flushing a chunk whenever the internal buffer fills.
** @param    pw   - the writer to append to
** @param    data - bytes to write
** @param    size - number of bytes in data
*/
void Lz4StreamWriterWrite(Lz4StreamWriter* pw, const void* data, size_t size)
{
    const uint8_t* p = (const uint8_t*)data;
    while (size > 0)
    {
        size_t room = LZ4_STREAM_CHUNK_SIZE - pw->inBufPos;
        size_t take = (size < room) ? size : room;
        memcpy(pw->inBuf + pw->inBufPos, p, take);
        pw->inBufPos += take;
        p    += take;
        size -= take;

        if (pw->inBufPos == LZ4_STREAM_CHUNK_SIZE)
            FlushChunk(pw);
    }
}

/*
** Function: Lz4StreamWriterClose
** @brief    Flushes any buffered bytes, ends the LZ4 frame, closes the
**           writer, and frees it.
** @param    pw - the writer to close (no longer valid after this call)
*/
void Lz4StreamWriterClose(Lz4StreamWriter* pw)
{
    FlushChunk(pw);

    size_t endSize = LZ4F_compressEnd(pw->cctx, pw->outBuf, pw->outBufSize, nullptr);
    if (LZ4F_isError(endSize))
        Fatal(FATAL_FILE_OPEN, "Lz4StreamWriterClose: LZ4 frame end failed: %s", LZ4F_getErrorName(endSize));
    if (endSize > 0 && fwrite(pw->outBuf, 1, endSize, pw->f) != endSize)
        Fatal(FATAL_FILE_OPEN, "Lz4StreamWriterClose: trailer write failed");

    LZ4F_freeCompressionContext(pw->cctx);
    fclose(pw->f);
    MemFree(pw->inBuf);
    MemFree(pw->outBuf);
    MemFree(pw);
}

/*
** ============================================================
** Lz4StreamReader
** ============================================================
*/

/*
** Type:    __Lz4StreamReader
** @brief   Concrete state behind the opaque Lz4StreamReader handle.
**          Reads compressed chunks from disk into compBuf on demand,
**          decompresses into decBuf, and serves bytes from decBuf --
**          never holds more than one chunk's worth of compressed or
**          decompressed data at a time.
*/
struct __Lz4StreamReader
{
    FILE*       f;
    LZ4F_dctx*  dctx;
    uint8_t*    compBuf;
    size_t      compBufPos;
    size_t      compBufFilled;
    uint8_t*    decBuf;
    size_t      decBufPos;
    size_t      decBufFilled;
    bool        frameDone;
};

/*
** Function: Lz4StreamReaderOpen
** @brief    Opens a file written by Lz4StreamWriter for sequential reading.
** @param    path - file path to open for reading
** @return   A new Lz4StreamReader, or nullptr if the file is missing or corrupt.
*/
Lz4StreamReader* Lz4StreamReaderOpen(const char* path)
{
    FILE* f = fopen(path, "rb");
    if (!f) return nullptr;

    Lz4StreamReader* r = (Lz4StreamReader*)MemMalloc("Lz4StreamReader", sizeof(Lz4StreamReader));
    if (!r) { fclose(f); Fatal(FATAL_ALLOCATION_FAILED, "Lz4StreamReaderOpen: cannot allocate reader"); }
    memset(r, 0, sizeof(*r));
    r->f = f;

    if (LZ4F_isError(LZ4F_createDecompressionContext(&r->dctx, LZ4F_VERSION)))
    {
        fclose(f);
        MemFree(r);
        Fatal(FATAL_ALLOCATION_FAILED, "Lz4StreamReaderOpen: LZ4 decomp context failed");
    }

    r->compBuf = (uint8_t*)MemMalloc("Lz4StreamReaderComp", LZ4_STREAM_CHUNK_SIZE);
    r->decBuf  = (uint8_t*)MemMalloc("Lz4StreamReaderDec",  LZ4_STREAM_CHUNK_SIZE);
    if (!r->compBuf || !r->decBuf)
        Fatal(FATAL_ALLOCATION_FAILED, "Lz4StreamReaderOpen: cannot allocate buffers");

    return r;
}

/*
** Function: RefillDecBuf
** @brief    Refills r->decBuf by decompressing the next chunk, reading more
**           compressed bytes from disk as needed.
** @param    r - the reader to refill
** @return   true if decBuf now has bytes available; false at end of stream.
*/
static bool RefillDecBuf(Lz4StreamReader* r)
{
    while (r->decBufPos >= r->decBufFilled)
    {
        if (r->frameDone) return false;

        if (r->compBufPos >= r->compBufFilled)
        {
            r->compBufFilled = fread(r->compBuf, 1, LZ4_STREAM_CHUNK_SIZE, r->f);
            r->compBufPos    = 0;
            if (r->compBufFilled == 0) { r->frameDone = true; return false; }
        }

        size_t srcSize = r->compBufFilled - r->compBufPos;
        size_t dstSize = LZ4_STREAM_CHUNK_SIZE;
        size_t ret = LZ4F_decompress(r->dctx, r->decBuf, &dstSize,
                                     r->compBuf + r->compBufPos, &srcSize, nullptr);
        if (LZ4F_isError(ret))
        {
            Fatal(FATAL_FILE_OPEN, "Lz4StreamReader: LZ4 decompress error: %s", LZ4F_getErrorName(ret));
        }
        r->compBufPos   += srcSize;
        r->decBufPos     = 0;
        r->decBufFilled  = dstSize;
        if (ret == 0) r->frameDone = true;
        /* dstSize may be 0 (consuming LZ4 overhead); loop again either way. */
    }
    return true;
}

/*
** Function: Lz4StreamReaderRead
** @brief    Reads up to maxBytes decompressed bytes into outBuf.
** @param    r        - the reader to read from
** @param    outBuf   - destination buffer, at least maxBytes
** @param    maxBytes - maximum number of bytes to read
** @return   Number of bytes actually read; 0 means end of stream.
*/
size_t Lz4StreamReaderRead(Lz4StreamReader* r, void* outBuf, size_t maxBytes)
{
    uint8_t* out = (uint8_t*)outBuf;
    size_t   got = 0;
    while (got < maxBytes)
    {
        if (!RefillDecBuf(r)) break;
        size_t avail = r->decBufFilled - r->decBufPos;
        size_t take  = (maxBytes - got < avail) ? (maxBytes - got) : avail;
        memcpy(out + got, r->decBuf + r->decBufPos, take);
        r->decBufPos += take;
        got          += take;
    }
    return got;
}

/*
** Function: Lz4StreamReaderClose
** @brief    Closes and frees a reader, and nulls the caller's pointer to it.
** @param    ppReader - address of the reader pointer to close
*/
void Lz4StreamReaderClose(Lz4StreamReader** ppReader)
{
    if (!ppReader || !*ppReader) return;
    Lz4StreamReader* r = *ppReader;
    if (r->dctx) LZ4F_freeDecompressionContext(r->dctx);
    fclose(r->f);
    MemFree(r->compBuf);
    MemFree(r->decBuf);
    MemFree(r);
    *ppReader = nullptr;
}
