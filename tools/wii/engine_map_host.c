/* Host reference of the Wii engine build's real-map run (HWI-015B).
 *
 * The same engine units, platform layer and run (port/wii/engine/
 * engine_map_run.c) as the Wii's engine.dol, compiled for the native ports'
 * i686 ABI with the Linux build's game flags (tools/wii/run_engine_map_host.py),
 * reading the little-endian staging of the same map. libogc's few services
 * come from tools/wii/engine_host_shim.c, MEM2's arena mapped at the Wii run's
 * addresses. It writes the Wii's report lines to
 * sd:/halo-wii-engine/engine.log under its working directory.
 *
 * usage: engine_map_host <scenario path> <cycles> <arena2 low> <arena2 high> <placement offset> */
#include <execinfo.h>
#include <malloc.h>
#include <signal.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "../../port/wii/engine/wii_platform.h"
#include "../../port/wii/engine/engine_hooks.h"
#include "../../port/wii/engine/engine_map_run.h"
#include "../../port/wii/engine/real_map_scenario.h"
#include "../../port/wii/runtime/runtime_start.h"
#include "engine_host_shim.h"

#define GENERAL_BYTES (1024u * 1024u)

int wii_platform_configure(void);
int wii_platform_checks(void);

static const char *stage = "start";
static int failures;

static void check(int ok, const char *what)
{
    if (!ok) {
        failures++;
        wii_log("FAIL %s\n", what);
    }
}

/* a fault's return addresses (tools/wii/run_engine_map_host.py resolves them) */
static void fault(int signal_number)
{
    void *frames[64];
    int count = backtrace(frames, 64);
    char line[64];
    int length = snprintf(line, sizeof line, "FAULT signal=%d stage=%s\n", signal_number, stage);

    if (write(2, line, (size_t)length) < 0) {
    }
    backtrace_symbols_fd(frames, count, 2);
    _exit(128 + signal_number);
}

/* the Wii run's vertical-retrace frame times (its VSYNC_DT lines, in order),
replayed for the host's runs paced by the retrace */
static float *replay_frames;
static long replay_count, replay_next;

static float replay_dt(void *context)
{
    (void)context;
    return replay_next < replay_count ? replay_frames[replay_next++] : 1.0f / 60.0f;
}

static int replay_load(const char *path)
{
    FILE *file = fopen(path, "rb");
    unsigned long bits;

    if (!file)
        return 0;
    replay_frames = malloc(sizeof(float) * 8192);
    while (replay_frames && replay_count < 8192 && fscanf(file, "%lx", &bits) == 1) {
        memcpy(&replay_frames[replay_count], &bits, sizeof(float));
        replay_count++;
    }
    fclose(file);
    return replay_count > 0;
}

static long heap_in_use(void *context)
{
    (void)context;
    return (long)mallinfo2().uordblks;
}

void wii_driver_halt(const char *error_text)
{
    wii_log("HALT stage=%s expected=0 error=\"%s\"\n", stage, error_text ? error_text : "");
    wii_log("END target=engine_host failures=%d result=fail (halted in stage %s)\n", failures + 1, stage);
    wii_log_close();
    exit(1);
}

/* (the engine's own main, source/shell/shell_xbox.c, stays linked, as on the
 * Wii: the link wraps main) */
