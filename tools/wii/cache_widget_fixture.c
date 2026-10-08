#include "cache_widget_fixture.h"
#include "cache_widget_probe.h"
#include <stdlib.h>
#include <string.h>

enum
{
    BLOB_BYTES = 65536,
    OBJECT_BYTES = 65728,
    GUARD = 32,
    ROOT_A = 512,
    EVENT_A = ROOT_A + 1004,
    CHILD_A = EVENT_A + 72,
    ROOT_B = CHILD_A + 80,
    SERIAL_BYTES = 2160,
    NODE_CAPACITY = 40
};

#define BASE UINT32_C(0x803a6000)
#define DATUM_A UINT32_C(0x12340001)
#define DATUM_B UINT32_C(0x23450002)
#define BITMAP_DATUM UINT32_C(0x34560003)

struct widget_context
{
    FILE *report;
    const char *name;
    unsigned cases, checks, failures;
    int collect;
};

struct widget_storage
{
    unsigned char *placement[2], *authored, *expected, *serialized, *published_before;
    void *workspace;
    size_t workspace_bytes;
    struct cache_arena_owner owner;
};

static int check(struct widget_context *context, int condition, const char *assertion)
{
    ++context->checks;
    if (condition)
        return 1;
    ++context->failures;
    if (fprintf(context->report, "CACHE_WIDGET FAIL case=%s assertion=%s check=%u\n",
                context->name, assertion, context->checks) < 0 || fflush(context->report) != 0)
        return 0;
    return context->collect;
}

#define CHECK(condition, assertion) \
    do \
    { \
        if (!check(context, (condition), (assertion))) \
            return 1; \
    } while (0)

static int abort_case(struct widget_context *context, const char *reason)
{
    (void)fprintf(context->report, "CACHE_WIDGET ABORT case=%s reason=%s\n", context->name, reason);
    (void)fflush(context->report);
    return 1;
}

static void word(unsigned char *bytes, size_t offset, uint32_t value)
{
    bytes[offset] = (unsigned char)value;
    bytes[offset + 1] = (unsigned char)(value >> 8);
    bytes[offset + 2] = (unsigned char)(value >> 16);
    bytes[offset + 3] = (unsigned char)(value >> 24);
}

static void half(unsigned char *bytes, size_t offset, uint16_t value)
{
    bytes[offset] = (unsigned char)value;
    bytes[offset + 1] = (unsigned char)(value >> 8);
}

static void authored_reference(unsigned char *bytes, size_t offset, uint32_t group,
                               uint32_t datum, uint32_t name)
{
    word(bytes, offset, group);
    word(bytes, offset + 4, name);
    word(bytes, offset + 8, 0);
    word(bytes, offset + 12, datum);
}

static void authored_instance(unsigned char *bytes, size_t ordinal, uint32_t group,
                              uint32_t datum, size_t root_offset, size_t name_offset)
{
    size_t offset = 64 + ordinal * 32;
    word(bytes, offset, group);
    word(bytes, offset + 4, UINT32_MAX);
    word(bytes, offset + 8, UINT32_MAX);
    word(bytes, offset + 12, datum);
    word(bytes, offset + 16, BASE + (uint32_t)name_offset);
    word(bytes, offset + 20, BASE + (uint32_t)root_offset);
    word(bytes, offset + 24, UINT32_C(0xabcd0123));
    word(bytes, offset + 28, UINT32_C(0x98765432));
}

static void authored_root(unsigned char *bytes, size_t offset)
{
    static const size_t refs[] = {56, 236, 252, 340, 356, 420};
    static const uint32_t groups[] = {UINT32_C(0x6269746d), UINT32_C(0x75737472),
        UINT32_C(0x666f6e74), UINT32_C(0x6269746d), UINT32_C(0x6269746d), CACHE_WIDGET_GROUP};
    static const size_t blocks[] = {72, 84, 96, 724, 992};
    static const uint32_t colors[] = {UINT32_C(0x3f800000), UINT32_C(0x80000000),
                                     UINT32_C(0x7fc12345), UINT32_C(0x00800001)};
    memset(bytes + offset, 0xc7, 1004);
    half(bytes, offset, 0);
    half(bytes, offset + 2, 4);
    memset(bytes + offset + 4, 0, 32);
    memcpy(bytes + offset + 4, "authored-widget", 15);
    half(bytes, offset + 36, (uint16_t)-7);
    half(bytes, offset + 38, 258);
    half(bytes, offset + 40, 32767);
    half(bytes, offset + 42, 32768);
    word(bytes, offset + 44, UINT32_C(0x92345678));
    word(bytes, offset + 48, UINT32_C(0xf1234567));
    word(bytes, offset + 52, UINT32_C(0x87654321));
    for (size_t index = 0; index < 6; ++index)
    {
        authored_reference(bytes, offset + refs[index], groups[index], UINT32_MAX,
                           UINT32_C(0xf1231234));
        word(bytes, offset + refs[index] + 8, UINT32_C(0x80000001));
    }
    for (size_t index = 0; index < 5; ++index)
    {
        word(bytes, offset + blocks[index], 0);
        word(bytes, offset + blocks[index] + 4, UINT32_MAX);
        word(bytes, offset + blocks[index] + 8, UINT32_C(0xfeed0000) + (uint32_t)index);
    }
    for (size_t index = 0; index < 4; ++index)
    {
        word(bytes, offset + 268 + index * 4, colors[index]);
        half(bytes, offset + 372 + index * 2, (uint16_t)(index + 111));
        half(bytes, offset + 380 + index * 2, (uint16_t)(index + 222));
    }
    half(bytes, offset + 284, 2);
    half(bytes, offset + 286, UINT16_C(0xabcd));
    half(bytes, offset + 302, UINT16_MAX);
    half(bytes, offset + 304, (uint16_t)-123);
    half(bytes, offset + 306, 234);
    word(bytes, offset + 336, UINT32_C(0xe1234567));
}

