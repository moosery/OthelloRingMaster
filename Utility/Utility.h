/*
** Filename:  Utility.h
**
** Purpose:
**   Umbrella header that pulls in every module of the Utility project, so
**   consumers can #include "Utility.h" once instead of picking individual
**   headers.
**
** Notes:
**   SysMemInfo.h was previously included twice here; removed as a harmless
**   duplicate (its own #pragma once made it a no-op, not a bug).
*/

#pragma once

/* Includes */
#include "ArenaMem.h"
#include "BinarySearch.h"
#include "BinSearchLE.h"
#include "ClockTick.h"
#include "DriveInfo.h"
#include "Error.h"
#include "FileAndDirUtils.h"
#include "Logger.h"
#include "Lz4Stream.h"
#include "Mem.h"
#include "RingStoreFile.h"
#include "RWLock.h"
#include "SysMemInfo.h"
#include "ThreadPool.h"
#include "WideCounter.h"
