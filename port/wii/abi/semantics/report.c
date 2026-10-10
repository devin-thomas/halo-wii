#include "report.h"
#include <inttypes.h>
#include <string.h>

_Static_assert(sizeof(float) == 4 && sizeof(double) == 8, "IEEE binary32/binary64 storage required");

static const char *effective(const struct wii_abi_report *report, const char *check_class)
{
    return report->contract_as != NULL && strcmp(check_class, WII_ABI_CONTRACT) == 0 ? report->contract_as
                                                                                      : check_class;
}

static const char *prefix(const struct wii_abi_report *report)
{
    return report->prefix != NULL ? report->prefix : "";
}

static void tally(struct wii_abi_report *report, const char *check_class, int failed)
{
    report->checks++;
    if (!failed) return;
    report->failures++;
    if (strcmp(check_class, WII_ABI_CONTRACT) == 0) report->contract_failures++;
    else if (strcmp(check_class, WII_ABI_ASSUMPTION) == 0) report->assumption_failures++;
    else report->candidate_failures++;
}

void wii_abi_check_u64(struct wii_abi_report *report, const char *id, const char *check_class,
                       uint64_t observed, uint64_t reference)
{
    int failed = observed != reference;
    check_class = effective(report, check_class);
    tally(report, check_class, failed);
    fprintf(report->out, "CHECK %s%s class=%s observed=0x%" PRIx64 " reference=0x%" PRIx64 " result=%s\n",
            prefix(report), id, check_class, observed, reference, failed ? "fail" : "pass");
}

void wii_abi_check_i64(struct wii_abi_report *report, const char *id, const char *check_class,
                       int64_t observed, int64_t reference)
{
    int failed = observed != reference;
    check_class = effective(report, check_class);
    tally(report, check_class, failed);
    fprintf(report->out, "CHECK %s%s class=%s observed=%" PRId64 " reference=%" PRId64 " result=%s\n",
            prefix(report), id, check_class, observed, reference, failed ? "fail" : "pass");
}

void wii_abi_check_text(struct wii_abi_report *report, const char *id, const char *check_class,
                        const char *observed, const char *reference)
{
    int failed = strcmp(observed, reference) != 0;
    check_class = effective(report, check_class);
    tally(report, check_class, failed);
    fprintf(report->out, "CHECK %s%s class=%s observed=\"%s\" reference=\"%s\" result=%s\n",
            prefix(report), id, check_class, observed, reference, failed ? "fail" : "pass");
}

static void hex(FILE *out, const uint8_t *bytes, size_t length)
{
    for (size_t i = 0; i < length; ++i) fprintf(out, "%02x", bytes[i]);
}

void wii_abi_check_bytes(struct wii_abi_report *report, const char *id, const char *check_class,
                         const uint8_t *observed, const uint8_t *reference, size_t length)
{
    int failed = memcmp(observed, reference, length) != 0;
    check_class = effective(report, check_class);
    tally(report, check_class, failed);
    fprintf(report->out, "CHECK %s%s class=%s observed=", prefix(report), id, check_class);
    hex(report->out, observed, length);
    fprintf(report->out, " reference=");
    hex(report->out, reference, length);
    fprintf(report->out, " result=%s\n", failed ? "fail" : "pass");
}

void wii_abi_observe_u64(struct wii_abi_report *report, const char *id, uint64_t value)
{
    report->observations++;
    fprintf(report->out, "OBS %s%s value=0x%" PRIx64 "\n", prefix(report), id, value);
}

void wii_abi_observe_bytes(struct wii_abi_report *report, const char *id, const uint8_t *bytes, size_t length)
{
    report->observations++;
    fprintf(report->out, "OBS %s%s value=", prefix(report), id);
    hex(report->out, bytes, length);
    fputc('\n', report->out);
}

#define FNV64_OFFSET UINT64_C(0xcbf29ce484222325)
#define FNV64_PRIME UINT64_C(0x100000001b3)
#define FNV32_OFFSET UINT32_C(0x811c9dc5)
#define FNV32_PRIME UINT32_C(0x01000193)

static uint64_t fnv64(uint64_t hash, uint64_t value, unsigned bytes)
{
    for (unsigned i = 0; i < bytes; ++i) {
        hash ^= (value >> (8 * i)) & 0xffu;
        hash *= FNV64_PRIME;
    }
    return hash;
}

