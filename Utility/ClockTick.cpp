/*
** Filename:  ClockTick.cpp
**
** Purpose:
**   Implements the elapsed-time (ClockTick) and wall-clock-timestamp
**   (ClockTime) facilities declared in ClockTick.h.
*/

/* Includes */
#include "ClockTick.h"
#include <string.h>

using namespace std;

/* Globals */
static chrono::steady_clock theClock;   /* shared monotonic clock source for all ClockTick measurements */

/* Functions */

/*
** Function: ClockGetSystemClockTime
** @brief    Captures the current wall-clock time into pClockTime, formatted
**           down to the nanosecond.
** @details  Formats the whole-second portion with strftime, then appends
**           the sub-second nanosecond remainder as a fixed-width zero-padded
**           field so the resulting string stays lexicographically
**           comparable via strcmp.
** @param    pClockTime - the ClockTime to fill in
*/
void ClockGetSystemClockTime(PClockTime pClockTime)
{
    auto      now          = std::chrono::system_clock::now();
    time_t    theTime      = std::chrono::system_clock::to_time_t(now);
    std::tm*  theLocalTime = std::localtime(&theTime);

    std::strftime(pClockTime->strTime, sizeof(pClockTime->strTime), "%Y%m%d%H%M%S", theLocalTime);

    /* Sub-second remainder: the full nanosecond count modulo 1 second, so it
    ** always fits the fixed 9-digit field appended after the seconds.
    */
    auto nanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count() % 1000000000;
    snprintf(pClockTime->strTime + 14, sizeof(pClockTime->strTime) - 14, "%09lld", nanoseconds);
}

/*
** Function: ClockCompareSystemClockTime
** @brief    Compares two ClockTime timestamps.
** @param    pT1 - first ClockTime
** @param    pT2 - second ClockTime
** @return   <0/0/>0 as pT1 is before/equal/after pT2 (per strcmp on strTime).
*/
int ClockCompareSystemClockTime(PClockTime pT1, PClockTime pT2)
{
    return strcmp(pT1->strTime, pT2->strTime);
}

/*
** Function: ClockStart
** @brief    Captures the current monotonic time into pClockTick as the
**           starting point for a later elapsed-time measurement.
** @param    pClockTick - the ClockTick to initialize
*/
void ClockStart(PClockTick pClockTick)
{
    pClockTick->startingTick = theClock.now();
}

/*
** Function: ClockNanosSinceStart
** @brief    Returns the number of nanoseconds elapsed since ClockStart was
**           called on pClockTick.
** @param    pClockTick - a ClockTick previously initialized by ClockStart
** @return   Elapsed nanoseconds.
*/
long long ClockNanosSinceStart(PClockTick pClockTick)
{
    chrono::time_point<chrono::steady_clock>  endingTick = theClock.now();
    chrono::duration<long long, std::nano>    elapsed    = (endingTick - pClockTick->startingTick);

    return (elapsed.count());
}

/*
** Function: ClockMillisSinceStart
** @brief    Returns the number of milliseconds elapsed since ClockStart was
**           called on pClockTick.
** @param    pClockTick - a ClockTick previously initialized by ClockStart
** @return   Elapsed milliseconds.
*/
long long ClockMillisSinceStart(PClockTick pClockTick)
{
    chrono::time_point<chrono::steady_clock>  endingTick = theClock.now();
    chrono::duration<double, std::milli>      elapsed    = (endingTick - pClockTick->startingTick);

    return ((long long)elapsed.count());
}

/*
** Function: ClockCompare
** @brief    Compares the starting ticks of two ClockTicks.
** @param    pC1 - first ClockTick
** @param    pC2 - second ClockTick
** @return   1 if pC1 started after pC2, -1 if before, 0 if equal.
*/
int ClockCompare(PClockTick pC1, PClockTick pC2)
{
    /* Explicit three-way comparison: time_point only guarantees ordering
    ** operators, not a subtraction that would collapse to a single sign.
    */
    if (pC1->startingTick > pC2->startingTick)
        return 1;
    else if (pC1->startingTick < pC2->startingTick)
        return -1;
    else
        return 0;
}

/*
** Function: ClockPrintNanos
** @brief    Prints the raw nanosecond count of pClockTick's starting tick.
** @param    fpOut      - stream to print to
** @param    pClockTick - the ClockTick to print
*/
void ClockPrintNanos(FILE* fpOut, PClockTick pClockTick)
{
    fprintf(fpOut, "The ClockTick Nanos are: %zd\n", pClockTick->startingTick.time_since_epoch().count());
}
