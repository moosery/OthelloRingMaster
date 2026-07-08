/*
** Filename:  Lz4Stream.h
**
** Purpose:
**   Declares Lz4StreamWriter/Lz4StreamReader: a streaming, bounded-memory
**   LZ4-frame compressor/decompressor for callers that produce or consume
**   records one at a time and must never require a whole file (or even a
**   whole raw intermediate file) to fit in memory or land on disk
**   uncompressed first. Unlike Utility/RingStoreFile.h's UINT64_PAIR-
**   specific delta+varint+LZ4 encoding, this treats the byte stream as
**   opaque -- no delta encoding, just LZ4 framing over whatever bytes
**   Lz4StreamWriterWrite is given.
**
** Notes:
**   Buffers up to LZ4_STREAM_CHUNK_SIZE raw bytes before compressing a
**   chunk (write side) or decompressing the next chunk on demand (read
**   side) -- mirrors the same buffer-and-flush-on-full discipline
**   Utility/RingStoreFile.h's compressed writer/reader already use, just
**   without the delta+varint layer in between.
*/

#pragma once

/* Includes */
#include <stdint.h>
#include <stddef.h>

/* Structures and Types */

/*
** Type:    Lz4StreamWriter
** @brief   Opaque streaming LZ4-frame writer.
*/
typedef struct __Lz4StreamWriter Lz4StreamWriter;

/*
** Type:    Lz4StreamReader
** @brief   Opaque streaming LZ4-frame reader.
*/
typedef struct __Lz4StreamReader Lz4StreamReader;

/* Constants */
#define LZ4_STREAM_CHUNK_SIZE (256 * 1024)

/* Functions */

/*
** Function: Lz4StreamWriterOpen
** @brief    Opens path for streaming LZ4-frame-compressed output.
** @param    path - file path to create (overwritten if it exists)
** @return   A new Lz4StreamWriter. Fatals on failure (never returns nullptr).
*/
Lz4StreamWriter* Lz4StreamWriterOpen(const char* path);

/*
** Function: Lz4StreamWriterWrite
** @brief    Appends size bytes of data to the stream, compressing and
**           flushing a chunk whenever the internal buffer fills.
** @param    pw   - the writer to append to
** @param    data - bytes to write
** @param    size - number of bytes in data
*/
void Lz4StreamWriterWrite(Lz4StreamWriter* pw, const void* data, size_t size);

/*
** Function: Lz4StreamWriterClose
** @brief    Flushes any buffered bytes, ends the LZ4 frame, closes the
**           writer, and frees it.
** @param    pw - the writer to close (no longer valid after this call)
*/
void Lz4StreamWriterClose(Lz4StreamWriter* pw);

/*
** Function: Lz4StreamReaderOpen
** @brief    Opens a file written by Lz4StreamWriter for sequential reading.
** @param    path - file path to open for reading
** @return   A new Lz4StreamReader, or nullptr if the file is missing or corrupt.
*/
Lz4StreamReader* Lz4StreamReaderOpen(const char* path);

/*
** Function: Lz4StreamReaderRead
** @brief    Reads up to maxBytes decompressed bytes into outBuf.
** @param    r        - the reader to read from
** @param    outBuf   - destination buffer, at least maxBytes
** @param    maxBytes - maximum number of bytes to read
** @return   Number of bytes actually read; 0 means end of stream.
*/
size_t Lz4StreamReaderRead(Lz4StreamReader* r, void* outBuf, size_t maxBytes);

/*
** Function: Lz4StreamReaderClose
** @brief    Closes and frees a reader, and nulls the caller's pointer to it.
** @param    ppReader - address of the reader pointer to close
*/
void Lz4StreamReaderClose(Lz4StreamReader** ppReader);
