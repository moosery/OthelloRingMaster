/*
** Filename:  FileAndDirUtils.cpp
**
** Purpose:
**   Implements the helpers declared in FileAndDirUtils.h:
**     - CreateFullPath walks a path string component by component, calling
**       _mkdir on each directory prefix in turn so every missing
**       intermediate directory gets created, in order.
**     - FileCloseOrFatal, FileDeleteWithRetry/FileDeleteOrFatal and
**       FileWriteSentinel/FileWriteSentinelOrFatal are the checked
**       close/delete/marker-write routines (see the header's Notes).
*/

/* Includes */
#include <stdio.h>
#include <direct.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <string.h>
#include <errno.h>
#include <windows.h>
#include "Error.h"
#include "Utility.h"

/* Functions */
static bool createPath(char* pPathToCreate);   /* forward declaration -- defined after CreateFullPath below, sorted by name among this file's helpers */

/*
** Function: CreateFullPath
** @brief    Creates every missing directory component of pszFullPath, in
**           order, so the full path exists afterward.
** @details  Scans pszFullPath one character at a time, copying it into
**           tempName. Each time a path separator (or the terminating NUL)
**           is reached, tempName up to that point is a complete directory
**           prefix, so createPath is called on it -- meaning a path like
**           "C:\a\b\c" triggers _mkdir on "C:\a", then "C:\a\b", then
**           "C:\a\b\c" in turn. A drive letter prefix ("C:\") is copied
**           verbatim up front, since it is never itself a directory to create.
** @param    pszFullPath - the full path whose directory components should exist
** @return   true if the full path exists (or was created) afterward; false on failure.
*/
bool CreateFullPath(const char* pszFullPath)
{
    bool    result                             = true;                 /* overall success/failure of the whole path creation         */
    char    tempName[MAX_FULL_PATH_NAME + 1];                          /* path built up one component at a time                      */
    size_t  nameLen                            = strlen(pszFullPath);  /* length of the input path                                   */
    int     currIdx                            = 0;                    /* current scan position within pszFullPath                   */
    bool    hadADirName                        = false;                /* true once tempName holds a real (non-empty) path component */

    memset(tempName, 0, sizeof(tempName));

    /* Reject up front rather than overflowing tempName partway through the scan. */
    if (nameLen > MAX_FULL_PATH_NAME)
    {
        result = false;
        Error(UTIL_RC_Path_Too_Long, "The path '%s' is larger than allowed (%zu)\n", pszFullPath, (size_t)MAX_FULL_PATH_NAME);
    }
    else
    {
        /* A drive letter prefix ("C:" or "C:\") is copied verbatim and
        ** skipped over -- it is never itself a directory to create.
        */
        if (nameLen >= 2 && pszFullPath[1] == ':')
        {
            currIdx = 2;
            tempName[0] = pszFullPath[0];
            tempName[1] = pszFullPath[1];
            if (nameLen >= 3 && (pszFullPath[2] == '\\' || pszFullPath[2] == '/'))
            {
                currIdx = 3;
                tempName[2] = '\\';
            }
        }
    }

    for (; currIdx <= nameLen && result; currIdx++)
    {
        switch (pszFullPath[currIdx])
        {
            /* End of string or a path separator both mark the end of a
            ** directory component -- create everything gathered so far.
            */
            case '\0':
            case '\\':
            case '/':
                if (hadADirName)
                    result = createPath(tempName);
                tempName[currIdx] = '\\';
                break;
            default:
                tempName[currIdx] = pszFullPath[currIdx];
                hadADirName = true;
        }
    }

    return result;
}

/*
** Function: createPath
** @brief    Creates a single directory, treating "already exists" as success.
** @param    pPathToCreate - the directory path to create
** @return   true on success or if the directory already existed; false otherwise.
*/
static bool createPath(char* pPathToCreate)
{
    int  mkDirResult = _mkdir(pPathToCreate);
    bool result      = true;

    /* EEXIST means another component (or a previous run) already created
    ** this directory -- that is success, not failure, for our purposes.
    */
    if (mkDirResult != 0 && errno != EEXIST)
    {
        Error(UTIL_RC_Could_Not_Create_Directory, "The directory '%s' cannot be created (%d)", pPathToCreate, errno);
        result = false;
    }

    return result;
}

