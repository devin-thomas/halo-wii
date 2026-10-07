#include "packet_array_policy.h"
#include <limits.h>

const char *packet_array_error_name(enum packet_array_error error)
{
    switch (error) {
    case PACKET_ARRAY_OK: return "ok";
    case PACKET_ARRAY_ARGUMENT: return "invalid_argument_or_owned_plan";
    case PACKET_ARRAY_SCHEMA: return "invalid_schema";
    case PACKET_ARRAY_MISSING_END: return "missing_child_or_root_end";
    case PACKET_ARRAY_EXTENT: return "native_reserve_overflow";
    case PACKET_ARRAY_SIZE: return "stable_native_size_mismatch";
    case PACKET_ARRAY_ALLOCATION: return "workspace_allocation_failed";
    case PACKET_ARRAY_VERSION: return "runtime_version_outside_definition";
    case PACKET_ARRAY_CAPACITY: return "insufficient_native_capacity";
    case PACKET_ARRAY_LENGTH: return "wire_length_outside_signed_short";
    case PACKET_ARRAY_COUNT: return "negative_or_excess_count";
    case PACKET_ARRAY_WIRE: return "truncated_wire_or_capacity";
    case PACKET_ARRAY_STRING: return "missing_bounded_string_terminator";
    case PACKET_ARRAY_BUDGET: return "invalid_plan_or_visit_budget";
    case PACKET_ARRAY_OVERLAP: return "overlapping_native_and_wire_buffers";
    }
    return "unknown_array_error";
}

static boolean fail(struct packet_array_result *r, enum packet_array_error error)
{
    r->error = error;
    return FALSE;
}

struct schema_frame { size_t array_index; uint32_t extent; };

boolean packet_array_compile(const struct data_packet_definition *definition, size_t bound,
                             struct packet_array_plan *plan, struct packet_array_result *r)
{
    match_assert(__FILE__, __LINE__, r);
    *r = (struct packet_array_result){0};
    if (!plan || plan->nodes || !definition || bound == 0 || bound > (size_t)SHRT_MAX)
        return fail(r, PACKET_ARRAY_ARGUMENT);
    if (!definition->name || !definition->fields || definition->size < 0 ||
        definition->version < 0 || definition->version > UNSIGNED_CHAR_MAX) {
        r->schema_error = !definition->name ? PACKET_VERIFY_MISSING_NAME :
            !definition->fields ? PACKET_VERIFY_MISSING_FIELDS :
            definition->size < 0 ? PACKET_VERIFY_NEGATIVE_SIZE : PACKET_VERIFY_VERSION_RANGE;
        return fail(r, PACKET_ARRAY_SCHEMA);
    }
    /* Signed-short schema spans bound heap use and every forward schema visit.
     * Explicit frames avoid a C call stack proportional to adversarial depth. */
    struct packet_array_node *nodes = calloc(bound, sizeof(*nodes));
    struct schema_frame *stack = calloc(bound, sizeof(*stack));
    if (!nodes || !stack) {
        free(nodes); free(stack);
        return fail(r, PACKET_ARRAY_ALLOCATION);
    }
    size_t top = 0, depth = 1, consumed = 0;
    stack[0].array_index = SIZE_MAX;
    for (size_t i = 0; i < bound; ++i) {
        r->field_index = i;
        struct packet_array_node *node = &nodes[i];
        node->field = definition->fields[i];
        node->next = i + 1;
        if (node->field.type == _data_packet_field_end) {
            if (top == 0) { consumed = i + 1; break; }
            uint32_t child = stack[top].extent;
            struct packet_array_node *parent = &nodes[stack[top].array_index];
            --top;
            if (child && (uint32_t)parent->field.count > ((uint32_t)SHRT_MAX - 2) / child) {
                r->field_index = (size_t)(parent - nodes);
                fail(r, PACKET_ARRAY_EXTENT); break;
            }
            parent->child_extent = child;
            parent->extent = 2 + (uint32_t)parent->field.count * child;
            parent->field.size = (short)parent->extent;
            parent->next = i + 1;
            if (parent->extent > (uint32_t)SHRT_MAX - stack[top].extent) {
                fail(r, PACKET_ARRAY_EXTENT); break;
            }
            stack[top].extent += parent->extent;
        } else {
            struct data_packet_field checked = node->field;
            /* The shared flat verifier validates count/gates for array headers
             * using a byte extent; reserve multiplication follows child END. */
            if (checked.type == _data_packet_field_array) checked.type = _data_packet_field_bytes;
            if (!packet_verifier_flat_extent(&checked, &node->extent, &r->schema_error)) {
                fail(r, PACKET_ARRAY_SCHEMA); break;
            }
            if (node->field.type == _data_packet_field_array) {
                if (top + 1 >= bound) { fail(r, PACKET_ARRAY_MISSING_END); break; }
                ++top; stack[top] = (struct schema_frame){i, 0};
                if (depth < top + 1) depth = top + 1;
            } else {
                node->field.size = (short)node->extent;
                if (node->extent > (uint32_t)SHRT_MAX - stack[top].extent) {
                    fail(r, PACKET_ARRAY_EXTENT); break;
                }
                stack[top].extent += node->extent;
            }
        }
    }
    if (!r->error && !consumed) {
        r->field_index = bound;
        fail(r, PACKET_ARRAY_MISSING_END);
    }
    r->native_required = stack[0].extent;
    if (!r->error && r->native_required != (uint32_t)definition->size)
        fail(r, PACKET_ARRAY_SIZE);
    free(stack);
    if (r->error) { free(nodes); return FALSE; }
    *plan = (struct packet_array_plan){nodes, consumed, depth, r->native_required, definition->version};
    return TRUE;
}

