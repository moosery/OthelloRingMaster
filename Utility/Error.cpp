/*
** Filename:  Error.cpp
**
** Purpose:
**   Implements the recoverable-error (Error/ErrorGetLast/ErrorGetLastReason/
**   ErrorPrint) and fatal-termination (Fatal) functions declared in Error.h,
**   plus CrashHandlerInstall, which makes a native crash leave a readable
**   record in the log instead of a log that just stops.
**   Recoverable error state is thread-local so concurrent threads never
**   clobber each other's last-error/reason.
**
** Notes:
**   The crash-handling code deliberately uses only raw Win32 calls and
**   static buffers -- no malloc, no printf family, no stdio. A native crash
**   can mean the heap or the C runtime's own locks are damaged (a real
**   crash in this project was a corrupted stdio lock), so anything that
**   allocates or takes a C runtime lock could hang or fault again exactly
**   when its output is needed. Windows Error Reporting is left to write the
**   crash dump as before; the handler only adds text to the log.
*/

/* Includes */
#include <stdarg.h>
#include "Error.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <signal.h>
#include <exception>
#include <windows.h>
#include <intrin.h>
#include "Logger.h"

/* Macros and Defines */
#define MAX_ERROR_LEN         4095
#define MAX_FATAL_LEN         8191   /* longest fully formatted Fatal() message kept before truncation                      */
#define CRASH_TEXT_MAX        16384  /* size of the static buffer a crash report is assembled in                           */
#define CRASH_PATH_MAX        1024   /* longest log path the crash handler remembers                                       */
#define CRASH_STACK_BYTES     8192   /* how much of the faulting thread's stack is copied out and scanned                  */
#define CRASH_MAX_CANDIDATES  96     /* most return-address candidates listed in one report                                */

/* Globals */
thread_local RC   lastError = RC_SUCCESS;
thread_local char errorReason[MAX_ERROR_LEN + 1];

static char           g_crashLogPath[CRASH_PATH_MAX] = { 0 };   /* log file the crash report is appended to; empty = console only          */
static volatile LONG  g_crashReportStarted           = 0;       /* 0 until the first crash report begins; guards against re-entry          */
static char           g_crashText[CRASH_TEXT_MAX];              /* the crash report as it is being assembled (only touched under the guard) */
static size_t         g_crashTextLen                 = 0;       /* bytes of g_crashText filled so far                                       */
static unsigned char  g_crashStack[CRASH_STACK_BYTES];          /* copy of the faulting thread's stack (only touched under the guard)       */

/* Functions */
static void  crashAppend(const char* pszText);                                              /* forward declarations -- defined after CrashHandlerInstall below, sorted by name among this file's helpers */
static void  crashAppendDec(uint64_t value);
static void  crashAppendHex(uint64_t value, int minDigits);
static void  crashAppendModuleOffset(uint64_t address);
static void  crashAppendTimestamp();
static void  crashEmit();
static const char* crashExceptionName(DWORD code);
static LONG WINAPI crashFilter(EXCEPTION_POINTERS* pException);
static void  crashLine(const char* pszWhat);
static void  crashOnAbort(int signalNumber);
static void  crashOnInvalidParameter(const wchar_t* pExpression, const wchar_t* pFunction,
                                     const wchar_t* pFile, unsigned int line, uintptr_t reserved);
