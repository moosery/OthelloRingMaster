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
#include <map>

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
** Constants: NO_PROGRESS_STALE_MS / NO_PROGRESS_LINK_FALLBACK_MS
** @brief    v1.0.6 gave the old duration-only stuck check role-specific
**           allowances (15 min flush/final-merge, 4 hr consol, 96 hr iMerge)
**           after real level-16/17 data showed a flat "consol is always
**           fast" guess false-positiving on perfectly healthy, still-moving
**           merges. v1.0.7 replaces duration entirely with real progress:
**           every reserved node now links to whichever live counter its job
**           is updating (RegistryLinkProgress, Registry.h) -- a node is only
**           suspicious once that counter stops moving, not once a clock runs
**           out. This scales to any level/file size with zero per-role
**           tuning, and catches an actual leak FASTER than the old flat
**           thresholds did (a truly abandoned reservation goes stale within
**           a few minutes, not hours). NO_PROGRESS_LINK_FALLBACK_MS is only
**           for the rare node with no progress link at all (iMerge's
**           single-file MoveFileExA path, which reports no incremental
**           progress) -- generous, since that's a real but normally-quick
**           cross-drive copy, not merge work.
*/
static const uint64_t NO_PROGRESS_STALE_MS         = 3ULL * 60 * 1000;    /* 3 minutes with zero movement */
static const uint64_t NO_PROGRESS_LINK_FALLBACK_MS = 60ULL * 60 * 1000;   /* 60 minutes, unlinked nodes only */

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

    /* Per-node progress-staleness tracking, per drive: last observed value of
    ** whatever counter a reserved node is linked to, and when it last
    ** actually changed. Rebuilt fresh each pass from the prior pass's
    ** values, so a node that disappears (finished/removed) is naturally
    ** dropped rather than accumulating forever.
    */
    struct ProgressState { int64_t lastValue; ClockTick lastChangeTick; };
    std::map<std::string, ProgressState> progressTrack[MAX_WRITERS];

    while (!pSt->terminateThreads)
    {
        Sleep((DWORD)auditIntervalMs(pCtx));
        if (pSt->terminateThreads) break;

        for (int wi = 0; wi < pSt->numMergeWriters; wi++)
        {
            /* Snapshot the registry briefly under lock. */
            struct Snap { std::string filename; int64_t size; bool reserved; uint8_t reservedBy;
                          ClockTick sinceTick; volatile int64_t* pProgress; };
            std::vector<Snap> snap;
            EnterCriticalSection(&pSt->driveRegistryCS[wi]);
            for (auto& n : pSt->driveRegistry[wi])
                snap.push_back({ n.filename, n.physfilesize, n.isReserved, n.reservedBy,
                                  n.reservedSinceTick, n.pProgressBytes });
            LeaveCriticalSection(&pSt->driveRegistryCS[wi]);

            std::set<std::string> thisPassSuspects;
            std::map<std::string, ProgressState> thisPassTrack;

            for (auto& s : snap)
            {
                if (s.reserved)
                {
                    long long idleMs;
                    bool      stale;

                    if (s.pProgress)
                    {
                        /* Linked to a live counter -- alive as long as it's
                        ** still moving, no matter how long that legitimately
                        ** takes at whatever data volume this level has.
                        */
                        int64_t cur = *s.pProgress;
                        ProgressState st;
                        auto it = progressTrack[wi].find(s.filename);
                        if (it != progressTrack[wi].end() && it->second.lastValue == cur)
                            st = it->second;             /* unchanged -- keep the stale-clock running */
                        else
                        {
                            st.lastValue = cur;
                            ClockStart(&st.lastChangeTick);   /* first sight, or genuinely moved -- reset */
                        }
                        thisPassTrack[s.filename] = st;
                        idleMs = ClockMillisSinceStart(&st.lastChangeTick);
                        stale  = (uint64_t)idleMs > NO_PROGRESS_STALE_MS;
                    }
                    else
                    {
                        /* No progress link (e.g. iMerge's single-file move) --
                        ** fall back to a generous flat allowance since the
                        ** age of the reservation is all there is to go on.
                        */
                        idleMs = ClockMillisSinceStart(&s.sinceTick);
                        stale  = (uint64_t)idleMs > NO_PROGRESS_LINK_FALLBACK_MS;
                    }

                    if (stale)
                    {
                        std::string key = s.filename + "|stuck";
                        thisPassSuspects.insert(key);
                        if (prevSuspects[wi].count(key))
                            LoggerLog("WARNING RegistryAuditor: '%s' reserved, no progress for %.1f min (role %d) -- possible leaked reservation\n",
                                      s.filename.c_str(), idleMs / 60000.0, (int)s.reservedBy);
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

            prevSuspects[wi]   = std::move(thisPassSuspects);
            progressTrack[wi]  = std::move(thisPassTrack);
        }
    }
}