static void authored_blob(unsigned char *bytes)
{
    memset(bytes, 0x5b, BLOB_BYTES);
    memset(bytes, 0, 36);
    word(bytes, 0, BASE + 64);
    word(bytes, 4, UINT32_C(0x45670000));
    word(bytes, 12, 4);
    word(bytes, 32, UINT32_C(0x74616773));
    authored_instance(bytes, 0, UINT32_C(0x73636e72), UINT32_C(0x45670000), 14000, 256);
    authored_instance(bytes, 1, CACHE_WIDGET_GROUP, DATUM_A, ROOT_A, 264);
    authored_instance(bytes, 2, CACHE_WIDGET_GROUP, DATUM_B, ROOT_B, 272);
    authored_instance(bytes, 3, UINT32_C(0x6269746d), BITMAP_DATUM, 15000, 280);
    memcpy(bytes + 256, "scnr", 5);
    memcpy(bytes + 264, "a", 2);
    memcpy(bytes + 272, "b", 2);
    memcpy(bytes + 280, "bitmap", 7);
    authored_root(bytes, ROOT_A);
    authored_root(bytes, ROOT_B);
    word(bytes, ROOT_B + 268, UINT32_C(0x7f800000));
    authored_reference(bytes, ROOT_A + 56, UINT32_C(0x6269746d), BITMAP_DATUM, BASE + 280);
    authored_reference(bytes, ROOT_B + 56, UINT32_C(0x6269746d), BITMAP_DATUM, BASE + 280);
    word(bytes, ROOT_A + 84, 1);
    word(bytes, ROOT_A + 88, BASE + EVENT_A);
    word(bytes, ROOT_A + 992, 1);
    word(bytes, ROOT_A + 996, BASE + CHILD_A);
    memset(bytes + EVENT_A, 0xa4, 72);
    word(bytes, EVENT_A, UINT32_C(0x81234567));
    half(bytes, EVENT_A + 4, (uint16_t)-7);
    half(bytes, EVENT_A + 6, 32767);
    authored_reference(bytes, EVENT_A + 8, CACHE_WIDGET_GROUP, UINT32_MAX, UINT32_MAX);
    authored_reference(bytes, EVENT_A + 24, UINT32_C(0x736e6421), UINT32_MAX, UINT32_MAX);
    memset(bytes + EVENT_A + 40, 0, 32);
    memcpy(bytes + EVENT_A + 40, "authored-script", 15);
    memset(bytes + CHILD_A, 0xd4, 80);
    authored_reference(bytes, CHILD_A, CACHE_WIDGET_GROUP, DATUM_B, BASE + 272);
    memset(bytes + CHILD_A + 16, 0, 32);
    memcpy(bytes + CHILD_A + 16, "authored-child", 14);
    word(bytes, CHILD_A + 48, UINT32_C(0x89abcdef));
    half(bytes, CHILD_A + 52, UINT16_MAX);
    half(bytes, CHILD_A + 54, 32767);
    half(bytes, CHILD_A + 56, 32768);
}

static int bind(struct widget_context *context, struct widget_storage *storage, unsigned placement,
                unsigned offset, struct cache_arena_handle *handle)
{
    struct cache_arena_request request = {BLOB_BYTES, 1};
    struct cache_arena_plan plan;
    struct cache_arena_result result;
    memset(storage->placement[placement], 0x6a, OBJECT_BYTES);
    unsigned char *span = storage->placement[placement] + GUARD + offset;
    memcpy(span, storage->authored, BLOB_BYTES);
    int accepted = cache_arena_plan_build(&request, 1, span, BLOB_BYTES + 64, 64, &plan, &result) &&
                   cache_arena_owner_bind(&storage->owner, &plan, span, BLOB_BYTES + 64, &result) &&
                   cache_arena_owner_handle(&storage->owner, 0, handle, &result);
    CHECK(accepted, "truthful_caller_slot_bound");
    if (!accepted)
        return abort_case(context, "owner_bind_failed");
    return 0;
}

static int release_owner(struct widget_context *context, struct widget_storage *storage)
{
    struct cache_arena_result result;
    CHECK(cache_arena_owner_release(&storage->owner, &result), "owner_release_advances_generation");
    return 0;
}

