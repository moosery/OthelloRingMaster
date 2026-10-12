/*
** Filename:  GpuInfo.cu
**
** Purpose:
**   Implements GetGpuInformation/PrintGpuInformation declared in GpuInfo.h.
*/

/* Includes */
#include "GpuInfo.h"
#include "Logger.h"
#include <cuda_runtime.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* Macros and Defines */

/*
** Fatals the whole process on any CUDA error -- GPU capability detection
** happens once at startup, before any solver state exists, so there is
** nothing safe to continue running with if the device can't be queried.
*/
#define CUDA_CHECK(call) \
    do { \
        cudaError_t _e = (call); \
        if (_e != cudaSuccess) { \
            fprintf(stderr, "CUDA error at %s:%d  %s\n", \
                    __FILE__, __LINE__, cudaGetErrorString(_e)); \
            exit(1); \
        } \
    } while (0)

/* Functions */

/*
** Function: GetGpuInformation
** @brief    Queries the first CUDA device and fills pInfo.
** @param    pInfo - out: filled with the queried device's information
*/
void GetGpuInformation(GpuInformation* pInfo)
{
    memset(pInfo, 0, sizeof(*pInfo));

    /* Host wait mode: block (sleep) instead of spin. Left at CUDA's default
    ** (cudaDeviceScheduleAuto), a process with more CPU cores than GPUs gets
    ** spin-waiting -- every cudaStreamSynchronize/cudaMemcpy the feeder makes
    ** (one after every GpuProcessBatch, plus GpuFlushPrepare's sort/dedup)
    ** burned a full core at 100% doing nothing but polling, keeping one core
    ** pinned hot for the entire multi-month run. Blocking-sync parks the
    ** thread on an OS event until the GPU signals completion instead. This is
    ** a pure host-side waiting change: the same work runs on the GPU in the
    ** same order, board order and every computed result are unchanged.
    **
    ** Must be the process's first CUDA call, before any context exists (the
    ** solver makes no CUDA call before this function) -- set first, then read
    ** back and verified rather than trusted, so a driver that silently ignored
    ** it would stop the run loudly here instead of quietly spinning again.
    */
    CUDA_CHECK(cudaSetDeviceFlags(cudaDeviceScheduleBlockingSync));

    int deviceIndex = 0;
    CUDA_CHECK(cudaGetDevice(&deviceIndex));
    pInfo->deviceIndex = deviceIndex;

    unsigned int deviceFlags = 0;
    CUDA_CHECK(cudaGetDeviceFlags(&deviceFlags));
    if ((deviceFlags & cudaDeviceScheduleMask) != cudaDeviceScheduleBlockingSync)
    {
        fprintf(stderr, "CUDA error at %s:%d  device flags 0x%x after cudaSetDeviceFlags -- "
                        "blocking-sync wait mode did not take effect\n",
                __FILE__, __LINE__, deviceFlags);
        exit(1);
    }
    pInfo->deviceFlags = deviceFlags;

    cudaDeviceProp p = {};
    CUDA_CHECK(cudaGetDeviceProperties(&p, deviceIndex));

    strncpy(pInfo->name, p.name, sizeof(pInfo->name) - 1);
    pInfo->computeCapabilityMajor = p.major;
    pInfo->computeCapabilityMinor = p.minor;
    pInfo->smCount                = p.multiProcessorCount;
    pInfo->maxThreadsPerSM        = p.maxThreadsPerMultiProcessor;
    pInfo->warpSize               = p.warpSize;
    pInfo->totalGlobalMemBytes    = p.totalGlobalMem;
    pInfo->l2CacheSizeBytes       = p.l2CacheSize;
    pInfo->asyncEngineCount       = p.asyncEngineCount;

    /* Optimal batch: fill all SMs with 256-thread blocks, capped at 64K so a
    ** single batch never grows unreasonably large on very big GPUs.
    */
    int sat = p.multiProcessorCount * (p.maxThreadsPerMultiProcessor / 256) * 256;
    pInfo->optimalBatchSize = (sat < 65536) ? sat : 65536;
}

/*
** Function: PrintGpuInformation
** @brief    Prints a summary of GPU information to the log.
** @param    p - the device information to print
*/
void PrintGpuInformation(const GpuInformation* p)
{
    double vramGB = (double)p->totalGlobalMemBytes / (1024.0 * 1024.0 * 1024.0);
    double l2MB   = (double)p->l2CacheSizeBytes    / (1024.0 * 1024.0);

    LoggerLog("GPU [%d]: %s  (sm_%d%d)\n",
              p->deviceIndex, p->name,
              p->computeCapabilityMajor, p->computeCapabilityMinor);
    LoggerLog("  SMs                : %d  x  %d threads/SM  (warp=%d)\n",
              p->smCount, p->maxThreadsPerSM, p->warpSize);
    LoggerLog("  VRAM               : %.0f GB\n", vramGB);
    LoggerLog("  L2 Cache           : %.0f MB\n", l2MB);
    LoggerLog("  Async Engines      : %d\n", p->asyncEngineCount);
    LoggerLog("  Host Wait Mode     : %s  (device flags 0x%x)\n",
              ((p->deviceFlags & cudaDeviceScheduleMask) == cudaDeviceScheduleBlockingSync) ? "blocking-sync" : "OTHER",
              p->deviceFlags);
    LoggerLog("  Optimal Batch Size : %d\n", p->optimalBatchSize);
}
