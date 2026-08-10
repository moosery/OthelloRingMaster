/*
** Filename:  Registry.h
**
** Purpose:
**   Declares the operations on the per-drive file registry
**   (OthelloRingMasterState.driveRegistry, RegistryFileNode -- both declared
**   in OthelloTypes.h, mirroring how DriveLedger.h's data lives in that same
**   struct). This is the sole source of truth this project uses for "does
**   this file exist, and is it currently in use" -- every ticket-number-
**   derived proxy for the same question (mwNextFileIdx-as-logic,
**   ClaimRegistry, mwXxxFilesConsumed, pConsolUp/mwXxxConsolidatedUpTo,
**   mwXxxPhysicalFileCount) is retired in favor of this.
**
** Notes:
**   RegistryNextFileIdx is the ONLY thing here that still hands out a
**   number, and it is used exclusively to build a unique filename --
**   nothing in this file, or anywhere else, may use it (or any node's
**   position in the list) to decide whether real backlog exists. That
**   separation is the one property that keeps this design from
**   reintroducing the bug class it replaces (see
**   project_writer_drive_registry_redesign memory for the full incident
**   this was built from).
**
**   A node's `physfilesize` field is the signal for "is this a brand-new
**   file still being written" (0 until the writer finishes) vs. "an
**   existing, finished file now claimed as merge input" (its real size,
**   already set at creation, unchanged by being reserved again as input) --
**   RegistryReserveOne (existing files) never touches reservedBytes;
**   RegistryReserveNew (brand-new files) always does. This is also exactly
**   the distinction DriveSpaceAuditor.h's from-scratch recomputation relies
**   on: `used += (physfilesize > 0) ? physfilesize : reservedBytes` for
**   every node, uniformly, regardless of which role reserved it.
**
**   Every function here locks OthelloRingMasterState.driveRegistryCS[wi]
**   only for the list mutation/scan itself, never across real file I/O --
**   same discipline the retired ClaimRegistry used.
*/

#pragma once

/* Includes */
#include "OthelloTypes.h"
#include "DriveLedger.h"

/* Functions */

/*
** Function: RegistryInit
** @brief    Initializes one drive's registry lock and clears its list. Call
**           once per writer drive during solver init (InitSolver.cpp),
**           before any other Registry* function touches that drive.
** @param    pSt      - solver state
** @param    writerIdx - which writer drive (0..numMergeWriters-1)
*/
static inline void RegistryInit(POthelloRingMasterState pSt, int writerIdx)
{
    InitializeCriticalSection(&pSt->driveRegistryCS[writerIdx]);
    pSt->driveRegistry[writerIdx].clear();
    pSt->nextFileIdx[writerIdx] = 0;
}

/*
** Function: RegistryTeardown
** @brief    Releases one drive's registry lock. Call once per writer drive
**           during solver cleanup (InitSolver.cpp's CleanupSolver).
** @param    pSt      - solver state
** @param    writerIdx - which writer drive
*/
static inline void RegistryTeardown(POthelloRingMasterState pSt, int writerIdx)
{
    DeleteCriticalSection(&pSt->driveRegistryCS[writerIdx]);
}

/*
** Function: RegistryResetForLevel
** @brief    Clears one drive's registry and naming counter at a new level's
**           start. Safe to call unconditionally -- by the time a new level
**           begins, the previous level's flush/iMerge/consolidator pools and
**           auditors are all guaranteed idle (see the final-merge stop
**           sequence), so nothing can be mid-reservation here.
** @param    pSt      - solver state
** @param    writerIdx - which writer drive
*/
static inline void RegistryResetForLevel(POthelloRingMasterState pSt, int writerIdx)
{
    EnterCriticalSection(&pSt->driveRegistryCS[writerIdx]);
    pSt->driveRegistry[writerIdx].clear();
    LeaveCriticalSection(&pSt->driveRegistryCS[writerIdx]);
    pSt->nextFileIdx[writerIdx] = 0;
}

/*
** Function: RegistryNextFileIdx
** @brief    Lock-free, atomic per-drive counter for a new file's naming
**           index. NAMING ONLY -- never consulted anywhere for a backlog,
**           trigger, or any other logic decision. Callers build their own
**           real filename from the returned index (RSFNameWriterFile and
**           friends, RSFFileName.h, for writer-drive files; RSFNameImergeFile
**           and friends for iMerge output) using whatever directory/pattern
**           is appropriate for their own operation -- this function knows
**           nothing about paths.
** @param    pSt      - solver state
** @param    writerIdx - which writer drive
** @return   The next unique index for this drive (never reused within a level).
*/
static inline int RegistryNextFileIdx(POthelloRingMasterState pSt, int writerIdx)
{
    return (int)InterlockedIncrement((volatile LONG*)&pSt->nextFileIdx[writerIdx]) - 1;
}

