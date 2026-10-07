#ifndef WII_MEMORY_SUBSET_SHIM_H
#define WII_MEMORY_SUBSET_SHIM_H

/* Isolated test services, not a replacement engine platform header. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define __int64 long long
typedef unsigned char byte;
typedef unsigned short word;
typedef byte boolean;
#define TRUE 1
#define FALSE 0
#define UNSIGNED_CHAR_MAX 255
#define UNSIGNED_SHORT_MAX 65535
#define csmemcpy memcpy
#define csmemset memset
#define match_assert(file, line, condition) do { if (!(condition)) { \
    fprintf(stderr, "MEMORY ASSERT %s:%d\n", file, line); exit(2); } } while (0)
#define display_assert(message, file, line, fatal) do { \
    (void)(message); (void)(fatal); \
    fprintf(stderr, "MEMORY ASSERT %s:%d\n", file, line); } while (0)
static _Noreturn inline void system_exit(long status) { exit((int)status); }
#include "memory/byte_swapping.h"
#include "memory/data_encoding.h"

_Static_assert(sizeof(long) == 4 && sizeof(short) == 2 && sizeof(__int64) == 8,
               "Subset requires the engine's 32-bit long / 16-bit short ABI");

#endif
