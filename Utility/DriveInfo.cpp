/*
** Filename:  DriveInfo.cpp
**
** Purpose:
**   Implements the drive detection, benchmarking, and free-space-tracking
**   functions declared in DriveInfo.h: QueryOneDrive/GetDriveInformation
**   detect each drive's type/space (and NVMe/rotational/NAS status via
**   Windows storage IOCTLs), BenchmarkOneDrive measures single-stream
**   write/read throughput, a small hand-rolled JSON reader/writer persists
**   benchmark results to a per-cache-dir driveinfo.json so repeat runs skip
**   re-benchmarking, and RefreshDriveFreeSpace/PrintDriveInformation round
**   out the reporting/refresh side.
*/

/* Includes */
#include "DriveInfo.h"
#include "FileAndDirUtils.h"
#include "Error.h"
#include "Logger.h"
#define NOMINMAX
#include <windows.h>
#include <winioctl.h>
#include <ntddstor.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <algorithm>
#include <vector>

/* Constants */

/* Bumped 1 -> 2 when the multi-dir concurrency sweep (optimalDirs/combined*)
** was removed: pre-v2 cached writeMBs numbers were measured at whatever
** concurrency the old sweep landed on, which is a different, no-longer-
** comparable quantity to the new single-stream number. A version mismatch
** forces a full re-benchmark rather than silently trusting stale numbers
** that look like valid JSON but mean something different now.
*/
static const int CACHE_JSON_VERSION = 2;

/* Functions */

/*
** ============================================================================
** Cache  (single JSON file: driveinfo.json)
** Detection fields (free space, drive type, etc.) are always re-queried
** fresh; only benchmark fields are loaded from cache.
** ============================================================================
*/

/*
** Function: BuildCacheFilePath
** @brief    Builds the full path to the driveinfo.json cache file within cacheDir.
** @param    cacheDir - directory containing the cache file
** @param    out      - buffer to receive the built path
** @param    outSz    - size in bytes of out
*/
static void BuildCacheFilePath(const char* cacheDir, char* out, size_t outSz)
{
    snprintf(out, outSz, "%s\\driveinfo.json", cacheDir);
}

/* ---- minimal JSON helpers (flat objects only) ---- */

/*
** Function: JsStr
** @brief    Extracts a quoted string value for key from a flat JSON object.
** @param    obj   - JSON object text to search
** @param    key   - key name to search for
** @param    out   - buffer to receive the extracted string
** @param    outSz - size in bytes of out
** @return   true if key was found and its string value copied to out.
*/
static bool JsStr(const char* obj, const char* key, char* out, size_t outSz)
{
    char search[128];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char* p = strstr(obj, search);
    if (!p) return false;
    p += strlen(search);
    while (*p == ' ' || *p == ':' || *p == '\t' || *p == '\n' || *p == '\r') ++p;
    if (*p != '"') return false;
    ++p;
    size_t i = 0;
    while (*p && *p != '"' && i < outSz - 1) out[i++] = *p++;
    out[i] = '\0';
    return true;
}

/*
** Function: JsDbl
** @brief    Extracts a numeric value for key from a flat JSON object.
** @param    obj - JSON object text to search
** @param    key - key name to search for
** @param    out - out: the parsed double value
** @return   true if key was found and its numeric value parsed into out.
*/
static bool JsDbl(const char* obj, const char* key, double* out)
{
    char search[128];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char* p = strstr(obj, search);
    if (!p) return false;
    p += strlen(search);
    while (*p == ' ' || *p == ':' || *p == '\t' || *p == '\n' || *p == '\r') ++p;
    char*  end;
    double v = strtod(p, &end);
    if (end == p) return false;
    *out = v;
    return true;
}

/*
** Function: JsInt
** @brief    Extracts an integer value for key from a flat JSON object.
** @param    obj - JSON object text to search
** @param    key - key name to search for
** @param    out - out: the parsed integer value
** @return   true if key was found and its numeric value parsed into out.
*/
static bool JsInt(const char* obj, const char* key, int* out)
{
    double v;
    if (!JsDbl(obj, key, &v)) return false;
    *out = (int)v;
    return true;
}

