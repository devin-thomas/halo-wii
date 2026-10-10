#ifndef WII_MEMORY_STRATEGY_CSERIES_SHIM_H
#define WII_MEMORY_STRATEGY_CSERIES_SHIM_H
/* Authored services for the actual engine units source/memory/data.c and
 * source/memory/memory_pool.c in the HWI-007 game-state prototype. Not the
 * engine's cseries.h: only what those two units and their headers name.
 * A failed engine assertion is recorded (gs_engine_assert_*) instead of
 * exiting; the prototype checks the count after every engine call. */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define __CSERIES_H
typedef unsigned char byte;
typedef unsigned short word;
typedef byte boolean;
#define TRUE 1
#define FALSE 0
#define NONE -1
#define NUMBEROF(array) (sizeof(array) / sizeof(array[0]))

extern unsigned long gs_engine_assert_count;
extern long gs_engine_assert_line;
void gs_engine_assert(long line);

/* (the engine's own source-path strings are not kept: only the line) */
#define match_assert(file, line, expr) do { if (!(expr)) gs_engine_assert(line); } while (0)
#define match_vassert(file, line, expr, string) do { if (!(expr)) { (void)(string); gs_engine_assert(line); } } while (0)

extern char temporary[256];
#define csprintf(buffer, ...) (snprintf((buffer), sizeof(temporary), __VA_ARGS__), (buffer))
#define csmemset memset
#define csmemcpy memcpy
#define csmemmove memmove
#define csstrncpy(destination, source, count) strncpy((destination), (source), (count))
#define match_malloc(file, line, size) malloc(size)
#define match_free(file, line, pointer) free(pointer)

_Static_assert(sizeof(long) == 4 && sizeof(short) == 2 && sizeof(void *) == 4,
               "the engine units need the engine's 32-bit long and pointer ABI");
#endif