void packet_array_destroy(struct packet_array_plan *plan)
{
    if (plan) { free(plan->nodes); *plan = (struct packet_array_plan){0}; }
}

struct codec_frame {
    size_t index, start;
    const byte *source, *source_base;
    byte *destination, *destination_base;
    uint32_t remaining, stride;
};
struct codec {
    const struct packet_array_plan *plan;
    struct data_encoding_state state;
    struct packet_array_result *result;
    boolean decode;
    short version;
};

static boolean write_memory(struct codec *c, const void *source, short count, long selector)
{
    if (c->state.overflow) return FALSE;
    if (!count) return TRUE;
    /* Existing scalar candidate asserts before checking an exhausted cursor. */
    if (c->state.offset == c->state.buffer_size) { c->state.overflow = TRUE; return FALSE; }
    return candidate_encode_memory(&c->state, source, count, selector);
}

static boolean write_count(struct codec *c, short count, short maximum)
{
    if (c->state.buffer_size - c->state.offset < (maximum <= 255 ? 1 : 2)) {
        c->state.overflow = TRUE; return FALSE;
    }
    return candidate_encode_integer(&c->state, count, maximum);
}

static boolean flat_field(struct codec *c, const struct packet_array_node *node,
                          const byte *source, byte *destination)
{
    const struct data_packet_field *f = &node->field;
    long selector = f->type == _data_packet_field_shorts ? -2 :
        f->type == _data_packet_field_longs ? -4 : f->type == _data_packet_field_int64s ? -8 : 1;
    if (f->type == _data_packet_field_pad) return TRUE;
    if (f->type == _data_packet_field_string) {
        size_t limit = (size_t)f->count + 1;
        size_t available = c->decode ? (size_t)(c->state.buffer_size - c->state.offset) : limit;
        size_t scan = available < limit ? available : limit;
        const byte *start = c->decode ? c->state.buffer + c->state.offset : source;
        const byte *end = memchr(start, 0, scan);
        if (!end) {
            c->state.overflow = TRUE;
            return fail(c->result, available < limit ? PACKET_ARRAY_WIRE : PACKET_ARRAY_STRING);
        }
        short length = (short)(end - start + 1);
        if (!c->decode) return write_memory(c, source, length, 1);
        void *decoded = candidate_decode_memory(&c->state, length, 1);
        if (!decoded) return FALSE;
        memcpy(destination, decoded, (size_t)length); return TRUE;
    }
    if (f->type == _data_packet_field_data) {
        short count;
        if (c->decode) {
            count = (short)candidate_decode_integer(&c->state, f->count);
            if (c->state.overflow) return FALSE;
        } else memcpy(&count, source, sizeof(count));
        if (count < 0 || count > f->count) return fail(c->result, PACKET_ARRAY_COUNT);
        if (c->decode) {
            memcpy(destination, &count, sizeof(count));
            void *decoded = candidate_decode_memory(&c->state, count, 1);
            if (!decoded) return FALSE;
            memcpy(destination + 2, decoded, (size_t)count); return TRUE;
        }
        return write_count(c, count, f->count) && write_memory(c, source + 2, count, 1);
    }
    if (!c->decode) return write_memory(c, source, f->count, selector);
    void *decoded = candidate_decode_memory(&c->state, f->count, selector);
    if (!decoded) return FALSE;
    memcpy(destination, decoded, node->extent); return TRUE;
}