/*
** Function: JsBool
** @brief    Extracts a boolean value for key from a flat JSON object.
** @param    obj - JSON object text to search
** @param    key - key name to search for
** @param    out - out: the parsed boolean value
** @return   true if key was found and its value was recognizably true/false.
*/
static bool JsBool(const char* obj, const char* key, bool* out)
{
    char search[128];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char* p = strstr(obj, search);
    if (!p) return false;
    p += strlen(search);
    while (*p == ' ' || *p == ':' || *p == '\t') ++p;
    if (strncmp(p, "true", 4) == 0)  { *out = true;  return true; }
    if (strncmp(p, "false", 5) == 0) { *out = false; return true; }
    return false;
}

/*
** Function: ExtractDriveBlock
** @brief    Finds the JSON object containing "letter": "X" and returns a
**           malloc'd copy of just that object.
** @param    json   - full JSON document text to search
** @param    letter - the drive letter to find (case-insensitive)
** @return   A malloc'd copy of the matching object, or nullptr if not found. Caller must free().
*/
static char* ExtractDriveBlock(const char* json, char letter)
{
    char target[32];
    snprintf(target, sizeof(target), "\"letter\": \"%c\"", (char)toupper((unsigned char)letter));

    const char* found = strstr(json, target);
    if (!found) return nullptr;

    const char* start = found;
    while (start > json && *start != '{') --start;
    if (*start != '{') return nullptr;

    const char* end = found;
    while (*end && *end != '}') ++end;
    if (!*end) return nullptr;
    ++end;

    size_t len = (size_t)(end - start);
    char*  buf = (char*)malloc(len + 1);
    if (!buf) return nullptr;
    memcpy(buf, start, len);
    buf[len] = '\0';
    return buf;
}

/*
** Function: CacheVersionMatches
** @brief    Reports whether the cache file's top-level "version" matches CACHE_JSON_VERSION.
** @details  A missing or mismatched version means the file predates a
**           semantic change (e.g. writeMBs used to be measured at
**           multi-stream concurrency) -- treat the whole cache as invalid
**           rather than trust numbers that parse fine but no longer mean
**           what the current code assumes they mean.
** @param    jsonText - full JSON document text to check
** @return   true if the document's version field equals CACHE_JSON_VERSION.
*/
static bool CacheVersionMatches(const char* jsonText)
{
    int version = -1;
    return JsInt(jsonText, "version", &version) && version == CACHE_JSON_VERSION;
}

/*
** Function: LoadCacheJSON
** @brief    Loads benchmark fields from JSON text for the given drive letter.
** @param    jsonText       - full JSON document text to search
** @param    letter         - the drive letter to load
** @param    expectedSerial - if non-empty, the cached entry's serial must match this or the entry is rejected
** @param    pOut           - out: benchmarkValid/writeMBs/readMBs/timestamp filled in on success
** @return   true only if a matching valid entry is found and the serial matches.
*/
static bool LoadCacheJSON(const char* jsonText, char letter, const char* expectedSerial,
                          DriveInformation* pOut)
{
    char* block = ExtractDriveBlock(jsonText, letter);
    if (!block) return false;

    bool ok = false;
    do {
        char serial[64] = {};
        JsStr(block, "serial", serial, sizeof(serial));
        if (expectedSerial && expectedSerial[0] && serial[0] &&
            strcmp(serial, expectedSerial) != 0)
            break;

        bool valid = false;
        if (!JsBool(block, "benchmarkValid", &valid) || !valid) break;

        double writeMBs = 0, readMBs = 0;
        char   timestamp[32] = {};

        if (!JsDbl(block, "writeMBs", &writeMBs)) break;
        if (!JsDbl(block, "readMBs",  &readMBs))  break;
        JsStr(block, "timestamp", timestamp, sizeof(timestamp));

        pOut->benchmarkValid = true;
        pOut->writeMBs       = writeMBs;
        pOut->readMBs        = readMBs;
        memcpy(pOut->timestamp, timestamp, sizeof(pOut->timestamp));
        ok = true;
    } while (false);

    free(block);
    return ok;
}

