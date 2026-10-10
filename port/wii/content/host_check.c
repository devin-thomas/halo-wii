/* Host build of the content loader checks (HWI-008C): the same loaders and
 * case runner as the Wii self-test, without GX. Used by
 * tools/wii/test_content_loaders.py on authored fixtures and to pre-check a
 * private sample before a Dolphin run.
 *
 * usage: host_check <cases.txt> <path prefix> */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "content_check.h"
#include "hra_animation.h"

static uint64_t host_clock(void) { return (uint64_t)clock(); }

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: host_check <cases.txt> <path prefix>\n");
        return 2;
    }
    static struct content_case cases[CONTENT_CHECK_MAX_CASES];
    int count = content_read_cases(argv[1], argv[2], cases, CONTENT_CHECK_MAX_CASES);
    if (count <= 0) {
        printf("CASES ok=0\n");
        return 1;
    }
    uint32_t scratch_bytes = HRA_MAX_STREAM_BYTES + 64u * 1024u;
    unsigned char *scratch = malloc(scratch_bytes);
    if (scratch == NULL)
        return 1;
    unsigned passed = 0;
    for (int i = 0; i < count; ++i) {
        struct content_outcome o;
        content_run_case(&cases[i], scratch, scratch_bytes, NULL, host_clock, &o);
        printf("CASE id=%s kind=%s expect=%s got=%s file_sha=%d decoded_match=%d units=%u decoded=%s result=%s\n",
               cases[i].id, content_kind_name(cases[i].kind), content_error_name(cases[i].expect),
               content_error_name(o.error), o.file_ok, o.decoded_match, (unsigned)o.units, o.decoded,
               o.pass ? "pass" : "fail");
        passed += o.pass ? 1u : 0u;
    }
    free(scratch);
    printf("SUMMARY cases=%d passed=%u\n", count, passed);
    return passed == (unsigned)count ? 0 : 1;
}