static boolean excluded_field(struct codec *c, const struct packet_array_node *node,
                              byte *destination)
{
    const struct data_packet_field *f = &node->field;
    if (c->decode && f->type != _data_packet_field_array)
        return packet_policy_excluded(&c->state, f, destination);
    short span;
    switch (f->type) {
    case _data_packet_field_pad: span = 0; break;
    case _data_packet_field_string: span = 1; break;
    case _data_packet_field_data:
    case _data_packet_field_array: span = f->count <= 255 ? 1 : 2; break;
    default: span = f->count; break;
    }
    if (!c->decode) return write_memory(c, NULL, span, 1);
    if (!candidate_decode_memory(&c->state, span, 1)) return FALSE;
    memset(destination, 0, node->extent); return TRUE;
}

static boolean run(struct codec *c, const void *source, void *destination)
{
    struct codec_frame *frames = calloc(c->plan->depth, sizeof(*frames));
    if (!frames) {
        c->result->wire_used = c->state.offset;
        return fail(c->result, PACKET_ARRAY_ALLOCATION);
    }
    frames[0] = (struct codec_frame){0, 0, source, source, destination, destination, 1, c->plan->extent};
    size_t top = 0;
    uint32_t visits = 0;
    boolean done = FALSE;
    while (!done && c->result->error == PACKET_ARRAY_OK) {
        struct codec_frame *frame = &frames[top];
        if (frame->index >= c->plan->count) { fail(c->result, PACKET_ARRAY_BUDGET); break; }
        size_t index = frame->index;
        const struct packet_array_node *node = &c->plan->nodes[index];
        c->result->field_index = index;
        if (node->field.type == _data_packet_field_end) {
            if (--frame->remaining) {
                if (frame->source_base) frame->source_base += frame->stride;
                if (frame->destination_base) frame->destination_base += frame->stride;
                frame->source = frame->source_base; frame->destination = frame->destination_base;
                frame->index = frame->start;
            } else if (top) --top;
            else done = TRUE;
            continue;
        }
        /* Only non-END visits count: every such field reserves >=1 native byte,
         * array headers reserve >=2, and zero-extent child loops are skipped. */
        if (++visits > c->plan->extent || node->next <= index || node->next >= c->plan->count) {
            fail(c->result, PACKET_ARRAY_BUDGET); break;
        }
        const byte *field_source = frame->source;
        byte *field_destination = frame->destination;
        frame->index = node->next;
        if (frame->source) frame->source += node->extent;
        if (frame->destination) frame->destination += node->extent;
        boolean eligible = c->version >= node->field.minimum_version &&
            (!node->field.maximum_version || c->version <= node->field.maximum_version);
        boolean ok;
        if (!eligible) ok = excluded_field(c, node, field_destination);
        else if (node->field.type != _data_packet_field_array)
            ok = flat_field(c, node, field_source, field_destination);
        else {
            short count;
            if (c->decode) {
                count = (short)candidate_decode_integer(&c->state, node->field.count);
                if (c->state.overflow) break;
            } else memcpy(&count, field_source, sizeof(count));
            if (count < 0 || count > node->field.count) { fail(c->result, PACKET_ARRAY_COUNT); break; }
            if (c->decode) memcpy(field_destination, &count, sizeof(count));
            else if (!write_count(c, count, node->field.count)) break;
            ok = TRUE;
            if (count && node->child_extent) {
                if (top + 1 >= c->plan->depth) { fail(c->result, PACKET_ARRAY_BUDGET); break; }
                ++top;
                const byte *child_source = field_source ? field_source + 2 : NULL;
                byte *child_destination = field_destination ? field_destination + 2 : NULL;
                frames[top] = (struct codec_frame){index + 1, index + 1,
                    child_source, child_source, child_destination, child_destination,
                    (uint32_t)count, node->child_extent};
            }
        }
        if (!ok || c->state.overflow) break;
    }
    free(frames);
    if (!done && !c->result->error) fail(c->result, PACKET_ARRAY_WIRE);
    c->result->wire_used = c->state.offset;
    return done && !c->result->error;
}

