/*
** Filename:  Logger.cpp
**
** Purpose:
**   Implements the dual-output logger (LoggerInit/LoggerLog/LoggerLogFileOnly)
**   declared in Logger.h.
**
** Notes:
**   Every write's result is checked. A logger that keeps returning success
**   after its log file has stopped accepting data (disk full, network share
**   dropped, handle broken) makes a dead run look like a quiet one -- the
**   log simply ends with nothing to say why. So a failed write is reported
**   once per destination on stderr, which is the one stream that does not
**   depend on the destination that just failed.
*/

/* Includes */
#include <stdio.h>
#include <stdarg.h>
#include <errno.h>
#include <share.h>
#include <atomic>
#include "Utility.h"

/* Globals */
static FILE*             g_filePtr                     = nullptr;   /* open handle to the current log file, or nullptr if none is open      */
static char              g_logFileName[MAX_FULL_PATH_NAME] = { 0 }; /* path of the current log file, for messages/re-init reporting         */
static std::atomic<bool> g_stdoutFailureReported{ false };          /* true once a failed stdout write has been reported (report only once) */
static std::atomic<bool> g_fileFailureReported{ false };            /* true once a failed log-file write has been reported (report only once) */

/* Functions */
static void noteLogWriteFailure(const char* pszDestination, std::atomic<bool>& alreadyReported);   /* forward declaration -- defined after LoggerLogFileOnly below, sorted by name among this file's helpers */

/*
** Function: LoggerInit
** @brief    Opens logFileName for writing as the destination for LoggerLog.
**           If a log file is already open, it is closed first. If
**           logFileName is nullptr, logging to a file is disabled (LoggerLog
**           still writes to stdout).
** @param    logFileName - path of the log file to open, or nullptr to disable file logging
*/
void LoggerInit(const char* logFileName)
{
    /* Re-init: close whatever file is currently open before switching. */
    if (g_filePtr != nullptr)
    {
        fprintf(stderr, "LoggerInit: Logger already initialized with file '%s'; closing previous file\n", g_logFileName);
        (void)fclose(g_filePtr);
        g_filePtr = nullptr;
    }

    /* A fresh file gets a fresh chance to report a write failure. */
    g_fileFailureReported = false;

    if (logFileName != nullptr)
    {
        snprintf(g_logFileName, MAX_FULL_PATH_NAME, "%s", logFileName);

        /* _SH_DENYNO: allow other processes/handles to read the log file
        ** concurrently while this process is still writing to it.
        */
        g_filePtr = _fsopen(g_logFileName, "w", _SH_DENYNO);
        if (g_filePtr == nullptr)
        {
            fprintf(stderr, "LoggerInit: Failed to open log file '%s' for writing\n", g_logFileName);
        }
    }
    else
    {
        fprintf(stderr, "LoggerInit: No log file name provided; logging disabled\n");
    }
}

/*
** Function: LoggerLog
** @brief    Writes a formatted message to stdout, and also to the log file
**           if LoggerInit successfully opened one.
** @details  Writes to stdout whether or not a log file is open -- before
**           LoggerInit runs (or when file logging is disabled) messages
**           still reach the console, as Logger.h documents, instead of
**           being dropped without a trace. Each destination's write and
**           flush results are checked; a failure is reported once on
**           stderr (see noteLogWriteFailure) and the other destination is
**           still written.
** @param    format - printf-style format string
** @param    ...    - format arguments for format
*/
void LoggerLog(const char* format, ...)
{
    va_list args;    /* argument list consumed by the stdout write               */
    va_list args2;   /* independent copy of the arguments for the log-file write */

    /* Two independent va_list walks are needed since vfprintf consumes
    ** its va_list -- args2 lets the same arguments be written twice
    ** (stdout, then the log file).
    */
    va_start(args, format);
    va_copy(args2, args);

    /* A negative vfprintf result or a non-zero fflush means the console
    ** did not take the text -- say so once rather than fail silently.
    */
    if (vfprintf(stdout, format, args) < 0 || fflush(stdout) != 0)
        noteLogWriteFailure("stdout", g_stdoutFailureReported);

    /* The log file may not exist (logging disabled, or LoggerInit not yet
    ** called); that is the only case where skipping it is correct.
    */
    if (g_filePtr != nullptr)
    {
        /* Same check for the persisted log -- this is the write whose
        ** failure most needs to be visible, since nothing else would
        ** ever reveal that the log stopped growing.
        */
        if (vfprintf(g_filePtr, format, args2) < 0 || fflush(g_filePtr) != 0)
            noteLogWriteFailure(g_logFileName, g_fileFailureReported);
    }

    va_end(args2);
    va_end(args);
}

/*
** Function: LoggerLogFileOnly
** @brief    Writes a formatted message to the log file only, never to stdout.
** @details  For callers that have already put the same text on the console
**           by another route (Fatal() prints on stderr) and only need it
**           to also be in the persisted log, so the console does not show
**           the message twice. A no-op if no log file is open.
** @param    format - printf-style format string
** @param    ...    - format arguments for format
*/
void LoggerLogFileOnly(const char* format, ...)
{
    va_list args;   /* argument list for the log-file write */

    /* No log file means there is nowhere to persist the text -- nothing to do. */
    if (g_filePtr == nullptr)
        return;

    va_start(args, format);

    /* Same write/flush check as LoggerLog: a failure here is reported once
    ** on stderr instead of being lost.
    */
    if (vfprintf(g_filePtr, format, args) < 0 || fflush(g_filePtr) != 0)
        noteLogWriteFailure(g_logFileName, g_fileFailureReported);

    va_end(args);
}

/*
** Function: noteLogWriteFailure
** @brief    Reports, once per destination, that a log write failed.
** @details  Uses stderr, the one stream that does not depend on the
**           destination that just failed. Reported only once so a dead
**           destination does not turn every later log line into another
**           error line.
** @param    pszDestination - "stdout" or the log file's path, for the message
** @param    alreadyReported - the once-only flag for this destination; set by this call
*/
static void noteLogWriteFailure(const char* pszDestination, std::atomic<bool>& alreadyReported)
{
    int savedErrno = errno;   /* errno left by the failed write, captured before any other call can change it */

    /* exchange() returns the previous value, so exactly one thread sees
    ** false and reports; every later failure (and any concurrent one)
    ** stays quiet.
    */
    if (alreadyReported.exchange(true))
        return;

    fprintf(stderr, "LOGGER: writing to '%s' failed (errno=%d). Later output to it may be missing -- "
                    "the log for this run may be incomplete. (Reported once.)\n",
            pszDestination, savedErrno);
    (void)fflush(stderr);
}
