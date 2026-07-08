/*
** Filename:  StoreLevelScan.cpp
**
** Purpose:
**   Implements FindDeepestCompleteLevel declared in StoreLevelScan.h.
*/

/* Includes */
#include "StoreLevelScan.h"
#include "RSFFileName.h"
#include <windows.h>

/* Functions */

/*
** Function: FindDeepestCompleteLevel
** @brief    See StoreLevelScan.h.
*/
int FindDeepestCompleteLevel(const char* storeDir, int boardSize)
{
    int deepest = -1;

    for (int level = 0; level < CALC_MAX_LEVELS; level++)
    {
        char sentPath[MAX_FULL_PATH_NAME];
        SentinelNameComplete(sentPath, sizeof(sentPath), storeDir, boardSize, level);

        if (GetFileAttributesA(sentPath) == INVALID_FILE_ATTRIBUTES)
            break;

        deepest = level;
    }

    return deepest;
}
