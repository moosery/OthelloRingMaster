/*
** Filename:  TerminalClassify.h
**
** Purpose:
**   Declares ClassifyTerminalOutcome: the one-line piece-count
**   classification a terminal board's own (black, white, tie) outcome is
**   determined from, shared between TerminalLevelBootstrap.cpp (every
**   board at the deepest level is terminal) and the non-terminal
**   backward step (individual boards can still be terminal at any level).
*/

#pragma once

/* Includes */
#include "OthelloBasics.h"
#include "OutcomeTriple.h"
#include <bit>

/* Functions */

/*
** Function: ClassifyTerminalOutcome
** @brief    Classifies a terminal board directly from its final piece
**           count -- more black discs wins, more white wins, equal ties.
** @param    key - the board to classify (terminal: neither player can move)
** @return   OUTCOME_BLACK_WIN, OUTCOME_WHITE_WIN, or OUTCOME_TIE.
*/
static inline int ClassifyTerminalOutcome(const BOARD_KEY& key)
{
    int blackCount = std::popcount(key.ullCellsInUse & key.ullCellColors);
    int whiteCount = std::popcount(key.ullCellsInUse) - blackCount;

    if (blackCount > whiteCount) return OUTCOME_BLACK_WIN;
    if (whiteCount > blackCount) return OUTCOME_WHITE_WIN;
    return OUTCOME_TIE;
}
