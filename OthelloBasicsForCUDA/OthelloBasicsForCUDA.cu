/*
** Filename:  OthelloBasicsForCUDA.cu
**
** Purpose:
**   Implements OBCuda_GetBoardConsts (declared in OthelloBasicsForCUDA.h):
**   the one non-inline function in this module, kept in its own
**   compilation unit so every .cu file that includes the header doesn't
**   need to re-emit it.
*/

/* Includes */
#include "OthelloBasicsForCUDA.h"

/* Functions */

/*
** Function: OBCuda_GetBoardConsts
** @brief    Captures the current g_board* globals into a DevBoardConsts for
**           device code to use.
** @return   The current board-size constants.
*/
DevBoardConsts OBCuda_GetBoardConsts()
{
    DevBoardConsts c;
    c.boardMask      = g_boardMask;
    c.boardRightEdge = g_boardRightEdge;
    c.boardLeftEdge  = g_boardLeftEdge;
    return c;
}
