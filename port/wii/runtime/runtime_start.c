#include "runtime_start.h"

#if defined(GEKKO)
#include <string.h>

static uint32_t fpscr(void)
{
    double value;
    uint64_t bits;
    __asm__ volatile("mffs %0" : "=f"(value) : : "memory");
    memcpy(&bits, &value, sizeof(bits));
    return (uint32_t)bits;
}

int wii_runtime_start(struct wii_runtime_start_record *record)
{
    record->fp_control_before = fpscr();
    /* clear FPSCR[NI] only; rounding mode (round to nearest) and the
       exception enables are left as libogc sets them */
    __asm__ volatile("mtfsb0 29" : : : "memory");
    record->fp_control_after = fpscr();
    return (record->fp_control_after & WII_RUNTIME_FPSCR_NI) == 0;
}
#else
/* Host builds of Wii fixtures: x86 and the other ports start in IEEE mode
   (MXCSR FTZ/DAZ clear); nothing to change. */
int wii_runtime_start(struct wii_runtime_start_record *record)
{
    record->fp_control_before = 0;
    record->fp_control_after = 0;
    return 1;
}
#endif
