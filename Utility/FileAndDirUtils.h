/*
** Filename:  FileAndDirUtils.h
**
** Purpose:
**   Declares the file and directory helpers shared across the solution:
**     - CreateFullPath, a Windows equivalent of "mkdir -p": creates every
**       missing intermediate directory component of a path, one path
**       component at a time.
**     - FileDeleteWithRetry / FileDeleteOrFatal: delete a file, riding out
**       the short-lived locks that antivirus scanners and indexers place on
**       large files, and stop the process (rather than carry on) if the
**       file genuinely cannot be removed.
**     - FileWriteSentinel / FileWriteSentinelOrFatal: write a small marker
**       file (a sentinel or checkpoint) with every step checked and flushed,
**       and never leave a half-written marker behind.
**     - FileCloseOrFatal: close a buffered write stream and stop the process
**       if its final flush or close fails.
**
** Notes:
**   The checked helpers exist because Windows and the C runtime report a
**   failed delete, write or close only through a return value, and every one
**   of those failures used to be silently discarded. A delete that quietly
**   fails leaves a stale file that a later directory scan will merge into the
**   wrong level; a write or close that quietly fails leaves a file with no
**   trailer (or a marker that says something untrue), and the inputs it was
**   built from may already be gone. Stopping loudly at the moment of failure
**   is always better than either.
*/

#pragma once

/* Includes */
#include <stdio.h>
#include <stddef.h>

/* Macros and Defines */
#define MAX_FULL_PATH_NAME 4000

/* Functions */

/*
** Function: CreateFullPath
** @brief    Creates every missing directory component of pszFullPath, in
**           order, so the full path exists afterward.
** @param    pszFullPath - the full path whose directory components should exist
** @return   true if the full path exists (or was created) afterward; false on failure.
*/
bool CreateFullPath(const char* pszFullPath);

/*
** Function: FileDeleteWithRetry
** @brief    Deletes a file, retrying with growing pauses if it is briefly
**           locked; a file that is already gone counts as success.
** @param    pszPath     - full path of the file to delete
** @param    maxAttempts - total delete attempts (pauses of 0.25 s doubling up to 30 s between them)
** @param    pLastError  - out (optional): the Windows error from the last failed attempt
** @return   true if the file no longer exists afterward; false if it could not be removed.
*/
bool FileDeleteWithRetry(const char* pszPath, int maxAttempts, unsigned long* pLastError);

/*
** Function: FileDeleteOrFatal
** @brief    Deletes a file that MUST go away, retrying through short locks,
**           and stops the process (naming the file and the reason) if it cannot.
** @param    pszPath - full path of the file to delete
** @param    pszWhat - what the file is, for the failure message (e.g. "merged input")
*/
void FileDeleteOrFatal(const char* pszPath, const char* pszWhat);

/*
** Function: FileWriteSentinel
** @brief    Writes a small marker file from up to two byte ranges, checking
**           every step (create, each write, flush to disk, close). On any
**           failure the partial file is removed so no half-written marker
**           survives.
** @param    pszPath    - full path of the file to create (overwritten if it exists)
** @param    pPart1     - first byte range (may be NULL if part1Bytes is 0)
** @param    part1Bytes - length of the first range
** @param    pPart2     - second byte range (may be NULL if part2Bytes is 0)
** @param    part2Bytes - length of the second range
** @param    pLastError - out (optional): the Windows error from the failing step
** @return   true if the whole file was written, flushed and closed; false otherwise.
*/
bool FileWriteSentinel(const char* pszPath, const void* pPart1, size_t part1Bytes,
                       const void* pPart2, size_t part2Bytes, unsigned long* pLastError);

/*
** Function: FileWriteSentinelOrFatal
** @brief    FileWriteSentinel with a few spaced retries (a network share may
**           be briefly unreachable), then stops the process if it still fails.
** @param    pszPath    - full path of the file to create
** @param    pPart1     - first byte range
** @param    part1Bytes - length of the first range
** @param    pPart2     - second byte range
** @param    part2Bytes - length of the second range
** @param    pszWhat    - what the marker is, for the failure message (e.g. "the _merging sentinel")
*/
void FileWriteSentinelOrFatal(const char* pszPath, const void* pPart1, size_t part1Bytes,
                              const void* pPart2, size_t part2Bytes, const char* pszWhat);

/*
** Function: FileCloseOrFatal
** @brief    Flushes and closes a buffered write stream, stopping the process
**           if the flush, an earlier buffered write error, or the close fails.
** @param    pf      - the stream to close
** @param    pszPath - the file's path, for the failure message
*/
void FileCloseOrFatal(FILE* pf, const char* pszPath);