static int projection_checks(struct widget_context *context, const struct cache_widget_graph *graph)
{
    struct cache_widget_result result;
    struct cache_widget_projection node;
    struct cache_widget_element item;
    int accepted = cache_widget_graph_node(graph, 0, &node, &result);
    CHECK(accepted, "live_root_projection");
    if (!accepted)
        return abort_case(context, "root_projection_failed");
    CHECK(node.datum == DATUM_A && node.type == 0 && node.controller == 4 && node.justification == 2,
          "independent_datum_and_enum_values");
    CHECK(node.raw.offset == ROOT_A && node.raw.length == 1004 && node.name.length == 16,
          "complete_raw_root_and_bounded_string_span");
    CHECK(node.bounds[0] == -7 && node.bounds[1] == 258 && node.bounds[2] == 32767 &&
          node.bounds[3] == INT16_MIN && node.flags == UINT32_C(0x92345678) &&
          node.auto_close_word == UINT32_C(0xf1234567) && node.fade_word == UINT32_C(0x87654321),
          "signed_rectangle_and_unsigned_numeric_words");
    CHECK(node.text_color_bits[0] == UINT32_C(0x3f800000) &&
          node.text_color_bits[1] == UINT32_C(0x80000000) &&
          node.text_color_bits[2] == UINT32_C(0x7fc12345) &&
          node.text_color_bits[3] == UINT32_C(0x00800001),
          "float_one_negative_zero_nan_payload_subnormal_bit_identity");
    CHECK(node.text_box_flags == UINT16_C(0xabcd) && node.string_list_index == -1 &&
          node.horizontal_offset == -123 && node.vertical_offset == 234 &&
          node.list_flags == UINT32_C(0xe1234567), "remaining_named_root_scalar_values");
    for (size_t index = 0; index < 4; ++index)
        CHECK(node.list_header_bounds[index] == (int16_t)(111 + index) &&
              node.list_footer_bounds[index] == (int16_t)(222 + index), "header_footer_rectangle_order");
    CHECK(node.references[0].datum == BITMAP_DATUM && node.references[0].group == UINT32_C(0x6269746d) &&
          node.references[0].name_length_word == 0 && node.references[0].name_address == BASE + 280 &&
          node.references[0].name.offset == 280 && node.references[0].name.length == 7,
          "live_reference_zero_name_length_accepted");
    const uint32_t none_groups[] = {UINT32_C(0x75737472), UINT32_C(0x666f6e74),
                                   UINT32_C(0x6269746d), UINT32_C(0x6269746d), CACHE_WIDGET_GROUP};
    for (size_t index = 1; index < 6; ++index)
        CHECK(node.references[index].datum == UINT32_MAX &&
              node.references[index].name_address == UINT32_C(0xf1231234) &&
              node.references[index].name_length_word == UINT32_C(0x80000001) &&
              node.references[index].name.length == 0 && node.references[index].group == none_groups[index - 1],
              "none_reference_words_preserved_unfollowed");
    const size_t strides[] = {36, 72, 34, 80, 80};
    for (size_t index = 0; index < 5; ++index)
        CHECK(node.blocks[index].definition_word == UINT32_C(0xfeed0000) + index &&
              node.blocks[index].stride == strides[index] &&
              node.blocks[index].count == (index == CACHE_WIDGET_EVENT || index == CACHE_WIDGET_CHILD ? 1 : 0) &&
              node.blocks[index].address == (index == CACHE_WIDGET_EVENT ? BASE + EVENT_A :
                  index == CACHE_WIDGET_CHILD ? BASE + CHILD_A : UINT32_MAX),
              "definition_word_never_native_pointer");
    accepted = cache_widget_graph_element(graph, 0, CACHE_WIDGET_EVENT, 0, &item, &result);
    CHECK(accepted, "event_projection");
    if (!accepted)
        return abort_case(context, "event_projection_failed");
    CHECK(item.flags == UINT32_C(0x81234567) && item.event_type == -7 && item.function == 32767 &&
          item.references[0].datum == UINT32_MAX && item.references[1].datum == UINT32_MAX &&
          item.references[0].name_address == UINT32_MAX && item.references[1].name_address == UINT32_MAX &&
          item.references[0].group == CACHE_WIDGET_GROUP && item.references[1].group == UINT32_C(0x736e6421) &&
          item.references[0].name_length_word == 0 && item.references[1].name_length_word == 0 &&
          item.text.length == 16,
          "event_flags_signed_selectors_none_references_and_script");
    accepted = cache_widget_graph_element(graph, 0, CACHE_WIDGET_CHILD, 0, &item, &result);
    CHECK(accepted, "child_projection");
    if (!accepted)
        return abort_case(context, "child_projection_failed");
    CHECK(item.references[0].datum == DATUM_B && item.references[0].group == CACHE_WIDGET_GROUP &&
          item.references[0].name_address == BASE + 272 && item.references[0].name_length_word == 0 &&
          item.flags == UINT32_C(0x89abcdef) && item.controller == -1 &&
          item.vertical_offset == 32767 && item.horizontal_offset == INT16_MIN,
          "child_full_identity_signed_coordinates_and_flags");
    CHECK(graph->count == 2 && graph->serialized_size == SERIAL_BYTES && graph->nodes[0].height == 1 &&
          graph->nodes[1].datum == DATUM_B && graph->nodes[1].height == 0 &&
          graph->nodes[1].text_color_bits[0] == UINT32_C(0x7f800000),
          "two_node_loaded_child_graph");
    return 0;
}

