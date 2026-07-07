/*
** Filename:  ClockTick.h
**
** Purpose:
**   Declares two independent timing facilities:
**     - ClockTick / ClockStart / ClockNanosSinceStart / ClockMillisSinceStart /
**       ClockCompare / ClockPrintNanos: elapsed-time measurement built on
**       std::chrono::steady_clock (monotonic -- never affected by wall-clock
**       adjustments), for timing how long an operation takes.
**     - ClockTime / ClockGetSystemClockTime / ClockCompareSystemClockTime:
**       a totally-ordered wall-clock timestamp string, down to the
**       nanosecond, for comparing or logging real points in time across runs.
**   Also declares GetCurrentDateTimeString, a filename-safe
**   ("YYYY-MM-DD_HH-MM-SS", no colons or spaces) date/time string helper.
*/

#pragma once

/* Includes */
#include <chrono>
#include <time.h>
#include <stdio.h>

/* Structures and Types */

/*
** Type:    ClockTick
** @brief   A single monotonic starting point captured by ClockStart, used to
**          measure elapsed time via ClockNanosSinceStart/ClockMillisSinceStart.
*/
typedef struct _ClockTick
{
    std::chrono::time_point<std::chrono::steady_clock> startingTick;   /* monotonic timestamp captured by ClockStart */
} ClockTick, * PClockTick;

/*
** Type:    ClockTime
** @brief   A wall-clock timestamp, stored as a fixed-width, lexicographically
**          comparable string down to the nanosecond.
*/
typedef struct _ClockTime
{
    char strTime[24];   /* "YYYYMMDDHHMMSSnnnnnnnnn" -- fixed-width so two ClockTime strings can be compared with plain strcmp */
} ClockTime, * PClockTime;

/* Functions */

/*
** Function: GetCurrentDateTimeString
** @brief    Fills outStr with the local date/time in "YYYY-MM-DD_HH-MM-SS"
**           format, suitable for use in file names (no colons or spaces).
** @param    outStr  - buffer to receive the formatted string
** @param    outSize - size in bytes of outStr
*/
inline void GetCurrentDateTimeString(char* outStr, size_t outSize)
{
    time_t     t  = time(nullptr);
    struct tm  tm = {};

    localtime_s(&tm, &t);
    snprintf(outStr, outSize, "%04d-%02d-%02d_%02d-%02d-%02d",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec);
}

/*
** Function: ClockStart
** @brief    Captures the current monotonic time into pClockTick as the
**           starting point for a later elapsed-time measurement.
** @param    pClockTick - the ClockTick to initialize
*/
void ClockStart(PClockTick pClockTick);

/*
** Function: ClockNanosSinceStart
** @brief    Returns the number of nanoseconds elapsed since ClockStart was
**           called on pClockTick.
** @param    pClockTick - a ClockTick previously initialized by ClockStart
** @return   Elapsed nanoseconds.
*/
long long ClockNanosSinceStart(PClockTick pClockTick);

/*
** Function: ClockMillisSinceStart
** @brief    Returns the number of milliseconds elapsed since ClockStart was
**           called on pClockTick.
** @param    pClockTick - a ClockTick previously initialized by ClockStart
** @return   Elapsed milliseconds.
*/
long long ClockMillisSinceStart(PClockTick pClockTick);

/*
** Function: ClockCompare
** @brief    Compares the starting ticks of two ClockTicks.
** @param    pC1 - first ClockTick
** @param    pC2 - second ClockTick
** @return   1 if pC1 started after pC2, -1 if before, 0 if equal.
*/
int ClockCompare(PClockTick pC1, PClockTick pC2);

/*
** Function: ClockPrintNanos
** @brief    Prints the raw nanosecond count of pClockTick's starting tick.
** @param    fpOut      - stream to print to
** @param    pClockTick - the ClockTick to print
*/
void ClockPrintNanos(FILE* fpOut, PClockTick pClockTick);

/*
** Function: ClockGetSystemClockTime
** @brief    Captures the current wall-clock time into pClockTime, formatted
**           down to the nanosecond.
** @param    pClockTime - the ClockTime to fill in
*/
void ClockGetSystemClockTime(PClockTime pClockTime);

/*
** Function: ClockCompareSystemClockTime
** @brief    Compares two ClockTime timestamps.
** @param    pT1 - first ClockTime
** @param    pT2 - second ClockTime
** @return   <0/0/>0 as pT1 is before/equal/after pT2 (per strcmp on strTime).
*/
int ClockCompareSystemClockTime(PClockTime pT1, PClockTime pT2);
