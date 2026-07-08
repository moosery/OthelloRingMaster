/*
** Filename:  GpuInfo.h
**
** Purpose:
**   Declares GpuInformation, a snapshot of the first CUDA device's identity,
**   compute, memory, and concurrency characteristics, plus derived
**   scheduling hints (optimalBatchSize/recommendedWorkerCount) used to size
**   the live solver's GPU batches and CPU worker-thread counts.
**
** Notes:
**   Adapted from an earlier solver implementation, unchanged -- pure
**   CUDA-runtime-API capability query, no Othello-specific coupling at all.
*/

#pragma once

/* Includes */
#include <stdint.h>
#include <stddef.h>

/* Structures and Types */

/*
** Type:    GpuInformation
** @brief   Identity, compute, memory, and concurrency characteristics of one
**          CUDA device, plus derived batch/worker-count scheduling hints.
*/
typedef struct __GpuInformation
{
    /* --- Identity --- */
    int   deviceIndex;
    char  name[256];
    int   computeCapabilityMajor;
    int   computeCapabilityMinor;

    /* --- Compute --- */
    int  smCount;          /* streaming multiprocessor count */
    int  maxThreadsPerSM;   /* max resident threads per SM    */
    int  warpSize;

    /* --- Memory --- */
    size_t  totalGlobalMemBytes;
    int     l2CacheSizeBytes;

    /* --- Concurrency --- */
    int  asyncEngineCount;   /* hardware DMA copy engines */

    /* --- Derived --- */
    int  optimalBatchSize;        /* boards per GPU batch to saturate SMs               */
    int  recommendedWorkerCount;  /* suggested CPU worker threads (from async engine count) */
} GpuInformation, * PGpuInformation;

/* Functions */

/*
** Function: GetGpuInformation
** @brief    Queries the first CUDA device and fills pInfo.
** @details  Calls exit(1) with a message on any CUDA error.
** @param    pInfo - out: filled with the queried device's information
*/
void GetGpuInformation(GpuInformation* pInfo);

/*
** Function: PrintGpuInformation
** @brief    Prints a summary of GPU information to the log.
** @param    pInfo - the device information to print
*/
void PrintGpuInformation(const GpuInformation* pInfo);