static int lifetime_cases(struct widget_context *context, struct widget_storage *storage)
{
    context->name = "two_heap_placements_offsets_and_64_reloads";
    for (unsigned cycle = 0; cycle < 64; ++cycle)
    {
        for (unsigned placement = 0; placement < 2; ++placement)
        {
            unsigned offset = cycle % 8;
            struct cache_arena_handle handle;
            struct cache_widget_graph graph;
            struct cache_widget_result result;
            ++context->cases;
            if (bind(context, storage, placement, offset, &handle))
                return 1;
            memcpy(storage->expected, storage->placement[placement], OBJECT_BYTES);
            int accepted = cache_widget_graph_decode(&storage->owner, &handle, BLOB_BYTES, BASE, DATUM_A,
                storage->workspace, storage->workspace_bytes, NODE_CAPACITY, &graph, &result);
            CHECK(accepted && result.error == CACHE_WIDGET_OK, "decode_bounded_actual_shape");
            if (!accepted)
                return abort_case(context, "valid_graph_decode_failed");
            if (projection_checks(context, &graph))
                return 1;
            memset(storage->serialized, 0x6a, OBJECT_BYTES);
            size_t used = SIZE_MAX;
            accepted = cache_widget_graph_serialize(&graph, storage->serialized + GUARD + offset,
                SERIAL_BYTES, &used, &result);
            CHECK(accepted && used == SERIAL_BYTES, "exact_capacity_numeric_le_serialization");
            if (!accepted)
                return abort_case(context, "valid_graph_serialize_failed");
            CHECK(memcmp(storage->serialized + GUARD + offset, storage->authored + ROOT_A,
                         SERIAL_BYTES) == 0, "independent_authored_bytes_and_opaque_spans_roundtrip");
            memset(storage->published_before, 0x6a, OBJECT_BYTES);
            memcpy(storage->published_before + GUARD + offset, storage->authored + ROOT_A, SERIAL_BYTES);
            CHECK(memcmp(storage->serialized, storage->published_before, OBJECT_BYTES) == 0,
                  "entire_serial_destination_guards_unchanged");
            CHECK(memcmp(storage->placement[placement], storage->expected, OBJECT_BYTES) == 0,
                  "entire_resident_object_and_reserve_unchanged");
            if (release_owner(context, storage))
                return 1;
            struct cache_widget_projection stale, before;
            memset(&stale, 0x7b, sizeof(stale));
            memcpy(&before, &stale, sizeof(before));
            CHECK(!cache_widget_graph_node(&graph, 0, &stale, &result) && result.error == CACHE_WIDGET_STATE &&
                  memcmp(&stale, &before, sizeof(stale)) == 0, "released_owner_rejects_stale_projection_atomically");
            used = 456;
            CHECK(!cache_widget_graph_serialize(&graph, storage->serialized, OBJECT_BYTES, &used, &result) &&
                  result.error == CACHE_WIDGET_STATE && used == 456 &&
                  memcmp(storage->serialized, storage->published_before, OBJECT_BYTES) == 0,
                  "released_owner_rejects_stale_serialization_before_io");
            struct cache_arena_handle alternate;
            if (bind(context, storage, 1 - placement, offset, &alternate))
                return 1;
            CHECK(!cache_widget_graph_node(&graph, 0, &stale, &result) && result.error == CACHE_WIDGET_STATE,
                  "same_live_control_new_placement_old_epoch_rejected");
            if (release_owner(context, storage))
                return 1;
            cache_widget_graph_release(&graph);
            struct cache_widget_graph zero;
            memset(&zero, 0, sizeof(zero));
            CHECK(memcmp(&graph, &zero, sizeof(graph)) == 0, "graph_release_zeros_borrowed_control");
        }
    }
    return 0;
}

static int rejection(struct widget_context *context, struct widget_storage *storage,
                     enum cache_widget_error expected, size_t resident, uint32_t base,
                     uint32_t datum, size_t capacity, size_t workspace_bytes)
{
    struct cache_arena_handle handle;
    struct cache_widget_graph graph, before;
    struct cache_widget_result result;
    ++context->cases;
    if (bind(context, storage, 0, 3, &handle))
        return 1;
    memset(storage->workspace, 0x7c, storage->workspace_bytes);
    size_t published_bytes = capacity * sizeof(struct cache_widget_projection);
    if (published_bytes > storage->workspace_bytes)
        published_bytes = storage->workspace_bytes;
    memcpy(storage->published_before, storage->workspace, published_bytes);
    memset(&graph, 0x9a, sizeof(graph));
    memcpy(&before, &graph, sizeof(graph));
    memcpy(storage->expected, storage->placement[0], OBJECT_BYTES);
    CHECK(!cache_widget_graph_decode(&storage->owner, &handle, resident, base, datum,
              storage->workspace, workspace_bytes, capacity, &graph, &result) && result.error == expected,
          "descriptive_malformed_rejection");
    CHECK(memcmp(&graph, &before, sizeof(graph)) == 0 &&
          memcmp(storage->workspace, storage->published_before, published_bytes) == 0,
          "graph_and_published_half_atomic_on_rejection_scratch_separate");
    CHECK(memcmp(storage->placement[0], storage->expected, OBJECT_BYTES) == 0,
          "malformed_input_entire_storage_immutable");
    return release_owner(context, storage);
}

