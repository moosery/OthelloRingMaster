/*
** Filename:  Error.h
**
** Purpose:
**   Declares the process-wide error/fatal reporting facility used across the
**   whole solution (not just Utility):
**     - Error()/ErrorGetLast()/ErrorGetLastReason()/ErrorPrint() record a
**       recoverable error plus a formatted reason string, thread-locally, so
**       a caller can report or react to it without immediately terminating.
**     - Fatal() prints a message and terminates the process immediately, for
**       conditions where continuing to run is unsafe (corrupted state, a
**       failed I/O the caller cannot recover from, etc.).
**   RC is the shared return/error-code type. Each subsystem is given its own
**   10000-wide numeric range (RC_BOARD_BASE, RC_BP_BASE, ...) so error codes
**   from different subsystems never collide, and FATAL_* codes below
**   RC_BOARD_BASE are reserved specifically for Fatal() call sites.
*/

#pragma once

/* Includes */
#include <stdio.h>

/* Structures and Types */
typedef size_t RC;

/* Constants */

/* RC namespacing: each subsystem's error codes live in their own 10000-wide range. */
constexpr auto RC_SUCCESS    = 0;
constexpr auto RC_FATAL_BASE = 0;  /* Additive offset only — fatal codes are RC_FATAL_BASE+1 through 9999; 0 is reserved as RC_SUCCESS */
constexpr auto RC_BOARD_BASE = 10000;
constexpr auto RC_BP_BASE    = 20000;
constexpr auto RC_STACK_BASE = 30000;
constexpr auto RC_CACHE_BASE = 40000;
constexpr auto RC_BTP_BASE   = 50000;
constexpr auto RC_HS_BASE    = 60000;
constexpr auto RC_FS_BASE    = 70000;
constexpr auto RC_UTIL_BASE  = 80000;
constexpr auto RC_FI_BASE    = 90000;

/* FATAL return codes */
constexpr auto FATAL_ALLOCATION_FAILED                = RC_FATAL_BASE + 1;
constexpr auto FATAL_BP_DELETE                        = RC_FATAL_BASE + 2;
constexpr auto FATAL_BP_NODE_EMPTY                    = RC_FATAL_BASE + 3;
constexpr auto FATAL_BP_DUP_KEY                       = RC_FATAL_BASE + 4;
constexpr auto FATAL_BP_INTEGRITY_CHECK_FAILED        = RC_FATAL_BASE + 5;
constexpr auto FATAL_FILE_OPEN                        = RC_FATAL_BASE + 6;
constexpr auto FATAL_BP_FIND                          = RC_FATAL_BASE + 7;
constexpr auto FATAL_MOVE_FIND_FAILED                 = RC_FATAL_BASE + 8;
constexpr auto FATAL_BOARD_FIND_FAILED                = RC_FATAL_BASE + 9;
constexpr auto FATAL_BOARD_NOT_PLAYED                 = RC_FATAL_BASE + 10;
constexpr auto FATAL_BP_INSERT                        = RC_FATAL_BASE + 11;
constexpr auto FATAL_BOARD_REPLAY                     = RC_FATAL_BASE + 12;
constexpr auto FATAL_BAD_BOARD_STATE                  = RC_FATAL_BASE + 13;
constexpr auto FATAL_READ_CURSOR_LARGER_THAN_WRITE    = RC_FATAL_BASE + 14;
constexpr auto FATAL_BOARDS_TO_PROCESS_FAILED         = RC_FATAL_BASE + 15;
constexpr auto FATAL_BTP_CHECKPT_FAILED               = RC_FATAL_BASE + 16;
constexpr auto FATAL_FS_TOO_MANY_RECORDS_FOR_FILE     = RC_FATAL_BASE + 17;
constexpr auto FATAL_FS_BACKUP_FAILED                 = RC_FATAL_BASE + 18;
constexpr auto FATAL_FS_FILE_LOAD_FAILED              = RC_FATAL_BASE + 19;
constexpr auto FATAL_FS_FILE_FREE_FAILED              = RC_FATAL_BASE + 20;
constexpr auto FATAL_CACHE_CHECKIN_INVALID            = RC_FATAL_BASE + 21;
constexpr auto FATAL_FS_FIND_BEST_FIT_FILE_FAILED     = RC_FATAL_BASE + 22;
constexpr auto FATAL_FS_ITERATION_FAILED              = RC_FATAL_BASE + 23;
constexpr auto FATAL_BOARD_UPDATE_FAILED              = RC_FATAL_BASE + 24;
constexpr auto FATAL_BTP_SORT_WRITE_FAILED            = RC_FATAL_BASE + 25;
constexpr auto FATAL_SEEK_FAILED                      = RC_FATAL_BASE + 26;
constexpr auto FATAL_READ_FAILED                      = RC_FATAL_BASE + 27;
constexpr auto FATAL_FI_FLUSH_FAILED                  = RC_FATAL_BASE + 28;
constexpr auto FATAL_TS_UNBALANCED_TREE               = RC_FATAL_BASE + 29;
constexpr auto FATAL_MAX_MOVES_EXCEEDED               = RC_FATAL_BASE + 30;
constexpr auto FATAL_DRIVE_CACHE_WRITE_FAILED         = RC_FATAL_BASE + 31;
constexpr auto FATAL_CREATE_DIR_FAILED                = RC_FATAL_BASE + 32;
constexpr auto FATAL_INSUFFICIENT_MEMORY              = RC_FATAL_BASE + 33;
constexpr auto FATAL_INVALID_BOARD_SIZE               = RC_FATAL_BASE + 34;
constexpr auto FATAL_DRIVE_SPACE                      = RC_FATAL_BASE + 35;
constexpr auto FATAL_GPU_ERROR                        = RC_FATAL_BASE + 36;
constexpr auto FATAL_MERGE_LOGIC_ERROR                = RC_FATAL_BASE + 37;

/* Utility Implementation errors */
constexpr auto UTIL_RC_Success                    = RC_SUCCESS;
constexpr auto UTIL_RC_Could_Not_Create_Directory = RC_UTIL_BASE + 1;
constexpr auto UTIL_RC_Path_Too_Long              = RC_UTIL_BASE + 2;

/* Functions */

/*
** Function: Error
** @brief    Records a recoverable error and a formatted reason string on the
**           calling thread, retrievable later via ErrorGetLast/ErrorGetLastReason.
** @param    error       - the RC error code to record
** @param    pszReasonFmt - printf-style format string describing the error
** @param    ...         - format arguments for pszReasonFmt
*/
void Error(RC error, const char* pszReasonFmt, ...);

/*
** Function: ErrorGetLast
** @brief    Returns the last error code recorded by Error() on this thread.
** @return   The last RC recorded, or RC_SUCCESS if none has been recorded.
*/
RC ErrorGetLast();

/*
** Function: ErrorGetLastReason
** @brief    Returns the formatted reason string from the last Error() call
**           on this thread.
** @return   Pointer to a thread-local buffer; valid until the next Error() call on this thread.
*/
char* ErrorGetLastReason();

/*
** Function: ErrorPrint
** @brief    Prints the last recorded error code and reason string.
** @param    fpOut - stream to print to
*/
void ErrorPrint(FILE* fpOut);

/*
** Function: Fatal
** @brief    Prints a formatted message to stderr and terminates the process
**           immediately with rc as the exit code. Never returns.
** @param    rc          - exit code to terminate with
** @param    pszReasonFmt - printf-style format string describing the fatal condition
** @param    ...         - format arguments for pszReasonFmt
*/
void Fatal(RC rc, const char* pszReasonFmt, ...);
