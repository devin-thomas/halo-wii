/* Host reference of the Wii engine build's boot mode (HWI-016D).
 *
 * The same engine units, platform layer and boot-mode engine side
 * (port/wii/engine/boot_hooks.c, port/wii/render/render_ui.c) as the Wii's
 * engine.dol, compiled for the native ports' i686 ABI
 * (tools/wii/run_boot_host.py), reading the little-endian staging of the
 * same maps. The engine's own main runs through the original menus as on
 * the Wii; nothing is drawn (boot_host_render_stub.c), the frame clock is
 * the Wii's (boot_hooks.h), and the GameCube pad is the Wii launch's,
 * replayed scan by scan from its log (its BOOT_PAD lines). The screens'
 * game-state digests (BOOT_SCREEN, BOOT_ENVIRONMENT) are then compared with
 * the Wii's.
 *
 * usage: boot_menu_host <arena2 low> <arena2 high> <placement offset> <pad file> <max frames>
 *        <environment frames> <settle frames> */
#include <execinfo.h>
#include <signal.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "../../port/wii/engine/wii_platform.h"
#include "../../port/wii/engine/wii_input.h"
#include "../../port/wii/engine/boot_hooks.h"
#include "../../port/wii/engine/real_map_scenario.h"
#include "../../port/wii/render/wii_render_ui.h"
#include "../../port/wii/runtime/runtime_start.h"
#include "engine_host_shim.h"

#define GENERAL_BYTES (1024u * 1024u)
#define MAX_PAD_CHANGES 4096

int wii_platform_configure(void);
int __real_main(void);

struct pad_change {
    unsigned long scan;
    struct wii_pad_state state;
};

static struct pad_change pad_changes[MAX_PAD_CHANGES];
static long pad_change_count;
static unsigned long pad_scans;
static struct wii_pad_state pad_now;

static void fault(int signal_number)
{
    void *frames[64];
    int count = backtrace(frames, 64);
    char line[64];
    int length = snprintf(line, sizeof line, "FAULT signal=%d stage=boot_host\n", signal_number);

    if (write(2, line, (size_t)length) < 0) {
    }
    backtrace_symbols_fd(frames, count, 2);
    _exit(128 + signal_number);
}

void wii_driver_halt(const char *error_text)
{
    wii_log("HALT stage=boot_host expected=0 error=\"%s\"\n", error_text ? error_text : "");
    wii_log("END target=engine_host mode=boot result=fail (halted)\n");
    wii_log_close();
    exit(1);
}

/* "<scan> <connected> <buttons> <sx> <sy> <cx> <cy> <l> <r>" lines, scans ascending */
static int pad_load(const char *path)
{
    FILE *file = fopen(path, "rb");
    unsigned long scan;
    unsigned int connected, buttons, l, r;
    int sx, sy, cx, cy;

    if (!file)
        return 0;
    while (pad_change_count < MAX_PAD_CHANGES &&
           fscanf(file, "%lu %x %x %d %d %d %d %u %u", &scan, &connected, &buttons, &sx, &sy, &cx, &cy, &l, &r) == 9) {
        struct pad_change *change = &pad_changes[pad_change_count++];

        change->scan = scan;
        change->state.connected = (int)(connected & 1u);
        change->state.buttons = (unsigned short)buttons;
        change->state.stick_x = (signed char)sx;
        change->state.stick_y = (signed char)sy;
        change->state.cstick_x = (signed char)cx;
        change->state.cstick_y = (signed char)cy;
        change->state.trigger_l = (unsigned char)l;
        change->state.trigger_r = (unsigned char)r;
    }
    fclose(file);
    return pad_change_count > 0;
}

/* the Wii launch's pad, scan by scan (one scan a frame, XGetDeviceChanges) */
static unsigned int pad_scan(void)
{
    long index;

    for (index = 0; index < pad_change_count && pad_changes[index].scan <= pad_scans; index++)
        pad_now = pad_changes[index].state;
    if (pad_change_count == 0)
        pad_now.connected = 1;
    pad_scans++;
    return pad_now.connected ? 1u : 0u;
}

static void pad_read(int port, struct wii_pad_state *state)
{
    memset(state, 0, sizeof(*state));
    if (port == 0)
        *state = pad_now;
}

/* the Wii's check without its manifest: the staged file is there and is one
this build reads (the manifest's hashes are the Wii driver's) */
static int map_check(const char *map_name, char *reason, unsigned long reason_size)
{
    const char *name = strrchr(map_name, '\\');
    char path[256];
    unsigned char table[12];
    FILE *file;
    size_t got;

    name = name ? name + 1 : map_name;
    snprintf(path, sizeof path, WII_ENGINE_DATA_ROOT "/maps/%s.wmap", name);
    file = fopen(path, "rb");
    if (!file) {
        snprintf(reason, reason_size, "The map %s is not on the SD card.", name);
        return 1;
    }
    got = fread(table, 1, sizeof table, file);
    fclose(file);
    if (got != sizeof table || memcmp(table, "HWIMAP01", 8)) {
        snprintf(reason, reason_size, "The map %s is not a staged map this build reads.", name);
        return 2;
    }
    return 0;
}

static void finish(int result, const char *reason)
{
    wii_log("UNSUPPORTED names=%lu calls=%lu\n", wii_unsupported_names(), wii_unsupported_calls());
    wii_log("END target=engine_host mode=boot reason=%s pad_scans=%lu result=%s\n", reason, pad_scans,
            result ? "pass" : "fail");
    wii_log_close();
    exit(result ? 0 : 1);
}

int __wrap_main(int argc, char **argv)
{
    struct wii_runtime_start_record start;
    struct wii_arena_report arena;
    struct wii_boot_ui_config config;
    int code;

    if (argc != 8) {
        fprintf(stderr, "usage: boot_menu_host <arena2 low> <arena2 high> <offset> <pad file> <max frames> "
                        "<environment frames> <settle frames>\n");
        return 2;
    }
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
    if (!host_arena_map(strtoul(argv[1], NULL, 0), strtoul(argv[2], NULL, 0)) ||
        !wii_log_open(WII_ENGINE_ROOT "/engine.log"))
        return 2;
    wii_log("BEGIN target=engine_host mode=boot pads=%d\n", pad_load(argv[4]));
    wii_log("BOOT_HOST pad_changes=%ld\n", pad_change_count);
    if (!wii_arena_open(strtoul(argv[3], NULL, 0), GENERAL_BYTES, &arena))
        wii_log("FAIL arena_open\n");
    wii_log("ARENA open offset=%s state=%p tags=%p\n", argv[3], (void *)arena.region_base[_wii_arena_game_state],
            (void *)arena.region_base[_wii_arena_tag_cache]);
    wii_platform_configure();

    wii_xinput_set_pad_source(pad_scan, pad_read);
    memset(&config, 0, sizeof config);
    config.max_frames = strtol(argv[5], NULL, 10);
    config.environment_frames = strtol(argv[6], NULL, 10);
    config.screen_settle_frames = strtol(argv[7], NULL, 10);
    config.overlay = "";
    config.finish = finish;
    config.map_check = map_check;
    wii_render_ui_register(&config);
    wii_boot_set_frame_clock(1);
    real_map_record_allocations(1);
    wii_log("BOOT engine main begins\n");
    code = __real_main();
    wii_log("BOOT_FAIL the engine's main returned %d\n", code);
    finish(0, "engine_main_returned");
    return 1;
}