/*
** Function: SaveCacheJSON
** @brief    Writes all drives with valid benchmark data to the JSON cache file.
** @param    path - full path of the cache file to write
** @param    pMDI - the drive set to save
*/
static void SaveCacheJSON(const char* path, const PMachineDriveInfo pMDI)
{
    FILE* f = fopen(path, "w");
    if (!f)
        Fatal(FATAL_DRIVE_CACHE_WRITE_FAILED,
              "DriveInfo: cannot write cache file '%s'", path);

    fprintf(f, "{\n  \"version\": %d,\n  \"drives\": [\n", CACHE_JSON_VERSION);

    bool first = true;
    for (int i = 0; i < pMDI->numDrives; i++) {
        const DriveInformation* p = &pMDI->drives[i];
        if (!p->benchmarkValid) continue;
        if (!first) fprintf(f, ",\n");
        first = false;
        fprintf(f,
            "    {\n"
            "      \"letter\": \"%c\",\n"
            "      \"serial\": \"%s\",\n"
            "      \"benchmarkValid\": true,\n"
            "      \"writeMBs\": %.2f,\n"
            "      \"readMBs\": %.2f,\n"
            "      \"timestamp\": \"%s\"\n"
            "    }",
            (char)toupper((unsigned char)p->driveLetter),
            p->serial,
            p->writeMBs,
            p->readMBs,
            p->timestamp);
    }

    fprintf(f, "\n  ]\n}\n");
    fclose(f);
}

/*
** ============================================================================
** Drive detection
** ============================================================================
*/

#ifndef BusTypeNvme
#define BusTypeNvme ((STORAGE_BUS_TYPE)0x11)
#endif

/*
** Function: OpenDriveHandle
** @brief    Opens a zero-access handle to a volume or physical drive path,
**           suitable only for DeviceIoControl queries (not read/write).
** @param    path - the volume or physical-drive path to open (e.g. "\\\\.\\C:")
** @return   The opened handle, or INVALID_HANDLE_VALUE on failure.
*/
static HANDLE OpenDriveHandle(const char* path)
{
    return CreateFileA(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                       nullptr, OPEN_EXISTING, 0, nullptr);
}

/*
** Function: QuerySeekPenalty
** @brief    Queries whether a physical drive incurs a seek penalty (i.e. is rotational/HDD).
** @param    hPhys        - open handle to the physical drive
** @param    outRotational - out: true if the drive incurs a seek penalty
** @return   true if the query succeeded.
*/
static bool QuerySeekPenalty(HANDLE hPhys, bool& outRotational)
{
    STORAGE_PROPERTY_QUERY q = {};
    q.PropertyId = StorageDeviceSeekPenaltyProperty;
    q.QueryType  = PropertyStandardQuery;
    DEVICE_SEEK_PENALTY_DESCRIPTOR spd = {};
    DWORD returned = 0;
    if (!DeviceIoControl(hPhys, IOCTL_STORAGE_QUERY_PROPERTY,
                         &q, sizeof(q), &spd, sizeof(spd), &returned, nullptr))
        return false;
    outRotational = (spd.IncursSeekPenalty == TRUE);
    return true;
}

/*
** Function: QueryDeviceProps
** @brief    Queries a physical drive's bus type (to detect NVMe) and serial number.
** @param    hPhys      - open handle to the physical drive
** @param    outBusType - out: the drive's storage bus type
** @param    serial     - buffer to receive the trimmed serial number, or nullptr to skip
** @param    serialSz   - size in bytes of serial
** @return   true if the query succeeded.
*/
static bool QueryDeviceProps(HANDLE hPhys, STORAGE_BUS_TYPE& outBusType,
                              char* serial, size_t serialSz)
{
    STORAGE_PROPERTY_QUERY q = {};
    q.PropertyId = StorageDeviceProperty;
    q.QueryType  = PropertyStandardQuery;
    char  buf[1024] = {};
    DWORD returned  = 0;
    if (!DeviceIoControl(hPhys, IOCTL_STORAGE_QUERY_PROPERTY,
                         &q, sizeof(q), buf, sizeof(buf), &returned, nullptr))
        return false;
    auto* desc = reinterpret_cast<STORAGE_DEVICE_DESCRIPTOR*>(buf);
    outBusType = desc->BusType;
    if (serial && serialSz > 0) {
        serial[0] = '\0';
        if (desc->SerialNumberOffset != 0 && desc->SerialNumberOffset < returned) {
            strncpy_s(serial, serialSz, buf + desc->SerialNumberOffset, _TRUNCATE);
            size_t len = strlen(serial);
            while (len > 0 && serial[len - 1] == ' ') serial[--len] = '\0';
        }
    }
    return true;
}