static boolean prepare(const struct packet_array_plan *plan, void *wire, long wire_length,
                       long native_capacity, struct packet_array_result *r)
{
    match_assert(__FILE__, __LINE__, r);
    *r = (struct packet_array_result){0};
    if (!plan || !plan->nodes || !plan->count || !plan->depth || !wire)
        return fail(r, PACKET_ARRAY_ARGUMENT);
    r->native_required = plan->extent;
    if (native_capacity < 0 || (uint32_t)native_capacity < plan->extent)
        return fail(r, PACKET_ARRAY_CAPACITY);
    if (wire_length < 0 || wire_length > SHRT_MAX) return fail(r, PACKET_ARRAY_LENGTH);
    return TRUE;
}

static boolean disjoint(const void *native, uint32_t extent, const void *wire, long length,
                        struct packet_array_result *r)
{
    uintptr_t a = (uintptr_t)native, b = (uintptr_t)wire;
    if (extent > UINTPTR_MAX - a || (uintptr_t)length > UINTPTR_MAX - b)
        return fail(r, PACKET_ARRAY_ARGUMENT);
    if (extent && length && a < b + (uintptr_t)length && b < a + extent)
        return fail(r, PACKET_ARRAY_OVERLAP);
    return TRUE;
}

boolean packet_array_encode(const struct packet_array_plan *plan, long version,
                            const void *native, long native_capacity, void *wire,
                            long wire_capacity, struct packet_array_result *r)
{
    if (!prepare(plan, wire, wire_capacity, native_capacity, r)) return FALSE;
    if (!native) return fail(r, PACKET_ARRAY_ARGUMENT);
    if (!disjoint(native, plan->extent, wire, wire_capacity, r)) return FALSE;
    if (version == -1) version = plan->version;
    if (version < 0 || version > plan->version || (!plan->version && version))
        return fail(r, PACKET_ARRAY_VERSION);
    struct codec c = {plan, {0}, r, FALSE, (short)version};
    data_encode_new(&c.state, wire, wire_capacity);
    if (plan->version) {
        byte prefix = (byte)version;
        if (!write_memory(&c, &prefix, 1, 1)) return fail(r, PACKET_ARRAY_WIRE);
    }
    return run(&c, native, NULL);
}

boolean packet_array_decode(const struct packet_array_plan *plan, void *wire, long wire_length,
                            void *native, long native_capacity, short *version,
                            struct packet_array_result *r)
{
    if (!prepare(plan, wire, wire_length, native_capacity, r)) return FALSE;
    if (!native) return fail(r, PACKET_ARRAY_ARGUMENT);
    if (!disjoint(native, plan->extent, wire, wire_length, r)) return FALSE;
    struct codec c = {plan, {0}, r, TRUE, 0};
    data_decode_new(&c.state, wire, wire_length);
    if (plan->version) c.version = (short)candidate_decode_byte(&c.state);
    r->wire_used = c.state.offset;
    if (c.state.overflow) return fail(r, PACKET_ARRAY_WIRE);
    if (version) *version = c.version;
    if (c.version > plan->version) return fail(r, PACKET_ARRAY_VERSION);
    return run(&c, NULL, native);
}
