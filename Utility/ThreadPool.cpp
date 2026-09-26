/*
** Filename:  ThreadPool.cpp
**
** Purpose:
**   Implements the ThreadPool class declared in ThreadPool.h: a fixed-size
**   worker-thread pool pulling jobs from a bounded queue. Also implements
**   the checked event helpers (CreateEventOrFatal, SetEventOrFatal,
**   CloseHandleOrFatal, WaitForEventsOrFatal) used wherever one thread waits
**   for another's completion signal.
**
** Notes:
**   The event helpers exist because every wait/create/set/close call has a
**   failure result that must never be ignored. An ignored failed wait
**   returns immediately, and the caller then carries on while the work it
**   was waiting for is still running -- silent corruption of whatever that
**   work was still using. These helpers stop the process with the real cause
**   instead, and turn an unbounded wait into one that periodically says
**   what it is still waiting for.
*/

/* Includes */
#include "ThreadPool.h"
#include <Windows.h>
#include <string>
#include <wchar.h>
#include "Error.h"
#include "Logger.h"

using namespace std;

//#define THREADPOOL_VERBOSE

/* Macros and Defines */
#define WAIT_SLICE_MS          60000ULL                /* how long one wait blocks before it re-checks and may report              */
#define WAIT_FIRST_REPORT_MS   (15ULL * 60 * 1000)     /* no-probe fallback: first "still waiting" note once a wait has lasted this long */
#define WAIT_REPEAT_REPORT_MS  (30ULL * 60 * 1000)     /* no-probe fallback: the note then repeats every this long                  */
#define WAIT_ETA_MIN_SAMPLE_MS (5ULL * 60 * 1000)      /* with a probe: measure this long before trusting a rate for an ETA         */
#define WAIT_OVERDUE_SLACK_MS  (60ULL * 60 * 1000)     /* with a probe: report once the wait is this far past its own ETA           */
#define WAIT_OVERDUE_REPEAT_MS (60ULL * 60 * 1000)     /* with a probe: the overdue note then repeats every this long               */
#define WAIT_STALL_MS          (30ULL * 60 * 1000)     /* with a probe: report if progress has not moved for this long              */

/* Functions */

/*
** Method: ThreadPool::Start
** @brief  Spins up num_threads worker OS threads, each running ThreadLoop.
*/
void ThreadPool::Start()
{
    /* store()-initialized rather than memset: memset-ing a std::atomic<bool>
    ** array isn't strictly portable (the standard requires construction, not
    ** just zeroed bytes), even though it happens to work on every real target
    ** this project builds for. Cheap enough to do it the correct way.
    */
    isBusyArray = (std::atomic<bool>*)MemMalloc((char*)"bool.Array.ThreadPool", sizeof(std::atomic<bool>) * num_threads);

    /* Every other allocation in the project stops with a clear message on
    ** failure; without this the loop below would write through a null pointer.
    */
    if (isBusyArray == NULL)
        Fatal(FATAL_ALLOCATION_FAILED, "ThreadPool::Start: cannot allocate the busy-flag array for pool '%s' (%u threads)\n",
              m_threadName.c_str(), (unsigned)num_threads);

    for (uint32_t i = 0; i < num_threads; i++)
        isBusyArray[i].store(false, std::memory_order_relaxed);
    readyCount.store(0, std::memory_order_relaxed);

    threads.resize(num_threads);
    for (uint32_t i = 0; i < num_threads; i++)
    {
        threads.at(i) = thread([this, i] { this->ThreadLoop(i); });
        num_running++;
    }
    isStarted = true;
}

/*
** Method: ThreadPool::QueueJob
** @brief  Enqueues job for a worker thread to run, blocking the caller
**         if the queue is already at MAX_QUEUE_DEPTH.
** @param  job - the work to run; called as job(workerIndex) by whichever worker picks it up
** @return true if the job was queued; false if the pool is shutting down and the job was
**         dropped. The caller MUST NOT wait for a dropped job to finish -- it never will.
*/
bool ThreadPool::QueueJob(const std::function<void(uint32_t)>& job)
{
    {
        unique_lock<mutex> lock(queue_mutex);
        queue_not_full.wait(lock, [this] { return jobs.size() < MAX_QUEUE_DEPTH || should_terminate; });

        /* A pool that is stopping discards new work. Say so through the
        ** return value: a caller that then waited for this job would wait
        ** forever, which used to hang a shutdown with no explanation.
        */
        if (should_terminate)
            return false;
        jobs.push(job);
    }
    mutex_condition.notify_one();
    return true;
}

