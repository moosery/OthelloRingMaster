/*
** Filename:  CounterWidthConfig.h
**
** Purpose:
**   Declares CounterWidthConfig: the single persistent cache-directory file
**   recording which tier width (see Utility/WideCounter.h) each level of a
**   given board size actually needed the last time it was processed. Loaded
**   once at startup as the calculator's starting guess per level (default
**   COUNTER_WIDTH_NIBBLE for anything never yet measured), and updated in
**   place -- with monotonic propagation to shallower, not-yet-processed
**   levels -- whenever a level's width guess proves too narrow.
**
** Notes:
**   Same one-flat-JSON-file, load-tolerant/save-fatal-on-failure style as
**   Utility/DriveInfo.cpp's driveinfo.json cache -- "much like the device
**   config" per the project's design discussion. One file per board size
**   (the filename embeds boardSize), living in the calculator's cache
**   directory alongside that same machine/drive-info cache.
*/

#pragma once

/* Includes */
#include "CalculatorTypes.h"

/* Constants */

/*
** COUNTER_WIDTH_NIBBLE
** @brief  Sentinel meaning "this level's tier is narrower than 1 byte" --
**         the 2-boards-per-3-bytes nibble packing (see
**         CalculatorCountsFile.h's NibbleCountsWriter/Reader). Also the
**         default starting guess for any level never yet measured, since
**         width only ever grows from here.
*/
#define COUNTER_WIDTH_NIBBLE 0

/* Structures and Types */

/*
** Type:    CounterWidthConfig
** @brief   One board size's full per-level tier-width table.
*/
typedef struct __CounterWidthConfig
{
    uint8_t boardSize;
    uint8_t byteWidth[CALC_MAX_LEVELS];  /* index = level; COUNTER_WIDTH_NIBBLE or 1, 2, 4, 8, 9, 10, ... bytes */
} CounterWidthConfig;

/* Functions */

/*
** Function: CounterWidthConfigInit
** @brief    Resets pConfig to fresh defaults: every level guessed at the
**           narrowest (nibble) tier.
** @param    pConfig   - out: the config to initialize
** @param    boardSize - the board size this config is for
*/
void CounterWidthConfigInit(CounterWidthConfig* pConfig, int boardSize);

/*
** Function: CounterWidthConfigLoad
** @brief    Initializes pConfig to fresh defaults, then overlays any
**           entries found in this board size's cache file. A missing,
**           corrupt, or board-size-mismatched cache file is not an error --
**           it just means every level starts from the fresh-default guess,
**           mirroring Utility/DriveInfo.cpp's load-tolerant precedent.
** @param    pConfig   - out: the config to fill
** @param    cacheDir  - directory containing the cache file
** @param    boardSize - the board size to load
*/
void CounterWidthConfigLoad(CounterWidthConfig* pConfig, const char* cacheDir, int boardSize);

/*
** Function: CounterWidthConfigSave
** @brief    Writes pConfig to its board size's cache file in cacheDir.
** @param    pConfig  - the config to save
** @param    cacheDir - directory to contain the cache file
*/
void CounterWidthConfigSave(const CounterWidthConfig* pConfig, const char* cacheDir);

/*
** Function: CounterWidthConfigGet
** @brief    Reads one level's current width guess.
** @param    pConfig - the config to read
** @param    level   - the level to look up (must be in 0..CALC_MAX_LEVELS-1)
** @return   COUNTER_WIDTH_NIBBLE or a byte width (1, 2, 4, 8, 9, 10, ...). Fatals if level is out of range.
*/
int CounterWidthConfigGet(const CounterWidthConfig* pConfig, int level);

/*
** Function: CounterWidthConfigBumpLevel
** @brief    Records that level just proved it needs newByteWidth, and
**           propagates that lower bound to every shallower level (index
**           less than level) whose current guess is narrower -- since
**           width only ever grows as level number decreases, those levels
**           cannot need less than what a deeper level already required.
**           Logs the level's own change and every shallower level bumped
**           as a result (never a silent update).
** @param    pConfig      - in/out: the config to update
** @param    level        - the level whose overflow triggered this bump
** @param    newByteWidth - the width that just proved sufficient for level
*/
void CounterWidthConfigBumpLevel(CounterWidthConfig* pConfig, int level, int newByteWidth);
