/*
** Filename:  Error.cpp
**
** Purpose:
**   Implements the recoverable-error (Error/ErrorGetLast/ErrorGetLastReason/
**   ErrorPrint) and fatal-termination (Fatal) functions declared in Error.h.
**   Recoverable error state is thread-local so concurrent threads never
**   clobber each other's last-error/reason.
*/

/* Includes */
#include <stdarg.h>
#include "Error.h"
#include <stdio.h>
#include <stdlib.h>

/* Constants */
#define MAX_ERROR_LEN 4095

/* Globals */
thread_local RC   lastError = RC_SUCCESS;
thread_local char errorReason[MAX_ERROR_LEN + 1];

/* Functions */

/*
** Function: Error
** @brief    Records a recoverable error and a formatted reason string on the
**           calling thread, retrievable later via ErrorGetLast/ErrorGetLastReason.
** @param    error       - the RC error code to record
** @param    pszReasonFmt - printf-style format string describing the error
** @param    ...         - format arguments for pszReasonFmt
*/
void Error(RC error, const char* pszReasonFmt, ...)
{
    va_list argptr;

    lastError = error;
    va_start(argptr, pszReasonFmt);
    vsnprintf(errorReason, sizeof(errorReason), pszReasonFmt, argptr);
    va_end(argptr);
}

/*
** Function: ErrorGetLast
** @brief    Returns the last error code recorded by Error() on this thread.
** @return   The last RC recorded, or RC_SUCCESS if none has been recorded.
*/
RC ErrorGetLast()
{
    return lastError;
}

/*
** Function: ErrorGetLastReason
** @brief    Returns the formatted reason string from the last Error() call
**           on this thread.
** @return   Pointer to a thread-local buffer; valid until the next Error() call on this thread.
*/
char* ErrorGetLastReason()
{
    return errorReason;
}

/*
** Function: ErrorPrint
** @brief    Prints the last recorded error code and reason string.
** @param    fpOut - stream to print to
*/
void ErrorPrint(FILE* fpOut)
{
    fprintf(fpOut, "(%zu): %s\n", lastError, errorReason);
}

/*
** Function: Fatal
** @brief    Prints a formatted message to stderr and terminates the process
**           immediately with rc as the exit code. Never returns.
** @param    rc          - exit code to terminate with
** @param    pszReasonFmt - printf-style format string describing the fatal condition
** @param    ...         - format arguments for pszReasonFmt
*/
void Fatal(RC rc, const char* pszReasonFmt, ...)
{
    va_list argptr;

    va_start(argptr, pszReasonFmt);
    vfprintf(stderr, pszReasonFmt, argptr);
    va_end(argptr);

    /* Ensure the message reaches the console/log before the process dies. */
    fflush(stderr);
    exit((int)rc);
}
