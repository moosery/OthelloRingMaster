/*
** Filename:  RWLock.h
**
** Purpose:
**   Declares a named wrapper around std::shared_mutex: RWLockInit/RWLockFree
**   manage the lock's lifetime, the RWLockRead.../RWLockWrite... functions
**   take and release shared (read) or exclusive (write) ownership, and
**   every call takes a pszLocation string identifying the call site -- used
**   by RWLock.cpp's optional debug-log and lock-accounting builds (both
**   compiled out by default) to report exactly where a lock was
**   taken/held/released.
*/

#pragma once

/* Includes */
#include <shared_mutex>

/* Macros and Defines */
#define LOCKNAME_SIZE  32

/* Structures and Types */

/*
** Type:    RWLock
** @brief   A named reader/writer lock: a fixed-size name (for debug/stats
**          reporting) plus the underlying std::shared_mutex, heap-allocated
**          by RWLockInit so RWLock itself stays a plain, relocatable struct.
*/
typedef struct _RWLock
{
    char                lockName[LOCKNAME_SIZE + 1];
    char                _pad[7];      /* explicit padding: lockName[33] ends at byte 32; pRwLock needs 8-byte alignment at byte 40 */
    std::shared_mutex*  pRwLock;
} RWLock, * PRWLock;

/* Functions */

/*
** Function: RWLockInit
** @brief    Names pLock and allocates its underlying shared_mutex.
** @param    pszLockName - name recorded on the lock, for debug/stats reporting
** @param    pszLocation - call-site identifier, for debug/stats reporting
** @param    pLock       - the lock to initialize
*/
void RWLockInit(const char* pszLockName, const char* pszLocation, PRWLock pLock);

/*
** Function: RWLockReadLock
** @brief    Takes a shared (read) lock on pLock, blocking until available.
** @param    pszLocation - call-site identifier, for debug/stats reporting
** @param    pLock       - the lock to acquire
*/
void RWLockReadLock(const char* pszLocation, PRWLock pLock);

/*
** Function: RWLockReadTryLock
** @brief    Attempts to take a shared (read) lock on pLock, retrying up to
**           attempts times with a short sleep between tries.
** @param    pszLocation - call-site identifier, for debug/stats reporting
** @param    pLock       - the lock to acquire
** @param    attempts    - maximum number of attempts before giving up
** @return   true if the lock was acquired.
*/
bool RWLockReadTryLock(const char* pszLocation, PRWLock pLock, int attempts);

/*
** Function: RWLockReadUnlock
** @brief    Releases a shared (read) lock previously acquired on pLock.
** @param    pszLocation - call-site identifier, for debug/stats reporting
** @param    pLock       - the lock to release
*/
void RWLockReadUnlock(const char* pszLocation, PRWLock pLock);

/*
** Function: RWLockWriteTryLock
** @brief    Attempts to take an exclusive (write) lock on pLock, retrying up
**           to attempts times with a short sleep between tries.
** @param    pszLocation - call-site identifier, for debug/stats reporting
** @param    pLock       - the lock to acquire
** @param    attempts    - maximum number of attempts before giving up
** @return   true if the lock was acquired.
*/
bool RWLockWriteTryLock(const char* pszLocation, PRWLock pLock, int attempts);

/*
** Function: RWLockWriteLock
** @brief    Takes an exclusive (write) lock on pLock, blocking until available.
** @param    pszLocation - call-site identifier, for debug/stats reporting
** @param    pLock       - the lock to acquire
*/
void RWLockWriteLock(const char* pszLocation, PRWLock pLock);

/*
** Function: RWLockWriteUnlock
** @brief    Releases an exclusive (write) lock previously acquired on pLock.
** @param    pszLocation - call-site identifier, for debug/stats reporting
** @param    pLock       - the lock to release
*/
void RWLockWriteUnlock(const char* pszLocation, PRWLock pLock);

/*
** Function: RWLockFree
** @brief    Frees pLock's underlying shared_mutex.
** @param    pszLocation - call-site identifier, for debug/stats reporting
** @param    pLock       - the lock to free
*/
void RWLockFree(const char* pszLocation, PRWLock pLock);

/*
** Function: RWLockStats
** @brief    Prints this thread's current/maximum read- and write-lock
**           counts. A no-op unless the DO_STATS or DEBUG_ON build option in
**           RWLock.cpp is enabled.
*/
void RWLockStats();