static void  crashOnTerminate();

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
** @brief    Prints a timestamp-prefixed, formatted message to stderr, also
**           writes it to the log file, and terminates the process
**           immediately with rc as the exit code. Never returns.
** @param    rc          - exit code to terminate with
** @param    pszReasonFmt - printf-style format string describing the fatal condition
** @param    ...         - format arguments for pszReasonFmt
** @details  The timestamp is applied here, once, so every existing and future
**           Fatal() call site gets it for free -- without it, a Fatal buried
**           in a multi-hour/day real run (this project's normal timescale)
**           gives no way to correlate the failure against other logs
**           (Windows Event Viewer, network drop times, etc.) after the fact.
**           The message is formatted once into a local buffer so the same
**           text goes to both destinations. It used to reach only the
**           console, so a run that Fataled left a log that simply ended,
**           with the reason visible only to whoever happened to be watching
**           the console window.
*/
__declspec(noreturn) void Fatal(RC rc, const char* pszReasonFmt, ...)
{
    SYSTEMTIME  st            = {};   /* wall-clock time of the failure, for the message prefix         */
    char        message[MAX_FATAL_LEN + 1];   /* the complete timestamp-prefixed message              */
    int         prefixLen     = 0;    /* characters the timestamp prefix took                           */
    size_t      messageLen    = 0;    /* total characters in message, to test for a trailing newline    */
    va_list     argptr;               /* the caller's variable arguments                                */

    GetLocalTime(&st);
    prefixLen = snprintf(message, sizeof(message), "[%04d-%02d-%02d %02d:%02d:%02d] ",
                         st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    if (prefixLen < 0 || prefixLen >= (int)sizeof(message))
        prefixLen = 0;

    va_start(argptr, pszReasonFmt);
    vsnprintf(message + prefixLen, sizeof(message) - (size_t)prefixLen, pszReasonFmt, argptr);
    va_end(argptr);

    /* Callers are inconsistent about ending their message with a newline;
    ** make sure the console and the log both end on a line boundary so
    ** the shell prompt or the next log line never lands mid-message.
    */
    messageLen = strlen(message);
    if (messageLen == 0 || message[messageLen - 1] != '\n')
    {
        if (messageLen < sizeof(message) - 1)
        {
            message[messageLen]     = '\n';
            message[messageLen + 1] = '\0';
        }
    }

    /* Console first -- it is the destination that has always worked. The
    ** results are deliberately not acted on: if stderr itself is gone there
    ** is no other stream left to report that on, and the log write below
    ** still happens.
    */
    (void)fputs(message, stderr);

    /* Ensure the message reaches the console before the process dies. */
    (void)fflush(stderr);

    /* Then the log file, so a run that Fataled leaves its reason in the
    ** persisted log, not just on a console window nobody may be watching.
    */
    LoggerLogFileOnly("FATAL (exit code %d): %s", (int)rc, message);

    exit((int)rc);
}

/*
** Function: CrashHandlerInstall
** @brief    Arranges for a native crash, an uncaught C++ exception, a C
**           runtime invalid-parameter failure or an abort() to leave a
**           readable record in the log file and on stderr.
** @details  Installs four hooks: an unhandled-exception filter (access
**           violations, invalid handles, heap corruption and the like), a
**           std::terminate handler, a C runtime invalid-parameter handler
**           and a SIGABRT handler. Each one only WRITES a record and then
**           lets the normal failure path continue, so Windows Error
**           Reporting still produces its crash dump exactly as before.
**           Failures that call __fastfail directly (the stack-buffer-
**           overrun class) cannot be intercepted by any handler; Fatal()
**           covers the failures this project raises deliberately.
** @param    pszLogPath - path of the log file the record is appended to; may be nullptr for console only
*/
void CrashHandlerInstall(const char* pszLogPath)
{
    /* Remember the log path. It is copied into a static buffer because the
    ** filter runs with no guarantee that the caller's string still exists.
    */
    if (pszLogPath != nullptr)
        snprintf(g_crashLogPath, sizeof(g_crashLogPath), "%s", pszLogPath);
    else
        g_crashLogPath[0] = '\0';

    /* Native faults: the hook that catches what the log could not explain. */
    SetUnhandledExceptionFilter(crashFilter);

    /* An uncaught C++ exception ends in std::terminate, which never
    ** reaches the exception filter.
    */
    std::set_terminate(crashOnTerminate);

    /* The C runtime's invalid-parameter path ends in a fast-fail with no
    ** message; log before letting it proceed.
    */
    _set_invalid_parameter_handler(crashOnInvalidParameter);

    /* abort() raises SIGABRT; log it, then let the default handling finish. */
    signal(SIGABRT, crashOnAbort);
}

/*
** Function: crashAppend
** @brief    Appends a NUL-terminated string to the crash report buffer,
**           silently truncating if the buffer is full.
** @param    pszText - the text to append
*/
static void crashAppend(const char* pszText)
{
    /* Copy one character at a time and stop when the buffer is full --
    ** no library call, so this is safe even with a damaged heap.
    */
    while (*pszText != '\0' && g_crashTextLen < CRASH_TEXT_MAX - 1)
        g_crashText[g_crashTextLen++] = *pszText++;

    g_crashText[g_crashTextLen] = '\0';
}

/*
** Function: crashAppendDec
** @brief    Appends an unsigned value in decimal to the crash report buffer.
** @param    value - the number to append
*/
static void crashAppendDec(uint64_t value)
{
    char digits[24];    /* the number's digits, built least significant first */
    int  count = 0;     /* how many digits were produced                      */

    /* A zero has no digits from the loop, so it is handled as its own case. */
    if (value == 0)
        digits[count++] = '0';

    while (value > 0 && count < (int)sizeof(digits))
    {
        digits[count++] = (char)('0' + (value % 10));
        value /= 10;
    }

    /* The digits were produced in reverse; append them back to front. */
    while (count > 0 && g_crashTextLen < CRASH_TEXT_MAX - 1)
        g_crashText[g_crashTextLen++] = digits[--count];

    g_crashText[g_crashTextLen] = '\0';
}

/*
** Function: crashAppendHex
** @brief    Appends an unsigned value in hexadecimal (no 0x prefix, upper
**           case) to the crash report buffer, zero-padded to a minimum width.
** @param    value     - the number to append
** @param    minDigits - minimum number of hex digits to emit (zero-padded)
*/
static void crashAppendHex(uint64_t value, int minDigits)
{
    static const char hexDigits[] = "0123456789ABCDEF";   /* digit lookup table                            */
    char              digits[20];                         /* the number's digits, most significant first   */
    int               count = 0;                          /* how many digits are in the buffer             */
    int               shift = 60;                         /* bit position of the next nibble to extract    */

    /* Walk from the top nibble down, skipping leading zeros until the
    ** minimum width says otherwise.
    */
    for (; shift >= 0; shift -= 4)
    {
        int nibble = (int)((value >> shift) & 0xF);

        if (count == 0 && nibble == 0 && (shift / 4) >= minDigits)
            continue;

        digits[count++] = hexDigits[nibble];
    }

    /* An all-zero value with a small minimum width still needs one digit. */
    if (count == 0)
        digits[count++] = '0';

    for (int i = 0; i < count && g_crashTextLen < CRASH_TEXT_MAX - 1; i++)
        g_crashText[g_crashTextLen++] = digits[i];

    g_crashText[g_crashTextLen] = '\0';
}

/*
** Function: crashAppendModuleOffset
** @brief    Appends an address as "module.ext+0xOFFSET" when it falls inside
**           a loaded module, or as a bare hex address when it does not.
** @details  The module-relative form is what lets a report be turned into
**           function names and line numbers later, using the symbol file
**           that matches the exact build that crashed.
** @param    address - the code address to describe
*/
static void crashAppendModuleOffset(uint64_t address)
{
    HMODULE  hModule        = nullptr;          /* module containing the address                       */
    char     path[MAX_PATH] = { 0 };            /* that module's full file path                        */
    DWORD    pathLen        = 0;                /* characters GetModuleFileNameA returned              */
    DWORD    nameStart      = 0;                /* index in path where the bare file name begins       */

    /* UNCHANGED_REFCOUNT avoids touching the module's reference count, so
    ** the lookup has no side effects on the loader state.
    */
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)(uintptr_t)address, &hModule) && hModule != nullptr)
    {
        pathLen = GetModuleFileNameA(hModule, path, MAX_PATH);
        if (pathLen > 0 && pathLen < MAX_PATH)
        {
            /* Keep only the file name; the directory is noise in a report. */
            for (DWORD i = 0; i < pathLen; i++)
            {
                if (path[i] == '\\' || path[i] == '/')
                    nameStart = i + 1;
            }

            crashAppend(path + nameStart);
            crashAppend("+0x");
            crashAppendHex(address - (uint64_t)(uintptr_t)hModule, 1);
            return;
        }
    }

    /* Not inside any loaded module (or the lookup failed): the raw address is all there is. */
    crashAppend("0x");
    crashAppendHex(address, 1);
    crashAppend(" (no module)");
}

