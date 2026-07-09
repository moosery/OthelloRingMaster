/*
** Filename:  OutcomeTriple.h
**
** Purpose:
**   Declares OutcomeTriple/NibbleOutcomeTriple: the (black, white, tie)
**   counter this calculator sums while walking backward through the
**   tree, built on top of Utility's generic WideCounterAdd/
**   NibbleCounterAdd. This is the one domain-specific layer -- Utility's
**   own WideCounter.h knows nothing about Othello or outcomes, just
**   "arbitrary-precision counter, checked-before-the-add."
**
** Notes:
**   All three counters at a given level always share one common width --
**   this module never mixes widths within one triple. See
**   project_adaptive_counter_width_design memory for the full tier list
**   and why all three counters share a width.
*/

#pragma once

/* Includes */
#include "Utility.h"
#include "CalculatorTypes.h"   /* WinTieLossTriple */

/* Structures and Types */

/*
** Type:    NibbleOutcomeTriple
** @brief   The three counters at the narrowest tier: each held in a
**          whole byte for convenience, but only 0-14 is ever a
**          legitimate count -- 15 is reserved (see
**          Utility/WideCounter.h's NibbleCounterAdd). Disk packing (2
**          boards per 3 bytes) is a separate, file-format concern, not
**          represented here.
*/
typedef struct __NibbleOutcomeTriple
{
    uint8_t black;
    uint8_t white;
    uint8_t tie;
} NibbleOutcomeTriple;

/*
** Type:    OutcomeTriple
** @brief   One (black, white, tie) counter, each an up-to-256-bit
**          unsigned value stored little-endian (byte 0 is least
**          significant). Only the first byteWidth bytes of each counter
**          are meaningful for a given level's tier -- every function here
**          takes byteWidth explicitly rather than storing it, since it's
**          a per-level property, not a per-instance one.
*/
typedef struct __OutcomeTriple
{
    uint8_t black[WIDE_COUNTER_MAX_BYTES];
    uint8_t white[WIDE_COUNTER_MAX_BYTES];
    uint8_t tie[WIDE_COUNTER_MAX_BYTES];
} OutcomeTriple;

/* Outcome selector, shared by both triple types' SetOneHot functions. */
#define OUTCOME_BLACK_WIN 0
#define OUTCOME_WHITE_WIN 1
#define OUTCOME_TIE       2

/* Functions */

/*
** Function: NibbleOutcomeTripleAdd
** @brief    Adds pAddend into pAccum -- all three counters commit
**           together or not at all, so an overflow in just one (e.g.
**           tie) never leaves the other two half-updated.
** @param    pAccum  - in/out: running total; left unmodified if this would overflow
** @param    pAddend - value to add; each field must already be in 0-14
** @return   false if adding any single field would reach/exceed 15 (the
**           reserved sentinel) -- caller must abort-and-retry this level
**           at a wider tier. true if the add succeeded.
*/
bool NibbleOutcomeTripleAdd(NibbleOutcomeTriple* pAccum, const NibbleOutcomeTriple* pAddend);

/*
** Function: NibbleOutcomeTripleSetZero
** @brief    Zeroes all three counters.
** @param    pTriple - the triple to zero
*/
void NibbleOutcomeTripleSetZero(NibbleOutcomeTriple* pTriple);

/*
** Function: NibbleOutcomeTripleSetOneHot
** @brief    Sets pTriple to a direct terminal-board classification: the
**           named outcome's counter becomes 1, the other two stay 0.
** @param    pTriple - out: the triple to set
** @param    outcome - OUTCOME_BLACK_WIN / OUTCOME_WHITE_WIN / OUTCOME_TIE
*/
void NibbleOutcomeTripleSetOneHot(NibbleOutcomeTriple* pTriple, int outcome);

/*
** Function: OutcomeTripleSetZero
** @brief    Zeroes all three counters.
** @param    pTriple    - the triple to zero
** @param    byteWidth  - this level's tier width in bytes
*/
void OutcomeTripleSetZero(OutcomeTriple* pTriple, int byteWidth);

/*
** Function: OutcomeTripleSetOneHot
** @brief    Sets pTriple to a direct terminal-board classification: the
**           named outcome's counter becomes 1, the other two stay 0.
** @param    pTriple    - out: the triple to set
** @param    byteWidth  - this level's tier width in bytes
** @param    outcome    - OUTCOME_BLACK_WIN / OUTCOME_WHITE_WIN / OUTCOME_TIE
*/
void OutcomeTripleSetOneHot(OutcomeTriple* pTriple, int byteWidth, int outcome);

/*
** Function: OutcomeTripleAdd
** @brief    Adds pAddend into pAccum -- all three counters commit
**           together or not at all, same reasoning as
**           NibbleOutcomeTripleAdd above.
** @param    pAccum     - in/out: running total; left unmodified if this would overflow
** @param    pAddend    - value to add
** @param    byteWidth  - this level's tier width in bytes (must match both triples)
** @return   false if any of the three counters would reach/exceed the
**           reserved all-ones sentinel for byteWidth. true if the add succeeded.
*/
bool OutcomeTripleAdd(OutcomeTriple* pAccum, const OutcomeTriple* pAddend, int byteWidth);

/*
** Function: WinTieLossTripleAccumulateNibble
** @brief    Adds pAddend (one board's own final nibble-tier outcome --
**           a direct terminal one-hot value, or the recursively-summed
**           result of a non-terminal board) into pAccum's running
**           uint64_t display total. Unlike the old terminal-only
**           approximation this replaces, accumulating every processed
**           board's own value this way makes the running total exact --
**           in particular, level 0 always has exactly one (non-terminal)
**           board, so its accumulated total after this call IS the real,
**           fully validated game count, not an approximation.
** @param    pAccum  - in/out: running WinTieLossTriple display total
** @param    pAddend - one board's own nibble-tier outcome triple
** @param    level   - for the Fatal message only, if this ever overflows
*/
void WinTieLossTripleAccumulateNibble(WinTieLossTriple* pAccum, const NibbleOutcomeTriple* pAddend, int level);

/*
** Function: WinTieLossTripleAccumulateWide
** @brief    Same as WinTieLossTripleAccumulateNibble, for a wide-tier
**           OutcomeTriple. Fatals if byteWidth exceeds 8 (pAddend's own
**           value can't fit in a uint64_t at all) or if adding it would
**           overflow pAccum -- a display-only limit, unrelated to the
**           persisted counts file's own arbitrary-precision correctness
**           (see OutcomeTriple's own comments); reachable only at scales
**           far beyond anything this project has run so far, and a real
**           signal to revisit this display mechanism if it ever fires,
**           not something to silently truncate around.
** @param    pAccum     - in/out: running WinTieLossTriple display total
** @param    pAddend    - one board's own wide-tier outcome triple
** @param    byteWidth  - pAddend's tier width in bytes
** @param    level      - for the Fatal message only
*/
void WinTieLossTripleAccumulateWide(WinTieLossTriple* pAccum, const OutcomeTriple* pAddend, int byteWidth, int level);
