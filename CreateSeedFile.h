/*
** Filename:  CreateSeedFile.h
**
** Purpose:
**   Declares CreateSeedFile, which writes the Othello starting position as
**   level 0's single seed record so the solver has something to read on its
**   very first pass.
**
** Notes:
**   Adapted from an earlier solver implementation. The one real change:
**   uses OthelloBasics's BoardKeyAllocateFirstBoard (a precomputed
**   ring-ordered constant) instead of a row-major starting-position
**   builder -- this is the one necessary CPU-side exception to the
**   CPU-organizes/GPU-solves boundary (see project memory), and the
**   ring-ordered constant it returns is already validated in OthelloBasics.
**   Everything else (RSF I/O, sentinel write) is otherwise unchanged.
*/

#pragma once

/* Includes */
#include "OthelloTypes.h"

/* Functions */

/*
** Function: CreateSeedFile
** @brief    Writes the standard Othello starting position as a single
**           BOARD_KEY record to level 0's store file. No-ops if the file
**           already exists (supports resuming a run). Fatals on any I/O
**           failure.
** @param    pConfig - run configuration (boardSize, storeDrive/dir via pState)
** @param    pState  - solver state (storeDirectory)
*/
void CreateSeedFile(POthelloRingMasterConfig pConfig, POthelloRingMasterState pState);