static int malformed_cases(struct widget_context *context, struct widget_storage *storage)
{
    static const size_t blocks[] = {72, 84, 96, 724, 992};
    static const unsigned maxima[] = {64, 32, 32, 32, 32};
    for (size_t kind = 0; kind < 5; ++kind)
    {
        context->name = "signed_negative_and_source_maximum_plus_one_counts";
        authored_blob(storage->authored);
        word(storage->authored, ROOT_A + blocks[kind], UINT32_MAX);
        if (rejection(context, storage, CACHE_WIDGET_COUNT, BLOB_BYTES, BASE, DATUM_A,
                      NODE_CAPACITY, storage->workspace_bytes))
            return 1;
        authored_blob(storage->authored);
        word(storage->authored, ROOT_A + blocks[kind], maxima[kind] + 1);
        if (rejection(context, storage, CACHE_WIDGET_COUNT, BLOB_BYTES, BASE, DATUM_A,
                      NODE_CAPACITY, storage->workspace_bytes))
            return 1;
    }
    struct mutation { size_t offset; uint32_t value; enum cache_widget_error error; };
    static const struct mutation mutations[] = {
        {ROOT_A + 88, BASE - 1, CACHE_WIDGET_SPAN},
        {ROOT_A + 88, BASE + BLOB_BYTES - 71, CACHE_WIDGET_SPAN},
        {ROOT_A + 996, BASE + BLOB_BYTES - 79, CACHE_WIDGET_SPAN},
        {ROOT_A + 56, CACHE_WIDGET_GROUP, CACHE_WIDGET_GROUP_ERROR},
        {ROOT_A + 68, BITMAP_DATUM ^ UINT32_C(0x10000), CACHE_WIDGET_DATUM},
        {ROOT_A + 60, BASE + BLOB_BYTES, CACHE_WIDGET_SPAN},
        {CHILD_A + 12, UINT32_C(0x9999ffff), CACHE_WIDGET_DATUM},
        {CHILD_A, UINT32_C(0x6269746d), CACHE_WIDGET_GROUP_ERROR},
        {CHILD_A + 12, DATUM_A, CACHE_WIDGET_CYCLE},
        {ROOT_A, 7, CACHE_WIDGET_ENUM},
        {ROOT_A, UINT32_MAX, CACHE_WIDGET_ENUM},
        {ROOT_A + 2, UINT32_MAX, CACHE_WIDGET_ENUM},
        {ROOT_A + 2, 5, CACHE_WIDGET_ENUM},
        {ROOT_A + 284, 3, CACHE_WIDGET_ENUM},
        {ROOT_A + 284, UINT32_MAX, CACHE_WIDGET_ENUM},
        {64 + 2 * 32, UINT32_C(0x6269746d), CACHE_WIDGET_GROUP_ERROR},
        {64 + 2 * 32 + 20, BASE + BLOB_BYTES - 1003, CACHE_WIDGET_SPAN}
    };
    context->name = "malformed_addresses_enums_full_datums_groups_and_cycle";
    for (size_t index = 0; index < sizeof(mutations) / sizeof(mutations[0]); ++index)
    {
        authored_blob(storage->authored);
        word(storage->authored, mutations[index].offset, mutations[index].value);
        if (rejection(context, storage, mutations[index].error, BLOB_BYTES, BASE, DATUM_A,
                      NODE_CAPACITY, storage->workspace_bytes))
            return 1;
    }
    const size_t string_offsets[] = {ROOT_A + 4, EVENT_A + 40, CHILD_A + 16};
    context->name = "bounded_fixed_strings_and_live_name_without_nul";
    for (size_t index = 0; index < 3; ++index)
    {
        authored_blob(storage->authored);
        memset(storage->authored + string_offsets[index], 'x', 32);
        if (rejection(context, storage, CACHE_WIDGET_STRING, BLOB_BYTES, BASE, DATUM_A,
                      NODE_CAPACITY, storage->workspace_bytes))
            return 1;
    }
    authored_blob(storage->authored);
    memset(storage->authored + 30000, 'x', BLOB_BYTES - 30000);
    word(storage->authored, CHILD_A + 4, BASE + 30000);
    if (rejection(context, storage, CACHE_WIDGET_STRING, BLOB_BYTES, BASE, DATUM_A,
                  NODE_CAPACITY, storage->workspace_bytes))
        return 1;
    context->name = "workspace_budget_truncation_encoded_domain_overflow_and_root_none";
    authored_blob(storage->authored);
    if (rejection(context, storage, CACHE_WIDGET_WORKSPACE, BLOB_BYTES, BASE, DATUM_A, 1,
                  storage->workspace_bytes) ||
        rejection(context, storage, CACHE_WIDGET_WORKSPACE, BLOB_BYTES, BASE, DATUM_A,
                  NODE_CAPACITY, storage->workspace_bytes - 1) ||
        rejection(context, storage, CACHE_WIDGET_SPAN, ROOT_B + 1003, BASE, DATUM_A,
                  NODE_CAPACITY, storage->workspace_bytes) ||
        rejection(context, storage, CACHE_WIDGET_OVERFLOW, BLOB_BYTES, UINT32_MAX - 10, DATUM_A,
                  NODE_CAPACITY, storage->workspace_bytes) ||
        rejection(context, storage, CACHE_WIDGET_DATUM, BLOB_BYTES, BASE, UINT32_MAX,
                  NODE_CAPACITY, storage->workspace_bytes))
        return 1;
    context->name = "root_and_block_ownership_overlap";
    authored_blob(storage->authored);
    word(storage->authored, ROOT_A + 72, 1);
    word(storage->authored, ROOT_A + 76, BASE + ROOT_A + 108);
    if (rejection(context, storage, CACHE_WIDGET_OVERLAP, BLOB_BYTES, BASE, DATUM_A,
                  NODE_CAPACITY, storage->workspace_bytes))
        return 1;
    return 0;
}