/*
** Function: QueryDiskExtents
** @brief    Queries which physical disk number(s) back a given volume.
** @param    hVolume     - open handle to the volume
** @param    diskNums    - out: array to receive the physical disk numbers
** @param    maxDiskNums - capacity of diskNums
** @return   Number of disk extents found (0 on failure), capped to maxDiskNums.
*/
static int QueryDiskExtents(HANDLE hVolume, DWORD diskNums[], int maxDiskNums)
{
    char  buf[sizeof(VOLUME_DISK_EXTENTS) + 8 * sizeof(DISK_EXTENT)] = {};
    DWORD returned = 0;
    if (!DeviceIoControl(hVolume, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS,
                         nullptr, 0, buf, sizeof(buf), &returned, nullptr))
        return 0;
    auto* vde   = reinterpret_cast<VOLUME_DISK_EXTENTS*>(buf);
    int   count = (int)vde->NumberOfDiskExtents;
    if (count > maxDiskNums) count = maxDiskNums;
    for (int i = 0; i < count; i++)
        diskNums[i] = vde->Extents[i].DiskNumber;
    return count;
}

/*
** Function: QueryOneDrive
** @brief    Detects one drive's type/space and (for local, non-NAS drives)
**           NVMe/rotational status, primary disk number, and serial.
** @param    letter - the drive letter to query
** @param    pOut   - out: zeroed and filled in with detection results
*/
static void QueryOneDrive(char letter, DriveInformation* pOut)
{
    memset(pOut, 0, sizeof(*pOut));
    pOut->driveLetter    = letter;
    pOut->primaryDiskNum = -1;

    char rootPath[4] = { letter, ':', '\\', '\0' };

    pOut->isNas = (GetDriveTypeA(rootPath) == DRIVE_REMOTE);

    ULARGE_INTEGER freeBytesAvail = {}, totalBytes = {}, totalFree = {};
    if (GetDiskFreeSpaceExA(rootPath, &freeBytesAvail, &totalBytes, &totalFree)) {
        pOut->totalBytes  = totalBytes.QuadPart;
        pOut->freeBytes   = freeBytesAvail.QuadPart;
        pOut->usableBytes = (pOut->freeBytes > DRIVE_SAFETY_MARGIN_BYTES)
                          ? pOut->freeBytes - DRIVE_SAFETY_MARGIN_BYTES : 0;
    }

    /* NAS/network drives don't support IOCTL queries -- skip them. */
    if (pOut->isNas) {
        pOut->available = (pOut->totalBytes > 0);
        return;
    }

    char   volPath[7] = { '\\','\\','.','\\', letter, ':', '\0' };
    HANDLE hVol       = OpenDriveHandle(volPath);
    if (hVol == INVALID_HANDLE_VALUE) {
        pOut->available = (pOut->totalBytes > 0);
        return;
    }

    DWORD diskNums[16] = {};
    int   numExtents   = QueryDiskExtents(hVol, diskNums, 16);
    CloseHandle(hVol);

    if (numExtents < 1) {
        pOut->available = false;
        return;
    }
    pOut->primaryDiskNum = (int)diskNums[0];
    pOut->numSpindles    = numExtents;

    char physPath[32];
    snprintf(physPath, sizeof(physPath), "\\\\.\\PhysicalDrive%d", pOut->primaryDiskNum);
    HANDLE hPhys = OpenDriveHandle(physPath);
    if (hPhys == INVALID_HANDLE_VALUE) {
        pOut->available = false;
        return;
    }

    bool rotational = false;
    QuerySeekPenalty(hPhys, rotational);
    pOut->isRotational = rotational;

    STORAGE_BUS_TYPE busType = BusTypeUnknown;
    QueryDeviceProps(hPhys, busType, pOut->serial, sizeof(pOut->serial));
    pOut->isNvme = (busType == BusTypeNvme);

    CloseHandle(hPhys);
    pOut->available = true;
}

/*
** ============================================================================
** Benchmark
** ============================================================================
*/

static const size_t BENCH_CHUNK = 4ULL * 1024 * 1024;   /* 4 MB I/O chunk */

/*
** Function: BenchNowSecs
** @brief    Returns a high-resolution timestamp in seconds, for measuring
**           benchmark pass durations.
** @return   Current time in seconds, from QueryPerformanceCounter.
*/
static double BenchNowSecs()
{
    LARGE_INTEGER freq, t;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart / (double)freq.QuadPart;
}