/*
** Function: crashAppendTimestamp
** @brief    Appends the current local time as YYYY-MM-DD HH:MM:SS.
*/
static void crashAppendTimestamp()
{
    SYSTEMTIME st = {};   /* current local time */

    GetLocalTime(&st);

    crashAppendDec(st.wYear);
    crashAppend("-");
    if (st.wMonth < 10)  crashAppend("0");
    crashAppendDec(st.wMonth);
    crashAppend("-");
    if (st.wDay < 10)    crashAppend("0");
    crashAppendDec(st.wDay);
    crashAppend(" ");
    if (st.wHour < 10)   crashAppend("0");
    crashAppendDec(st.wHour);
    crashAppend(":");
    if (st.wMinute < 10) crashAppend("0");
    crashAppendDec(st.wMinute);
    crashAppend(":");
    if (st.wSecond < 10) crashAppend("0");
    crashAppendDec(st.wSecond);
}

/*
** Function: crashEmit
** @brief    Writes the assembled crash report to stderr and to the end of
**           the log file, then empties the buffer.
** @details  Uses WriteFile on raw handles. The log is opened for append
**           with full sharing, so it coexists with the logger's own open
**           handle and the report lands after everything already logged.
*/
static void crashEmit()
{
    HANDLE  hErr      = GetStdHandle(STD_ERROR_HANDLE);   /* the console's error stream                 */
    HANDLE  hLog      = INVALID_HANDLE_VALUE;             /* the log file, opened for append            */
    DWORD   written   = 0;                                /* bytes accepted by the last WriteFile       */

    /* Console first: it needs no path and is what a person watching sees.
    ** The write results in this function are deliberately not acted on --
    ** this runs while the process is already failing, and a failed write
    ** has no better place left to be reported.
    */
    if (hErr != nullptr && hErr != INVALID_HANDLE_VALUE)
        (void)WriteFile(hErr, g_crashText, (DWORD)g_crashTextLen, &written, nullptr);

    /* Then the log file, if a path was given. FILE_APPEND_DATA makes every
    ** write land at the true end of the file no matter what the logger's
    ** own handle has done.
    */
    if (g_crashLogPath[0] != '\0')
    {
        hLog = CreateFileA(g_crashLogPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hLog != INVALID_HANDLE_VALUE)
        {
            (void)WriteFile(hLog, g_crashText, (DWORD)g_crashTextLen, &written, nullptr);
            (void)FlushFileBuffers(hLog);
            (void)CloseHandle(hLog);
        }
    }

    g_crashTextLen = 0;
    g_crashText[0] = '\0';
}