static int limits_cases(struct widget_context *context, struct widget_storage *storage)
{
    static const size_t offsets[] = {72, 84, 96, 724, 992};
    static const size_t strides[] = {36, 72, 34, 80, 80};
    static const unsigned maxima[] = {64, 32, 32, 32, 32};
    context->name = "all_five_source_block_capacities_and_named_element_fields";
    for (size_t kind = 0; kind < 5; ++kind)
    {
        ++context->cases;
        authored_blob(storage->authored);
        word(storage->authored, ROOT_A + offsets[kind], maxima[kind]);
        word(storage->authored, ROOT_A + offsets[kind] + 4, BASE + 10000);
        memset(storage->authored + 10000, 0, maxima[kind] * strides[kind]);
        for (size_t index = 0; index < maxima[kind]; ++index)
        {
            size_t offset = 10000 + index * strides[kind];
            if (kind == CACHE_WIDGET_EVENT)
            {
                authored_reference(storage->authored, offset + 8, CACHE_WIDGET_GROUP, UINT32_MAX, UINT32_MAX);
                authored_reference(storage->authored, offset + 24, UINT32_C(0x736e6421), UINT32_MAX, UINT32_MAX);
            }
            else if (kind == CACHE_WIDGET_CONDITIONAL || kind == CACHE_WIDGET_CHILD)
                authored_reference(storage->authored, offset, CACHE_WIDGET_GROUP, UINT32_MAX, UINT32_MAX);
            else if (kind == CACHE_WIDGET_INPUT)
                half(storage->authored, offset, (uint16_t)(index + 123));
            else
                half(storage->authored, offset + 32, (uint16_t)(index + 234));
        }
        struct cache_arena_handle handle;
        struct cache_widget_graph graph;
        struct cache_widget_result result;
        if (bind(context, storage, 0, 7, &handle))
            return 1;
        int accepted = cache_widget_graph_decode(&storage->owner, &handle, BLOB_BYTES, BASE, DATUM_A,
            storage->workspace, storage->workspace_bytes, NODE_CAPACITY, &graph, &result);
        CHECK(accepted, "source_maximum_count_accepted_without_reduction");
        if (!accepted)
            return abort_case(context, "maximum_count_decode_failed");
        for (size_t index = 0; index < maxima[kind]; ++index)
        {
            struct cache_widget_element item;
            accepted = cache_widget_graph_element(&graph, 0, (enum cache_widget_block_kind)kind, index,
                                                   &item, &result);
            CHECK(accepted && item.raw.length == strides[kind], "every_maximum_block_record_accessible");
            if (!accepted)
                return abort_case(context, "maximum_element_failed");
            if (kind == CACHE_WIDGET_INPUT || kind == CACHE_WIDGET_REPLACE)
                CHECK(item.function == (int16_t)(index + (kind == CACHE_WIDGET_INPUT ? 123 : 234)),
                      "input_or_replacement_function_numeric_value");
        }
        size_t used = 0;
        CHECK(cache_widget_graph_serialize(&graph, storage->serialized, OBJECT_BYTES, &used, &result) &&
              used == graph.serialized_size, "maximum_block_serialization_complete");
        if (release_owner(context, storage))
            return 1;
    }
    return 0;
}

static void chain_blob(unsigned char *bytes, size_t count)
{
    authored_blob(bytes);
    word(bytes, 12, (uint32_t)(count + 1));
    for (size_t index = 0; index < count; ++index)
    {
        size_t ordinal = index + 1;
        size_t root_offset = 2048 + index * 1084;
        uint32_t datum = UINT32_C(0x56780000) + (uint32_t)ordinal;
        authored_instance(bytes, ordinal, CACHE_WIDGET_GROUP, datum, root_offset, 1900);
        authored_root(bytes, root_offset);
        if (index + 1 < count)
        {
            word(bytes, root_offset + 992, 1);
            word(bytes, root_offset + 996, BASE + (uint32_t)root_offset + 1004);
            memset(bytes + root_offset + 1004, 0, 80);
            authored_reference(bytes, root_offset + 1004, CACHE_WIDGET_GROUP, datum + 1, BASE + 1900);
        }
    }
    memcpy(bytes + 1900, "chain-name", 11);
}