/*
** Function: BenchAlloc
** @brief    Allocates a page-aligned buffer for unbuffered (FILE_FLAG_NO_BUFFERING) I/O.
** @param    bytes - number of bytes to allocate
** @return   Pointer to the allocated buffer, or nullptr on failure.
*/
static void* BenchAlloc(size_t bytes)
{
    return VirtualAlloc(nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
}

/*
** Function: BenchFree
** @brief    Frees a buffer allocated by BenchAlloc. Safe to call with nullptr.
** @param    p - the buffer to free
*/
static void BenchFree(void* p)
{
    if (p) VirtualFree(p, 0, MEM_RELEASE);
}

/*
** Function: BenchWritePass
** @brief    Writes fileBytes worth of buf to path, unbuffered, and measures throughput.
** @param    path      - file path to write (overwritten if it exists)
** @param    buf       - buffer written repeatedly to fill the file
** @param    fileBytes - total bytes to write
** @return   Measured write throughput in MB/s, or 0.0 on failure/short write.
*/
static double BenchWritePass(const char* path, void* buf, size_t fileBytes)
{
    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (h == INVALID_HANDLE_VALUE) return 0.0;
    double t0  = BenchNowSecs();
    size_t rem = fileBytes;
    while (rem > 0) {
        DWORD chunk   = (DWORD)std::min(BENCH_CHUNK, rem);
        DWORD written = 0;
        if (!WriteFile(h, buf, chunk, &written, nullptr) || written != chunk) break;
        rem -= written;
    }
    double elapsed = BenchNowSecs() - t0;
    CloseHandle(h);
    return (elapsed > 0.0 && rem == 0) ? (double)fileBytes / (1024.0 * 1024.0 * elapsed) : 0.0;
}

/*
** Function: BenchReadPass
** @brief    Reads fileBytes from path into buf, unbuffered/sequential, and measures throughput.
** @param    path      - file path to read (must already exist with at least fileBytes)
** @param    buf       - scratch buffer read into (contents discarded)
** @param    fileBytes - total bytes to read
** @return   Measured read throughput in MB/s, or 0.0 on failure/short read.
*/
static double BenchReadPass(const char* path, void* buf, size_t fileBytes)
{
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_FLAG_NO_BUFFERING | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) return 0.0;
    double t0  = BenchNowSecs();
    size_t rem = fileBytes;
    while (rem > 0) {
        DWORD chunk     = (DWORD)std::min(BENCH_CHUNK, rem);
        DWORD bytesRead = 0;
        if (!ReadFile(h, buf, chunk, &bytesRead, nullptr) || bytesRead != chunk) break;
        rem -= chunk;
    }
    double elapsed = BenchNowSecs() - t0;
    CloseHandle(h);
    return (elapsed > 0.0 && rem == 0) ? (double)fileBytes / (1024.0 * 1024.0 * elapsed) : 0.0;
}

/*
** Function: BenchMedian
** @brief    Returns the median of a list of benchmark pass results.
** @param    v - the values to take the median of (sorted in place)
** @return   The median value, or 0.0 if v is empty.
*/
static double BenchMedian(std::vector<double> v)
{
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    size_t n = v.size();
    return (n % 2 == 0) ? (v[n / 2 - 1] + v[n / 2]) * 0.5 : v[n / 2];
}

