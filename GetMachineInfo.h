/*
** Filename:  GetMachineInfo.h
**
** Purpose:
**   Declares MachineInfo (a bundle of memory-budget, drive, and GPU
**   detection results) and GetMachineInfo, the single entry point that
**   populates all three at startup.
**
** Notes:
**   Adapted from an earlier solver implementation, unchanged -- no
**   Othello-specific coupling.
*/

#pragma once

/* Includes */
#include "Utility.h"
#include "GpuInfo.h"

/* Structures and Types */

/*
** Type:    MachineInfo
** @brief   Bundles the three machine-capability probes (memory, drives, GPU)
**          into one struct passed around the solver at startup.
*/
typedef struct __MachineInfo
{
    MemoryInfo        g_memInfo = {};
    MachineDriveInfo  g_drives  = {};
    GpuInformation    g_gpuInfo = {};
} MachineInfo, * PMachineInfo;

/* Functions */

/*
** Function: GetMachineInfo
** @brief    Runs all three machine-capability probes (memory budget, drive
**           detection, GPU query) and fills pMachineInfo.
** @param    pCacheDir        - directory for the drive-benchmark cache
** @param    pDriveStr        - drive letters to probe (see GetDriveInformation)
** @param    memoryLimitBytes - --memory-limit override (0 = none, use MM_RECOMMENDED against real free RAM)
** @param    pMachineInfo     - out: filled with the probed machine information
*/
void GetMachineInfo(char* pCacheDir, char* pDriveStr, uint64_t memoryLimitBytes, PMachineInfo pMachineInfo);