static int depth_cases(struct widget_context *context, struct widget_storage *storage)
{
    context->name = "source_depth_32_edges_explicit_stack_and_shared_completed_child";
    chain_blob(storage->authored, 33);
    struct cache_arena_handle handle;
    struct cache_widget_graph graph;
    struct cache_widget_result result;
    ++context->cases;
    if (bind(context, storage, 1, 1, &handle))
        return 1;
    int accepted = cache_widget_graph_decode(&storage->owner, &handle, BLOB_BYTES, BASE,
        UINT32_C(0x56780001), storage->workspace, storage->workspace_bytes, NODE_CAPACITY, &graph, &result);
    CHECK(accepted && graph.count == 33 && graph.nodes[0].height == 32,
          "root_depth_zero_32_child_edges_allowed_without_c_recursion");
    if (!accepted)
        return abort_case(context, "maximum_depth_failed");
    if (release_owner(context, storage))
        return 1;
    context->name = "source_depth_33_rejected_atomically";
    chain_blob(storage->authored, 34);
    if (rejection(context, storage, CACHE_WIDGET_DEPTH, BLOB_BYTES, BASE, UINT32_C(0x56780001),
                  NODE_CAPACITY, storage->workspace_bytes))
        return 1;
    context->name = "duplicate_child_dag_and_column_extended_edge";
    authored_blob(storage->authored);
    word(storage->authored, ROOT_A + 992, 2);
    word(storage->authored, ROOT_A + 996, BASE + 10000);
    memcpy(storage->authored + 10000, storage->authored + CHILD_A, 80);
    memcpy(storage->authored + 10080, storage->authored + CHILD_A, 80);
    half(storage->authored, ROOT_A, 3);
    authored_reference(storage->authored, ROOT_A + 420, CACHE_WIDGET_GROUP, DATUM_B, BASE + 272);
    ++context->cases;
    if (bind(context, storage, 0, 5, &handle))
        return 1;
    accepted = cache_widget_graph_decode(&storage->owner, &handle, BLOB_BYTES, BASE, DATUM_A,
        storage->workspace, storage->workspace_bytes, NODE_CAPACITY, &graph, &result);
    CHECK(accepted && graph.count == 2 && graph.nodes[0].height == 1,
          "completed_dependency_reused_without_false_cycle_or_duplicate_root");
    if (release_owner(context, storage))
        return 1;
    authored_blob(storage->authored);
    half(storage->authored, ROOT_A, 3);
    word(storage->authored, ROOT_A + 992, 0);
    authored_reference(storage->authored, ROOT_A + 420, CACHE_WIDGET_GROUP, DATUM_B, BASE + 272);
    ++context->cases;
    if (bind(context, storage, 0, 5, &handle))
        return 1;
    accepted = cache_widget_graph_decode(&storage->owner, &handle, BLOB_BYTES, BASE, DATUM_A,
        storage->workspace, storage->workspace_bytes, NODE_CAPACITY, &graph, &result);
    CHECK(accepted && graph.count == 2 && graph.nodes[1].datum == DATUM_B,
          "column_list_extended_edge_followed_without_any_child_edge");
    return release_owner(context, storage);
}

static int capacity_cases(struct widget_context *context, struct widget_storage *storage)
{
    context->name = "serializer_boundaries_and_whole_destination_atomicity";
    authored_blob(storage->authored);
    struct cache_arena_handle handle;
    struct cache_widget_graph graph;
    struct cache_widget_result result;
    if (bind(context, storage, 0, 0, &handle))
        return 1;
    int accepted = cache_widget_graph_decode(&storage->owner, &handle, BLOB_BYTES, BASE, DATUM_A,
        storage->workspace, storage->workspace_bytes, NODE_CAPACITY, &graph, &result);
    CHECK(accepted, "capacity_setup_decode");
    if (!accepted)
        return abort_case(context, "capacity_setup_failed");
    const size_t capacities[] = {0, 1003, 1004, 1075, 1076, 1155, 1156, 2159};
    for (size_t index = 0; index < sizeof(capacities) / sizeof(capacities[0]); ++index)
    {
        ++context->cases;
        memset(storage->serialized, 0x7d, OBJECT_BYTES);
        memset(storage->expected, 0x7d, OBJECT_BYTES);
        size_t used = 999;
        CHECK(!cache_widget_graph_serialize(&graph, storage->serialized, capacities[index], &used, &result) &&
              result.error == CACHE_WIDGET_CAPACITY && result.required == SERIAL_BYTES && used == 999 &&
              memcmp(storage->serialized, storage->expected, OBJECT_BYTES) == 0,
              "incomplete_serial_capacity_no_prefix_write");
    }
    ++context->cases;
    size_t used = 999;
    memcpy(storage->expected, storage->placement[0], OBJECT_BYTES);
    CHECK(!cache_widget_graph_serialize(&graph, storage->owner.span + ROOT_A, SERIAL_BYTES, &used, &result) &&
          result.error == CACHE_WIDGET_OVERLAP && used == 999 &&
          memcmp(storage->placement[0], storage->expected, OBJECT_BYTES) == 0,
          "serializer_source_alias_rejected_before_io");
    struct cache_widget_projection node, node_before;
    struct cache_widget_element item, item_before;
    memset(&node, 0x91, sizeof(node));
    memset(&item, 0x91, sizeof(item));
    memcpy(&node_before, &node, sizeof(node));
    memcpy(&item_before, &item, sizeof(item));
    ++context->cases;
    CHECK(!cache_widget_graph_node(&graph, 2, &node, &result) && result.error == CACHE_WIDGET_COUNT &&
          memcmp(&node, &node_before, sizeof(node)) == 0, "node_index_bound_atomic");
    CHECK(!cache_widget_graph_element(&graph, 0, CACHE_WIDGET_EVENT, 1, &item, &result) &&
          result.error == CACHE_WIDGET_COUNT && memcmp(&item, &item_before, sizeof(item)) == 0,
          "element_index_bound_atomic");
    return release_owner(context, storage);
}