/*
** Method: ThreadPool::Stop
** @brief  Signals every worker thread to terminate, discards any
**         not-yet-started queued jobs, and joins all worker threads.
*/
void ThreadPool::Stop()
{
#ifdef THREADPOOL_VERBOSE
    printf("Stopping thread pool:\n");
    (void)fflush(stdout);
#endif
    isStarted = false;
    {
#ifdef THREADPOOL_VERBOSE
        printf("Popping jobs: \n");
        (void)fflush(stdout);
#endif
        unique_lock<mutex> lock(queue_mutex);
        should_terminate = true;
        while (!jobs.empty())
            jobs.pop();
    }
    mutex_condition.notify_all();
    queue_not_full.notify_all();

#ifdef THREADPOOL_VERBOSE
    printf("Joining threads: \n");
    (void)fflush(stdout);
#endif
    for (thread& active_thread : threads)
    {
        active_thread.join();
        num_running--;
    }
    threads.clear();
    if (isBusyArray)
    {
        MemFree(isBusyArray);
        isBusyArray = NULL;
    }
#ifdef THREADPOOL_VERBOSE
    printf("Done Stopping: \n");
    (void)fflush(stdout);
#endif
}

/*
** Method: ThreadPool::IsBusy
** @brief  Reports whether the pool has queued work or any worker
**         currently executing a job.
** @return true if the pool is not idle.
*/
bool ThreadPool::IsBusy()
{
    bool poolbusy;
    if (!isStarted)
        return false;

    {
        unique_lock<mutex> lock(queue_mutex);
        int idleCount = NumIdle();

        poolbusy = !jobs.empty();

        /* Even with an empty queue, the pool counts as busy if some worker
        ** is still mid-job -- IsBusy means "nothing to wait for", not just
        ** "no queued work".
        */
        if (!poolbusy)
        {
            if (idleCount != num_threads)
                poolbusy = true;
        }
    }
    return poolbusy;
}

/*
** Method: ThreadPool::NumIdle
** @brief  Counts worker threads not currently executing a job.
** @return Number of idle worker threads, or 0 if the pool hasn't been started.
*/
int ThreadPool::NumIdle()
{
    int idleCnt = 0;
    if (!isStarted)
        return idleCnt;

    for (uint32_t idx = 0; idx < num_running; idx++)
    {
        if (!isBusyArray[idx].load(std::memory_order_relaxed))
            idleCnt++;
    }

    return idleCnt;
}

/*
** Method: ThreadPool::IsStarted
** @brief  Reports whether Start() has been called (and Stop() has not
**         since undone it).
** @return true if the pool is started.
*/
bool ThreadPool::IsStarted()
{
    return isStarted;
}

/*
** Method: ThreadPool::IsReady
** @brief  Reports whether every worker thread has actually begun
**         executing ThreadLoop.
** @return true once readyCount reaches num_threads.
*/
bool ThreadPool::IsReady()
{
    return isStarted && readyCount.load(std::memory_order_acquire) == num_threads;
}

/*
** Method: ThreadPool::WaitUntilReady
** @brief  Blocks until IsReady() is true.
*/
void ThreadPool::WaitUntilReady()
{
    /* One-time startup cost (microseconds to low milliseconds) -- a spin/yield
    ** loop is simpler than a condition variable for something this short-lived
    ** and never on a hot path.
    */
    while (!IsReady())
    {
        std::this_thread::yield();
    }
}

/*
** Method: ThreadPool::QueueDepth
** @brief  Returns the current number of jobs waiting in the queue.
** @return Current queue depth.
*/
size_t ThreadPool::QueueDepth()
{
    size_t result = 0;

    {
        unique_lock<mutex> lock(queue_mutex);

        result = jobs.size();
    }

    return result;
}