/*
** Function: FileCloseOrFatal
** @brief    Flushes and closes a buffered write stream, stopping the process
**           if the flush, an earlier buffered write error, or the close fails.
** @details  With a buffered stream, fwrite succeeding only means the bytes
**           reached the C runtime's buffer; the real disk write (and any
**           disk-full or dropped-share error) happens at flush/close. So the
**           flush and close results, plus the stream's sticky error flag,
**           are the only place such a failure shows up -- and when the file's
**           trailer is still in that buffer, ignoring them leaves a file with
**           no trailer while the caller believes it is complete.
** @param    pf      - the stream to close
** @param    pszPath - the file's path, for the failure message
*/
void FileCloseOrFatal(FILE* pf, const char* pszPath)
{
    int flushResult = 0;   /* 0 if the flush succeeded, EOF otherwise                          */
    int flushErrno  = 0;   /* errno right after the flush, before any other call can change it  */
    int hadError    = 0;   /* non-zero if the stream had recorded an error at any point         */
    int closeResult = 0;   /* 0 if the close succeeded, EOF otherwise                          */
    int closeErrno  = 0;   /* errno right after the close                                       */

    /* fflush pushes any buffered bytes to the OS now, so a failure surfaces here. */
    flushResult = fflush(pf);
    flushErrno  = errno;

    /* The sticky error flag catches an earlier failed buffered write too. */
    hadError = ferror(pf);

    /* fclose flushes once more and releases the handle; its own failure is checked too. */
    closeResult = fclose(pf);
    closeErrno  = errno;

    if (flushResult != 0 || hadError != 0 || closeResult != 0)
        Fatal(FATAL_FILE_WRITE_FAILED,
              "Writing '%s' failed at close (flush=%d errno=%d, stream error=%d, close=%d errno=%d) -- "
              "the file may be missing its trailer or truncated; nothing built from it can be trusted\n",
              pszPath, flushResult, flushErrno, hadError, closeResult, closeErrno);
}

/*
** Function: FileDeleteOrFatal
** @brief    Deletes a file that MUST go away, retrying through short locks,
**           and stops the process (naming the file and the reason) if it cannot.
** @details  Ten attempts with pauses growing from 0.25 s to 30 s (about a
**           minute and a half in all) ride out an antivirus scan or indexer
**           holding a large file. A file that still cannot be removed is a
**           real problem: end-of-level merges find their inputs by scanning
**           directories, so a leftover file would later be merged into the
**           wrong level.
** @param    pszPath - full path of the file to delete
** @param    pszWhat - what the file is, for the failure message (e.g. "merged input")
*/
void FileDeleteOrFatal(const char* pszPath, const char* pszWhat)
{
    unsigned long lastError = 0;   /* Windows error from the last failed attempt */

    if (!FileDeleteWithRetry(pszPath, 10, &lastError))
        Fatal(FATAL_FILE_DELETE_FAILED,
              "Cannot delete %s '%s' (Windows error %lu) after 10 attempts over about 90 seconds. "
              "Something (a virus scanner, indexer or another program) is holding it open. "
              "It must be removed -- a file left behind could be merged into a later level. "
              "Close whatever has it open, then restart.\n",
              pszWhat, pszPath, lastError);
}

/*
** Function: FileDeleteWithRetry
** @brief    Deletes a file, retrying with growing pauses if it is briefly
**           locked; a file that is already gone counts as success.
** @param    pszPath     - full path of the file to delete
** @param    maxAttempts - total delete attempts (pauses of 0.25 s doubling up to 30 s between them)
** @param    pLastError  - out (optional): the Windows error from the last failed attempt
** @return   true if the file no longer exists afterward; false if it could not be removed.
*/
bool FileDeleteWithRetry(const char* pszPath, int maxAttempts, unsigned long* pLastError)
{
    DWORD delayMs   = 250;     /* pause before the next attempt; doubles each time up to the cap */
    DWORD lastError = 0;       /* Windows error from the most recent failed attempt              */
    bool  deleted   = false;   /* true once the file is confirmed gone                           */

    for (int attempt = 0; attempt < maxAttempts && !deleted; attempt++)
    {
        /* Pause before every attempt but the first: whatever is holding the
        ** file (a scanner, an indexer) normally lets go within seconds.
        */
        if (attempt > 0)
        {
            Sleep(delayMs);
            delayMs = (delayMs >= 15000) ? 30000 : delayMs * 2;
        }

        if (DeleteFileA(pszPath))
        {
            deleted = true;
            continue;
        }

        lastError = GetLastError();

        /* Already gone -- someone else removed it -- is the outcome we wanted. */
        if (lastError == ERROR_FILE_NOT_FOUND || lastError == ERROR_PATH_NOT_FOUND)
        {
            deleted = true;
            continue;
        }

        /* Access denied is often just a read-only attribute: clear it and
        ** try once more straight away rather than burning a paused attempt.
        */
        if (lastError == ERROR_ACCESS_DENIED && SetFileAttributesA(pszPath, FILE_ATTRIBUTE_NORMAL))
        {
            if (DeleteFileA(pszPath))
            {
                deleted = true;
                continue;
            }

            lastError = GetLastError();
            if (lastError == ERROR_FILE_NOT_FOUND || lastError == ERROR_PATH_NOT_FOUND)
                deleted = true;
        }
    }

    if (!deleted && pLastError != nullptr)
        *pLastError = (unsigned long)lastError;

    return deleted;
}