/*
** Function: RegistryReserveNew
** @brief    Registers a brand-new file that's about to be written (a flush
**           output, a consolidation-merge output, or an iMerge output) and
**           reserves its worst-case space via DriveReserve (DriveLedger.h)
**           in the same call, so the two can never get out of sync. On
**           DriveReserve failure, no node is created (nothing to unwind).
** @param    pSt            - solver state
** @param    writerIdx      - which writer drive
** @param    player         - RSF_PLAYER_BLACK or RSF_PLAYER_WHITE
** @param    reservedBy     - REGISTRY_RESERVED_FLUSH/CONSOL/IMERGE/FINAL_MERGE
** @param    filename       - full path, already built by the caller (see
**                            RegistryNextFileIdx's doc comment)
** @param    driveLetter    - the drive DriveReserve should debit (matches
**                            writerIdx's own drive; passed explicitly rather
**                            than derived, since the caller already has it)
** @param    reserveBytes   - worst-case byte reservation for this write
** @return   Pointer to the new node (stable for the node's whole lifetime --
**           std::list never invalidates other elements' references on
**           insert/erase), or nullptr if DriveReserve failed (real low space).
*/
static inline PRegistryFileNode RegistryReserveNew(POthelloRingMasterState pSt, int writerIdx,
                                                     int player, uint8_t reservedBy,
                                                     const char* filename, char driveLetter,
                                                     int64_t reserveBytes)
{
    if (!DriveReserve(pSt, driveLetter, reserveBytes))
        return nullptr;

    RegistryFileNode node = {};
    node.color              = (uint8_t)player;
    strncpy_s(node.filename, sizeof(node.filename), filename, _TRUNCATE);
    node.physfilesize        = 0;
    node.isReserved          = true;
    node.reservedBy          = reservedBy;
    node.reservedBytes       = reserveBytes;
    node.reservedSinceTickMs = GetTickCount64();

    EnterCriticalSection(&pSt->driveRegistryCS[writerIdx]);
    pSt->driveRegistry[writerIdx].push_back(node);
    PRegistryFileNode pNode = &pSt->driveRegistry[writerIdx].back();
    LeaveCriticalSection(&pSt->driveRegistryCS[writerIdx]);

    return pNode;
}

/*
** Function: RegistryFinishNew
** @brief    Marks a RegistryReserveNew node as done: records its real size
**           and clears the reservation. Does NOT reclaim the gap between
**           reserveBytes and the real size -- that's DriveReclaim
**           (DriveLedger.h), called explicitly by the caller right
**           alongside this, same as every existing successful-merge site
**           already does.
** @param    pSt      - solver state
** @param    writerIdx - which writer drive
** @param    pNode     - the node RegistryReserveNew returned
** @param    realSize  - the file's actual final on-disk size
*/
static inline void RegistryFinishNew(POthelloRingMasterState pSt, int writerIdx,
                                      PRegistryFileNode pNode, int64_t realSize)
{
    EnterCriticalSection(&pSt->driveRegistryCS[writerIdx]);
    pNode->physfilesize  = realSize;
    pNode->isReserved    = false;
    pNode->reservedBy    = REGISTRY_RESERVED_NONE;
    pNode->reservedBytes = 0;
    LeaveCriticalSection(&pSt->driveRegistryCS[writerIdx]);
}

/*
** Function: RegistryAbandonNew
** @brief    Removes a RegistryReserveNew node whose write never happened
**           (e.g. the merge that would have produced it terminated, or
**           later validation for the batch it belonged to failed). Erases
**           the node entirely -- unlike RegistryUnreserveOne, an abandoned
**           new-file placeholder has no real content to make available
**           again, it simply never existed. Caller is responsible for
**           deleting any partial real file and for DriveReclaim'ing
**           reserveBytes back (DriveLedger.h) -- this function only updates
**           the registry itself.
** @param    pSt      - solver state
** @param    writerIdx - which writer drive
** @param    pNode     - the node RegistryReserveNew returned
*/
static inline void RegistryAbandonNew(POthelloRingMasterState pSt, int writerIdx, PRegistryFileNode pNode)
{
    EnterCriticalSection(&pSt->driveRegistryCS[writerIdx]);
    pSt->driveRegistry[writerIdx].remove_if(
        [pNode](const RegistryFileNode& n) { return &n == pNode; });
    LeaveCriticalSection(&pSt->driveRegistryCS[writerIdx]);
}