/*
** Function: BenchmarkOneDrive
** @brief    Bare "how fast can one thread write/read this drive" benchmark:
**           runs numPasses write+read passes (discarding the first as
**           warmup) and records the median throughput.
** @details  No concurrency, no multi-directory sweep -- that question
**           belongs to whatever project actually needs it (and has the real
**           compression pipeline to model), not to Utility.
** @param    letter    - drive letter to benchmark
** @param    fileBytes - size of the temporary benchmark file
** @param    numPasses - number of passes to run (first pass is discarded as warmup)
** @param    verbose   - if true, logs progress via LoggerLog
** @param    pOut      - out: benchmarkValid/writeMBs/readMBs filled in
*/
static void BenchmarkOneDrive(
    char letter,
    size_t fileBytes, int numPasses, bool verbose,
    DriveInformation* pOut)
{
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%c:\\drv_bench_tmp.dat", letter);
    DeleteFileA(path);

    void* buf = BenchAlloc(BENCH_CHUNK);
    if (!buf) {
        pOut->benchmarkValid = false;
        return;
    }
    memset(buf, 0xA5, BENCH_CHUNK);

    if (verbose)
        LoggerLog("    Benchmarking %c: (%zu MB x %d passes, pass 1 discarded)...\n",
               letter, fileBytes / (1024 * 1024), numPasses);

    std::vector<double> writeResults, readResults;
    for (int pass = 0; pass < numPasses; pass++) {
        double w = BenchWritePass(path, buf, fileBytes);
        double r = BenchReadPass(path, buf, fileBytes);

        /* The first pass warms up the drive/filesystem cache and tends to
        ** be an outlier -- discard it rather than let it skew the median.
        */
        if (pass == 0) {
            if (verbose) LoggerLog("      pass 1 (warmup) discarded\n");
            continue;
        }

        writeResults.push_back(w);
        readResults.push_back(r);

        if (verbose)
            LoggerLog("      pass %d: write %.0f MB/s  read %.0f MB/s\n", pass + 1, w, r);
    }

    DeleteFileA(path);
    BenchFree(buf);

    double writeMBs = BenchMedian(writeResults);
    double readMBs  = BenchMedian(readResults);

    if (verbose)
        LoggerLog("      Result: write %.0f MB/s  read %.0f MB/s\n", writeMBs, readMBs);

    pOut->benchmarkValid = (writeMBs > 0.0 && readMBs > 0.0);
    pOut->writeMBs       = writeMBs;
    pOut->readMBs        = readMBs;
}

/*
** ============================================================================
** RefreshDriveFreeSpace
** ============================================================================
*/

/*
** Function: RefreshDriveFreeSpace
** @brief    Re-queries just freeBytes/usableBytes/totalBytes for each drive,
**           without re-running any benchmark.
** @param    pMDI - the drive set to refresh in place
*/
void RefreshDriveFreeSpace(PMachineDriveInfo pMDI)
{
    for (int i = 0; i < pMDI->numDrives; i++) {
        DriveInformation* p = &pMDI->drives[i];
        if (!p->available) continue;
        char rootPath[4] = { p->driveLetter, ':', '\\', '\0' };
        ULARGE_INTEGER freeBytesAvail = {}, totalBytes = {}, totalFree = {};
        if (GetDiskFreeSpaceExA(rootPath, &freeBytesAvail, &totalBytes, &totalFree)) {
            p->totalBytes  = totalBytes.QuadPart;
            p->freeBytes   = freeBytesAvail.QuadPart;
            p->usableBytes = (p->freeBytes > DRIVE_SAFETY_MARGIN_BYTES)
                           ? p->freeBytes - DRIVE_SAFETY_MARGIN_BYTES : 0;
        }
    }
}

/*
** ============================================================================
** GetDriveInformation
** ============================================================================
*/

