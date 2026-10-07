#ifndef WII_PACKET_VERIFIER_POLICY_H
#define WII_PACKET_VERIFIER_POLICY_H
#include "packet_shim.h"
#include <stddef.h>

enum packet_verifier_error {
    PACKET_VERIFY_OK,
    PACKET_VERIFY_NULL_DEFINITION,
    PACKET_VERIFY_NEGATIVE_SIZE,
    PACKET_VERIFY_VERSION_RANGE,
    PACKET_VERIFY_MISSING_NAME,
    PACKET_VERIFY_MISSING_FIELDS,
    PACKET_VERIFY_FIELD_BOUND,
    PACKET_VERIFY_MISSING_END,
    PACKET_VERIFY_FIELD_TYPE,
    PACKET_VERIFY_FIELD_COUNT,
    PACKET_VERIFY_FIELD_GATE,
    PACKET_VERIFY_UNSUPPORTED_ARRAY,
    PACKET_VERIFY_FIELD_EXTENT,
    PACKET_VERIFY_TOTAL_EXTENT,
    PACKET_VERIFY_SIZE_MISMATCH
};
struct packet_verifier_result {
    enum packet_verifier_error error;
    size_t field_index, fields_consumed;
    uint32_t native_extent, field_extent;
};
/* Caller supplies a truthful accessible schema bound; arrays are unsupported.
 * Always revalidate, then commit sizes/initialized only after full success. */
boolean packet_verifier_diagnostic(struct data_packet_definition *, size_t field_bound,
                                   struct packet_verifier_result *);
const char *packet_verifier_error_name(enum packet_verifier_error);
/* Nonmutating latent extent for the separate all-version array reserve policy. */
boolean packet_verifier_flat_extent(const struct data_packet_field *, uint32_t *,
                                    enum packet_verifier_error *);
#endif