/*
** Method: ThreadPool::ThreadLoop
** @brief  Body run by each worker OS thread: repeatedly waits for a job
**         or termination, then runs the job.
** @param  idx - this worker's index (0..num_threads-1), passed through to each job
*/
void ThreadPool::ThreadLoop(uint32_t idx)
{
    /* UTF-8 name -> UTF-16 for SetThreadDescription. (<codecvt> is deprecated in
    ** C++17; the Windows call does the same job.) A name that fails to convert
    ** just leaves the thread unnamed -- SetThreadDescription then gets an empty string.
    */
    std::wstring  wide;
    int           wideLen = MultiByteToWideChar(CP_UTF8, 0, m_threadName.c_str(), -1, nullptr, 0);
    if (wideLen > 0)
    {
        wide.resize((size_t)wideLen);
        if (MultiByteToWideChar(CP_UTF8, 0, m_threadName.c_str(), -1, &wide[0], wideLen) > 0)
            wide.resize(wcslen(wide.c_str()));
        else
            wide.clear();
    }
    LPCWSTR       result = wide.c_str();
    HRESULT       r      = SetThreadDescription(GetCurrentThread(), result);

    /* The description only names the thread in debuggers and dumps, so a
    ** failure is not fatal -- but say so (once for the whole process, not
    ** once per thread) rather than have thread names quietly missing from
    ** the next crash dump.
    */
    if (FAILED(r))
    {
        static std::atomic<bool> s_descriptionFailureReported{ false };   /* true once the failure has been logged */

        if (!s_descriptionFailureReported.exchange(true))
            LoggerLog("ThreadPool: SetThreadDescription failed (HRESULT 0x%08lX) -- worker threads will be unnamed in debuggers and crash dumps\n",
                      (unsigned long)r);
    }

    /* Marks this OS thread as genuinely running, as opposed to merely having
    ** had std::thread's constructor return on the caller's side (which says
    ** nothing about whether the new thread has actually been scheduled yet).
    */
    readyCount.fetch_add(1, std::memory_order_release);

    while (true)
    {
        isBusyArray[idx].store(false, std::memory_order_relaxed);
        function<void(uint32_t)> job;
        {
            unique_lock<mutex> lock(queue_mutex);
            mutex_condition.wait(lock, [this] { return !jobs.empty() || should_terminate; });
            if (should_terminate)
            {
                return;
            }
            isBusyArray[idx].store(true, std::memory_order_relaxed);
            job = jobs.front();
            jobs.pop();
        }
        queue_not_full.notify_one();

        job(idx);
    }
}

/*
** Function: CloseHandleOrFatal
** @brief    Closes a handle and stops the process if the close fails.
** @details  A failed CloseHandle means the handle was already closed or was
**           never valid -- a double close somewhere, which can also close a
**           recycled handle value that now belongs to something else. That is
**           a bug worth stopping for, not a result to discard.
** @param    hHandle - the handle to close
** @param    pszWhat - what the handle was, for the failure message
*/
void CloseHandleOrFatal(HANDLE hHandle, const char* pszWhat)
{
    /* GetLastError is read straight after the failed call, before anything
    ** else can overwrite it.
    */
    if (!CloseHandle(hHandle))
    {
        DWORD closeError = GetLastError();   /* Windows error from the failed close */

        Fatal(FATAL_SYNC_FAILED, "CloseHandle failed for %s (handle %p, Windows error %lu) -- a double close or an invalid handle\n",
              pszWhat, hHandle, (unsigned long)closeError);
    }
}

