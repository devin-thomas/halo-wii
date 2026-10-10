#ifndef WII_ABI_ENGINE_SHIM_H
#define WII_ABI_ENGINE_SHIM_H
/* Authored service stand-ins for the generated actual-engine excerpts
   (tools/wii/run_abi_semantics.py). Engine declarations and bodies are
   extracted verbatim; only these spellings are supplied here. */

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
/* newlib's <math.h> defines log2 as a function-like macro (log(x)/_M_LN2);
   the game's math.h on the Wii must drop it before halo_math.h renames log2
   (measured by this fixture's PPC compile). */
#undef log2
/* sin, cos, pow and the rest are the shared halo_ versions on every native
   port (port/include/halo_math.h, port/third_party/musl-math) */
#include "halo_math.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* MSVC C __inline: a private copy per unit, as port/linux/include/halo_linux_prefix.h
   (MinGW headers predefine some of these spellings) */
#undef __inline
#undef _inline
#undef __forceinline
#define __inline static __inline__
#define _inline static __inline__
#define __forceinline static __inline__ __attribute__((always_inline))

#ifndef TRUE
#define TRUE 1
#endif
#ifndef FALSE
#define FALSE 0
#endif

/* Assertions are counted, never fatal, so a failed engine assertion is
   observable in the report instead of stopping the fixture. */
extern unsigned long wii_abi_engine_assertions;
#define match_assert(file, line, expr) if (!(expr)) { wii_abi_engine_assertions++; }
#define match_vassert(file, line, expr, string) if (!(expr)) { wii_abi_engine_assertions++; }
#define match_dassert(file, line, expr, diagnostic) do { match_vassert(file, line, expr, diagnostic); } while (FALSE)
#define assert(expr) match_assert(__FILE__, __LINE__, expr)
#define vassert(expr, string) match_vassert(__FILE__, __LINE__, expr, string)

/* Win32 spellings of port/linux/src/xbox_kernel.c's interlocked bodies:
   LONG is 32 bits on the XDK, LLP64 Windows and PPC32 alike. */
#define WINAPI
typedef long LONG;
typedef LONG *LPLONG;

#endif
