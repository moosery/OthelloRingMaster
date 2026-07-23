/*
** Filename:  ClaimRegistry.h
**
** Purpose:
**   Declares the per-(writer, color) claim registry operations: which
**   writer-file indices are currently "spoken for" -- a file actively being
**   written (a flush, or a background-consolidation output), or an existing
**   file claimed as input to a live consolidation/cross-drive merge. Flush
**   and consolidation both write directly to the final filename from the
**   moment a file is opened (no temp-name-then-rename), so a directory scan
**   must check ClaimIsHeld before trusting anything it finds -- a partial
**   file is otherwise indistinguishable from a complete one by size/
**   attributes alone.
**
**     ClaimSingle       -- claim one fresh index unconditionally; Fatals if
**                          already claimed (a real logic bug -- FileTicketNext
**                          guarantees this index was never handed out before)
**     ClaimTryRange     -- claim N existing indices all-or-nothing; false
**                          (nothing claimed) if any already held -- a normal,
**                          expected race, not an error
**     ClaimReleaseOne / ClaimReleaseRange -- release index/indices; Fatals if
**                          any given index isn't currently held
**     ClaimIsHeld       -- peek whether one index is currently claimed
**
** Notes:
**   The struct type (ClaimRegistry) lives in OthelloTypes.h alongside
**   WriterDriveStats/LevelStats, since its operations here need
**   POthelloRingMasterState. Each instance's CRITICAL_SECTION is held only
**   for the .claimed set mutation/check itself, never across any I/O --
**   unlike the old fileIndexCS it replaces (see OthelloTypes.h's
**   mwNextFileIdx comment for the corruption that fixed). Initialized/
**   destroyed in InitSolver.cpp/CleanupSolver the same way fileIndexCS used
**   to be; per-level reset just clears .claimed (OthelloRingMaster.cpp), the
**   CRITICAL_SECTION itself is never re-init'd mid-run.
*/

#pragma once

/* Includes */
#include "OthelloTypes.h"
#include <windows.h>

/* Functions */

/*
** Function: ClaimSingle
** @brief    Claims one fresh index unconditionally. Fatals if already
**           claimed -- FileTicketNext guarantees a fresh ticket was never
**           handed out before, so a collision here is a real bookkeeping
**           bug, not a race.
** @param    pSt       - solver state
** @param    writerIdx - which writer drive
** @param    player    - RSF_PLAYER_BLACK or RSF_PLAYER_WHITE
** @param    idx       - the index to claim
*/
static inline void ClaimSingle(POthelloRingMasterState pSt, int writerIdx, int player, int idx)
{
    ClaimRegistry* pReg = &pSt->claimRegistry[writerIdx][player];
    EnterCriticalSection(&pReg->cs);
    bool inserted = pReg->claimed.insert(idx).second;
    LeaveCriticalSection(&pReg->cs);
    if (!inserted)
        Fatal(FATAL_MERGE_LOGIC_ERROR,
              "ClaimSingle: index %d already claimed for writer %d player %d -- "
              "FileTicketNext/claim bookkeeping is broken", idx, writerIdx, player);
}

/*
** Function: ClaimTryRange
** @brief    All-or-nothing claim over a batch of existing-file indices --
**           either every index in the batch is unclaimed and all get
**           claimed atomically, or none do. Losing this race is normal
**           (another scanner got there first); callers must abandon
**           cleanly, not retry in a loop.
** @param    pSt       - solver state
** @param    writerIdx - which writer drive
** @param    player    - RSF_PLAYER_BLACK or RSF_PLAYER_WHITE
** @param    indices   - indices to claim
** @param    count     - number of indices
** @return   true if the whole batch was claimed; false (nothing claimed) if
**           any index was already held.
*/
static inline bool ClaimTryRange(POthelloRingMasterState pSt, int writerIdx, int player,
                                  const int* indices, int count)
{
    ClaimRegistry* pReg = &pSt->claimRegistry[writerIdx][player];
    EnterCriticalSection(&pReg->cs);
    for (int i = 0; i < count; i++)
    {
        if (pReg->claimed.find(indices[i]) != pReg->claimed.end())
        {
            LeaveCriticalSection(&pReg->cs);
            return false;   /* lost the race -- normal, not an error */
        }
    }
    for (int i = 0; i < count; i++)
        pReg->claimed.insert(indices[i]);
    LeaveCriticalSection(&pReg->cs);
    return true;
}

/*
** Function: ClaimReleaseRange
** @brief    Releases one or more previously-claimed indices. Fatals if any
**           given index isn't currently held -- release without a matching
**           claim is a real bookkeeping bug, never an expected outcome.
** @param    pSt       - solver state
** @param    writerIdx - which writer drive
** @param    player    - RSF_PLAYER_BLACK or RSF_PLAYER_WHITE
** @param    indices   - indices to release
** @param    count     - number of indices
*/
static inline void ClaimReleaseRange(POthelloRingMasterState pSt, int writerIdx, int player,
                                      const int* indices, int count)
{
    ClaimRegistry* pReg = &pSt->claimRegistry[writerIdx][player];
    EnterCriticalSection(&pReg->cs);
    for (int i = 0; i < count; i++)
    {
        if (pReg->claimed.erase(indices[i]) == 0)
        {
            LeaveCriticalSection(&pReg->cs);
            Fatal(FATAL_MERGE_LOGIC_ERROR,
                  "ClaimReleaseRange: index %d not held for writer %d player %d -- "
                  "release without a matching claim", indices[i], writerIdx, player);
        }
    }
    LeaveCriticalSection(&pReg->cs);
}

/*
** Function: ClaimReleaseOne
** @brief    Releases a single previously-claimed index. See ClaimReleaseRange.
** @param    pSt       - solver state
** @param    writerIdx - which writer drive
** @param    player    - RSF_PLAYER_BLACK or RSF_PLAYER_WHITE
** @param    idx       - the index to release
*/
static inline void ClaimReleaseOne(POthelloRingMasterState pSt, int writerIdx, int player, int idx)
{
    ClaimReleaseRange(pSt, writerIdx, player, &idx, 1);
}

/*
** Function: ClaimIsHeld
** @brief    Peeks whether one index is currently claimed -- used when
**           scanning a directory for available candidates.
** @param    pSt       - solver state
** @param    writerIdx - which writer drive
** @param    player    - RSF_PLAYER_BLACK or RSF_PLAYER_WHITE
** @param    idx       - the index to check
** @return   true if currently claimed.
*/
static inline bool ClaimIsHeld(POthelloRingMasterState pSt, int writerIdx, int player, int idx)
{
    ClaimRegistry* pReg = &pSt->claimRegistry[writerIdx][player];
    EnterCriticalSection(&pReg->cs);
    bool held = pReg->claimed.find(idx) != pReg->claimed.end();
    LeaveCriticalSection(&pReg->cs);
    return held;
}
