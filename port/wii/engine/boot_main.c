/*
 * BOOT_MAIN.C
 *
 * HWI-016D: the boot mode's driver half (libogc). With
 * WII_ENGINE_ROOT/boot.txt on the card, the Wii driver (wii_engine_main.c),
 * after its video, storage, log, arena and platform checks, runs the
 * engine's own main through the original menus instead of the map run:
 *
 * 1. boot.txt: "key=value" lines:
 *      mode=menu                 (required)
 *      max_frames=<n>            the launch ends after n frames (default 5400)
 *      environment_frames=<n>    it ends (pass) n frames after a map other than
 *                                the menus' draws; 0 = stay in the menus until
 *                                max_frames, which then passes (default 150)
 *      settle_frames=<n>         a screen is sampled once it stood n frames (45)
 *      texture_budget=<bytes>    the interface's resident HWT1 bytes (3 MiB)
 *      clock=frame|real          the frame clock (boot_hooks.h; default frame)
 *      error_frames=<n>          a boot error screen stays n frames (240)
 *      expect=refusal            a load-failure test: max_frames passes when a map
 *                                was refused and the menus' map is loaded again
 * 2. manifest.txt (written by tools/wii/boot_stage.py on the host): the
 *    source identity, the converter generation, and every staged map with its
 *    bytes and SHA-256. Each map file on the card is hashed and compared; the
 *    results go to the log and an on-screen line names the identities.
 * 3. The UI map must be on the card and match the manifest: otherwise an
 *    actionable error screen (what is missing or damaged, how to stage it),
 *    drawn with the built-in font, and the launch ends without starting the
 *    engine. Nothing on the card is written but the log and the counters.
 * 4. The engine's own main (__real_main): shell start-up, then main_loop, with
 *    the Wii rasterizer's boot hooks (render_ui.c), XInput on the GameCube pad
 *    (wii_xinput.c) and the frame clock. A map the menus ask for is checked
 *    against the card and the manifest before the engine commits to it
 *    (boot_hooks.h); a refusal is the engine's own error dialog.
 * 5. The engine side ends the launch (render_ui.c's finish): the result, the
 *    totals, boot_result.txt (compared at the next launch), then exit.
 */
#include <gccore.h>
#include <ogc/lwp_watchdog.h>
#include <errno.h>
#include <inttypes.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "build_id.h"
#include "wii_platform.h"
#include "wii_input.h"
#include "boot_hooks.h"
#include "boot_main.h"
#include "real_map_scenario.h"
#include "wii_render.h"
#include "wii_render_ui.h"
#include "content_common.h"

#define BOOT_TXT WII_ENGINE_ROOT "/boot.txt"
#define BOOT_MANIFEST WII_ENGINE_ROOT "/manifest.txt"
#define BOOT_RESULT WII_ENGINE_ROOT "/boot_result.txt"
#define BOOT_MAPS WII_ENGINE_DATA_ROOT "/maps"
#define BOOT_MAX_MANIFEST_FILES 16
#define BOOT_MANIFEST_BYTES 4096

struct boot_manifest_file {
    char name[32];        /* the map's name (<name>.wmap) */
    unsigned long bytes;
    char sha256[65];
    int present, verified;
    char found_sha256[65];
};

static struct {
    /* boot.txt */
    int menu;
    long max_frames, environment_frames, settle_frames, error_frames;
    unsigned long texture_budget;
    int frame_clock;
    int expect_refusal;
    /* manifest.txt */
    char source[40], generation[40], stager[40];
    struct boot_manifest_file files[BOOT_MAX_MANIFEST_FILES];
    int file_count;
    int manifest_read;
    char overlay[160];
    /* the run */
    const struct wii_boot_driver *driver;
    u64 started;
    unsigned long pad_scans;
    struct wii_pad_state pads[WII_PAD_PORTS];
    struct wii_pad_state last_logged, last_logged2;
    char previous_result[96];
    int have_previous;
} boot;

/* ---------- configuration */

int wii_boot_requested(void)
{
    struct stat information;
    return stat(BOOT_TXT, &information) == 0;
}

