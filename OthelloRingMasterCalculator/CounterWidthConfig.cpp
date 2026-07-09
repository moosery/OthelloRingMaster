/*
** Filename:  CounterWidthConfig.cpp
**
** Purpose:
**   Implements CounterWidthConfig, declared in CounterWidthConfig.h: a
**   single flat-JSON cache file per board size, load-tolerant/save-fatal,
**   same minimal hand-rolled JSON style as Utility/DriveInfo.cpp's
**   driveinfo.json cache.
*/

/* Includes */
#include "CounterWidthConfig.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Constants */
static const int COUNTER_WIDTH_CONFIG_JSON_VERSION = 1;

/* Internal Helpers */

/*
** Function: BuildCounterWidthConfigPath
** @brief    Builds the full path to this board size's cache file within cacheDir.
** @param    cacheDir  - directory containing the cache file
** @param    boardSize - the board size the file is for
** @param    out       - buffer to receive the built path
** @param    outSz     - size in bytes of out
*/
static void BuildCounterWidthConfigPath(const char* cacheDir, int boardSize, char* out, size_t outSz)
{
    snprintf(out, outSz, "%s\\counterwidthconfig_%dx%d.json", cacheDir, boardSize, boardSize);
}

/*
** Function: JsInt
** @brief    Extracts an integer value for key from a flat JSON object/array-element text.
** @param    obj - JSON text to search
** @param    key - key name to search for
** @param    out - out: the parsed integer value
** @return   true if key was found and its numeric value parsed into out.
*/
static bool JsInt(const char* obj, const char* key, int* out)
{
    char search[64];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char* p = strstr(obj, search);
    if (!p) return false;
    p += strlen(search);
    while (*p == ' ' || *p == ':' || *p == '\t' || *p == '\n' || *p == '\r') ++p;
    char* end;
    long  v = strtol(p, &end, 10);
    if (end == p) return false;
    *out = (int)v;
    return true;
}

/*
** Function: WidthLabel
** @brief    Formats a width value for log messages ("nibble" or "N bytes").
** @param    byteWidth - COUNTER_WIDTH_NIBBLE or a byte width
** @param    out       - buffer to receive the label
** @param    outSz     - size in bytes of out
*/
static void WidthLabel(int byteWidth, char* out, size_t outSz)
{
    if (byteWidth == COUNTER_WIDTH_NIBBLE)
        snprintf(out, outSz, "nibble");
    else
        snprintf(out, outSz, "%d bytes", byteWidth);
}

/* Functions */

/*
** Function: CounterWidthConfigInit
** @brief    See CounterWidthConfig.h.
*/
void CounterWidthConfigInit(CounterWidthConfig* pConfig, int boardSize)
{
    pConfig->boardSize = (uint8_t)boardSize;
    memset(pConfig->byteWidth, COUNTER_WIDTH_NIBBLE, sizeof(pConfig->byteWidth));
}

/*
** Function: CounterWidthConfigLoad
** @brief    See CounterWidthConfig.h. A missing file, a version mismatch,
**           or a board-size mismatch all leave the fresh defaults from
**           CounterWidthConfigInit in place -- not an error, since every
**           level guessed at COUNTER_WIDTH_NIBBLE is always a safe
**           (if possibly slow-to-converge) starting point.
*/
void CounterWidthConfigLoad(CounterWidthConfig* pConfig, const char* cacheDir, int boardSize)
{
    CounterWidthConfigInit(pConfig, boardSize);

    char path[MAX_FULL_PATH_NAME];
    BuildCounterWidthConfigPath(cacheDir, boardSize, path, sizeof(path));

    FILE* f = fopen(path, "r");
    if (!f)
        return;

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    char* text = nullptr;
    if (sz > 0)
    {
        text = (char*)malloc((size_t)sz + 1);
        if (text)
        {
            fread(text, 1, (size_t)sz, f);
            text[sz] = '\0';
        }
    }
    fclose(f);

    if (!text)
        return;

    do
    {
        int version = -1;
        if (!JsInt(text, "version", &version) || version != COUNTER_WIDTH_CONFIG_JSON_VERSION)
            break;

        int fileBoardSize = -1;
        if (!JsInt(text, "boardSize", &fileBoardSize) || fileBoardSize != boardSize)
            break;

        /* Walk each "{ ... }" block inside the "levels" array in file order. */
        const char* cursor = strstr(text, "\"levels\"");
        if (!cursor)
            break;

        while ((cursor = strchr(cursor, '{')) != nullptr)
        {
            const char* blockEnd = strchr(cursor, '}');
            if (!blockEnd)
                break;

            size_t blockLen = (size_t)(blockEnd - cursor) + 1;
            char*  block     = (char*)malloc(blockLen + 1);
            if (block)
            {
                memcpy(block, cursor, blockLen);
                block[blockLen] = '\0';

                int level     = -1;
                int byteWidth = -1;
                if (JsInt(block, "level", &level) && JsInt(block, "byteWidth", &byteWidth) &&
                    level >= 0 && level < CALC_MAX_LEVELS)
                {
                    pConfig->byteWidth[level] = (uint8_t)byteWidth;
                }

                free(block);
            }

            cursor = blockEnd + 1;
        }
    } while (false);

    free(text);
}

