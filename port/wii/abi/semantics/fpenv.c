#include "fpenv.h"
#include <string.h>

#if defined(GEKKO) || defined(__powerpc__)
/* FPSCR[NI] (bit 29, IBM numbering; value 0x4): non-IEEE mode, in which the
   Gekko/750 flushes subnormal results and operands to zero. */
#define FLUSH_MASK UINT32_C(0x00000004)

uint32_t wii_abi_fp_control(void)
{
    double value;
    uint64_t bits;
    __asm__ volatile("mffs %0" : "=f"(value));
    memcpy(&bits, &value, sizeof(bits));
    return (uint32_t)bits;
}

static void set_control(uint32_t control)
{
    uint64_t bits = control;
    double value;
    memcpy(&value, &bits, sizeof(value));
    __asm__ volatile("mtfsf 0xff, %0" : : "f"(value));
}
#elif defined(__x86_64__) || defined(__i386__)
#include <xmmintrin.h>
/* MXCSR FTZ (bit 15) and DAZ (bit 6) */
#define FLUSH_MASK UINT32_C(0x00008040)

uint32_t wii_abi_fp_control(void)
{
    return _mm_getcsr();
}

static void set_control(uint32_t control)
{
    _mm_setcsr(control);
}
#else
#error "fpenv.c needs this target's floating-point control register"
#endif

uint32_t wii_abi_fp_ieee_subnormals(void)
{
    uint32_t saved = wii_abi_fp_control();
    set_control(saved & ~FLUSH_MASK);
    return saved;
}

void wii_abi_fp_restore(uint32_t saved)
{
    set_control(saved);
}

uint32_t wii_abi_fp_flush_mask(void)
{
    return FLUSH_MASK;
}