static void read_config(void)
{
    char text[1024] = "";
    FILE *file = fopen(BOOT_TXT, "rb");
    boot.max_frames = 5400;
    boot.environment_frames = 150;
    boot.settle_frames = 45;
    boot.error_frames = 240;
    boot.texture_budget = WII_UI_DEFAULT_TEXTURE_BUDGET;
    boot.frame_clock = 1;
    if (!file)
        return;
    size_t used = fread(text, 1, sizeof(text) - 1, file);
    fclose(file);
    text[used] = 0;
    for (char *line = strtok(text, "\r\n"); line; line = strtok(NULL, "\r\n")) {
        if (!strcmp(line, "mode=menu"))
            boot.menu = 1;
        else if (!strncmp(line, "max_frames=", 11))
            boot.max_frames = strtol(line + 11, NULL, 10);
        else if (!strncmp(line, "environment_frames=", 19))
            boot.environment_frames = strtol(line + 19, NULL, 10);
        else if (!strncmp(line, "settle_frames=", 14))
            boot.settle_frames = strtol(line + 14, NULL, 10);
        else if (!strncmp(line, "error_frames=", 13))
            boot.error_frames = strtol(line + 13, NULL, 10);
        else if (!strncmp(line, "texture_budget=", 15))
            boot.texture_budget = strtoul(line + 15, NULL, 10);
        else if (!strcmp(line, "expect=refusal"))
            boot.expect_refusal = 1;
        else if (!strcmp(line, "clock=real"))
            boot.frame_clock = 0;
        else if (!strcmp(line, "clock=frame"))
            boot.frame_clock = 1;
    }
}

/* ---------- the manifest */

static void hex_of(const unsigned char digest[32], char out[65])
{
    for (int i = 0; i < 32; ++i)
        snprintf(out + i * 2, 3, "%02x", digest[i]);
    out[64] = 0;
}

/* the SHA-256 of a file on the card, streamed; 0 if it cannot be read */
static int file_sha256(const char *path, unsigned long *bytes, char out[65])
{
    static unsigned char chunk[64 * 1024] __attribute__((aligned(32)));
    struct content_sha256 context;
    unsigned char digest[32];
    FILE *file = fopen(path, "rb");
    size_t got;
    if (!file)
        return 0;
    content_sha256_init(&context);
    *bytes = 0;
    while ((got = fread(chunk, 1, sizeof(chunk), file)) > 0) {
        content_sha256_update(&context, chunk, got);
        *bytes += (unsigned long)got;
    }
    int ok = !ferror(file);
    fclose(file);
    content_sha256_final(&context, digest);
    hex_of(digest, out);
    return ok;
}

static void read_manifest(void)
{
    char text[BOOT_MANIFEST_BYTES] = "";
    FILE *file = fopen(BOOT_MANIFEST, "rb");
    if (!file) {
        wii_log("BOOT_MANIFEST present=0 (no manifest.txt: the content is not identified)\n");
        return;
    }
    size_t used = fread(text, 1, sizeof(text) - 1, file);
    fclose(file);
    text[used] = 0;
    boot.manifest_read = 1;
    for (char *line = strtok(text, "\r\n"); line; line = strtok(NULL, "\r\n")) {
        char name[32], sha[65];
        unsigned long bytes;
        if (!strncmp(line, "source ", 7)) {
            sscanf(line + 7, "%39s", boot.source);
            continue;
        }
        if (!strncmp(line, "generation ", 11)) {
            sscanf(line + 11, "%39s", boot.generation);
            continue;
        }
        if (!strncmp(line, "stager ", 7)) {
            sscanf(line + 7, "%39s", boot.stager);
            continue;
        }
        if (sscanf(line, "map %31s %lu %64s", name, &bytes, sha) == 3 && boot.file_count < BOOT_MAX_MANIFEST_FILES &&
            strlen(sha) == 64) {
            struct boot_manifest_file *f = &boot.files[boot.file_count++];
            snprintf(f->name, sizeof(f->name), "%s", name);
            f->bytes = bytes;
            snprintf(f->sha256, sizeof(f->sha256), "%s", sha);
        }
    }
}

static struct boot_manifest_file *manifest_file(const char *name)
{
    for (int i = 0; i < boot.file_count; ++i)
        if (!strcmp(boot.files[i].name, name))
            return &boot.files[i];
    return NULL;
}