/*
** Function: FileWriteSentinel
** @brief    Writes a small marker file from up to two byte ranges, checking
**           every step (create, each write, flush to disk, close). On any
**           failure the partial file is removed so no half-written marker
**           survives.
** @details  A marker that is present but truncated is worse than one that
**           is absent: readers treat a present marker as evidence of a state
**           (a level complete, a merge in progress) and may act on a payload
**           that was never fully written. The flush to disk (not just the
**           OS cache) matters most for the checkpoint and the merge-in-
**           progress marker, whose whole purpose is to survive a crash.
** @param    pszPath    - full path of the file to create (overwritten if it exists)
** @param    pPart1     - first byte range (may be NULL if part1Bytes is 0)
** @param    part1Bytes - length of the first range
** @param    pPart2     - second byte range (may be NULL if part2Bytes is 0)
** @param    part2Bytes - length of the second range
** @param    pLastError - out (optional): the Windows error from the failing step
** @return   true if the whole file was written, flushed and closed; false otherwise.
*/
bool FileWriteSentinel(const char* pszPath, const void* pPart1, size_t part1Bytes,
                       const void* pPart2, size_t part2Bytes, unsigned long* pLastError)
{
    HANDLE hFile     = INVALID_HANDLE_VALUE;   /* the marker file being written                                  */
    DWORD  written   = 0;                      /* bytes accepted by the most recent WriteFile                    */
    DWORD  failError = 0;                      /* Windows error from whichever step failed                       */
    bool   ok        = true;                   /* still true while every step so far has succeeded               */

    hFile = CreateFileA(pszPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE)
    {
        if (pLastError != nullptr)
            *pLastError = (unsigned long)GetLastError();
        return false;
    }

    /* Each write must both succeed and accept every byte offered. */
    if (ok && part1Bytes > 0 && (!WriteFile(hFile, pPart1, (DWORD)part1Bytes, &written, nullptr) || written != (DWORD)part1Bytes))
    {
        failError = GetLastError();
        ok        = false;
    }

    if (ok && part2Bytes > 0 && (!WriteFile(hFile, pPart2, (DWORD)part2Bytes, &written, nullptr) || written != (DWORD)part2Bytes))
    {
        failError = GetLastError();
        ok        = false;
    }

    /* Push the bytes to the disk itself, not just the OS cache. */
    if (ok && !FlushFileBuffers(hFile))
    {
        failError = GetLastError();
        ok        = false;
    }

    /* A failed close can still mean lost data (a network share reporting
    ** the error only at close), so it counts -- but an earlier failure's
    ** error code is the more useful one to report.
    */
    if (!CloseHandle(hFile) && ok)
    {
        failError = GetLastError();
        ok        = false;
    }

    /* Never leave a half-written marker behind. The delete is best effort:
    ** if it also fails there is nothing more to do here, and the caller
    ** already treats this whole operation as failed.
    */
    if (!ok)
        (void)DeleteFileA(pszPath);

    if (!ok && pLastError != nullptr)
        *pLastError = (unsigned long)failError;

    return ok;
}

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
                              const void* pPart2, size_t part2Bytes, const char* pszWhat)
{
    unsigned long lastError = 0;   /* Windows error from the last failed attempt */

    for (int attempt = 0; attempt < 3; attempt++)
    {
        /* A share that dropped for a moment usually answers again within seconds. */
        if (attempt > 0)
            Sleep(2000);

        if (FileWriteSentinel(pszPath, pPart1, part1Bytes, pPart2, part2Bytes, &lastError))
            return;
    }

    Fatal(FATAL_FILE_WRITE_FAILED,
          "Cannot write %s '%s' (Windows error %lu) after 3 attempts -- the drive holding it is failing or unreachable. "
          "Continuing without it would leave the on-disk state saying something untrue.\n",
          pszWhat, pszPath, lastError);
}