/*
** Function: RegistryScanUnreserved
** @brief    Peek-only scan for currently-unreserved, real (physfilesize > 0)
**           files of one color on one drive, optionally size-capped. Used
**           by the consolidation master (size-capped, looking for >2
**           candidates) and by the iMerge pool (uncapped -- iMerge wants
**           every real file regardless of size, since its whole point is
**           clearing space, not staying under a local efficiency cap).
**           Does NOT reserve anything -- caller decides what to do with the
**           results, then calls RegistryReserveOne on whichever it wants
**           (which can race and lose against another thread; check its
**           return).
** @param    pSt          - solver state
** @param    writerIdx     - which writer drive
** @param    player        - RSF_PLAYER_BLACK or RSF_PLAYER_WHITE
** @param    maxSizeBytes  - only include files with physfilesize < this;
**                           pass -1 for no size limit
** @param    outArray      - caller-provided array to fill with node pointers
** @param    maxOut        - capacity of outArray
** @return   Number of nodes found (and written to outArray); may be less
**           than the true total if maxOut was hit.
*/
static inline int RegistryScanUnreserved(POthelloRingMasterState pSt, int writerIdx, int player,
                                          int64_t maxSizeBytes,
                                          PRegistryFileNode* outArray, int maxOut)
{
    int n = 0;

    EnterCriticalSection(&pSt->driveRegistryCS[writerIdx]);
    for (auto& node : pSt->driveRegistry[writerIdx])
    {
        if (n >= maxOut) break;
        if (node.color != (uint8_t)player) continue;
        if (node.isReserved) continue;
        if (node.physfilesize <= 0) continue;   /* still-being-written placeholders are never "available" */
        if (maxSizeBytes >= 0 && node.physfilesize >= maxSizeBytes) continue;
        outArray[n++] = &node;
    }
    LeaveCriticalSection(&pSt->driveRegistryCS[writerIdx]);

    return n;
}

/*
** Function: RegistryReserveOne
** @brief    Attempts to claim one existing, already-finished node (found via
**           RegistryScanUnreserved) as input to a consolidation/iMerge/
**           final-merge batch. Can lose a race against another thread that
**           claimed it first between the scan and this call -- that is a
**           normal, expected outcome, never a bug.
** @param    pSt      - solver state
** @param    writerIdx - which writer drive
** @param    pNode     - a node returned by RegistryScanUnreserved
** @param    reservedBy - REGISTRY_RESERVED_CONSOL/IMERGE/FINAL_MERGE
** @return   true if claimed; false if it was already reserved by someone else.
*/
static inline bool RegistryReserveOne(POthelloRingMasterState pSt, int writerIdx,
                                       PRegistryFileNode pNode, uint8_t reservedBy)
{
    bool claimed = false;

    EnterCriticalSection(&pSt->driveRegistryCS[writerIdx]);
    if (!pNode->isReserved)
    {
        pNode->isReserved          = true;
        pNode->reservedBy          = reservedBy;
        pNode->reservedSinceTickMs = GetTickCount64();
        claimed = true;
    }
    LeaveCriticalSection(&pSt->driveRegistryCS[writerIdx]);

    return claimed;
}

/*
** Function: RegistryUnreserveOne
** @brief    Releases a claim taken via RegistryReserveOne without removing
**           the node -- the file still exists, real and unchanged, just
**           available again (e.g. a consolidation batch that lost a
**           DriveReserve race for its own new output file after already
**           claiming its inputs releases each input this way). NOT for
**           RegistryReserveNew nodes -- see RegistryAbandonNew for those.
** @param    pSt      - solver state
** @param    writerIdx - which writer drive
** @param    pNode     - the node to release
*/
static inline void RegistryUnreserveOne(POthelloRingMasterState pSt, int writerIdx, PRegistryFileNode pNode)
{
    EnterCriticalSection(&pSt->driveRegistryCS[writerIdx]);
    pNode->isReserved = false;
    pNode->reservedBy = REGISTRY_RESERVED_NONE;
    LeaveCriticalSection(&pSt->driveRegistryCS[writerIdx]);
}

/*
** Function: RegistryRemoveNode
** @brief    Erases a node entirely -- for a real file that's just been
**           deleted (absorbed into a consolidation/iMerge/final-merge
**           output). Caller sequences the real DeleteFileA and any
**           DriveReclaim (DriveLedger.h) around this call itself; this
**           function only updates the registry.
** @param    pSt      - solver state
** @param    writerIdx - which writer drive
** @param    pNode     - the node to remove
*/
static inline void RegistryRemoveNode(POthelloRingMasterState pSt, int writerIdx, PRegistryFileNode pNode)
{
    EnterCriticalSection(&pSt->driveRegistryCS[writerIdx]);
    pSt->driveRegistry[writerIdx].remove_if(
        [pNode](const RegistryFileNode& n) { return &n == pNode; });
    LeaveCriticalSection(&pSt->driveRegistryCS[writerIdx]);
}