static void verify_manifest(void)
{
    u64 started = gettime();
    for (int i = 0; i < boot.file_count; ++i) {
        struct boot_manifest_file *f = &boot.files[i];
        char path[160];
        unsigned long bytes = 0;
        snprintf(path, sizeof(path), BOOT_MAPS "/%s.wmap", f->name);
        f->present = file_sha256(path, &bytes, f->found_sha256);
        f->verified = f->present && bytes == f->bytes && !strcmp(f->found_sha256, f->sha256);
        wii_log("BOOT_MANIFEST map=%s present=%d bytes=%lu expected_bytes=%lu sha256=%.16s expected=%.16s "
                "verified=%d\n",
                f->name, f->present, f->present ? bytes : 0ul, f->bytes, f->present ? f->found_sha256 : "-",
                f->sha256, f->verified);
    }
    wii_log("BOOT_MANIFEST source=%s generation=%s stager=%s maps=%d hash_ms=%lu\n", boot.source[0] ? boot.source : "-",
            boot.generation[0] ? boot.generation : "-", boot.stager[0] ? boot.stager : "-", boot.file_count,
            (unsigned long)ticks_to_millisecs(gettime() - started));
    /* the on-screen line: the build, the source and converter identities and
    each map's staged hash */
    int at = snprintf(boot.overlay, sizeof(boot.overlay), "BUILD %.8s SRC %.8s GEN %.16s", WII_BUILD_ID,
                      boot.source[0] ? boot.source : "-", boot.generation[0] ? boot.generation : "-");
    for (int i = 0; i < boot.file_count && at > 0 && at < (int)sizeof(boot.overlay) - 24; ++i)
        at += snprintf(boot.overlay + at, sizeof(boot.overlay) - (size_t)at, " %s %.8s%s", boot.files[i].name,
                       boot.files[i].sha256, boot.files[i].verified ? "" : "!");
    for (char *p = boot.overlay; *p; ++p)
        if (*p >= 'a' && *p <= 'z')
            *p = (char)(*p - 'a' + 'A');
}

/* ---------- the maps the menus ask for (boot_hooks.h) */

/* 0 if the map is on the card, is a staged map, and matches the manifest
when the manifest names it */
static int map_check(const char *map_name, char *reason, unsigned long reason_size)
{
    const char *name = strrchr(map_name, '\\');
    char path[160];
    unsigned char table[16];
    name = name ? name + 1 : map_name;
    snprintf(path, sizeof(path), BOOT_MAPS "/%s.wmap", name);
    FILE *file = fopen(path, "rb");
    if (!file) {
        snprintf(reason, reason_size,
                 "The map %s is not on the SD card. Stage it from your own copy with tools/wii/map_stage.py "
                 "and copy %s.wmap to sd:/halo-wii-engine/data/maps/.",
                 name, name);
        return 1;
    }
    size_t got = fread(table, 1, sizeof(table), file);
    fclose(file);
    if (got != sizeof(table) || memcmp(table, "HWIMAP01", 8) ||
        !(table[8] == 1 && table[9] == 2 && table[10] == 3 && table[11] == 4)) {
        snprintf(reason, reason_size,
                 "The map %s on the SD card is not a staged map this build reads. Stage it again with "
                 "tools/wii/map_stage.py; nothing was loaded.",
                 name);
        return 2;
    }
    struct boot_manifest_file *f = manifest_file(name);
    if (f && !f->verified) {
        snprintf(reason, reason_size,
                 "The map %s on the SD card does not match its manifest (SHA-256): it is damaged or out of "
                 "date. Stage it again with tools/wii/map_stage.py; nothing was loaded.",
                 name);
        return 3;
    }
    wii_log("BOOT_MAP_CHECK map=%s ok=1 manifest=%s\n", name, f ? "verified" : "not_listed");
    return 0;
}

/* ---------- the pad (wii_input.h) */

static unsigned int pad_scan(void)
{
    PADStatus status[PAD_CHANMAX];
    unsigned int connected = 0;
    unsigned int reset = 0;
    PAD_Read(status);
    for (int port = 0; port < WII_PAD_PORTS; ++port) {
        struct wii_pad_state *pad = &boot.pads[port];
        if (status[port].err == PAD_ERR_NO_CONTROLLER) {
            /* gone: looked for again (PAD_Reset), reported removed */
            memset(pad, 0, sizeof(*pad));
            reset |= PAD_CHAN0_BIT >> port;
            continue;
        }
        if (status[port].err != PAD_ERR_NONE) {
            /* a transfer not finished this time: the port stays as it was
            (PAD_ScanPads reported it gone, and the engine saw an unplug) */
            if (pad->connected)
                connected |= 1u << port;
            continue;
        }
        connected |= 1u << port;
        pad->connected = 1;
        pad->buttons = status[port].button;
        pad->stick_x = status[port].stickX;
        pad->stick_y = status[port].stickY;
        pad->cstick_x = status[port].substickX;
        pad->cstick_y = status[port].substickY;
        pad->trigger_l = status[port].triggerL;
        pad->trigger_r = status[port].triggerR;
    }
    if (reset)
        PAD_Reset(reset);
    if (memcmp(&boot.pads[0], &boot.last_logged, sizeof(boot.last_logged))) {
        wii_log("BOOT_PAD scan=%lu clock_ms=%lu connected=%x buttons=%04x stick=%d,%d cstick=%d,%d triggers=%u,%u\n",
                boot.pad_scans, wii_boot_clock_milliseconds(), connected, boot.pads[0].buttons, boot.pads[0].stick_x,
                boot.pads[0].stick_y, boot.pads[0].cstick_x, boot.pads[0].cstick_y, boot.pads[0].trigger_l,
                boot.pads[0].trigger_r);
        boot.last_logged = boot.pads[0];
    }
    /* (port 2: a second player, split screen) */
    if (memcmp(&boot.pads[1], &boot.last_logged2, sizeof(boot.last_logged2))) {
        wii_log("BOOT_PAD2 scan=%lu clock_ms=%lu connected=%x buttons=%04x stick=%d,%d cstick=%d,%d "
                "triggers=%u,%u\n",
                boot.pad_scans, wii_boot_clock_milliseconds(), connected, boot.pads[1].buttons, boot.pads[1].stick_x,
                boot.pads[1].stick_y, boot.pads[1].cstick_x, boot.pads[1].cstick_y, boot.pads[1].trigger_l,
                boot.pads[1].trigger_r);
        boot.last_logged2 = boot.pads[1];
    }
    boot.pad_scans++;
    return connected;
}