/*
** Function: crashExceptionName
** @brief    Returns a short name for the exception codes that matter here.
** @param    code - the exception code
** @return   A constant string; "unrecognized" for codes not listed.
*/
static const char* crashExceptionName(DWORD code)
{
    const char* pszName = "unrecognized";   /* result for codes the switch does not know */

    switch (code)
    {
        case 0xC0000005:
            pszName = "ACCESS_VIOLATION";
            break;
        case 0xC0000008:
            pszName = "INVALID_HANDLE";
            break;
        case 0xC00000FD:
            pszName = "STACK_OVERFLOW";
            break;
        case 0xC0000094:
            pszName = "INTEGER_DIVIDE_BY_ZERO";
            break;
        case 0xC0000374:
            pszName = "HEAP_CORRUPTION";
            break;
        case 0xC0000409:
            pszName = "STACK_BUFFER_OVERRUN / fast-fail";
            break;
        case 0xC000001D:
            pszName = "ILLEGAL_INSTRUCTION";
            break;
        case 0xE06D7363:
            pszName = "C++ exception";
            break;
        default:
            break;
    }

    return pszName;
}

/*
** Function: crashFilter
** @brief    The unhandled-exception filter: writes a crash report, then
**           declines to handle the exception so normal crash processing
**           (including the crash dump) still happens.
** @details  The report holds the time, thread, exception code and name,
**           the faulting address as module+offset, the key registers and a
**           list of return-address candidates found on the faulting
**           thread's stack. The candidates are filtered to addresses inside
**           the program's own image and shown as exe+offset, so they can be
**           resolved to functions later with the matching symbol file.
** @param    pException - the exception being reported
** @return   EXCEPTION_CONTINUE_SEARCH always.
*/
static LONG WINAPI crashFilter(EXCEPTION_POINTERS* pException)
{
    HMODULE               hExe        = GetModuleHandleA(nullptr);   /* the program's own image                                  */
    IMAGE_DOS_HEADER*     pDos        = nullptr;                     /* DOS header of that image                                 */
    IMAGE_NT_HEADERS*     pNt         = nullptr;                     /* NT headers of that image                                 */
    uint64_t              exeBase     = 0;                           /* load address of the program image                        */
    uint64_t              exeSize     = 0;                           /* size of the program image                                */
    SIZE_T                stackRead   = 0;                           /* bytes of stack actually copied out                       */
    int                   candidates  = 0;                           /* return-address candidates listed so far                  */

    /* Only the first crash writes a report. A second thread crashing at
    ** the same moment (very likely once the process is in trouble) would
    ** otherwise interleave its text into the first one's buffer.
    */
    if (InterlockedCompareExchange(&g_crashReportStarted, 1, 0) != 0)
        return EXCEPTION_CONTINUE_SEARCH;

    /* Without exception details there is nothing useful to report. */
    if (pException == nullptr || pException->ExceptionRecord == nullptr)
        return EXCEPTION_CONTINUE_SEARCH;

    g_crashTextLen = 0;
    g_crashText[0] = '\0';

    crashAppend("\r\n*** NATIVE CRASH ***\r\n");
    crashAppend("Time: ");
    crashAppendTimestamp();
    crashAppend("   Thread id: ");
    crashAppendDec(GetCurrentThreadId());
    crashAppend("\r\n");

    crashAppend("Exception: 0x");
    crashAppendHex(pException->ExceptionRecord->ExceptionCode, 8);
    crashAppend(" ");
    crashAppend(crashExceptionName(pException->ExceptionRecord->ExceptionCode));
    crashAppend("   flags 0x");
    crashAppendHex(pException->ExceptionRecord->ExceptionFlags, 1);
    crashAppend("\r\nFault address: ");
    crashAppendModuleOffset((uint64_t)(uintptr_t)pException->ExceptionRecord->ExceptionAddress);
    crashAppend("\r\n");

    /* An access violation carries what kind of access failed and where. */
    if (pException->ExceptionRecord->ExceptionCode == 0xC0000005 && pException->ExceptionRecord->NumberParameters >= 2)
    {
        crashAppend("Access violation: ");
        crashAppend(pException->ExceptionRecord->ExceptionInformation[0] == 0 ? "read of 0x" :
                    pException->ExceptionRecord->ExceptionInformation[0] == 1 ? "write to 0x" : "execute of 0x");
        crashAppendHex((uint64_t)pException->ExceptionRecord->ExceptionInformation[1], 1);
        crashAppend("\r\n");
    }

#if defined(_M_X64)
    if (pException->ContextRecord != nullptr)
    {
        CONTEXT* pCtx = pException->ContextRecord;   /* the faulting thread's registers */

        crashAppend("RIP=");  crashAppendHex(pCtx->Rip, 16);
        crashAppend(" RSP="); crashAppendHex(pCtx->Rsp, 16);
        crashAppend(" RBP="); crashAppendHex(pCtx->Rbp, 16);
        crashAppend("\r\nRAX="); crashAppendHex(pCtx->Rax, 16);
        crashAppend(" RCX="); crashAppendHex(pCtx->Rcx, 16);
        crashAppend(" RDX="); crashAppendHex(pCtx->Rdx, 16);
        crashAppend(" RBX="); crashAppendHex(pCtx->Rbx, 16);
        crashAppend("\r\nRSI="); crashAppendHex(pCtx->Rsi, 16);
        crashAppend(" RDI="); crashAppendHex(pCtx->Rdi, 16);
        crashAppend(" R8=");  crashAppendHex(pCtx->R8, 16);
        crashAppend(" R9=");  crashAppendHex(pCtx->R9, 16);
        crashAppend("\r\n");

        /* Find the program image's extent so stack values can be tested
        ** against it. The headers are the program's own and always readable.
        */
        pDos = (IMAGE_DOS_HEADER*)hExe;
        if (pDos != nullptr && pDos->e_magic == IMAGE_DOS_SIGNATURE)
        {
            pNt = (IMAGE_NT_HEADERS*)((unsigned char*)hExe + pDos->e_lfanew);
            if (pNt->Signature == IMAGE_NT_SIGNATURE)
            {
                exeBase = (uint64_t)(uintptr_t)hExe;
                exeSize = pNt->OptionalHeader.SizeOfImage;
            }
        }

        /* ReadProcessMemory copies without ever faulting (it reports the
        ** bytes it could read), which a raw read of a possibly damaged
        ** stack would not guarantee. Its return value is deliberately
        ** ignored: a partial read (stack running into a guard page) still
        ** reports how many bytes were copied in stackRead, and that count
        ** is all the scan below uses.
        */
        if (exeSize > 0)
            (void)ReadProcessMemory(GetCurrentProcess(), (LPCVOID)(uintptr_t)pCtx->Rsp,
                                    g_crashStack, CRASH_STACK_BYTES, &stackRead);

        crashAppend("Stack scan (program-image addresses found in the top ");
        crashAppendDec(stackRead);
        crashAppend(" bytes above RSP; offsets resolve with the matching .pdb):\r\n");

        for (SIZE_T off = 0; off + 8 <= stackRead && candidates < CRASH_MAX_CANDIDATES; off += 8)
        {
            uint64_t value = 0;   /* the stack qword under examination */

            memcpy(&value, g_crashStack + off, sizeof(value));

            /* Only values inside the program's own image are listed: they
            ** are the return addresses that identify which of OUR
            ** functions were live.
            */
            if (value >= exeBase && value < exeBase + exeSize)
            {
                crashAppend("  [rsp+0x");
                crashAppendHex(off, 1);
                crashAppend("] exe+0x");
                crashAppendHex(value - exeBase, 1);
                crashAppend("\r\n");
                candidates++;
            }
        }
    }
#endif

    crashAppend("*** end of crash report ***\r\n");
    crashEmit();

    /* Decline: the default handling still runs, so the crash dump is written as usual. */
    return EXCEPTION_CONTINUE_SEARCH;
}

