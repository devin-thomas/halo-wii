#ifndef WII_ABI_SEMANTICS_REPORT_H
#define WII_ABI_SEMANTICS_REPORT_H
/* Canonical report lines shared by the host and PPC builds.

   CHECK <id> class=<class> observed=<v> reference=<v> result=pass|fail
     contract          C/ABI behaviour every port must provide.
     engine_assumption what the engine source assumes (MSVC/x86 semantics);
                       a failure is a measured target finding, not a harness bug.
     candidate         a proposed remedy (explicit helper or build flag).
   OBS <id> value=<v>      an observation compared between targets only.
   MATH <id> count=<n> inputs=<digest> results=<digest> blocks=<16 x 8 hex>
        subnormal_inputs=<n> subnormal_results=<n> zero_results=<n> nan_results=<n>
                          compared between targets only. */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

struct wii_abi_report
{
    FILE *out;
    unsigned long checks;
    unsigned long failures;
    unsigned long contract_failures;
    unsigned long assumption_failures;
    unsigned long candidate_failures;
    unsigned long observations;
    unsigned long math_lines;
    /* prepended to every id; and, when set, the class recorded for checks
       declared as contracts (used by the candidate floating-point pass) */
    const char *prefix;
    const char *contract_as;
};

#define WII_ABI_CONTRACT "contract"
#define WII_ABI_ASSUMPTION "engine_assumption"
#define WII_ABI_CANDIDATE "candidate"

void wii_abi_check_u64(struct wii_abi_report *report, const char *id, const char *check_class,
                       uint64_t observed, uint64_t reference);
void wii_abi_check_i64(struct wii_abi_report *report, const char *id, const char *check_class,
                       int64_t observed, int64_t reference);
void wii_abi_check_text(struct wii_abi_report *report, const char *id, const char *check_class,
                        const char *observed, const char *reference);
void wii_abi_check_bytes(struct wii_abi_report *report, const char *id, const char *check_class,
                         const uint8_t *observed, const uint8_t *reference, size_t length);
void wii_abi_observe_u64(struct wii_abi_report *report, const char *id, uint64_t value);
void wii_abi_observe_bytes(struct wii_abi_report *report, const char *id, const uint8_t *bytes, size_t length);

/* FNV-1a over explicit little-endian byte order, so digests do not depend on
   the executing target's byte order. */
struct wii_abi_digest
{
    uint64_t all;
    uint64_t inputs;
    uint32_t blocks[16];
    unsigned long count;
    unsigned long block_size;
    /* classification of digested values (binary32 and binary64 results) */
    unsigned long subnormal_inputs;
    unsigned long subnormal_results;
    unsigned long zero_results;
    unsigned long nan_results;
};

void wii_abi_digest_begin(struct wii_abi_digest *digest, unsigned long expected_count);
void wii_abi_digest_input32(struct wii_abi_digest *digest, uint32_t bits);
void wii_abi_digest_input64(struct wii_abi_digest *digest, uint64_t bits);
void wii_abi_digest_result32(struct wii_abi_digest *digest, uint32_t bits);
void wii_abi_digest_result64(struct wii_abi_digest *digest, uint64_t bits);
/* Ends one case: advances the block index used by later results. */
void wii_abi_digest_next(struct wii_abi_digest *digest);
void wii_abi_digest_emit(struct wii_abi_report *report, const char *id, const struct wii_abi_digest *digest);

uint32_t wii_abi_f32_bits(float value);
uint64_t wii_abi_f64_bits(double value);
float wii_abi_f32_from_bits(uint32_t bits);
double wii_abi_f64_from_bits(uint64_t bits);

#endif