static void pad_read(int port, struct wii_pad_state *state)
{
    if (port >= 0 && port < WII_PAD_PORTS)
        *state = boot.pads[port];
}

/* ---------- the end */

static void write_result(const char *result)
{
    FILE *file = fopen(BOOT_RESULT, "w");
    if (!file)
        return;
    fprintf(file, "%s build=%s\n", result, WII_BUILD_ID);
    fclose(file);
}

static void read_previous_result(void)
{
    FILE *file = fopen(BOOT_RESULT, "rb");
    if (!file)
        return;
    size_t used = fread(boot.previous_result, 1, sizeof(boot.previous_result) - 1, file);
    fclose(file);
    boot.previous_result[used] = 0;
    char *newline = strchr(boot.previous_result, '\n');
    if (newline)
        *newline = 0;
    boot.have_previous = 1;
}

/* (the launch's start, apart from the boot state) */
static unsigned long long boot_started_us;

static void boot_end(int result, const char *reason) __attribute__((noreturn));

static void boot_end(int result, const char *reason)
{
    struct wii_xinput_report input;
    char line[96];
    wii_xinput_report_get(&input);
    wii_log("BOOT_INPUT scans=%lu reads=%lu packets=%lu last_buttons=%04x\n", input.scans, input.reads, input.packets,
            input.last_xbox_buttons);
    wii_log("UNSUPPORTED names=%lu calls=%lu\n", wii_unsupported_names(), wii_unsupported_calls());
    for (unsigned long index = 0; index < wii_unsupported_names(); index++)
        wii_log("UNSUPPORTED_NAME %s\n", wii_unsupported_name(index));
    snprintf(line, sizeof(line), "boot=%s reason=%s", result ? "pass" : "fail", reason);
    wii_log("PERSIST boot previous=%d previous_result=\"%s\" same_as_previous=%d\n", boot.have_previous,
            boot.have_previous ? boot.previous_result : "", boot.have_previous &&
            !strncmp(boot.previous_result, line, strlen(line)));
    write_result(line);
    wii_log("END target=engine mode=boot build=%s reason=%s elapsed_ms=%llu result=%s\n", WII_BUILD_ID, reason,
            (wii_time_microseconds() - boot_started_us) / 1000ull, result && wii_log_healthy() ? "pass" : "fail");
    wii_log_close();
    exit(result ? 0 : 1);
}

static void engine_finish(int result, const char *reason)
{
    boot_end(result, reason);
}

/* ---------- a boot error, before the engine starts */

static void boot_error_screen(const char *title, const char *lines[], int line_count, const char *reason)
{
    unsigned long crc = 0;
    int covered = 0;
    wii_log("BOOT_ERROR reason=%s title=\"%s\"\n", reason, title);
    for (int i = 0; i < line_count; ++i)
        wii_log("BOOT_ERROR_LINE %d \"%s\"\n", i, lines[i]);
    for (long frame = 0; frame < boot.error_frames; ++frame) {
        wii_ui_gx_window(0, 0, 640, 480);
        wii_ui_gx_fill(40, 120, 600, 160 + 16 * line_count + 40, 0xE0300000u);
        wii_ui_gx_text(56, 136, 2, 0xFFFFFFFFu, title);
        for (int i = 0; i < line_count; ++i)
            wii_ui_gx_text(56, 170 + 16 * i, 1, 0xFFFFE0A0u, lines[i]);
        wii_ui_gx_fill(0, 462, 640, 480, 0xA0000000u);
        wii_ui_gx_text(4, 465, 1, 0xFFC8C8C8u, boot.overlay);
        if (frame == 30) {
            crc = wii_ui_gx_sample_crc(&covered);
            wii_log("BOOT_ERROR_SCREEN reason=%s efb_crc=%08lx covered=%d frame=%ld\n", reason, crc, covered, frame);
        }
        wii_ui_gx_present();
    }
    /* the error was shown as expected: the launch passes as a load-failure
    test, the engine never started and the card is untouched */
    boot_end(1, reason);
}