int __wrap_main(int argc, char **argv)
{
    struct wii_runtime_start_record start;
    struct wii_arena_report arena;
    struct engine_map_run_config config;
    struct engine_map_run_report report;
    unsigned long low, high, offset;
    int cycles;

    if (argc != 6 && argc != 7) {
        fprintf(stderr, "usage: engine_map_host <scenario path> <cycles> <arena2 low> <arena2 high> <offset> "
                        "[<vertical retrace frame times>]\n");
        return 2;
    }
    cycles = atoi(argv[2]);
    low = strtoul(argv[3], NULL, 0);
    high = strtoul(argv[4], NULL, 0);
    offset = strtoul(argv[5], NULL, 0);
    wii_runtime_start(&start);
    signal(SIGSEGV, fault);
    signal(SIGBUS, fault);
    signal(SIGFPE, fault);
    signal(SIGILL, fault);
    mkdir("sd:", 0777);
    mkdir(WII_ENGINE_ROOT, 0777);
    mkdir(WII_ENGINE_DATA_ROOT, 0777);
    mkdir(WII_ENGINE_SAVE_ROOT, 0777);
    mkdir(WII_ENGINE_DATA_ROOT "/maps", 0777);
    if (!host_arena_map(low, high) || !wii_log_open(WII_ENGINE_ROOT "/engine.log"))
        return 2;
    wii_log("BEGIN target=engine_host scenario=%s cycles=%d\n", argv[1], cycles);
    stage = "arena";
    check(wii_arena_open(offset, GENERAL_BYTES, &arena), "arena_open");
    wii_log("ARENA open offset=%lu low=%p high_before=%p high_during=%p state=%p+%lu tags=%p+%lu sound=%p+%lu "
            "general=%p+%lu\n", offset, (void *)arena.arena_low_before, (void *)arena.arena_high_before,
            (void *)arena.arena_high_during, (void *)arena.region_base[_wii_arena_game_state],
            (unsigned long)arena.region_size[_wii_arena_game_state], (void *)arena.region_base[_wii_arena_tag_cache],
            (unsigned long)arena.region_size[_wii_arena_tag_cache], (void *)arena.region_base[_wii_arena_sound_cache],
            (unsigned long)arena.region_size[_wii_arena_sound_cache], (void *)arena.region_base[_wii_arena_general],
            (unsigned long)arena.region_size[_wii_arena_general]);
    stage = "platform";
    check(wii_platform_configure(), "platform_configure");
    check(!wii_platform_checks(), "platform_checks");
    stage = "shell_initialize";
    check(!wii_engine_shell_initialize(), "shell_initialize_reports_rasterizer");
    wii_log("STAGE shell_initialize error=\"%s\" unsupported=%lu\n", wii_engine_error_text(), wii_unsupported_names());
    stage = "game_initialize";
    check(wii_engine_game_initialize(), "game_initialize");
    wii_log("STAGE game_initialize game_state_used=%lu unsupported=%lu\n", wii_engine_game_state_used(),
            wii_unsupported_names());
    check(wii_engine_report_diagnostic_storage(), "diagnostic_storage");

    /* HWI015B_DUMP=<index>[,<index>...]:<tick> (diagnosis, engine_map_run_set_dump) */
    if (getenv("HWI015B_DUMP"))
        engine_map_run_set_dump(getenv("HWI015B_DUMP"), ".");
    stage = "map_run";
    memset(&config, 0, sizeof(config));
    config.scenario_path = argv[1];
    config.cycles = cycles;
    config.heap_in_use = heap_in_use;
    if (argc == 7) {
        int loaded = replay_load(argv[6]);

        wii_log("VSYNC_REPLAY loaded=%d frames=%ld\n", loaded, replay_count);
        if (loaded)
            config.vsync_dt = replay_dt;
    }
    if (!engine_map_run(&config, &report))
        failures += report.failures ? report.failures : 1;

    stage = "shutdown";
    wii_engine_shutdown();
    check(wii_arena_close(&arena), "arena_restored");
    wii_log("ARENA close high_after=%p high_before=%p\n", (void *)arena.arena_high_after,
            (void *)arena.arena_high_before);
    wii_log("UNSUPPORTED names=%lu calls=%lu\n", wii_unsupported_names(), wii_unsupported_calls());
    for (unsigned long index = 0; index < wii_unsupported_names(); index++)
        wii_log("UNSUPPORTED_NAME %s\n", wii_unsupported_name(index));
    wii_log("END target=engine_host failures=%d result=%s\n", failures, !failures ? "pass" : "fail");
    wii_log_close();
    return failures ? 1 : 0;
}
