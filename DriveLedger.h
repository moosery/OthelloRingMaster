/*
** Filename:  DriveLedger.h
**
** Purpose:
**   Declares the per-drive space ledger: tracks available bytes via our own
**   write/delete accounting instead of repeated OS queries. Initialized from
**   the OS once after cleanup (and re-initialized at each level start to
**   correct any accumulated drift). A flat DRIVE_SPACE_LOW_BYTES safety
**   buffer is reserved at init time so that no reservation can ever consume
**   the last bytes on any drive -- this accounts for OS filesystem overhead,
**   MFT growth, etc.
**
**     DriveInitLedger -- query OS, subtract safety buffer, store as baseline
**     DriveReserve    -- CAS-loop subtract; false (no change) if insufficient
**     DriveReclaim    -- unconditional add (file deleted or overestimate returned)
**     DriveDebit      -- unconditional subtract (actual bytes written; single-writer paths)
**     DriveAvailable  -- read current value (display / threshold checks)
**
** Notes:
**   Adapted from an earlier solver implementation; logic unchanged, only
**   the state type it operates on is renamed (-> OthelloRingMasterState).
*/

#pragma once

/* Includes */
#include "OthelloTypes.h"
#include <windows.h>

/* Functions */

/*
** Function: DriveInitLedger
** @brief    Queries the OS, subtracts the safety buffer, and stores the
**           result as a drive's available-bytes baseline.
** @details  Call after cleanup, before any writes begin for a level.
** @param    pSt    - solver state whose driveLedger entry gets initialized
** @param    letter - drive letter to query
*/
static inline void DriveInitLedger(POthelloRingMasterState pSt, char letter,
                                    uint64_t safetyBufferGBOverride = 0)
{
    char           root[4] = { letter, ':', '\\', '\0' };
    ULARGE_INTEGER freeAvail = {};

    /* Query the OS directly rather than trusting any cached figure -- this
    ** runs once per level to correct drift the ledger's own accounting
    ** might have accumulated. safetyBufferGBOverride (0 = use the real
    ** DRIVE_SPACE_LOW_GB default) lets a test run's --drive-space-low-gb
    ** shrink the effective ceiling so DriveReserve starts failing -- and
    ** therefore iMerge starts triggering -- on tiny real data, the same
    ** "lie about the constraint, not the logic" pattern --memory-limit
    ** already uses for flush frequency.
    */
    uint64_t safetyBufferBytes = safetyBufferGBOverride
        ? safetyBufferGBOverride * 1024ULL * 1024ULL * 1024ULL
        : DRIVE_SPACE_LOW_BYTES;

    /* The query's result MUST be checked: a failure leaves freeAvail at zero,
    ** which would silently seed this drive's ledger as completely full and
    ** send every later reservation down the out-of-space path for a reason
    ** that has nothing to do with space. A drive that is only briefly
    ** unreachable (a network share reconnecting) gets a few spaced retries
    ** first; if it stays unreachable, stopping here with the real cause is
    ** far clearer than the misleading out-of-space failure that would
    ** otherwise follow.
    */
    BOOL  queried   = FALSE;   /* true once the free-space query has succeeded     */
    DWORD lastError = 0;       /* Windows error code from the most recent failure */

    for (int attempt = 0; attempt < 5 && !queried; attempt++)
    {
        if (attempt > 0)
            Sleep(5000);

        queried = GetDiskFreeSpaceExA(root, &freeAvail, nullptr, nullptr);
        if (!queried)
            lastError = GetLastError();
    }

    if (!queried)
        Fatal(FATAL_DRIVE_SPACE,
              "DriveInitLedger: cannot query free space on %c: (Windows error %lu) after 5 attempts -- "
              "the drive may be disconnected. Refusing to continue with an unknown amount of free space.\n",
              letter, (unsigned long)lastError);

    int64_t available = (int64_t)freeAvail.QuadPart - (int64_t)safetyBufferBytes;
    if (available < 0) available = 0;
    InterlockedExchange64(
        (volatile LONG64*)&pSt->driveLedger[(unsigned char)(letter - 'A')],
        (LONG64)available);
}

/*
** Function: DriveReserve
** @brief    Atomically reserves bytes on a drive: subtracts from the ledger
**           only if available >= bytes.
** @param    pSt    - solver state whose driveLedger entry to reserve from
** @param    letter - drive letter to reserve on
** @param    bytes  - number of bytes to reserve
** @return   true on success; false (ledger unchanged) if insufficient space.
*/
[[nodiscard]] static inline bool DriveReserve(POthelloRingMasterState pSt, char letter, int64_t bytes)
{
    volatile LONG64* p = (volatile LONG64*)&pSt->driveLedger[(unsigned char)(letter - 'A')];
    LONG64           old = InterlockedCompareExchange64(p, 0, 0);

    /* CAS loop: retry if another thread updated the ledger between our read
    ** and our attempted write, so no reservation is ever lost to a race.
    */
    for (;;)
    {
        if (old < (LONG64)bytes) return false;
        LONG64 got = InterlockedCompareExchange64(p, old - (LONG64)bytes, old);
        if (got == old) return true;
        old = got;
    }
}

/*
** Function: DriveReclaim
** @brief    Returns bytes to the ledger (file deleted or overestimate corrected).
** @param    pSt    - solver state whose driveLedger entry to credit
** @param    letter - drive letter to credit
** @param    bytes  - number of bytes to return
*/
static inline void DriveReclaim(POthelloRingMasterState pSt, char letter, int64_t bytes)
{
    InterlockedAdd64(
        (volatile LONG64*)&pSt->driveLedger[(unsigned char)(letter - 'A')],
        (LONG64)bytes);
}

/*
** Function: DriveDebit
** @brief    Unconditionally subtracts actual bytes written, for single-writer
**           paths where no pre-reserve is needed because only one thread
**           touches the drive.
** @param    pSt    - solver state whose driveLedger entry to debit
** @param    letter - drive letter to debit
** @param    bytes  - number of bytes to subtract
*/
static inline void DriveDebit(POthelloRingMasterState pSt, char letter, int64_t bytes)
{
    InterlockedAdd64(
        (volatile LONG64*)&pSt->driveLedger[(unsigned char)(letter - 'A')],
        -(LONG64)bytes);
}

/*
** Function: DriveAvailable
** @brief    Reads the current available-bytes figure for a drive.
** @param    pSt    - solver state whose driveLedger entry to read
** @param    letter - drive letter to read
** @return   Current available bytes (for display or threshold comparison).
*/
static inline int64_t DriveAvailable(POthelloRingMasterState pSt, char letter)
{
    return (int64_t)InterlockedCompareExchange64(
        (volatile LONG64*)&pSt->driveLedger[(unsigned char)(letter - 'A')], 0, 0);
}
