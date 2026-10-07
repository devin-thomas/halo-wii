#include "packet_shim.h"

/* Diagnostic policy only: consume legacy encoder placeholders as raw bytes. */
boolean packet_policy_excluded(struct data_encoding_state *state,
                               const struct data_packet_field *field, void *destination)
{
    short span;
    if (field->count <= 0 || field->size < 0) {
        state->overflow = TRUE;
        return FALSE;
    }
    switch (field->type) {
    case _data_packet_field_bytes:
    case _data_packet_field_shorts:
    case _data_packet_field_longs:
    case _data_packet_field_int64s:
    case _data_packet_field_raw:
        span = field->count;
        break;
    case _data_packet_field_string:
        span = 1;
        break;
    case _data_packet_field_data:
        /* Valid packet counts are positive shorts: the selector is 1 or 2. */
        span = field->count <= UNSIGNED_CHAR_MAX ? 1 : 2;
        break;
    case _data_packet_field_pad:
        span = 0;
        break;
    default:
        /* Excluded arrays need a separate paired encoder/schema correction. */
        state->overflow = TRUE;
        return FALSE;
    }
    if (candidate_decode_memory(state, span, 1) == NULL) return FALSE;
    csmemset(destination, 0, field->size);
    return TRUE;
}
