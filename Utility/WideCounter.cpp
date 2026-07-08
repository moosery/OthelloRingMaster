/*
** Filename:  WideCounter.cpp
**
** Purpose:
**   Implements WideCounterAdd/NibbleCounterAdd, declared in WideCounter.h.
*/

/* Includes */
#include "WideCounter.h"
#include <string.h>

/* Functions */

/*
** Function: WideCounterAdd
** @brief    Adds addendBytes into accumBytes, writing the tentative
**           result into outBytes, checked before the add.
** @param    accumBytes  - current value, little-endian; not modified
** @param    addendBytes - value to add, little-endian
** @param    byteWidth   - width in bytes
** @param    outBytes    - out: the tentative sum; only meaningful if this returns true
** @return   false if the add would reach or exceed the reserved all-ones value for byteWidth.
*/
bool WideCounterAdd(const uint8_t* accumBytes, const uint8_t* addendBytes,
                    int byteWidth, uint8_t* outBytes)
{
    if (byteWidth == 1)
    {
        uint8_t a = accumBytes[0], b = addendBytes[0];
        const uint8_t maxUsable = 0xFE;
        if (b > (uint8_t)(maxUsable - a)) return false;
        outBytes[0] = (uint8_t)(a + b);
        return true;
    }
    if (byteWidth == 2)
    {
        uint16_t a, b;
        memcpy(&a, accumBytes, 2);
        memcpy(&b, addendBytes, 2);
        const uint16_t maxUsable = 0xFFFE;
        if (b > (uint16_t)(maxUsable - a)) return false;
        a = (uint16_t)(a + b);
        memcpy(outBytes, &a, 2);
        return true;
    }
    if (byteWidth == 4)
    {
        uint32_t a, b;
        memcpy(&a, accumBytes, 4);
        memcpy(&b, addendBytes, 4);
        const uint32_t maxUsable = 0xFFFFFFFEu;
        if (b > maxUsable - a) return false;
        a = a + b;
        memcpy(outBytes, &a, 4);
        return true;
    }
    if (byteWidth == 8)
    {
        uint64_t a, b;
        memcpy(&a, accumBytes, 8);
        memcpy(&b, addendBytes, 8);
        const uint64_t maxUsable = 0xFFFFFFFFFFFFFFFEull;
        if (b > maxUsable - a) return false;
        a = a + b;
        memcpy(outBytes, &a, 8);
        return true;
    }

    /* Bignum tier: byteWidth > 8, manual carry-chain byte by byte -- no
    ** CPU has a native integer wider than 8 bytes, so this is the only
    ** option regardless of exactly how wide byteWidth is.
    */
    int carry = 0;
    for (int i = 0; i < byteWidth; i++)
    {
        int sum = accumBytes[i] + addendBytes[i] + carry;
        outBytes[i] = (uint8_t)(sum & 0xFF);
        carry = (sum > 0xFF) ? 1 : 0;
    }
    if (carry) return false;   /* escaped the top byte -- doesn't fit at all */

    /* A "clean" add (no escaped carry) can still land exactly on the
    ** reserved all-ones value by chance -- check for that too.
    */
    bool allOnes = true;
    for (int i = 0; i < byteWidth; i++)
        if (outBytes[i] != 0xFF) { allOnes = false; break; }
    if (allOnes) return false;

    return true;
}

/*
** Function: NibbleCounterAdd
** @brief    The reserved-sentinel, checked-before-the-add convention at
**           4-bit scale.
** @param    accum  - current value (0-14)
** @param    addend - value to add (0-14)
** @param    pOut   - out: the tentative sum; only meaningful if this returns true
** @return   false if accum + addend would reach or exceed 15.
*/
bool NibbleCounterAdd(uint8_t accum, uint8_t addend, uint8_t* pOut)
{
    /* Values are always small (max 14+14=28) -- plain addition and
    ** comparison is exact, no overflow-safe subtraction trick needed at
    ** this scale (unlike WideCounterAdd's native tiers above).
    */
    int sum = accum + addend;
    if (sum >= 15) return false;
    *pOut = (uint8_t)sum;
    return true;
}
