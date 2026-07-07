/*
** Filename:  SysMemInfo.h
**
** Purpose:
**   Declares helpers for sizing a process's memory budget against currently
**   available physical RAM: MemoryInfo/MemoryMode describe what the caller
**   wants (a recommended fraction of free RAM, all of it, or a specific
**   size), CalcMemoryBudget resolves that request against the machine's
**   live free-RAM figure, and sizeToGBString/ParseMemorySize convert
**   between byte counts and human-readable size strings.
*/

#pragma once

/* Includes */
#include <stdint.h>
#include <string.h>
#include <windows.h>

/* Structures and Types */

/*
** Type:    MemoryMode
** @brief   How a caller wants its memory budget determined: a recommended
**          fraction of free RAM, as much free RAM as possible, or a
**          specific user-requested size (still capped by free RAM).
*/
enum MemoryMode { MM_RECOMMENDED = 0, MM_USE_MAX, MM_SPECIFIED };

/*
** Type:    MemoryInfo
** @brief   A memory budget request (requestedMode/requestedBytes) together
**          with the live physical-RAM figures and resolved budget that
**          CalcMemoryBudget fills in.
*/
typedef struct __MemoryInfo
{
    MemoryMode  requestedMode;    /* memory mode requested by user                            */
    uint64_t    requestedBytes;   /* if MM_SPECIFIED, the user-specified memory size          */
    uint64_t    totalPhys;        /* total physical RAM                                       */
    uint64_t    availPhys;        /* available (free) physical RAM                            */
    uint64_t    budgetedSize;     /* budgeted memory size based on mode and free RAM          */
} MemoryInfo, * PMemoryInfo;

/* Constants */

/* Fraction of free (available) physical RAM to use per memory mode.
** Adjust these compile-time constants to tune memory pressure.
*/
static constexpr double BUDGET_PCT_MAX         = 0.95;   /* leave ~5% of free RAM untouched */
static constexpr double BUDGET_PCT_RECOMMENDED = 0.95;   /* leave ~5% of free RAM untouched */

/* Functions */

/*
** Function: sizeToGBString
** @brief    Formats a byte count as a human-readable string, auto-selecting
**           GB/MB/KB/bytes depending on magnitude.
** @param    bytes      - the byte count to format
** @param    outStr     - buffer to receive the formatted string
** @param    outStrSize - size in bytes of outStr
*/
inline void sizeToGBString(uint64_t bytes, char* outStr, size_t outStrSize)
{
    if (bytes >= 1024ULL * 1024 * 1024)
        snprintf(outStr, outStrSize, "%.2f GB", (double)bytes / (1024ULL * 1024 * 1024));
    else if (bytes >= 1024ULL * 1024)
        snprintf(outStr, outStrSize, "%.2f MB", (double)bytes / (1024ULL * 1024));
    else if (bytes >= 1024ULL)
        snprintf(outStr, outStrSize, "%.2f KB", (double)bytes / 1024);
    else
        snprintf(outStr, outStrSize, "%llu bytes", bytes);
}

/*
** Function: ParseMemorySize
** @brief    Parses a human-written size string into a byte count.
** @details  Accepts "34GB", "34G", "12000MB", "12000M", "512KB", "512K", or
**           a plain integer (interpreted as bytes). The unit suffix is
**           matched case-insensitively.
** @param    s - the size string to parse
** @return   The parsed size in bytes, or 0 if s is null/empty.
*/
inline uint64_t ParseMemorySize(const char* s)
{
    if (!s || !*s)
        return 0;

    char*     end = nullptr;
    uint64_t  n   = (uint64_t)strtoull(s, &end, 10);

    /* No unit suffix at all -- the whole string was just digits. */
    if (!end || !*end)
        return n;

    while (*end == ' ')
        ++end;

    if (_stricmp(end, "GB") == 0 || _stricmp(end, "G") == 0) return n * 1024ULL * 1024 * 1024;
    if (_stricmp(end, "MB") == 0 || _stricmp(end, "M") == 0) return n * 1024ULL * 1024;
    if (_stricmp(end, "KB") == 0 || _stricmp(end, "K") == 0) return n * 1024ULL;
    return n;
}

/*
** Function: CalcMemoryBudget
** @brief    Resolves pMemInfo->requestedMode/requestedBytes against the
**           machine's current free physical RAM, filling in
**           totalPhys/availPhys/budgetedSize.
** @details  All modes are relative to currently-available (not total) RAM,
**           so the resolved budget adapts to whatever else is running on
**           the machine at the moment this is called.
** @param    pMemInfo - in: requestedMode/requestedBytes; out: totalPhys/availPhys/budgetedSize
*/
inline void CalcMemoryBudget(PMemoryInfo pMemInfo)
{
    MEMORYSTATUSEX ms = {};
    ms.dwLength = sizeof(ms);
    GlobalMemoryStatusEx(&ms);

    uint64_t budget;

    /* MM_SPECIFIED is still capped by BUDGET_PCT_MAX of free RAM -- a
    ** caller-requested size larger than what is actually available would
    ** otherwise commit to a budget the machine cannot back.
    */
    switch (pMemInfo->requestedMode)
    {
        case MM_USE_MAX:
            budget = (uint64_t)(ms.ullAvailPhys * BUDGET_PCT_MAX);
            break;
        case MM_SPECIFIED:
            budget = min(pMemInfo->requestedBytes, (uint64_t)(ms.ullAvailPhys * BUDGET_PCT_MAX));
            break;
        default:
            budget = (uint64_t)(ms.ullAvailPhys * BUDGET_PCT_RECOMMENDED);
            break;
    }

    pMemInfo->budgetedSize = budget;
    pMemInfo->totalPhys    = ms.ullTotalPhys;
    pMemInfo->availPhys    = ms.ullAvailPhys;
}
