/* HWI-006 semantics fixture entry: stdout on the host, an SD report on the Wii. */
#include "abi_semantics_build.h"
#include "engine_probe.h"
#include "fpenv.h"
#include "math_corpus.h"
#include "report.h"
#include "runtime_start.h"
#include "semantics.h"
#include <stdio.h>

#define REPORT_KIND "ABI_SEMANTICS"
#define REPORT_PATH "sd:/halo-wii-memory/abi-semantics.log"

#ifdef GEKKO
#include <errno.h>
#include <fat.h>
#include <gccore.h>
#include <sys/stat.h>
#endif

unsigned long wii_abi_engine_assertions;

int main(void)
{
    /* ADR-018: the shared Wii runtime start runs before any engine code */
    struct wii_runtime_start_record start;
    const int ieee = wii_runtime_start(&start);
    FILE *out = stdout;
#ifdef GEKKO
    VIDEO_Init();
    GXRModeObj *mode = VIDEO_GetPreferredMode(NULL);
    if (mode == NULL) return 2;
    void *framebuffer = SYS_AllocateFramebuffer(mode);
    if (framebuffer == NULL) return 2;
    framebuffer = MEM_K0_TO_K1(framebuffer);
    console_init(framebuffer, 20, 20, mode->fbWidth, mode->xfbHeight, mode->fbWidth * VI_DISPLAY_PIX_SZ);
    VIDEO_Configure(mode); VIDEO_SetNextFramebuffer(framebuffer); VIDEO_SetBlack(false); VIDEO_Flush();
    VIDEO_WaitVSync(); if (mode->viTVMode & VI_NON_INTERLACE) VIDEO_WaitVSync();
    if (!fatInitDefault()) { printf("SD unavailable\n"); return 2; }
    if (mkdir("sd:/halo-wii-memory", 0777) != 0 && errno != EEXIST) return 2;
    out = fopen(REPORT_PATH, "w");
    if (out == NULL) return 2;
#endif
    struct wii_abi_report report = {out, 0, 0, 0, 0, 0, 0, 0, NULL, NULL};
    if (fprintf(out, "BEGIN %s build=%s\n", REPORT_KIND, WII_ABI_SEMANTICS_BUILD_ID) < 0 || fflush(out)) return 2;
    const uint32_t flush_mask = wii_abi_fp_flush_mask();
    /* FPSCR on the Wii (libogc starts it with NI set), 0 on the host */
    wii_abi_observe_u64(&report, "runtime.fp_control_before_start", start.fp_control_before);
    wii_abi_observe_u64(&report, "runtime.fp_control_after_start", start.fp_control_after);
    wii_abi_check_u64(&report, "runtime.ieee_after_start", WII_ABI_CONTRACT, ieee, 1);
    wii_abi_check_u64(&report, "runtime.flush_bits_at_report", WII_ABI_CONTRACT, wii_abi_fp_control() & flush_mask, 0);
    wii_abi_semantics(&report);
    wii_abi_engine_probe(&report);
    wii_abi_math_corpus(&report);
    wii_abi_check_u64(&report, "runtime.flush_bits_at_end", WII_ABI_CONTRACT, wii_abi_fp_control() & flush_mask, 0);
    int result = report.contract_failures != 0 || report.candidate_failures != 0;
    fprintf(out, "%s SUMMARY checks=%lu failures=%lu contract_failures=%lu engine_assumption_failures=%lu "
            "candidate_failures=%lu observations=%lu math=%lu\n", REPORT_KIND, report.checks, report.failures,
            report.contract_failures, report.assumption_failures, report.candidate_failures, report.observations,
            report.math_lines);
    if (fprintf(out, "END %s build=%s result=%d\n", REPORT_KIND, WII_ABI_SEMANTICS_BUILD_ID, result) < 0 ||
        fflush(out) || ferror(out)) return 2;
#ifdef GEKKO
    if (fclose(out)) return 2;
    printf("ABI semantics: %d\n", result);
    for (unsigned i = 0; i < 60; ++i) VIDEO_WaitVSync();
#endif
    return result;
}
