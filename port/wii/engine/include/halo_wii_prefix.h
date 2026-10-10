/*
HALO_WII_PREFIX.H

Force-included ahead of every engine and platform translation unit of the
Wii engine build (wii_engine, tools/wii/engine_build.py), as
port/linux/include/halo_linux_prefix.h is for the Linux build. It reproduces
the MSVC/XDK environment the game source assumes, for devkitPPC GCC
(big-endian PowerPC), so the source stays as the other ports compile it.

What differs from the Linux prefix, and why:

- GCC rejects `static static`, which clang only warns about. The Linux prefix
  makes `__inline` static; here `__inline` is plain GNU89 inline instead,
  which emits an external definition in every unit, and the generated
  semantics header (tools/wii/engine_semantics.py) marks each such name
  `#pragma weak`: one definition is kept, as MSVC keeps one COMDAT copy.
  Functions explicitly `static __inline` stay static and get no pragma.
- GCC for PowerPC does not know __int64, __stdcall, __cdecl, __fastcall or
  __declspec; clang accepts them with -fms-extensions on any target.
- wchar_t comes from GCC's <stddef.h> under -fshort-wchar (16 bits, as on
  the Xbox). newlib types wint_t as unsigned int before
  port/linux/include/wchar.h could make it unsigned short; it holds the same
  values (WEOF is 0xFFFF either way) and no engine structure stores one.
- No _X86_/_M_IX86: nothing here is x86, and no XDK header in
  port/include/xdk tests them.
*/

#ifndef __HALO_WII_PREFIX_H
#define __HALO_WII_PREFIX_H

#if !defined(__PPC__) || !defined(__BIG_ENDIAN__)
#error the Wii engine build targets big-endian PowerPC
#endif
#if !defined(__WCHAR_MAX__) || __WCHAR_MAX__ != 0xffff
#error the Wii engine build requires -fshort-wchar (ADR-018)
#endif
#ifdef __CHAR_UNSIGNED__
#error the Wii engine build requires -fsigned-char (ADR-018)
#endif

/* wint_t first (newlib's), then wchar_t (the compiler's 16-bit type) */
#define __need_wint_t
#include <stddef.h>
#include <stddef.h>
#define __wint_t_defined 1

#define _STDCALL_SUPPORTED 1
#define _INTEGRAL_MAX_BITS 64
#define _WCHAR_T_DEFINED
#define _USE_MATH_DEFINES
/* the XDK's COM headers decorate methods with __export when _WIN32 is unset */
#define __export

/* ---------- MSVC inline semantics (see above) */

#define __inline __inline__
#define _inline __inline__
#define __forceinline __inline__
/* port/include/xdk/xdk_d3d8.h: `static __forceinline` otherwise, which
would make its definitions static after their ordinary prototypes */
#define D3DINLINE __inline__
/* newlib's own extern-inline helpers stay out of the way */
#define __NO_INLINE__ 1

/* ---------- __declspec(selectany) data (XDK D3DCONST tables) */

#define DECLSPEC_SELECTANY __attribute__((weak))

/* ---------- MSVC keywords */

#define __int64 long long
#define __int32 int
#define __int16 short
#define __int8 char
/* PowerPC has one calling convention. newlib's <sys/cdefs.h> spells
__fastcall as an x86 attribute; it is read first, so this one stands. */
#include <sys/cdefs.h>
#undef __fastcall
#define __stdcall
#define __cdecl
#define __fastcall
#define __declspec(specifier) __HALO_WII_DECLSPEC_##specifier
#define __HALO_WII_DECLSPEC_align(bytes) __attribute__((aligned(bytes)))
#define __HALO_WII_DECLSPEC_noreturn __attribute__((noreturn))
#define __HALO_WII_DECLSPEC_selectany __attribute__((weak))
#define __HALO_WII_DECLSPEC_dllexport
#define __HALO_WII_DECLSPEC_dllimport

/* ---------- MSVC intrinsics (port/wii/engine/wii_kernel.c) */

#define _InterlockedCompareExchange halo_linux_InterlockedCompareExchange
#define _InterlockedDecrement halo_linux_InterlockedDecrement
#define _InterlockedExchange halo_linux_InterlockedExchange
#define _InterlockedExchangeAdd halo_linux_InterlockedExchangeAdd
#define _InterlockedIncrement halo_linux_InterlockedIncrement

/* ---------- structured exception handling

As on Linux: only the top-level crash handler uses SEH; the guarded block
always runs and the handler is compiled out. */

#define __try if (1)
#define __except(filter) else if (0)
#define __finally
#define __leave

/* ---------- multiplayer session limits of the native builds (unchanged) */

#include "halo_port_limits.h"

/* ADR-015: the game state's base is chosen at run time in MEM2
(port/wii/engine/physical_memory_map_wii.c), not the native builds' fixed
0x81A00000 (halo_port_capacity.h), which ADR-016 measured outside usable
Wii memory. Its one user, game_state_initialize, passes it to
game_state_allocate_buffer, which checks the buffer is there. */
void *physical_memory_get_game_state_base_address(void);
#undef HALO_PORT_GAME_STATE_BASE_ADDRESS
#define HALO_PORT_GAME_STATE_BASE_ADDRESS ((unsigned long)physical_memory_get_game_state_base_address())

#ifndef HALO_LINUX_PLATFORM_LAYER
#define FD_SETSIZE HALO_PORT_FD_SETSIZE
#endif

#ifndef HALO_LINUX_PLATFORM_LAYER
#include "halo_linux_winsock_names.h"
#include "halo_linux_source_fixups.h"
#endif

#endif /* __HALO_WII_PREFIX_H */
