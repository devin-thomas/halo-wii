#ifndef WII_RUNTIME_START_H
#define WII_RUNTIME_START_H
/* Wii runtime start (ADR-018). Every Wii main that runs engine code calls
   wii_runtime_start first, before any engine code runs.

   Floating point: libogc starts the program with FPSCR[NI] set (non-IEEE
   mode), in which the Gekko flushes subnormal results to zero. Every other
   port simulates in IEEE arithmetic, and a system link game stays the same
   on every machine only if their results agree to the last bit
   (port/include/halo_math.h), so the runtime clears NI. FPSCR is part of
   each libogc thread's context: a thread that runs engine code calls this
   at its entry too. Its performance cost is measured on hardware
   (HWI-040/041); any relaxation must show unchanged simulation results. */

#include <stdint.h>

struct wii_runtime_start_record
{
    /* the floating-point control word before and after the start: FPSCR on
       the Wii; on other targets, which already start in IEEE mode, both 0 */
    uint32_t fp_control_before;
    uint32_t fp_control_after;
};

/* FPSCR[NI] (IBM bit 29) */
#define WII_RUNTIME_FPSCR_NI UINT32_C(0x00000004)

/* Returns nonzero when the floating-point environment is IEEE afterwards. */
int wii_runtime_start(struct wii_runtime_start_record *record);

#endif
