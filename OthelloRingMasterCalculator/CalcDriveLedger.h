/*
** Filename:  CalcDriveLedger.h
**
** Purpose:
**   Declares the calculator's per-drive scratch-space ledger: tracks
**   available bytes via our own reserve/reclaim accounting instead of
**   repeated OS queries, exactly mirroring OthelloRingMaster's own
**   DriveLedger.h. One combined ledger per drive -- both level+1's
**   lookup-source segments and level N's own output segments draw from
**   the same pool, so reserving for one correctly leaves less available
**   for the other, regardless of which is reserved first.
**
**     CalcDriveInitLedger -- query OS, subtract safety buffer, store as baseline
**     CalcDriveReserve    -- CAS-loop subtract; false (no change) if insufficient
**     CalcDriveReclaim    -- unconditional add (segment deleted or overestimate returned)
**     CalcDriveAvailable  -- read current value
**
** Notes:
**   Same "reserve a flat safety buffer at init so a reservation can never
**   consume the last bytes on a drive" reasoning as DriveLedger.h, sized
**   the same (CALC_DRIVE_SPACE_LOW_GB matches OthelloRingMaster's own
**   DRIVE_SPACE_LOW_GB) -- not Utility/DriveInfo.h's much larger
**   DRIVE_SAFETY_MARGIN_BYTES (200 GB), which is tuned for a different
**   purpose (leaving room for other concurrent activity during a live
**   forward solve, not this calculator's own scratch usage).
*/

#pragma once

/* Includes */
#include "CalculatorTypes.h"
#include <windows.h>

/* Constants */
#define CALC_DRIVE_SPACE_LOW_GB    20ULL
#define CALC_DRIVE_SPACE_LOW_BYTES (CALC_DRIVE_SPACE_LOW_GB * 1024ULL * 1024ULL * 1024ULL)

/* Functions */

/*
** Function: CalcDriveInitLedger
** @brief    Queries the OS, subtracts the safety buffer, and stores the
**           result as a drive's available-scratch-bytes baseline.
** @param    pState - calculator state whose driveLedger entry gets initialized
** @param    letter - drive letter to query
*/
static inline void CalcDriveInitLedger(POthelloRingMasterCalculatorState pState, char letter)
{
    char           root[4] = { letter, ':', '\\', '\0' };
    ULARGE_INTEGER freeAvail = {};

    GetDiskFreeSpaceExA(root, &freeAvail, nullptr, nullptr);
    int64_t available = (int64_t)freeAvail.QuadPart - (int64_t)CALC_DRIVE_SPACE_LOW_BYTES;
    if (available < 0) available = 0;
    InterlockedExchange64(
        (volatile LONG64*)&pState->driveLedger[(unsigned char)(letter - 'A')],
        (LONG64)available);
}

/*
** Function: CalcDriveReserve
** @brief    Atomically reserves bytes on a drive: subtracts from the ledger
**           only if available >= bytes.
** @param    pState - calculator state whose driveLedger entry to reserve from
** @param    letter - drive letter to reserve on
** @param    bytes  - number of bytes to reserve
** @return   true on success; false (ledger unchanged) if insufficient space.
*/
static inline bool CalcDriveReserve(POthelloRingMasterCalculatorState pState, char letter, int64_t bytes)
{
    volatile LONG64* p   = (volatile LONG64*)&pState->driveLedger[(unsigned char)(letter - 'A')];
    LONG64           old = InterlockedCompareExchange64(p, 0, 0);

    for (;;)
    {
        if (old < (LONG64)bytes) return false;
        LONG64 got = InterlockedCompareExchange64(p, old - (LONG64)bytes, old);
        if (got == old) return true;
        old = got;
    }
}

/*
** Function: CalcDriveReclaim
** @brief    Returns bytes to the ledger (segment deleted or overestimate corrected).
** @param    pState - calculator state whose driveLedger entry to credit
** @param    letter - drive letter to credit
** @param    bytes  - number of bytes to return
*/
static inline void CalcDriveReclaim(POthelloRingMasterCalculatorState pState, char letter, int64_t bytes)
{
    InterlockedAdd64(
        (volatile LONG64*)&pState->driveLedger[(unsigned char)(letter - 'A')],
        (LONG64)bytes);
}

/*
** Function: CalcDriveAvailable
** @brief    Reads the current available-scratch-bytes figure for a drive.
** @param    pState - calculator state whose driveLedger entry to read
** @param    letter - drive letter to read
** @return   Current available bytes.
*/
static inline int64_t CalcDriveAvailable(POthelloRingMasterCalculatorState pState, char letter)
{
    return (int64_t)InterlockedCompareExchange64(
        (volatile LONG64*)&pState->driveLedger[(unsigned char)(letter - 'A')], 0, 0);
}
