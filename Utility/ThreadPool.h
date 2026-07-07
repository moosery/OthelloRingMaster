/*
** Filename:  ThreadPool.h
**
** Purpose:
**   Declares ThreadPool: a fixed-size worker-thread pool that pulls jobs
**   (std::function<void(uint32_t)>, called with the worker's index) from a
**   bounded queue. Producers block (QueueJob) once the queue reaches
**   MAX_QUEUE_DEPTH, rather than growing it unbounded.
*/

#pragma once

/* Includes */
#include <vector>
#include <queue>
#include <functional>
#include "Mem.h"
#include <memory.h>
#include <string>
#include <thread>
#include <mutex>
#include <atomic>

/* Macros and Defines */
#define MAX_QUEUE_DEPTH 5000

/* Structures and Types */

/*
** Type:    ThreadPool
** @brief   A fixed-size pool of worker threads pulling jobs from a bounded
**          queue. Construct with the desired thread count and a name (used
**          for OS thread naming/debugging), then call Start() to actually
**          spin up the OS threads.
*/
class ThreadPool
{
public:
    /*
    ** Method: ThreadPool (constructor)
    ** @brief  Records the desired thread count and pool name. Does not
    **         start any OS threads -- call Start() for that.
    ** @param  numberThreadsNeeded - how many worker threads Start() should create
    ** @param  threadName          - name tag applied to each worker OS thread
    */
    ThreadPool(uint32_t numberThreadsNeeded, std::string threadName)
    {
        isStarted    = false;
        num_threads  = numberThreadsNeeded;
        m_threadName = threadName;
        num_running  = 0;
    }

    /*
    ** Method: ~ThreadPool (destructor)
    ** @brief  Stops the pool (joining every worker thread) if it is still running.
    */
    ~ThreadPool() { if (isStarted) Stop(); }

    /*
    ** Method: Start
    ** @brief  Spins up num_threads worker OS threads, each running ThreadLoop.
    */
    void Start();

    /*
    ** Method: QueueJob
    ** @brief  Enqueues job for a worker thread to run, blocking the caller
    **         if the queue is already at MAX_QUEUE_DEPTH.
    ** @param  job - the work to run; called as job(workerIndex) by whichever worker picks it up
    */
    void QueueJob(const std::function<void(uint32_t)>& job);

    /*
    ** Method: Stop
    ** @brief  Signals every worker thread to terminate, discards any
    **         not-yet-started queued jobs, and joins all worker threads.
    */
    void Stop();

    /*
    ** Method: IsBusy
    ** @brief  Reports whether the pool has queued work or any worker
    **         currently executing a job.
    ** @return true if the pool is not idle.
    */
    bool IsBusy();

    /*
    ** Method: NumIdle
    ** @brief  Counts worker threads not currently executing a job.
    ** @return Number of idle worker threads, or 0 if the pool hasn't been started.
    */
    int NumIdle();

    /*
    ** Method: IsStarted
    ** @brief  Reports whether Start() has been called (and Stop() has not
    **         since undone it).
    ** @return true if the pool is started.
    */
    bool IsStarted();

    /*
    ** Method: IsReady
    ** @brief  Reports whether every worker thread has actually begun
    **         executing ThreadLoop.
    ** @details This is a stronger guarantee than "Start() returned" or
    **          "NumIdle()==NumThreads()" -- both of those can be true before
    **          any OS thread has really started running (see the
    **          WaitUntilReady comment in ThreadPool.cpp for why that
    **          distinction matters).
    ** @return true once readyCount reaches num_threads.
    */
    bool IsReady();

    /*
    ** Method: WaitUntilReady
    ** @brief  Blocks until IsReady() is true.
    ** @details Call after Start() before dispatching any latency-sensitive
    **          work, so timing measurements don't include an unpredictable
    **          amount of thread-spin-up noise.
    */
    void WaitUntilReady();

    /*
    ** Method: NumThreads
    ** @brief  Returns the number of worker OS threads actually running.
    ** @return num_running.
    */
    uint32_t NumThreads()
    {
        return num_running;
    }

    /*
    ** Method: QueueDepth
    ** @brief  Returns the current number of jobs waiting in the queue.
    ** @return Current queue depth.
    */
    size_t QueueDepth();

private:
    std::string  m_threadName;   /* name tag applied to each worker OS thread */

    /*
    ** Method: ThreadLoop
    ** @brief  Body run by each worker OS thread: repeatedly waits for a job
    **         or termination, then runs the job.
    ** @param  idx - this worker's index (0..num_threads-1), passed through to each job
    */
    void ThreadLoop(uint32_t idx);

    int                    numIdle      = 0;
    uint32_t               num_threads;
    std::atomic<bool>*     isBusyArray  = NULL;   /* atomic: read by NumIdle()/IsBusy() without queue_mutex */
    bool                   isStarted;
    uint32_t               num_running;
    std::atomic<uint32_t>  readyCount{ 0 };       /* incremented by each worker as the first thing it does  */

    bool                                        should_terminate = false;   /* tells worker threads to stop looking for jobs and exit   */
    std::mutex                                  queue_mutex;                /* prevents data races to the job queue                     */
    std::condition_variable                     mutex_condition;            /* lets threads wait on new jobs or on termination          */
    std::condition_variable                     queue_not_full;             /* lets producers wait when the queue is at MAX_QUEUE_DEPTH */
    std::vector<std::thread>                    threads;
    std::queue<std::function<void(uint32_t)>>   jobs;
};
