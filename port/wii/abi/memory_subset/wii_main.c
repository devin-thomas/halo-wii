/* Dedicated asset-free diagnostic; no production engine boundary changes. */
#include <gccore.h>
#include <fat.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "fixture.h"
#include "subset_build_id.h"
#ifdef WII_MEMORY_CANDIDATE
#include "candidate.h"
#endif
#ifdef WII_MEMORY_PACKETS
#include "packet_fixture.h"
#endif
#ifdef WII_MEMORY_PACKET_POLICY
#include "packet_version_edges.h"
#endif
#ifdef WII_MEMORY_PACKET_VERIFIER
#include "packet_verifier_fixture.h"
#endif
#ifdef WII_MEMORY_PACKET_ARRAYS
#include "packet_array_fixture.h"
#endif
#ifdef WII_MEMORY_PACKET_GROUPS
#include "packet_group_fixture.h"
#endif
#ifdef WII_MEMORY_PACKET_CALLERS
#include "packet_caller_fixture.h"
#endif

int main(void)
{
    VIDEO_Init();
    GXRModeObj *mode = VIDEO_GetPreferredMode(NULL);
    if (mode == NULL) {
        fprintf(stderr, "MEMORY video mode unavailable\n");
        return 2;
    }
    void *framebuffer = SYS_AllocateFramebuffer(mode);
    if (framebuffer == NULL) {
        fprintf(stderr, "MEMORY framebuffer allocation failed\n");
        return 2;
    }
    framebuffer = MEM_K0_TO_K1(framebuffer);
    console_init(framebuffer, 20, 20, mode->fbWidth, mode->xfbHeight,
                 mode->fbWidth * VI_DISPLAY_PIX_SZ);
    VIDEO_Configure(mode);
    VIDEO_SetNextFramebuffer(framebuffer);
    VIDEO_SetBlack(false);
    VIDEO_Flush();
    VIDEO_WaitVSync();
    if (mode->viTVMode & VI_NON_INTERLACE) VIDEO_WaitVSync();
    printf("Asset-free scalar engine subset\nBuild: %s\n", WII_MEMORY_BUILD_ID);
    if (!fatInitDefault()) {
        fprintf(stderr, "MEMORY SD unavailable; no persisted result\n");
        return 2;
    }
    if (mkdir("sd:/halo-wii-memory", 0777) != 0 && errno != EEXIST) {
        fprintf(stderr, "MEMORY directory failed: %s\n", strerror(errno));
        return 2;
    }
#if defined(WII_MEMORY_PACKET_CALLERS)
    FILE *report = fopen("sd:/halo-wii-memory/callers.log", "a");
#elif defined(WII_MEMORY_PACKET_GROUPS)
    FILE *report = fopen("sd:/halo-wii-memory/groups.log", "a");
#elif defined(WII_MEMORY_PACKET_ARRAYS)
    FILE *report = fopen("sd:/halo-wii-memory/arrays.log", "a");
#elif defined(WII_MEMORY_PACKET_VERIFIER)
    FILE *report = fopen("sd:/halo-wii-memory/verifier.log", "a");
#elif defined(WII_MEMORY_PACKET_POLICY)
    FILE *report = fopen("sd:/halo-wii-memory/version.log", "a");
#elif defined(WII_MEMORY_PACKETS)
    FILE *report = fopen("sd:/halo-wii-memory/packets.log", "a");
#elif defined(WII_MEMORY_CANDIDATE)
    FILE *report = fopen("sd:/halo-wii-memory/compare.log", "a");
#else
    FILE *report = fopen("sd:/halo-wii-memory/subset.log", "a");
#endif
    if (report == NULL) {
        fprintf(stderr, "MEMORY log open failed: %s\n", strerror(errno));
        return 2;
    }
    if (fprintf(report, "BEGIN MEMORY build=%s\n", WII_MEMORY_BUILD_ID) < 0 ||
        fflush(report) != 0) {
        fclose(report);
        return 2;
    }
    /* Continue independent valid-input checks to expose all endian mismatches. */
#if defined(WII_MEMORY_PACKET_VERIFIER)
    if (fprintf(report, "SCALAR REFERENCE BEGIN\n") < 0 || fflush(report) != 0) {
        fclose(report); return 2;
    }
    int scalar_reference = wii_memory_subset(report, 1);
    if (fprintf(report, "SCALAR REFERENCE END result=%d\nSCALAR CANDIDATE BEGIN\n", scalar_reference) < 0 ||
        fflush(report) != 0) { fclose(report); return 2; }
    int scalar_candidate = wii_memory_candidate_subset(report, 1);
    int scalar_edges = wii_memory_candidate_edges(report, 1);
    if (fprintf(report, "SCALAR CANDIDATE END result=%d\n", scalar_candidate || scalar_edges) < 0 ||
        fflush(report) != 0) { fclose(report); return 2; }
    int original = wii_packet_compare(report, 1);
    int policy = wii_packet_policy_subset(report, 1);
    int edges = wii_packet_version_edges(report, 1);
    int verifier = wii_packet_verifier_fixture(report, 1);
    int arrays = 0;
    int groups = 0;
    int callers = 0;
#ifdef WII_MEMORY_PACKET_ARRAYS
    arrays = wii_packet_array_fixture(report, 1);
#endif
#ifdef WII_MEMORY_PACKET_GROUPS
    groups = wii_packet_group_fixture(report, 1);
#endif
#ifdef WII_MEMORY_PACKET_CALLERS
    callers = wii_packet_caller_fixture(report, 1);
#endif
    int result = scalar_reference || scalar_candidate || scalar_edges || original || policy || edges || verifier || arrays || groups || callers;
#elif defined(WII_MEMORY_PACKET_POLICY)
    int original = wii_packet_compare(report, 1);
    int policy = wii_packet_policy_subset(report, 1);
    int edges = wii_packet_version_edges(report, 1);
    int result = original || policy || edges;
#elif defined(WII_MEMORY_PACKETS)
    int result = wii_packet_compare(report, 1);
#elif defined(WII_MEMORY_CANDIDATE)
    if (fprintf(report, "REFERENCE BEGIN\n") < 0 || fflush(report) != 0) {
        fclose(report); return 2;
    }
    int reference = wii_memory_subset(report, 1);
    if (fprintf(report, "REFERENCE END result=%d\nCANDIDATE BEGIN\n", reference) < 0 ||
        fflush(report) != 0) { fclose(report); return 2; }
    int candidate = wii_memory_candidate_subset(report, 1);
    int edges = wii_memory_candidate_edges(report, 1);
    int result = candidate || edges;
    if (fprintf(report, "CANDIDATE END result=%d\n", result) < 0 || fflush(report) != 0) {
        fclose(report); return 2;
    }
#else
    int result = wii_memory_subset(report, 1);
#endif
    int write_result = fprintf(report, "END MEMORY build=%s result=%d\n", WII_MEMORY_BUILD_ID, result);
    int flush_result = fflush(report);
    int close_result = fclose(report);
    if (write_result < 0 || flush_result != 0 || close_result != 0) {
        fprintf(stderr, "MEMORY log completion failed\n");
        return 2;
    }
    printf("Subset result: %d; SD report complete\n", result);
    for (unsigned int i = 0; i < 60; ++i) VIDEO_WaitVSync();
    return result;
}