/*
** Function: CounterWidthConfigSave
** @brief    See CounterWidthConfig.h.
*/
void CounterWidthConfigSave(const CounterWidthConfig* pConfig, const char* cacheDir)
{
    char path[MAX_FULL_PATH_NAME];
    BuildCounterWidthConfigPath(cacheDir, pConfig->boardSize, path, sizeof(path));

    FILE* f = fopen(path, "w");
    if (!f)
        Fatal(FATAL_COUNTER_WIDTH_CONFIG_WRITE_FAILED,
              "CounterWidthConfigSave: cannot write cache file '%s'", path);

    fprintf(f, "{\n  \"version\": %d,\n  \"boardSize\": %d,\n  \"levels\": [\n",
            COUNTER_WIDTH_CONFIG_JSON_VERSION, pConfig->boardSize);

    bool first = true;
    for (int level = 0; level < CALC_MAX_LEVELS; level++)
    {
        if (!first) fprintf(f, ",\n");
        first = false;
        fprintf(f, "    { \"level\": %d, \"byteWidth\": %d }", level, pConfig->byteWidth[level]);
    }

    fprintf(f, "\n  ]\n}\n");
    fclose(f);
}

/*
** Function: CounterWidthConfigGet
** @brief    See CounterWidthConfig.h.
*/
int CounterWidthConfigGet(const CounterWidthConfig* pConfig, int level)
{
    if (level < 0 || level >= CALC_MAX_LEVELS)
        Fatal(FATAL_MERGE_LOGIC_ERROR, "CounterWidthConfigGet: level %d out of range (0..%d)", level, CALC_MAX_LEVELS - 1);

    return pConfig->byteWidth[level];
}

/*
** Function: CounterWidthConfigBumpLevel
** @brief    See CounterWidthConfig.h.
*/
void CounterWidthConfigBumpLevel(CounterWidthConfig* pConfig, int level, int newByteWidth)
{
    if (level < 0 || level >= CALC_MAX_LEVELS)
        Fatal(FATAL_MERGE_LOGIC_ERROR, "CounterWidthConfigBumpLevel: level %d out of range (0..%d)", level, CALC_MAX_LEVELS - 1);

    /* Callers also invoke this to re-confirm a level's already-correct
    ** width (propagating a floor to shallower levels after the fact) --
    ** that is NOT an overflow, so only log/write when newByteWidth is
    ** actually wider than what's already set.
    */
    if (newByteWidth != pConfig->byteWidth[level])
    {
        char oldLabel[16], newLabel[16];
        WidthLabel(pConfig->byteWidth[level], oldLabel, sizeof(oldLabel));
        WidthLabel(newByteWidth, newLabel, sizeof(newLabel));
        LoggerLog("CounterWidthConfig: level %d overflowed at %s, widening to %s\n", level, oldLabel, newLabel);

        pConfig->byteWidth[level] = (uint8_t)newByteWidth;
    }

    for (int shallower = 0; shallower < level; shallower++)
    {
        if (pConfig->byteWidth[shallower] < newByteWidth)
        {
            char shallowerOldLabel[16];
            WidthLabel(pConfig->byteWidth[shallower], shallowerOldLabel, sizeof(shallowerOldLabel));
            LoggerLog("CounterWidthConfig: propagating level %d's %s floor to shallower level %d (was %s)",
                      level, newLabel, shallower, shallowerOldLabel);
            pConfig->byteWidth[shallower] = (uint8_t)newByteWidth;
        }
    }
}
