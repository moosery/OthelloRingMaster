/*
** Filename:  RegistryAuditor.cpp
**
** Purpose:
**   Implements the registry-vs-disk background auditor (RegistryAuditor.h).
**   See that header and project_writer_drive_registry_redesign memory for
**   the full design: two separate checks (size-mismatch, stuck-reservation),
**   both WARNING-severity, both requiring 2+ consecutive passes before
**   being reported (tolerating the audit's own non-atomic observation
**   racing real concurrent activity).
*/

/* Includes */
#include "RegistryAuditor.h"
#include "Registry.h"
#include "RSFFileName.h"
#include "Logger.h"
#include <windows.h>
#include <string>
#include <vector>
#include <set>

/* Functions */

/*
** Function: stuckThresholdMs
** @brief    How long a node may sit reserved before RegistryAuditor
**           considers it suspicious. Role-aware:
**             - flush: always a single buffered write of one MW buffer,
**               genuinely fast regardless of level scale -- short allowance.
**             - consol: WRONG ASSUMPTION FIXED v1.0.6 -- originally assumed
**               "always fast (minutes at most, even for multi-GB files)",
**               but confirmed false against live production data at level
**               16/17 scale: a single consolidation job now k-way-merges
**               several tens-of-GB inputs on an 8-thread pool shared across
**               every other in-flight consol job, and real jobs were seen
**               still legitimately in-flight past 37 minutes (matching
**               ConsolidatorWorkerBody completion lines appeared later for
**               the same filenames -- not leaks, just slow at this scale).
**               Consol is bounded by MAX_FILE_SIZE (the consolidation size
**               cap) the way iMerge is not, so it doesn't need iMerge's full
**               96-hour allowance, but needs far more than 15 minutes.
**             - imerge: a cross-drive iMerge's own new-output node can
**               legitimately stay reserved for many real hours (confirmed
**               directly against live production data -- a single iMerge
**               session gathering across both NVMe drives has run 50+ hours).
*/
static uint64_t stuckThresholdMs(uint8_t reservedBy)
{
    if (reservedBy == REGISTRY_RESERVED_IMERGE)
        return 96ULL * 60 * 60 * 1000;   /* 96 hours */
    if (reservedBy == REGISTRY_RESERVED_CONSOL)
        return 4ULL * 60 * 60 * 1000;    /* 4 hours -- bounded by MAX_FILE_SIZE, but real
                                          ** jobs already run 30-40+ min at level 16/17 */
    return 15ULL * 60 * 1000;            /* 15 minutes -- flush/final-merge */
}

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
** Function: RegistryAuditorLoop
** @brief    See RegistryAuditor.h.
*/
void RegistryAuditorLoop(PSolveContext pCtx)
{
    POthelloRingMasterState pSt = pCtx->pState;

    /* Suspects carried from the previous pass, per drive -- a discrepancy
    ** only gets logged once it appears in two consecutive passes for the
    ** exact same filename+reason.
    */
    std::set<std::string> prevSuspects[MAX_WRITERS];

    while (!pSt->terminateThreads)
    {
        Sleep((DWORD)auditIntervalMs(pCtx));
        if (pSt->terminateThreads) break;

        for (int wi = 0; wi < pSt->numMergeWriters; wi++)
        {
            /* Snapshot the registry briefly under lock. */
            struct Snap { std::string filename; int64_t size; bool reserved; uint8_t reservedBy; uint64_t sinceMs; };
            std::vector<Snap> snap;
            EnterCriticalSection(&pSt->driveRegistryCS[wi]);
            for (auto& n : pSt->driveRegistry[wi])
                snap.push_back({ n.filename, n.physfilesize, n.isReserved, n.reservedBy, n.reservedSinceTickMs });
            LeaveCriticalSection(&pSt->driveRegistryCS[wi]);

            uint64_t nowMs = GetTickCount64();
            std::set<std::string> thisPassSuspects;

            for (auto& s : snap)
            {
                if (s.reserved)
                {
                    uint64_t heldMs = nowMs - s.sinceMs;
                    if (heldMs > stuckThresholdMs(s.reservedBy))
                    {
                        std::string key = s.filename + "|stuck";
                        thisPassSuspects.insert(key);
                        if (prevSuspects[wi].count(key))
                            LoggerLog("WARNING RegistryAuditor: '%s' reserved for %.1f min (role %d) -- possible leaked reservation\n",
                                      s.filename.c_str(), heldMs / 60000.0, (int)s.reservedBy);
                    }
                    continue;   /* size is expected to be 0/partial while reserved -- not a mismatch */
                }

                WIN32_FILE_ATTRIBUTE_DATA fad = {};
                if (!GetFileAttributesExA(s.filename.c_str(), GetFileExInfoStandard, &fad))
                {
                    std::string key = s.filename + "|missing";
                    thisPassSuspects.insert(key);
                    if (prevSuspects[wi].count(key))
                        LoggerLog("WARNING RegistryAuditor: '%s' is in the registry but missing on disk\n",
                                  s.filename.c_str());
                    continue;
                }
                int64_t realSize = ((int64_t)fad.nFileSizeHigh << 32) | (int64_t)fad.nFileSizeLow;
                if (realSize != s.size)
                {
                    std::string key = s.filename + "|size";
                    thisPassSuspects.insert(key);
                    if (prevSuspects[wi].count(key))
                        LoggerLog("WARNING RegistryAuditor: '%s' registry size %lld != real size %lld\n",
                                  s.filename.c_str(), (long long)s.size, (long long)realSize);
                }
            }

            prevSuspects[wi] = std::move(thisPassSuspects);
        }
    }
}