static int check_ui_map(void)
{
    char reason[256];
    int code = map_check("levels\\ui\\ui", reason, sizeof(reason));
    if (!code)
        return 1;
    wii_log("BOOT_UI_MAP check=%d reason=\"%s\"\n", code, reason);
    if (code == 1) {
        const char *lines[] = {
            "THE MENUS NEED THE UI MAP, WHICH IS NOT ON THE SD CARD:",
            "  SD:/HALO-WII-ENGINE/DATA/MAPS/UI.WMAP",
            "STAGE IT FROM YOUR OWN COPY OF THE GAME ON A PC:",
            "  PYTHON TOOLS/WII/MAP_STAGE.PY --MAP <YOUR UI.MAP> --OUTPUT <DIR>",
            "  (OR TOOLS/WII/BOOT_STAGE.PY), THEN COPY UI.WMAP THERE.",
            "NOTHING ON THE CARD WAS CHANGED. THE GAME DID NOT START.",
        };
        boot_error_screen("UI MAP MISSING", lines, 6, "ui_map_missing");
    } else {
        const char *lines[] = {
            "THE UI MAP ON THE SD CARD IS DAMAGED OR OUT OF DATE:",
            "  SD:/HALO-WII-ENGINE/DATA/MAPS/UI.WMAP",
            code == 2 ? "  IT IS NOT A STAGED MAP THIS BUILD READS." :
                        "  ITS SHA-256 DIFFERS FROM MANIFEST.TXT.",
            "STAGE IT AGAIN FROM YOUR OWN COPY ON A PC:",
            "  PYTHON TOOLS/WII/BOOT_STAGE.PY ... AND COPY IT AND MANIFEST.TXT.",
            "NOTHING WAS LOADED; THE CARD WAS NOT CHANGED.",
        };
        boot_error_screen("UI MAP DAMAGED", lines, 6, code == 2 ? "ui_map_not_staged_format" : "ui_map_corrupt");
    }
    return 0;
}

/* ---------- the run */

int wii_boot_run(const struct wii_boot_driver *driver)
{
    struct wii_boot_ui_config config;
    int code;

    memset(&boot, 0, sizeof(boot));
    boot.driver = driver;
    boot.started = gettime();
    boot_started_us = wii_time_microseconds();
    read_config();
    read_previous_result();
    wii_log("BOOT begin mode=%s max_frames=%ld environment_frames=%ld settle_frames=%ld texture_budget=%lu clock=%s "
            "previous_result=\"%s\"\n",
            boot.menu ? "menu" : "unknown", boot.max_frames, boot.environment_frames, boot.settle_frames,
            boot.texture_budget, boot.frame_clock ? "frame" : "real", boot.have_previous ? boot.previous_result : "");
    wii_ui_gx_configure(&driver->video, boot.texture_budget);
    read_manifest();
    verify_manifest();
    if (!boot.menu) {
        wii_log("BOOT_FAIL boot.txt has no mode=menu\n");
        return 0;
    }
    if (!check_ui_map())
        return 0;

    PAD_Init();
    wii_xinput_set_pad_source(pad_scan, pad_read);
    memset(&config, 0, sizeof(config));
    config.texture_budget = boot.texture_budget;
    config.max_frames = boot.max_frames;
    config.environment_frames = boot.environment_frames;
    config.screen_settle_frames = boot.settle_frames;
    config.expect_refusal = boot.expect_refusal;
    config.overlay = boot.overlay;
    config.finish = engine_finish;
    config.map_check = map_check;
    wii_render_ui_register(&config);
    wii_boot_set_frame_clock(boot.frame_clock);
    /* the game state's allocations, recorded as the engine's own start-up
    makes them, for the screens' digests */
    real_map_record_allocations(1);
    wii_log("BOOT engine main begins\n");
    code = driver->real_main();
    wii_log("BOOT_FAIL the engine's main returned %d (its start-up failed)\n", code);
    boot_end(0, "engine_main_returned");
    return 0;
}