static uint32_t fnv32(uint32_t hash, uint64_t value, unsigned bytes)
{
    for (unsigned i = 0; i < bytes; ++i) {
        hash ^= (uint32_t)((value >> (8 * i)) & 0xffu);
        hash *= FNV32_PRIME;
    }
    return hash;
}

void wii_abi_digest_begin(struct wii_abi_digest *digest, unsigned long expected_count)
{
    digest->all = FNV64_OFFSET;
    digest->inputs = FNV64_OFFSET;
    for (unsigned i = 0; i < 16; ++i) digest->blocks[i] = FNV32_OFFSET;
    digest->count = 0;
    digest->subnormal_inputs = digest->subnormal_results = digest->zero_results = digest->nan_results = 0;
    digest->block_size = expected_count / 16 + (expected_count % 16 != 0);
    if (digest->block_size == 0) digest->block_size = 1;
}

static unsigned block_of(const struct wii_abi_digest *digest)
{
    unsigned long block = digest->count / digest->block_size;
    return block < 16 ? (unsigned)block : 15u;
}

static int subnormal32(uint32_t bits) { return (bits & UINT32_C(0x7f800000)) == 0 && (bits & UINT32_C(0x007fffff)) != 0; }
static int subnormal64(uint64_t bits)
{
    return (bits & UINT64_C(0x7ff0000000000000)) == 0 && (bits & UINT64_C(0x000fffffffffffff)) != 0;
}

void wii_abi_digest_input32(struct wii_abi_digest *digest, uint32_t bits)
{
    digest->inputs = fnv64(digest->inputs, bits, 4);
    digest->subnormal_inputs += subnormal32(bits);
}

void wii_abi_digest_input64(struct wii_abi_digest *digest, uint64_t bits)
{
    digest->inputs = fnv64(digest->inputs, bits, 8);
    digest->subnormal_inputs += subnormal64(bits);
}

void wii_abi_digest_result32(struct wii_abi_digest *digest, uint32_t bits)
{
    digest->subnormal_results += subnormal32(bits);
    digest->zero_results += (bits & UINT32_C(0x7fffffff)) == 0;
    digest->nan_results += (bits & UINT32_C(0x7fffffff)) > UINT32_C(0x7f800000);
    digest->all = fnv64(digest->all, bits, 4);
    unsigned block = block_of(digest);
    digest->blocks[block] = fnv32(digest->blocks[block], bits, 4);
}

void wii_abi_digest_result64(struct wii_abi_digest *digest, uint64_t bits)
{
    digest->subnormal_results += subnormal64(bits);
    digest->zero_results += (bits & UINT64_C(0x7fffffffffffffff)) == 0;
    digest->nan_results += (bits & UINT64_C(0x7fffffffffffffff)) > UINT64_C(0x7ff0000000000000);
    digest->all = fnv64(digest->all, bits, 8);
    unsigned block = block_of(digest);
    digest->blocks[block] = fnv32(digest->blocks[block], bits, 8);
}

void wii_abi_digest_next(struct wii_abi_digest *digest)
{
    digest->count++;
}

void wii_abi_digest_emit(struct wii_abi_report *report, const char *id, const struct wii_abi_digest *digest)
{
    report->math_lines++;
    fprintf(report->out, "MATH %s%s count=%lu inputs=%016" PRIx64 " results=%016" PRIx64 " blocks=",
            prefix(report), id, digest->count, digest->inputs, digest->all);
    for (unsigned i = 0; i < 16; ++i) fprintf(report->out, "%08" PRIx32, digest->blocks[i]);
    fprintf(report->out, " subnormal_inputs=%lu subnormal_results=%lu zero_results=%lu nan_results=%lu",
            digest->subnormal_inputs, digest->subnormal_results, digest->zero_results, digest->nan_results);
    fputc('\n', report->out);
}

uint32_t wii_abi_f32_bits(float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

uint64_t wii_abi_f64_bits(double value)
{
    uint64_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

float wii_abi_f32_from_bits(uint32_t bits)
{
    float value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

double wii_abi_f64_from_bits(uint64_t bits)
{
    double value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}
