/*
ENGINE_HOOKS.H

The engine-side half of the Wii engine driver (HWI-015): the engine's own
start-up and shut-down sequences and reports, called by
port/wii/engine/wii_engine_main.c, which includes libogc and so cannot
include the engine's headers. Only C types here.
*/

#ifndef __HALO_WII_ENGINE_HOOKS_H
#define __HALO_WII_ENGINE_HOOKS_H

/* the engine's start-up as source/shell/shell_xbox.c main() does it:
physical_memory_allocate (from the Wii arena, physical_memory_map_wii.c),
then shell_initialize's steps (cseries, platform, errors, real math, game
state, rasterizer), with tag_files_open reported unsupported (see
engine_hooks.c). Returns the rasterizer's result: FALSE on the Wii, whose
rasterizer is unsupported; input and sound are not started after it, as
shell_initialize does not start them. */
int wii_engine_shell_initialize(void);
/* what main_loop runs next: console_initialize and game_initialize, the
subsystems' globals in the game state; then the scenario's allocations */
int wii_engine_game_initialize(void);
/* the engine's shut-down of what started: game_dispose, then shell_dispose's
steps for the shell services that initialized, and physical_memory_free */
void wii_engine_shutdown(void);

/* the engine's error report, as display_assert files a non-fatal one
(source/cseries/cseries.c: error() into d:\debug.txt) */
void wii_engine_report_warning(const char *text);
/* the engine's last error text (error_get) */
const char *wii_engine_error_text(void);
/* a fatal engine assertion (match_assert): display_assert, then
system_exit -> halt_and_catch_fire, which the Wii build reports and exits */
void wii_engine_fatal_assert(void);

/* the bytes of the game state's CPU part the engine has allocated */
unsigned long wii_engine_game_state_used(void);

/* HWI-015D: logs which diagnostic storage the engine compiled (the AI's
debug records, the profiler's frame history) and the AI debug state after
game_initialize; FALSE when the records are left out but ai_debug_initialize
did not run as upstream's */
int wii_engine_report_diagnostic_storage(void);

/* the halt report (wii_engine_main.c): the engine stops here */
void wii_driver_halt(const char *error_text) __attribute__((noreturn));

#endif
