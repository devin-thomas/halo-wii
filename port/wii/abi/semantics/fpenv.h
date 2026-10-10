#ifndef WII_ABI_FPENV_H
#define WII_ABI_FPENV_H
#include <stdint.h>

/* Floating-point control register: FPSCR on PPC, MXCSR on x86-64. */
uint32_t wii_abi_fp_control(void);
/* Changing it is the Wii runtime start's job (port/wii/runtime, ADR-018). */
/* the bits of the control word that flush subnormals */
uint32_t wii_abi_fp_flush_mask(void);

#endif
