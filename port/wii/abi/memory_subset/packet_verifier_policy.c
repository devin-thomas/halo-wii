#include "packet_verifier_policy.h"
#include <limits.h>

const char *packet_verifier_error_name(enum packet_verifier_error error)
{
    switch (error) {
    case PACKET_VERIFY_OK: return "ok";
    case PACKET_VERIFY_NULL_DEFINITION: return "null_definition";
    case PACKET_VERIFY_NEGATIVE_SIZE: return "negative_native_size";
    case PACKET_VERIFY_VERSION_RANGE: return "version_outside_byte_range";
    case PACKET_VERIFY_MISSING_NAME: return "missing_name";
    case PACKET_VERIFY_MISSING_FIELDS: return "missing_fields";
    case PACKET_VERIFY_FIELD_BOUND: return "invalid_field_bound";
    case PACKET_VERIFY_MISSING_END: return "missing_end_within_bound";
    case PACKET_VERIFY_FIELD_TYPE: return "invalid_field_type";
    case PACKET_VERIFY_FIELD_COUNT: return "nonpositive_field_count";
    case PACKET_VERIFY_FIELD_GATE: return "invalid_version_gate";
    case PACKET_VERIFY_UNSUPPORTED_ARRAY: return "unsupported_array_schema";
    case PACKET_VERIFY_FIELD_EXTENT: return "field_native_extent_overflow";
    case PACKET_VERIFY_TOTAL_EXTENT: return "total_native_extent_overflow";
    case PACKET_VERIFY_SIZE_MISMATCH: return "declared_native_size_mismatch";
    }
    return "unknown_diagnostic_error";
}

static boolean reject(struct packet_verifier_result *result, enum packet_verifier_error error)
{
    result->error = error;
    return FALSE;
}

static uint32_t native_field_extent(const struct data_packet_field *field)
{
    /* Positive short counts and these fixed ABI widths fit uint32_t before
     * rejecting extents outside the signed-short metadata representation. */
    uint32_t count = (uint32_t)field->count;
    switch (field->type) {
    case _data_packet_field_shorts: return count * sizeof(short);
    case _data_packet_field_longs: return count * sizeof(long);
    case _data_packet_field_int64s: return count * sizeof(__int64);
    case _data_packet_field_string: return count + 1;
    case _data_packet_field_data: return count + sizeof(short);
    case _data_packet_field_pad:
    case _data_packet_field_bytes:
    case _data_packet_field_raw: return count;
    default:
        match_assert(__FILE__, __LINE__, FALSE);
        system_exit(2);
    }
}

static boolean eligible(short version, const struct data_packet_field *field)
{
    return version >= field->minimum_version &&
           (field->maximum_version == 0 || version <= field->maximum_version);
}

boolean packet_verifier_diagnostic(struct data_packet_definition *definition, size_t field_bound,
                                   struct packet_verifier_result *result)
{
    match_assert(__FILE__, __LINE__, result);
    *result = (struct packet_verifier_result){PACKET_VERIFY_OK, 0, 0, 0, 0};
    if (definition == NULL) return reject(result, PACKET_VERIFY_NULL_DEFINITION);
    if (definition->size < 0) return reject(result, PACKET_VERIFY_NEGATIVE_SIZE);
    if (definition->version < 0 || definition->version > UNSIGNED_CHAR_MAX)
        return reject(result, PACKET_VERIFY_VERSION_RANGE);
    if (definition->name == NULL) return reject(result, PACKET_VERIFY_MISSING_NAME);
    if (definition->fields == NULL) return reject(result, PACKET_VERIFY_MISSING_FIELDS);
    if (field_bound == 0 || field_bound > (size_t)SHRT_MAX)
        return reject(result, PACKET_VERIFY_FIELD_BOUND);

    size_t index;
    for (index = 0; index < field_bound; ++index) {
        const struct data_packet_field *field = &definition->fields[index];
        result->field_index = index;
        result->field_extent = 0; /* Never reuse the preceding field's size. */
        if (field->type == _data_packet_field_end) {
            result->fields_consumed = index + 1;
            break;
        }
        if (field->type < 0 || field->type >= _data_packet_field_type_count)
            return reject(result, PACKET_VERIFY_FIELD_TYPE);
        if (field->count <= 0) return reject(result, PACKET_VERIFY_FIELD_COUNT);
        if (field->minimum_version < 0 || field->minimum_version > UNSIGNED_CHAR_MAX ||
            field->maximum_version < 0 || field->maximum_version > UNSIGNED_CHAR_MAX ||
            (field->maximum_version != 0 && field->minimum_version > field->maximum_version))
            return reject(result, PACKET_VERIFY_FIELD_GATE);
        /* No child traversal or reserve guess, including an excluded array. */
        if (field->type == _data_packet_field_array)
            return reject(result, PACKET_VERIFY_UNSUPPORTED_ARRAY);
        result->field_extent = native_field_extent(field);
        if (result->field_extent > (uint32_t)SHRT_MAX)
            return reject(result, PACKET_VERIFY_FIELD_EXTENT);
        if (!eligible(definition->version, field)) result->field_extent = 0;
        if (result->field_extent > (uint32_t)SHRT_MAX - result->native_extent)
            return reject(result, PACKET_VERIFY_TOTAL_EXTENT);
        result->native_extent += result->field_extent;
    }
    if (index == field_bound) {
        result->field_index = field_bound;
        return reject(result, PACKET_VERIFY_MISSING_END);
    }
    if (result->native_extent != (uint32_t)definition->size)
        return reject(result, PACKET_VERIFY_SIZE_MISMATCH);

    /* Commit only after the bounded schema fully validates. Caller must not
     * mutate the definition concurrently between validation and this pass. */
    for (size_t n = 0; n < index; ++n) {
        struct data_packet_field *field = &definition->fields[n];
        uint32_t extent = eligible(definition->version, field) ? native_field_extent(field) : 0;
        field->size = (short)extent;
    }
    definition->initialized = TRUE;
    return TRUE;
}