/*
** Function: crashLine
** @brief    Writes a one-line, timestamped crash note to stderr and the log.
** @param    pszWhat - what happened
*/
static void crashLine(const char* pszWhat)
{
    /* The same once-only guard as the exception filter: if a full report
    ** is already being written, this note adds nothing.
    */
    if (InterlockedCompareExchange(&g_crashReportStarted, 1, 0) != 0)
        return;

    g_crashTextLen = 0;
    g_crashText[0] = '\0';

    crashAppend("\r\n*** ABNORMAL TERMINATION ***\r\nTime: ");
    crashAppendTimestamp();
    crashAppend("   Thread id: ");
    crashAppendDec(GetCurrentThreadId());
    crashAppend("\r\n");
    crashAppend(pszWhat);
    crashAppend("\r\n");
    crashEmit();
}

/*
** Function: crashOnAbort
** @brief    SIGABRT handler: notes that abort() was called. Returning lets
**           abort() finish terminating the process.
** @param    signalNumber - the signal raised (SIGABRT)
*/
static void crashOnAbort(int signalNumber)
{
    (void)signalNumber;   /* only SIGABRT is routed here; nothing to distinguish */

    crashLine("abort() was called.");
}

/*
** Function: crashOnInvalidParameter
** @brief    C runtime invalid-parameter handler: notes the failure, then
**           fast-fails so the crash dump is still produced.
** @param    pExpression - the failed expression text (Release builds pass nullptr)
** @param    pFunction   - the C runtime function that rejected its argument (nullptr in Release)
** @param    pFile       - source file of the C runtime check (nullptr in Release)
** @param    line        - source line of the C runtime check
** @param    reserved    - unused
*/
static void crashOnInvalidParameter(const wchar_t* pExpression, const wchar_t* pFunction,
                                    const wchar_t* pFile, unsigned int line, uintptr_t reserved)
{
    (void)pExpression;   /* Release builds pass nothing useful; not worth converting wide text without the runtime */
    (void)pFunction;
    (void)pFile;
    (void)line;
    (void)reserved;

    crashLine("The C runtime rejected an invalid parameter (a library call was given a bad pointer, handle or size).");

    /* Default behavior for this condition is a fast-fail; keep it so the
    ** crash dump is produced exactly as before.
    */
    __fastfail(FAST_FAIL_INVALID_ARG);
}

/*
** Function: crashOnTerminate
** @brief    std::terminate handler: notes that an uncaught C++ exception (or
**           a noexcept violation) ended the program, then aborts.
*/
static void crashOnTerminate()
{
    crashLine("std::terminate was called (an uncaught C++ exception, or a noexcept function threw).");

    abort();
}