/*
** Function: GetDriveInformation
** @brief    Queries (and, unless a valid cache entry exists, benchmarks) an
**           array of drives.
** @details  Loads the JSON cache file once up front, then for each requested
**           drive: detects it, tries to satisfy its benchmark fields from a
**           version- and serial-matched cache entry, and only runs a real
**           benchmark if that fails. Any newly-benchmarked drives trigger a
**           cache file rewrite at the end.
** @param    pMachineDriveInfo - out: filled in with detection/benchmark results for each drive
** @param    pCacheDir         - directory for the driveinfo.json benchmark cache; NULL to disable caching
** @param    driveLetters      - null-terminated string of drive letters (e.g. "CDZ"); NULL to auto-enumerate all fixed local drives
** @param    forceBenchmark    - when true, always re-run the benchmark and overwrite the cache
*/
void GetDriveInformation(
    PMachineDriveInfo pMachineDriveInfo,
    const char*       pCacheDir,
    const char*       driveLetters,
    bool              forceBenchmark)
{
    std::string letters;
    if (driveLetters && *driveLetters) {
        letters = driveLetters;
    } else {
        DWORD mask = GetLogicalDrives();
        for (int i = 0; i < 26; i++) {
            if (!(mask & (1u << i))) continue;
            char root[4] = { (char)('A' + i), ':', '\\', '\0' };
            if (GetDriveTypeA(root) == DRIVE_FIXED)
                letters += (char)('A' + i);
        }
    }

    /* Load the JSON cache file once up front. */
    char  cachePath[MAX_PATH] = {};
    char* cacheText           = nullptr;
    if (pCacheDir && pCacheDir[0]) {
        BuildCacheFilePath(pCacheDir, cachePath, sizeof(cachePath));
        FILE* fc = fopen(cachePath, "r");
        if (fc) {
            fseek(fc, 0, SEEK_END);
            long sz = ftell(fc);
            fseek(fc, 0, SEEK_SET);
            if (sz > 0) {
                cacheText = (char*)malloc((size_t)sz + 1);
                if (cacheText) {
                    fread(cacheText, 1, (size_t)sz, fc);
                    cacheText[sz] = '\0';
                }
            }
            fclose(fc);
        }
    }

    bool anyBenchmarked = false;
    int  count          = 0;
    for (char ch : letters) {
        if (count >= MAX_SYSTEM_DRIVES) break;
        DriveInformation& info = pMachineDriveInfo->drives[count];

        QueryOneDrive(ch, &info);

        if (!info.available) {
            count++;
            continue;
        }

        bool cacheLoaded = false;

        if (cacheText && !forceBenchmark && CacheVersionMatches(cacheText))
            cacheLoaded = LoadCacheJSON(cacheText, ch, info.serial, &info);

        if (!cacheLoaded) {
            BenchmarkOneDrive(ch, 1024ULL * 1024 * 1024, info.isNas ? 3 : 5, true, &info);

            SYSTEMTIME st;
            GetLocalTime(&st);
            snprintf(info.timestamp, sizeof(info.timestamp),
                     "%04d-%02d-%02d %02d:%02d:%02d",
                     st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

            anyBenchmarked = true;
        }

        if (info.writeMBs >= FAST_DRIVE_MB_THRESHOLD)
            info.driveCategory = DRIVE_CAT_FAST;
        else if (info.writeMBs >= MEDIUM_DRIVE_MB_THRESHOLD)
            info.driveCategory = DRIVE_CAT_MEDIUM;
        else
            info.driveCategory = DRIVE_CAT_SLOW;

        count++;
    }

    pMachineDriveInfo->numDrives = count;

    if (anyBenchmarked && pCacheDir && pCacheDir[0]) {
        if (!CreateFullPath(pCacheDir))
            Fatal(FATAL_DRIVE_CACHE_WRITE_FAILED,
                  "DriveInfo: cannot create cache directory '%s'", pCacheDir);
        SaveCacheJSON(cachePath, pMachineDriveInfo);
    }

    free(cacheText);
}

/*
** ============================================================================
** PrintDriveInformation
** ============================================================================
*/

/*
** Function: PrintDriveInformation
** @brief    Prints a summary (detection + benchmark) for all drives.
** @param    pMachineDriveInfo - the drive set to report on
*/
void PrintDriveInformation(const PMachineDriveInfo pMachineDriveInfo)
{
    LoggerLog("Detected %d drive(s):\n", pMachineDriveInfo->numDrives);

    for (int i = 0; i < pMachineDriveInfo->numDrives; i++) {
        const DriveInformation* p = &pMachineDriveInfo->drives[i];
        if (!p->available) {
            LoggerLog("  %c:  [unavailable]\n", p->driveLetter);
            continue;
        }

        const char* typeStr = p->isNas        ? "NAS"
                            : p->isNvme       ? "NVMe"
                            : p->isRotational ? "HDD"
                            :                   "SSD";
        double totalTB  = (double)p->totalBytes  / (1024.0 * 1024 * 1024 * 1024);
        double freeTB   = (double)p->freeBytes   / (1024.0 * 1024 * 1024 * 1024);
        double usableTB = (double)p->usableBytes / (1024.0 * 1024 * 1024 * 1024);

        LoggerLog("  %c:  %-4s  disk#%d  spindles=%d"
               "  total=%.2f TB  free=%.2f TB  usable=%.2f TB\n",
               p->driveLetter, typeStr, p->primaryDiskNum, p->numSpindles,
               totalTB, freeTB, usableTB);

        if (p->benchmarkValid)
            LoggerLog("       bench: Write: %.0f MB/s  Read: %.0f MB/s  [%s]\n",
                   p->writeMBs, p->readMBs, p->timestamp);
        else
            LoggerLog("       bench: not available\n");
    }
}
