/*
** Filename:  FileTicket.h
**
** Purpose:
**   Declares FileTicketNext: a lock-free, per-(writer, color) atomic counter
**   that hands out a guaranteed-unique writer-file index to any caller (the
**   flush thread or any concurrent background-consolidation thread)
**   instantly, with no lock held across the write that follows.
**
**     FileTicketNext -- InterlockedIncrement; always succeeds, never blocks
**
** Notes:
**   Replaces the old fileIndexCS[ti][player]-protected read-before-write of
**   mwBlack/WhiteFileCount[ti] (found 2026-07-23 to force flush and
**   consolidation to serialize on the SAME lock for an entire merge's
**   duration, stalling the GPU pipeline -- see OthelloTypes.h's
**   mwNextFileIdx comment and ClaimRegistry.h). mwNextFileIdx[MAX_WRITERS][2]
**   is reset to 0 for every writer/color at the start of each level (writer
**   directories are wiped between levels, so indices never need to persist
**   across a restart).
*/

#pragma once

/* Includes */
#include "OthelloTypes.h"
#include <windows.h>

/* Functions */

/*
** Function: FileTicketNext
** @brief    Atomically hands out the next unique writer-file index for one
**           (writer, color) pair. Always succeeds; never blocks.
** @param    pSt       - solver state
** @param    writerIdx - which writer drive (0..numMergeWriters-1)
** @param    player    - RSF_PLAYER_BLACK or RSF_PLAYER_WHITE
** @return   A file index never handed out before for this (writerIdx, player)
**           pair this level.
*/
static inline int FileTicketNext(POthelloRingMasterState pSt, int writerIdx, int player)
{
    return (int)InterlockedIncrement((volatile LONG*)&pSt->mwNextFileIdx[writerIdx][player]) - 1;
}
