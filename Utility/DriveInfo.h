/*
** Filename:  DriveInfo.h
**
** Purpose:
**   Declares drive detection, benchmarking, and free-space-tracking
**   facilities: GetDriveInformation queries (and, unless a valid cache
**   entry exists, benchmarks) an array of drives; RefreshDriveFreeSpace
**   re-queries just the free-space figures cheaply; PrintDriveInformation
**   reports a summary. DriveCategory (FAST/MEDIUM/SLOW) classifies drives by
**   measured single-stream write throughput so callers can pick appropriate
**   roles (writer target vs. intermediate merge destination vs. store-only)
**   without hardcoding throughput assumptions per machine.
*/

#pragma once

/* Includes */
#include <stdint.h>
#include <stdbool.h>

/* Macros and Defines */
#define DRIVE_SAFETY_MARGIN_BYTES  (200ULL * 1024 * 1024 * 1024)   /* 200 GB */
#define MAX_SYSTEM_DRIVES 16

/* Single-stream write MB/s thresholds for drive categorization (based on
** benchmarked writeMBs). Below MEDIUM_DRIVE_MB_THRESHOLD: store-only (slow NAS etc.)
*/
#define FAST_DRIVE_MB_THRESHOLD    500.0   /* NVMe-class: writer targets these                */
#define MEDIUM_DRIVE_MB_THRESHOLD   50.0   /* HDD / fast NAS: intermediate merge destinations */

/* Structures and Types */

/*
** Type:    DriveCategory
** @brief   Classifies a drive by measured single-stream write throughput,
**          so callers can pick an appropriate role for it.
*/
typedef enum {
    DRIVE_CAT_FAST   = 0,   /* >= 500 MB/s -- writer targets                       */
    DRIVE_CAT_MEDIUM = 1,   /* >=  50 MB/s -- intermediate merge destinations      */
    DRIVE_CAT_SLOW   = 2,   /* <   50 MB/s -- store-only                           */
} DriveCategory;

/*
** Type:    DriveInformation
** @brief   Everything known about one drive: detection results (type,
**          space), an optional single-stream benchmark, and the
**          DriveCategory derived from that benchmark.
** @details The benchmark is deliberately single-stream only: this is just
**          "how fast can one thread write/read this drive," used for
**          FAST/MEDIUM/SLOW categorization. There is no concurrency sweep,
**          no "how many directories/threads" concept here -- that question
**          (if a drive needs N parallel compressor threads to keep up) is
**          answered by the solver project itself, not Utility, since
**          answering it needs the real compression pipeline, which Utility
**          must not depend on.
*/
typedef struct __DriveInformation {
    /* --- Detection --- */
    char      driveLetter;
    bool      available;       /* false if drive not accessible or query failed             */
    bool      isNvme;
    bool      isRotational;    /* true = HDD (seeks incur a penalty)                        */
    bool      isNas;           /* true = network/remote drive (DRIVE_REMOTE)                */
    int       primaryDiskNum;  /* physical disk number; -1 if unknown                       */
    int       numSpindles;
    uint64_t  totalBytes;
    uint64_t  freeBytes;
    uint64_t  usableBytes;     /* freeBytes - DRIVE_SAFETY_MARGIN_BYTES (0 if insufficient) */
    char      serial[64];

    /* --- Benchmark --- */
    bool    benchmarkValid;
    double  writeMBs;
    double  readMBs;
    char    timestamp[32];   /* "YYYY-MM-DD HH:MM:SS" when benchmark was last run */

    /* --- Categorization (derived from writeMBs after benchmark/cache load) --- */
    DriveCategory driveCategory;
} DriveInformation, * PDriveInformation;

/*
** Type:    MachineDriveInfo
** @brief   The fixed-size array of DriveInformation records for every drive
**          queried by GetDriveInformation, plus how many entries are valid.
*/
typedef struct __MachineDriveInfo
{
    DriveInformation  drives[MAX_SYSTEM_DRIVES];
    int               numDrives;
} MachineDriveInfo, * PMachineDriveInfo;

/* Functions */

/*
** Function: GetDriveInformation
** @brief    Queries (and, unless a valid cache entry exists, benchmarks) an
**           array of drives.
** @param    pMachineDriveInfo - out: filled in with detection/benchmark results for each drive
** @param    pCacheDir         - directory for the driveinfo.json benchmark cache; NULL to disable caching
** @param    driveLetters      - null-terminated string of drive letters (e.g. "CDZ"); NULL to auto-enumerate all fixed local drives
** @param    forceBenchmark    - when true, always re-run the benchmark and overwrite the cache
*/
void GetDriveInformation(
    PMachineDriveInfo pMachineDriveInfo,
    const char* pCacheDir,
    const char* driveLetters,
    bool        forceBenchmark = false);

/*
** Function: RefreshDriveFreeSpace
** @brief    Re-queries just freeBytes/usableBytes/totalBytes for each drive,
**           without re-running any benchmark.
** @details  Call this after any operation that frees space (recycle bin
**           flush, directory delete) to keep the stored numbers accurate.
** @param    pMDI - the drive set to refresh in place
*/
void RefreshDriveFreeSpace(PMachineDriveInfo pMDI);

/*
** Function: PrintDriveInformation
** @brief    Prints a summary (detection + benchmark) for all drives.
** @param    pDrive - the drive set to report on
*/
void PrintDriveInformation(const PMachineDriveInfo pDrive);
