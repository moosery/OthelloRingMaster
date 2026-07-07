/*
** Filename:  Logger.cpp
**
** Purpose:
**   Implements the dual-output logger (LoggerInit/LoggerLog) declared in
**   Logger.h.
*/

/* Includes */
#include <stdio.h>
#include <stdarg.h>
#include <share.h>
#include "Utility.h"

/* Globals */
static FILE* g_filePtr                     = nullptr;     /* open handle to the current log file, or nullptr if none is open */
static char  g_logFileName[MAX_FULL_PATH_NAME] = { 0 };   /* path of the current log file, for messages/re-init reporting    */

/* Functions */

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
        fclose(g_filePtr);
        g_filePtr = nullptr;
    }

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
** @param    format - printf-style format string
** @param    ...    - format arguments for format
*/
void LoggerLog(const char* format, ...)
{
    if (g_filePtr != nullptr)
    {
        va_list args, args2;

        /* Two independent va_list walks are needed since vfprintf consumes
        ** its va_list -- args2 lets the same arguments be written twice
        ** (stdout, then the log file).
        */
        va_start(args, format);
        va_copy(args2, args);
        vfprintf(stdout, format, args);
        fflush(stdout);

        if (g_filePtr != nullptr)
        {
            vfprintf(g_filePtr, format, args2);
            fflush(g_filePtr);
        }
        va_end(args2);
        va_end(args);
    }
}
