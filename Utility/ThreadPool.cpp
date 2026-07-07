/*
** Filename:  ThreadPool.cpp
**
** Purpose:
**   Implements the ThreadPool class declared in ThreadPool.h: a fixed-size
**   worker-thread pool pulling jobs from a bounded queue.
*/

/* Includes */
#include "ThreadPool.h"
#include <Windows.h>
#include <codecvt>

using namespace std;

//#define THREADPOOL_VERBOSE

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
*/
void ThreadPool::QueueJob(const std::function<void(uint32_t)>& job)
{
    {
        unique_lock<mutex> lock(queue_mutex);
        queue_not_full.wait(lock, [this] { return jobs.size() < MAX_QUEUE_DEPTH || should_terminate; });
        if (should_terminate)
            return;
        jobs.push(job);
    }
    mutex_condition.notify_one();
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
    fflush(stdout);
#endif
    isStarted = false;
    {
#ifdef THREADPOOL_VERBOSE
        printf("Popping jobs: \n");
        fflush(stdout);
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
    fflush(stdout);
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
    fflush(stdout);
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
    std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> converter;
    std::wstring  wide   = converter.from_bytes(m_threadName);
    LPCWSTR       result = wide.c_str();
    HRESULT       r      = SetThreadDescription(GetCurrentThread(), result);

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
