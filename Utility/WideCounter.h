/*
** Filename:  WideCounter.h
**
** Purpose:
**   Declares a generic, arbitrary-width unsigned counter primitive:
**   WideCounterAdd (checked before the add, not after) and
**   NibbleCounterAdd (the same idea at 4-bit scale). Not Othello-aware --
**   just "add one arbitrary-precision unsigned value into another,
**   reserving the top value as an overflow sentinel, and tell the caller
**   before it happens rather than after."
**
** Notes:
**   Promoted out of OthelloRingMasterCalculator's own retrograde-counter
**   design (see project_adaptive_counter_width_design memory) once it
**   became clear the arithmetic itself has no domain-specific coupling at
**   all -- any future project needing arbitrary-precision counters with
**   this same reserved-sentinel convention can reuse this directly.
**   Domain-specific usage (e.g. a 3-counter black/white/tie triple) is
**   built on top of this in whichever project needs it, not here.
*/

#pragma once

/* Includes */
#include <stdint.h>
#include <stdbool.h>

/* Constants */

/* Far beyond any width a realistic caller could need -- fixed so callers
** can use plain fixed-size buffers with no dynamic allocation.
*/
#define WIDE_COUNTER_MAX_BYTES 32

/* Functions */

/*
** Function: WideCounterAdd
** @brief    Adds addendBytes into accumBytes, writing the tentative
**           result into outBytes rather than mutating accumBytes in
**           place, checked before the add rather than after.
** @details  byteWidth <= 8 rides native CPU integer arithmetic with a
**           subtract-based pre-check: if `addend <= maxUsable - accum`
**           then `accum + addend <= maxUsable`, strictly below the
**           reserved all-ones value, by construction -- so no separate
**           post-check is ever needed at this width. byteWidth > 8 uses a
**           manual carry-chain (no CPU has a native integer wider than 8
**           bytes): a carry escaping the top byte is the overflow signal
**           directly; a "clean" add (no escaped carry) is still checked
**           afterward against the reserved all-ones value, since a
**           carry-chain add could land on it by chance even without an
**           escaped carry. Checking after a raw add would be
**           insufficient at any width -- unsigned wraparound doesn't
**           necessarily land near the top of the range, so a wrapped
**           result can look like a completely plausible, silently wrong
**           value (see project_adaptive_counter_width_design memory for
**           the concrete example that caught this).
** @param    accumBytes  - current value, little-endian (byte 0 least significant); not modified
** @param    addendBytes - value to add, little-endian
** @param    byteWidth   - width in bytes (1 and up; the reserved sentinel is all bits set for this width)
** @param    outBytes    - out: the tentative sum, little-endian; only meaningful if this returns true
** @return   false if the add would reach or exceed the reserved all-ones value for byteWidth (outBytes left untouched).
*/
bool WideCounterAdd(const uint8_t* accumBytes, const uint8_t* addendBytes,
                    int byteWidth, uint8_t* outBytes);

/*
** Function: NibbleCounterAdd
** @brief    The same reserved-sentinel, checked-before-the-add
**           convention as WideCounterAdd, at 4-bit scale: 0-14 is a
**           legitimate count, 15 is reserved to mean "overflowed."
** @param    accum  - current value (0-14)
** @param    addend - value to add (0-14)
** @param    pOut   - out: the tentative sum; only meaningful if this returns true
** @return   false if accum + addend would reach or exceed 15 (*pOut left untouched).
*/
bool NibbleCounterAdd(uint8_t accum, uint8_t addend, uint8_t* pOut);
