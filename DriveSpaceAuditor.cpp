/*
** Filename:  DriveSpaceAuditor.cpp
**
** Purpose:
**   Implements the drive-space-reconciliation background auditor
**   (DriveSpaceAuditor.h). See that header and
**   project_writer_drive_registry_redesign memory for the full design.
**
** Notes:
**   Simpler than the design conversation's original sketch, and more
**   robust for it: a real OS free-space query already reflects every
**   finished file's real size automatically -- there's no need to separately
**   sum finished-file sizes from the registry. The only thing the OS
**   doesn't know about is space already spoken for by in-progress
**   reservations (a write that hasn't landed, or hasn't fully landed, yet).
**   So the real reconciliation is just: real OS free space, minus the
**   standing safety buffer (same one DriveInitLedger already subtracts),
**   minus the sum of every currently-reserved node's reservedBytes on that
**   drive. That number IS what DriveLedger's tracked value should be,
**   derived from ground truth every pass -- immune to both external
**   interference and any internal DriveReserve/DriveReclaim bookkeeping
**   drift, not just one or the other.
*/

/* Includes */
#include "DriveSpaceAuditor.h"
#include "Registry.h"
#include "DriveLedger.h"
#include "Logger.h"
#include <windows.h>

/* Discrepancies smaller than this are treated as agreement (see
** reconcileOneDrive's deadband). Sized to swallow filesystem rounding and the
** small timing jitter between the running ledger and a live OS free-space
** query, while still catching a real external file copy/delete (which is
** typically much larger). Tunable if it proves too tight or too loose.
*/
static const int64_t DRIVE_AUDIT_TOLERANCE_BYTES = 4LL * 1024 * 1024 * 1024;

/* Functions */

/*
** Function: auditIntervalMs
*/
static uint64_t auditIntervalMs(PSolveContext pCtx)
{
    uint32_t sec = pCtx->pConfig->auditIntervalSecondsOverride
                 ? pCtx->pConfig->auditIntervalSecondsOverride
                 : (uint32_t)AUDIT_INTERVAL_SECONDS_DEFAULT;
    return (uint64_t)sec * 1000ULL;
}