/*
** Function: CreateEventOrFatal
** @brief    Creates an unnamed event and stops the process if creation fails.
** @details  A failed CreateEvent returns NULL. Passing NULL on to a wait makes
**           the wait fail immediately, and a caller that ignores that
**           proceeds while the work it meant to wait for is still running.
** @param    manualReset  - TRUE for a manual-reset event, FALSE for auto-reset
** @param    initialState - TRUE to create the event already signaled
** @param    pszWhat      - what the event is for, for the failure message
** @return   The new event handle (never NULL).
*/
HANDLE CreateEventOrFatal(BOOL manualReset, BOOL initialState, const char* pszWhat)
{
    HANDLE hEvent = CreateEventA(nullptr, manualReset, initialState, nullptr);   /* the new event, or NULL on failure */

    if (hEvent == nullptr)
    {
        DWORD createError = GetLastError();   /* Windows error from the failed create */

        Fatal(FATAL_SYNC_FAILED, "CreateEvent failed for %s (Windows error %lu)\n", pszWhat, (unsigned long)createError);
    }

    return hEvent;
}

/*
** Function: SetEventOrFatal
** @brief    Signals an event and stops the process if the signal fails.
** @details  A failed SetEvent leaves the waiter waiting for a signal that
**           will never come; the failure would otherwise surface only as a
**           hang with no explanation.
** @param    hEvent  - the event to signal
** @param    pszWhat - what the event is for, for the failure message
*/
void SetEventOrFatal(HANDLE hEvent, const char* pszWhat)
{
    if (!SetEvent(hEvent))
    {
        DWORD setError = GetLastError();   /* Windows error from the failed signal */

        Fatal(FATAL_SYNC_FAILED, "SetEvent failed for %s (handle %p, Windows error %lu)\n",
              pszWhat, hEvent, (unsigned long)setError);
    }
}

