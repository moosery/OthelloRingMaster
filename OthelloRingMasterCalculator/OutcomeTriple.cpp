/*
** Filename:  OutcomeTriple.cpp
**
** Purpose:
**   Implements NibbleOutcomeTriple/OutcomeTriple, declared in
**   OutcomeTriple.h, on top of Utility's generic WideCounterAdd/
**   NibbleCounterAdd.
*/

/* Includes */
#include "OutcomeTriple.h"
#include <string.h>

/* Functions */

/*
** Function: NibbleOutcomeTripleAdd
** @brief    Adds pAddend into pAccum, all three counters committing together or not at all.
** @param    pAccum  - in/out: running total; left unmodified if this would overflow
** @param    pAddend - value to add; each field must already be in 0-14
** @return   false if adding any single field would reach/exceed 15.
*/
bool NibbleOutcomeTripleAdd(NibbleOutcomeTriple* pAccum, const NibbleOutcomeTriple* pAddend)
{
    uint8_t blackOut, whiteOut, tieOut;

    bool blackOk = NibbleCounterAdd(pAccum->black, pAddend->black, &blackOut);
    bool whiteOk = NibbleCounterAdd(pAccum->white, pAddend->white, &whiteOut);
    bool tieOk   = NibbleCounterAdd(pAccum->tie,   pAddend->tie,   &tieOut);

    /* All three counters must fit, or none of them commit -- an add that
    ** overflows only the tie counter must not leave black/white
    ** half-updated.
    */
    if (!blackOk || !whiteOk || !tieOk)
        return false;

    pAccum->black = blackOut;
    pAccum->white = whiteOut;
    pAccum->tie   = tieOut;
    return true;
}

/*
** Function: NibbleOutcomeTripleSetZero
** @brief    Zeroes all three counters.
** @param    pTriple - the triple to zero
*/
void NibbleOutcomeTripleSetZero(NibbleOutcomeTriple* pTriple)
{
    pTriple->black = 0;
    pTriple->white = 0;
    pTriple->tie   = 0;
}

/*
** Function: NibbleOutcomeTripleSetOneHot
** @brief    Sets pTriple to a direct terminal-board classification.
** @param    pTriple - out: the triple to set
** @param    outcome - OUTCOME_BLACK_WIN / OUTCOME_WHITE_WIN / OUTCOME_TIE
*/
void NibbleOutcomeTripleSetOneHot(NibbleOutcomeTriple* pTriple, int outcome)
{
    NibbleOutcomeTripleSetZero(pTriple);
    if (outcome == OUTCOME_BLACK_WIN) pTriple->black = 1;
    else if (outcome == OUTCOME_WHITE_WIN) pTriple->white = 1;
    else pTriple->tie = 1;
}

/*
** Function: OutcomeTripleSetZero
** @brief    Zeroes all three counters.
** @param    pTriple    - the triple to zero
** @param    byteWidth  - unused (zeroing the whole fixed-size array is
**                        simpler and equally cheap, and guarantees no
**                        stale byte ever matters even if byteWidth
**                        changes between calls)
*/
void OutcomeTripleSetZero(OutcomeTriple* pTriple, int byteWidth)
{
    (void)byteWidth;
    memset(pTriple->black, 0, WIDE_COUNTER_MAX_BYTES);
    memset(pTriple->white, 0, WIDE_COUNTER_MAX_BYTES);
    memset(pTriple->tie,   0, WIDE_COUNTER_MAX_BYTES);
}

/*
** Function: OutcomeTripleSetOneHot
** @brief    Sets pTriple to a direct terminal-board classification.
** @param    pTriple    - out: the triple to set
** @param    byteWidth  - this level's tier width in bytes
** @param    outcome    - OUTCOME_BLACK_WIN / OUTCOME_WHITE_WIN / OUTCOME_TIE
*/
void OutcomeTripleSetOneHot(OutcomeTriple* pTriple, int byteWidth, int outcome)
{
    OutcomeTripleSetZero(pTriple, byteWidth);
    if (outcome == OUTCOME_BLACK_WIN) pTriple->black[0] = 1;
    else if (outcome == OUTCOME_WHITE_WIN) pTriple->white[0] = 1;
    else pTriple->tie[0] = 1;
}

/*
** Function: OutcomeTripleAdd
** @brief    Adds pAddend into pAccum, all three counters committing together or not at all.
** @param    pAccum     - in/out: running total; left unmodified if this would overflow
** @param    pAddend    - value to add
** @param    byteWidth  - this level's tier width in bytes
** @return   false if any of the three counters would overflow.
*/
bool OutcomeTripleAdd(OutcomeTriple* pAccum, const OutcomeTriple* pAddend, int byteWidth)
{
    uint8_t blackOut[WIDE_COUNTER_MAX_BYTES];
    uint8_t whiteOut[WIDE_COUNTER_MAX_BYTES];
    uint8_t tieOut[WIDE_COUNTER_MAX_BYTES];

    bool blackOk = WideCounterAdd(pAccum->black, pAddend->black, byteWidth, blackOut);
    bool whiteOk = WideCounterAdd(pAccum->white, pAddend->white, byteWidth, whiteOut);
    bool tieOk   = WideCounterAdd(pAccum->tie,   pAddend->tie,   byteWidth, tieOut);

    /* All three counters must fit, or none of them commit -- same
    ** all-or-nothing reasoning as NibbleOutcomeTripleAdd above.
    */
    if (!blackOk || !whiteOk || !tieOk)
        return false;

    memcpy(pAccum->black, blackOut, byteWidth);
    memcpy(pAccum->white, whiteOut, byteWidth);
    memcpy(pAccum->tie,   tieOut,   byteWidth);
    return true;
}