/*
** Function: reconcileOneDrive
** @brief    Recomputes real available bytes for one drive from ground
**           truth and corrects the ledger if it disagrees, with asymmetric
**           urgency: corrects immediately if the real number is LOWER than
**           the ledger's (unambiguous -- something external consumed
**           space, dangerous to leave stale since a future DriveReserve
**           could succeed on space that isn't really there); requires the
**           discrepancy to persist across 2 consecutive passes if the real
**           number is HIGHER (routinely confounded with normal reservation
**           overestimate slack, not worth correcting eagerly).
** @param    pSt            - solver state
** @param    driveLetter    - drive to reconcile
** @param    writerIdx      - registry index for this drive, or -1 if this
**                            drive has no registry (a medium/store drive --
**                            only outstanding reservations matter, and
**                            there are none to sum in that case)
** @param    wasHigherLastPass - in/out: persists across calls for the
**                            "real is higher" case's 2-pass requirement
*/
static void reconcileOneDrive(PSolveContext pCtx, char driveLetter, int writerIdx,
                               bool& wasHigherLastPass)
{
    POthelloRingMasterState pSt = pCtx->pState;

    char root[4] = { driveLetter, ':', '\\', '\0' };
    ULARGE_INTEGER freeAvail = {};
    if (!GetDiskFreeSpaceExA(root, &freeAvail, nullptr, nullptr))
        return;   /* transient failure (e.g. a NAS drive momentarily unreachable) -- try again next pass */

    int64_t outstandingReservations = 0;
    if (writerIdx >= 0)
    {
        EnterCriticalSection(&pSt->driveRegistryCS[writerIdx]);
        for (auto& n : pSt->driveRegistry[writerIdx])
            if (n.isReserved)
                outstandingReservations += n.reservedBytes;
        LeaveCriticalSection(&pSt->driveRegistryCS[writerIdx]);
    }

    /* Skip while a writer drive has an in-flight write outstanding. OS free
    ** space reflects the bytes written SO FAR, but the reservation counts the
    ** file's full worst-case size, so realAvailable would subtract those
    ** in-flight bytes twice and report a spurious shortfall -- every pass,
    ** for the whole duration of a big flush. During active I/O the ledger's
    ** own reserve/reclaim accounting is trusted; reconcile only in the
    ** quiescent gaps between flushes (which is also when an external change
    ** would actually be noticeable). This was the main source of the
    ** "correcting down / correcting up" churn seen on the first real run.
    */
    if (writerIdx >= 0 && outstandingReservations > 0)
    {
        wasHigherLastPass = false;
        return;
    }

    /* Same safety buffer the ledger itself was seeded with (DriveInitLedger,
    ** DriveLedger.h) -- writer drives only respect --drive-space-low-gb,
    ** matching InitSolver.cpp's own convention, so this auditor never
    ** "corrects" the ledger back to a different baseline than intended.
    */
    uint64_t safetyBufferGB = (writerIdx >= 0) ? pCtx->pConfig->driveSpaceLowGBOverride : 0;
    uint64_t safetyBufferBytes = safetyBufferGB ? safetyBufferGB * 1024ULL * 1024ULL * 1024ULL
                                                 : DRIVE_SPACE_LOW_BYTES;

    int64_t realAvailable = (int64_t)freeAvail.QuadPart - (int64_t)safetyBufferBytes - outstandingReservations;
    if (realAvailable < 0) realAvailable = 0;

    int64_t ledgerValue = DriveAvailable(pSt, driveLetter);
    int64_t diff        = realAvailable - ledgerValue;   /* positive = real has more room than the ledger thinks */

    /* Deadband: treat sub-threshold differences as agreement. Filesystem
    ** rounding and small ledger-vs-live-query timing jitter routinely produce
    ** fractional-GB differences that are not real drift; correcting them just
    ** spams the log and churns the ledger. Only a sizable, genuine divergence
    ** (an externally copied/deleted file) is worth acting on.
    */
    int64_t absDiff = (diff < 0) ? -diff : diff;
    if (absDiff < DRIVE_AUDIT_TOLERANCE_BYTES)
    {
        wasHigherLastPass = false;
        return;
    }

    if (diff < 0)
    {
        /* Real usage is higher than expected -- unambiguous, correct now. */
        LoggerLog("WARNING DriveSpaceAuditor: %c: ledger says %.2f GB free, real is %.2f GB -- "
                  "correcting down (external consumption or ledger drift)\n",
                  driveLetter, ledgerValue / (1024.0*1024.0*1024.0), realAvailable / (1024.0*1024.0*1024.0));
        DriveReclaim(pSt, driveLetter, diff);   /* diff is negative -- nets to a subtraction */
        wasHigherLastPass = false;
        return;
    }

    /* Real usage is lower than expected -- likely just normal reservation
    ** slack; require it to persist across 2 passes before correcting.
    */
    if (wasHigherLastPass)
    {
        LoggerLog("DriveSpaceAuditor: %c: ledger says %.2f GB free, real is %.2f GB -- "
                  "correcting up (persisted across 2 passes)\n",
                  driveLetter, ledgerValue / (1024.0*1024.0*1024.0), realAvailable / (1024.0*1024.0*1024.0));
        DriveReclaim(pSt, driveLetter, diff);
        wasHigherLastPass = false;
    }
    else
    {
        wasHigherLastPass = true;
    }
}

/*
** Function: DriveSpaceAuditorLoop
** @brief    See DriveSpaceAuditor.h.
*/
void DriveSpaceAuditorLoop(PSolveContext pCtx)
{
    POthelloRingMasterState pSt = pCtx->pState;

    bool writerHigherLastPass[MAX_WRITERS] = {};
    bool mergeDirHigherLastPass[MAX_WRITER_DRIVES] = {};
    bool storeHigherLastPass = false;

    while (!pSt->terminateThreads)
    {
        Sleep((DWORD)auditIntervalMs(pCtx));
        if (pSt->terminateThreads) break;

        for (int i = 0; i < pSt->numMergeWriters; i++)
            reconcileOneDrive(pCtx, pSt->mwDirectory[i][0], i, writerHigherLastPass[i]);

        for (int i = 0; i < pSt->numMergeDirs; i++)
            reconcileOneDrive(pCtx, pSt->mergeDirectory[i][0], -1, mergeDirHigherLastPass[i]);

        reconcileOneDrive(pCtx, pCtx->pConfig->storeDrive, -1, storeHigherLastPass);
    }
}