/*
** Function: RegistryRemoveByFilename
** @brief    Erases any node whose filename matches (case-insensitive), for a
**           real file just deleted where the caller has the path but not the
**           node pointer -- notably DoEndOfLevelMerge, which consumes and
**           deletes writer files by path. Keeps the registry truthful the
**           moment a file leaves disk, so the registry-vs-disk auditor
**           doesn't flag a stale node until the next RegistryResetForLevel.
**           No-op if no node matches (e.g. the path was on a non-writer drive
**           with no registry).
** @param    pSt      - solver state
** @param    writerIdx - which writer drive
** @param    filename  - full path of the file just deleted
*/
static inline void RegistryRemoveByFilename(POthelloRingMasterState pSt, int writerIdx, const char* filename)
{
    EnterCriticalSection(&pSt->driveRegistryCS[writerIdx]);
    pSt->driveRegistry[writerIdx].remove_if(
        [filename](const RegistryFileNode& n) { return _stricmp(n.filename, filename) == 0; });
    LeaveCriticalSection(&pSt->driveRegistryCS[writerIdx]);
}

/*
** Function: RegistryRebuildFromDisk
** @brief    Populates one drive's registry from a real directory scan of its
**           writer files -- used on restart when resuming from a valid
**           checkpoint (Checkpoint.h has already cross-checked the scan
**           results against the checkpoint's own integrity manifest by the
**           time this runs; see InitSolver.cpp). Every file found this way
**           is real and finished (nothing is ever left mid-write on disk --
**           a checkpoint is only taken after a full drain), so every node
**           gets isReserved=false and its real on-disk size immediately.
**           Call RegistryInit first to clear any prior state and set up the
**           lock; this function only appends.
** @param    pSt      - solver state
** @param    writerIdx - which writer drive
** @param    dir       - the writer directory to scan (pSt->mwDirectory[writerIdx])
*/
static inline void RegistryRebuildFromDisk(POthelloRingMasterState pSt, int writerIdx, const char* dir)
{
    for (int player = RSF_PLAYER_WHITE; player <= RSF_PLAYER_BLACK; player++)
    {
        char patterns[3][MAX_FULL_PATH_NAME];
        RSFPatternWriterFiles(patterns[0], sizeof(patterns[0]), dir, player);
        RSFZPatternWriterFiles(patterns[1], sizeof(patterns[1]), dir, player);
        RSFZLPatternWriterFiles(patterns[2], sizeof(patterns[2]), dir, player);

        for (int p = 0; p < 3; p++)
        {
            WIN32_FIND_DATAA fd;
            HANDLE h = FindFirstFileA(patterns[p], &fd);
            if (h == INVALID_HANDLE_VALUE) continue;

            do
            {
                RegistryFileNode node = {};
                node.color       = (uint8_t)player;
                snprintf(node.filename, sizeof(node.filename), "%s\\%s", dir, fd.cFileName);
                ULARGE_INTEGER sz;
                sz.LowPart       = fd.nFileSizeLow;
                sz.HighPart      = (DWORD)fd.nFileSizeHigh;
                node.physfilesize = (int64_t)sz.QuadPart;
                node.isReserved   = false;
                node.reservedBy   = REGISTRY_RESERVED_NONE;
                node.reservedBytes = 0;

                EnterCriticalSection(&pSt->driveRegistryCS[writerIdx]);
                pSt->driveRegistry[writerIdx].push_back(node);
                LeaveCriticalSection(&pSt->driveRegistryCS[writerIdx]);
            } while (FindNextFileA(h, &fd));
            FindClose(h);
        }
    }
}

/*
** Function: RegistryCount
** @brief    Real, current physical file count for one color on one drive --
**           replaces the old mwXxxPhysicalFileCount counters (this IS the
**           list length, computed fresh, never separately maintained).
**           Cheap: real per-drive file counts stay small (confirmed
**           repeatedly against live production data).
** @param    pSt      - solver state
** @param    writerIdx - which writer drive
** @param    player    - RSF_PLAYER_BLACK or RSF_PLAYER_WHITE
** @return   Number of real nodes (reserved or not) of that color on that drive.
*/
static inline int RegistryCount(POthelloRingMasterState pSt, int writerIdx, int player)
{
    int n = 0;

    EnterCriticalSection(&pSt->driveRegistryCS[writerIdx]);
    for (auto& node : pSt->driveRegistry[writerIdx])
        if (node.color == (uint8_t)player) n++;
    LeaveCriticalSection(&pSt->driveRegistryCS[writerIdx]);

    return n;
}
