/*
** Filename:  OthelloRingMaster.cpp
**
** Purpose:
**   Entry point for OthelloRingMaster. For now, just initializes the GPU
**   ring-permutation tables and runs the round-trip validation kernel
**   (see OthelloBasicsForCUDA/RingConversion.h), reporting pass/fail. This
**   is the smoke test that the ring<->row-major GPU boundary conversion
**   actually works before anything else in the solution depends on it.
**   Will grow into the real entry point (or be superseded by a dedicated
**   OthelloRingBlaster project) as later phases land.
*/

/* Includes */
#include <stdio.h>
#include <RingConversion.h>

/*
** Function: main
** @brief    Initializes the ring-permutation tables and runs the round-trip
**           validation kernel, printing the result.
** @return   0 if the validation passed, 1 otherwise.
*/
int main()
{
    printf("OthelloRingMaster\n");

    OBCuda_InitRingPermutationTables();

    bool ok = OBCuda_TestRingRoundTrip();
    printf("Ring<->row-major GPU boundary conversion: %s\n", ok ? "PASS" : "FAIL");

    return ok ? 0 : 1;
}
