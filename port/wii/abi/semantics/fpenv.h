#ifndef WII_ABI_FPENV_H
#define WII_ABI_FPENV_H
#include <stdint.h>

/* Floating-point control register: FPSCR on PPC, MXCSR on x86-64. */
uint32_t wii_abi_fp_control(void);
/* Clear subnormal flushing (PPC FPSCR[NI]; x86 MXCSR FTZ/DAZ) and return the
   previous control word for wii_abi_fp_restore. */
uint32_t wii_abi_fp_ieee_subnormals(void);
void wii_abi_fp_restore(uint32_t saved);
/* the bits of the control word that flush subnormals */
uint32_t wii_abi_fp_flush_mask(void);

#endif
