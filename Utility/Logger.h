/*
** Filename:  Logger.h
**
** Purpose:
**   Declares a simple dual-output logger: LoggerInit opens (or re-opens) a
**   log file, and LoggerLog writes a formatted message to both stdout and
**   that log file, so console output and a persisted log always match.
**   LoggerLogFileOnly writes to the log file alone, for text already shown
**   on the console by another route (see Fatal() in Error.cpp).
*/

#pragma once

/* Functions */

/*
** Function: LoggerInit
** @brief    Opens logFileName for writing as the destination for LoggerLog.
**           If a log file is already open, it is closed first. If
**           logFileName is nullptr, logging to a file is disabled (LoggerLog
**           still writes to stdout).
** @param    logFileName - path of the log file to open, or nullptr to disable file logging
*/
void LoggerInit(const char* logFileName);

/*
** Function: LoggerLog
** @brief    Writes a formatted message to stdout, and also to the log file
**           if LoggerInit successfully opened one. A failed write to either
**           destination is reported once on stderr.
** @param    format - printf-style format string
** @param    ...    - format arguments for format
*/
void LoggerLog(const char* format, ...);

/*
** Function: LoggerLogFileOnly
** @brief    Writes a formatted message to the log file only (never stdout).
**           A no-op if no log file is open.
** @param    format - printf-style format string
** @param    ...    - format arguments for format
*/
void LoggerLogFileOnly(const char* format, ...);