static int preflight_cases(struct widget_context *context, struct widget_storage *storage)
{
    context->name = "workspace_alignment_source_alias_and_size_helper_atomicity";
    authored_blob(storage->authored);
    struct cache_arena_handle handle;
    struct cache_widget_graph graph, before;
    struct cache_widget_result result;
    if (bind(context, storage, 0, 0, &handle))
        return 1;
    size_t needed = 0;
    int accepted = cache_widget_workspace_size(2, &needed, &result);
    CHECK(accepted && needed == 4 * sizeof(struct cache_widget_projection),
          "both_published_and_scratch_halves_charged");
    if (!accepted)
        return abort_case(context, "preflight_workspace_size_failed");
    memset(&graph, 0x93, sizeof(graph));
    memcpy(&before, &graph, sizeof(graph));
    memcpy(storage->expected, storage->placement[0], OBJECT_BYTES);
    ++context->cases;
    CHECK(!cache_widget_graph_decode(&storage->owner, &handle, BLOB_BYTES, BASE, DATUM_A,
              storage->owner.span + 4096, needed, 2, &graph, &result) && result.error == CACHE_WIDGET_OVERLAP &&
          memcmp(&graph, &before, sizeof(graph)) == 0 &&
          memcmp(storage->placement[0], storage->expected, OBJECT_BYTES) == 0,
          "scratch_alias_cannot_modify_source_before_decode");
    memset(storage->workspace, 0x72, storage->workspace_bytes);
    memcpy(storage->published_before, storage->workspace, storage->workspace_bytes);
    ++context->cases;
    CHECK(!cache_widget_graph_decode(&storage->owner, &handle, BLOB_BYTES, BASE, DATUM_A,
              (unsigned char *)storage->workspace + 1, needed, 2, &graph, &result) &&
          result.error == CACHE_WIDGET_WORKSPACE && memcmp(&graph, &before, sizeof(graph)) == 0 &&
          memcmp(storage->workspace, storage->published_before, storage->workspace_bytes) == 0,
          "unaligned_typed_workspace_rejected_without_any_write");
    const size_t invalid_capacities[] = {0, 65536, SIZE_MAX};
    for (size_t index = 0; index < 3; ++index)
    {
        ++context->cases;
        size_t unchanged = 123;
        CHECK(!cache_widget_workspace_size(invalid_capacities[index], &unchanged, &result) &&
              result.error == CACHE_WIDGET_WORKSPACE && unchanged == 123,
              "unsupported_workspace_request_size_output_atomic");
    }
    return release_owner(context, storage);
}

int wii_cache_widget_fixture(FILE *report, int collect)
{
    struct widget_context context_value = {report, "allocation", 0, 0, 0, collect};
    struct widget_context *context = &context_value;
    struct widget_storage storage;
    struct cache_widget_result result;
    int aborted = 0;
    memset(&storage, 0, sizeof(storage));
    if (!report)
        return 1;
    if (!cache_widget_workspace_size(NODE_CAPACITY, &storage.workspace_bytes, &result))
        return abort_case(context, "workspace_size_failed");
    storage.placement[0] = malloc(OBJECT_BYTES);
    storage.placement[1] = malloc(OBJECT_BYTES);
    storage.authored = malloc(BLOB_BYTES);
    storage.expected = malloc(OBJECT_BYTES);
    storage.serialized = malloc(OBJECT_BYTES);
    storage.published_before = malloc(OBJECT_BYTES);
    storage.workspace = malloc(storage.workspace_bytes);
    if (!storage.placement[0] || !storage.placement[1] || !storage.authored || !storage.expected ||
        !storage.serialized || !storage.published_before || !storage.workspace)
        aborted = abort_case(context, "authored_fixture_allocation_failed");
    else
    {
        authored_blob(storage.authored);
        aborted = lifetime_cases(context, &storage) || malformed_cases(context, &storage) ||
                  limits_cases(context, &storage) || depth_cases(context, &storage) ||
                  capacity_cases(context, &storage) || preflight_cases(context, &storage);
    }
    if (storage.owner.live)
    {
        struct cache_arena_result arena_result;
        if (!cache_arena_owner_release(&storage.owner, &arena_result))
            aborted = abort_case(context, "cleanup_release_failed");
    }
    free(storage.placement[0]);
    free(storage.placement[1]);
    free(storage.authored);
    free(storage.expected);
    free(storage.serialized);
    free(storage.published_before);
    free(storage.workspace);
    if (fprintf(report, "CACHE_WIDGET SUMMARY cases=%u checks=%u failures=%u aborted=%d "
                "projection_size=%lu root=1004 event=72 child=80 partial=1\n",
                context->cases, context->checks, context->failures, aborted,
                (unsigned long)sizeof(struct cache_widget_projection)) < 0 || fflush(report) != 0)
        return 1;
    return aborted || context->failures ? 1 : 0;
}
