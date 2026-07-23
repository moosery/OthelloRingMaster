/*
** Filename:  GetMachineInfo.cpp
**
** Purpose:
**   Implements GetMachineInfo declared in GetMachineInfo.h, plus its three
**   private per-probe helpers (memory budget, drive detection, GPU query).
*/

/* Includes */
#include "GetMachineInfo.h"

/* Functions */

/* Forward declarations for helpers defined below, alphabetically. */
static void getDriveInformation(char* pCacheDir, char* pDriveStr, PMachineDriveInfo driveInfo);
static void getGpuInformation(PGpuInformation pGpuInfo);
static void getSystemMemoryInfoBudget(uint64_t memoryLimitBytes, PMemoryInfo pMemInfo);

/*
** Function: GetMachineInfo
** @brief    Runs all three machine-capability probes and fills pMachineInfo.
** @param    pCacheDir        - directory for the drive-benchmark cache
** @param    pDriveStr        - drive letters to probe
** @param    memoryLimitBytes - --memory-limit override (0 = none)
** @param    pMachineInfo     - out: filled with the probed machine information
*/
void GetMachineInfo(char* pCacheDir, char* pDriveStr, uint64_t memoryLimitBytes, PMachineInfo pMachineInfo)
{
    getSystemMemoryInfoBudget(memoryLimitBytes, &pMachineInfo->g_memInfo);
    getDriveInformation(pCacheDir, pDriveStr, &(pMachineInfo->g_drives));
    getGpuInformation(&(pMachineInfo->g_gpuInfo));
}

/*
** Function: getDriveInformation
** @brief    Detects/benchmarks the configured drives and logs a summary.
** @param    pCacheDir - directory for the drive-benchmark cache
** @param    pDriveStr - drive letters to probe
** @param    driveInfo - out: filled with detection/benchmark results
*/
static void getDriveInformation(char* pCacheDir, char* pDriveStr, PMachineDriveInfo driveInfo)
{
    GetDriveInformation(driveInfo, pCacheDir, pDriveStr);
    PrintDriveInformation(driveInfo);
}

/*
** Function: getGpuInformation
** @brief    Queries the GPU and logs a summary.
** @param    pGpuInfo - out: filled with the queried device's information
*/
static void getGpuInformation(PGpuInformation pGpuInfo)
{
    GetGpuInformation(pGpuInfo);
    PrintGpuInformation(pGpuInfo);
}

/*
** Function: getSystemMemoryInfoBudget
** @brief    Resolves the memory budget against free RAM and prints it.
** @param    memoryLimitBytes - --memory-limit override (0 = none, use MM_RECOMMENDED)
** @param    pMemInfo         - out: filled with the resolved memory budget
*/
static void getSystemMemoryInfoBudget(uint64_t memoryLimitBytes, PMemoryInfo pMemInfo)
{
    if (memoryLimitBytes > 0)
    {
        pMemInfo->requestedMode  = MM_SPECIFIED;
        pMemInfo->requestedBytes = memoryLimitBytes;
    }
    else
    {
        pMemInfo->requestedMode  = MM_RECOMMENDED;
        pMemInfo->requestedBytes = 0;   /* not used for MM_RECOMMENDED */
    }

    CalcMemoryBudget(pMemInfo);

    /* Print the results */
    char physStr[64], availStr[64], budgetStr[64];
    sizeToGBString(pMemInfo->totalPhys,    physStr,   sizeof(physStr));
    sizeToGBString(pMemInfo->availPhys,    availStr,  sizeof(availStr));
    sizeToGBString(pMemInfo->budgetedSize, budgetStr, sizeof(budgetStr));
    printf("Total Physical RAM     : %s\n", physStr);
    printf("Available Physical RAM : %s\n", availStr);
    printf("Memory Budget          : %s%s\n", budgetStr, memoryLimitBytes > 0 ? "  (--memory-limit override)" : "");
}