/*
** Function: WaitForEventsOrFatal
** @brief    Waits until every handle in pHandles is signaled, reporting on
**           long waits and stopping the process if the wait itself fails.
** @details  Waits in one-minute slices instead of INFINITE. A timed-out
**           slice just means the work is still running (flushing many
**           gigabytes legitimately takes minutes), so the wait continues.
**           When the caller supplies a progress probe the wait computes its
**           own ETA from the observed rate (after a 5-minute sample) and
**           logs a note only if it runs an hour past that ETA, or if the
**           progress counter has not moved for 30 minutes. With no probe it
**           falls back to a plain note after 15 minutes, then every 30.
**           Either way a wait that will never end no longer looks the same
**           as a quiet run, and a long-but-healthy one stays quiet. Any
**           result other than "all signaled" or "timed out" (an invalid
**           handle, an abandoned wait) is a bug: continuing would let the
**           caller run on while the waited-for work is still using shared
**           memory, so the process stops with the real cause instead.
** @param    pHandles - the handles to wait on (all of them must be signaled)
** @param    count    - number of handles
** @param    pszWhat  - what is being waited for, for log/failure messages
** @param    probe    - optional progress probe; see the header
*/
void WaitForEventsOrFatal(HANDLE* pHandles, DWORD count, const char* pszWhat, const WaitProgressProbe& probe)
{
    const uint64_t startTickMs = GetTickCount64();   /* when this wait began                                                      */
    uint64_t waitedMs          = 0;                  /* total time spent waiting so far                                           */
    uint64_t nextNoteMs        = WAIT_FIRST_REPORT_MS; /* no-probe fallback: waited time at which the next note is due            */
    DWORD    waitResult        = 0;                  /* result of the most recent wait slice                                      */
    DWORD    waitError         = 0;                  /* Windows error captured right after a failed wait                          */

    /* Progress tracking, used only when a probe is supplied and answers. */
    bool     haveProgress      = false;   /* the probe has answered at least once with a usable total                            */
    uint64_t baseDone          = 0;       /* progress at the start of the current measuring window                               */
    uint64_t baseMs            = 0;       /* waitedMs at the start of the current measuring window                               */
    uint64_t lastDone          = 0;       /* progress at the previous slice                                                      */
    uint64_t lastMoveMs        = 0;       /* waitedMs when progress last increased                                               */
    uint64_t promisedTotalMs   = 0;       /* the total wait time this wait predicted for itself once it had a stable rate (0 = none yet) */
    uint64_t nextOverdueMs     = 0;       /* waitedMs at which the next overdue note is due (0 = not overdue yet)                */
    uint64_t nextStallMs       = WAIT_STALL_MS;   /* waitedMs at which a no-progress note is next allowed                       */

    for (;;)
    {
        waitResult = WaitForMultipleObjects(count, pHandles, TRUE, (DWORD)WAIT_SLICE_MS);

        /* Every handle signaled: the work is done. */
        if (waitResult == WAIT_OBJECT_0)
            return;

        /* Just this slice timing out: the work is still running. Decide
        ** whether it is running suspiciously, then keep waiting.
        */
        if (waitResult == WAIT_TIMEOUT)
        {
            waitedMs = GetTickCount64() - startTickMs;

            uint64_t done = 0, total = 0;
            const bool probed = probe && probe(&done, &total) && total > 0;

            if (!probed)
            {
                /* No progress information: all that can be said is how long it has been. */
                if (waitedMs >= nextNoteMs)
                {
                    LoggerLog("Still waiting on %s after %llu minutes (no progress information is available for this wait; "
                              "long flushes and merges are normal, but report it if the run looks stuck)\n",
                              pszWhat, (unsigned long long)(waitedMs / 60000ULL));
                    nextNoteMs = waitedMs + WAIT_REPEAT_REPORT_MS;
                }
                continue;
            }

            /* Progress went backwards: another flush or session took over the
            ** counters. Start measuring afresh rather than trusting an old rate.
            */
            if (!haveProgress || done < lastDone)
            {
                haveProgress    = true;
                baseDone        = done;
                baseMs          = waitedMs;
                lastMoveMs      = waitedMs;
                promisedTotalMs = 0;
                nextOverdueMs   = 0;
            }
            else if (done > lastDone)
                lastMoveMs = waitedMs;
            lastDone = done;

            const double pct = (total > 0) ? 100.0 * (double)done / (double)total : 0.0;

            /* (b) Nothing has moved for a long time: this is the "stuck" signal. */
            if (waitedMs - lastMoveMs >= WAIT_STALL_MS && waitedMs >= nextStallMs)
            {
                LoggerLog("NO PROGRESS on %s for %llu minutes (waited %llu minutes in all, %.1f%% done) -- report this if the run looks stuck\n",
                          pszWhat, (unsigned long long)((waitedMs - lastMoveMs) / 60000ULL),
                          (unsigned long long)(waitedMs / 60000ULL), pct);
                nextStallMs = waitedMs + WAIT_STALL_MS;
            }

            /* (a) Work out this wait's own ETA once the rate has had time to settle,
            ** and only speak up when it runs well past it.
            */
            if (promisedTotalMs == 0 && waitedMs - baseMs >= WAIT_ETA_MIN_SAMPLE_MS && done > baseDone)
            {
                const double rate      = (double)(done - baseDone) / (double)(waitedMs - baseMs);   /* progress units per ms */
                const double remaining = (total > done) ? (double)(total - done) / rate : 0.0;      /* ms                    */
                promisedTotalMs        = waitedMs + (uint64_t)remaining;
            }

            if (promisedTotalMs != 0 && waitedMs >= promisedTotalMs + WAIT_OVERDUE_SLACK_MS &&
                (nextOverdueMs == 0 || waitedMs >= nextOverdueMs))
            {
                LoggerLog("Still waiting on %s after %llu minutes -- it predicted about %llu minutes and is now over an hour past that (%.1f%% done); "
                          "report it if the run looks stuck\n",
                          pszWhat, (unsigned long long)(waitedMs / 60000ULL),
                          (unsigned long long)(promisedTotalMs / 60000ULL), pct);
                nextOverdueMs = waitedMs + WAIT_OVERDUE_REPEAT_MS;
            }
            continue;
        }

        /* Anything else -- WAIT_FAILED for an invalid handle, or an
        ** unexpected code -- means the wait did not do its job.
        */
        waitError = GetLastError();
        Fatal(FATAL_SYNC_FAILED, "Wait on %s failed: WaitForMultipleObjects returned 0x%lX (Windows error %lu, %lu handle(s))\n",
              pszWhat, (unsigned long)waitResult, (unsigned long)waitError, (unsigned long)count);
    }
}
