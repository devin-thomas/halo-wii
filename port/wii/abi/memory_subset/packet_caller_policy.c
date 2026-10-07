#include "packet_caller_policy.h"
#include "bungie_net/common/message_header.h"
#include <limits.h>

static boolean caller_fail(struct packet_caller_result *r, enum packet_caller_error error)
{
    r->error = error;
    return FALSE;
}
const char *packet_caller_error_name(enum packet_caller_error error)
{
    static const char *const names[] = {
        "ok", "argument", "length", "capacity", "overlap", "type", "flags", "identity", "group"
    };
    return (unsigned)error < sizeof(names) / sizeof(names[0]) ? names[error] : "unknown";
}
static boolean caller_prepare(const struct packet_group_plan *g, size_t bound,
                              struct packet_caller_result *r)
{
    match_assert(__FILE__, __LINE__, r);
    *r = (struct packet_caller_result){0};
    if (!packet_group_validate(g, bound, &r->group)) return caller_fail(r, PACKET_CALLER_GROUP);
    return TRUE;
}
static boolean spans(const void *a, long an, const void *b, long bn,
                     struct packet_caller_result *r)
{
    uintptr_t x = (uintptr_t)a, y = (uintptr_t)b;
    if ((uintptr_t)an > UINTPTR_MAX - x || (uintptr_t)bn > UINTPTR_MAX - y)
        return caller_fail(r, PACKET_CALLER_ARGUMENT);
    if (an && bn && x < y + (uintptr_t)bn && y < x + (uintptr_t)an)
        return caller_fail(r, PACKET_CALLER_OVERLAP);
    return TRUE;
}

boolean packet_caller_encode(const struct packet_group_plan *g, size_t bound,
                             const void *native, long native_capacity,
                             void *workspace, long workspace_capacity,
                             void *frame, long frame_capacity, short type,
                             long version, byte flags, struct packet_caller_result *r)
{
    if (!caller_prepare(g, bound, r)) return FALSE;
    if (!native || !workspace || !frame) return caller_fail(r, PACKET_CALLER_ARGUMENT);
    if (native_capacity < 0 || workspace_capacity < 0 || frame_capacity < 0)
        return caller_fail(r, PACKET_CALLER_CAPACITY);
    if (workspace_capacity > SHRT_MAX) return caller_fail(r, PACKET_CALLER_LENGTH);
    if (flags > MESSAGE_FLAG_BITS_MASK) return caller_fail(r, PACKET_CALLER_FLAGS);
    if (type < 0 || type >= g->type_count) return caller_fail(r, PACKET_CALLER_TYPE);
    const struct packet_array_plan *p = g->entries[type].plan;
    long extent = p ? (long)p->extent : 0;
    if (!spans(native, extent, workspace, workspace_capacity, r) ||
        !spans(native, extent, frame, frame_capacity, r) ||
        !spans(workspace, workspace_capacity, frame, frame_capacity, r)) return FALSE;
    short group_size = 0;
    if (!packet_group_encode(g, bound, native, native_capacity, workspace, workspace_capacity,
                             &group_size, type, version, &r->group))
        return caller_fail(r, PACKET_CALLER_GROUP);
    /* A separate signed-short object is widened numerically after validation;
     * writing one union member then reading the larger member is never used. */
    if (group_size < 1 || group_size > MAXIMUM_MESSAGE_SIZE - (long)sizeof(word))
        return caller_fail(r, PACKET_CALLER_LENGTH);
    r->frame_size = (long)group_size + (long)sizeof(word);
    if (r->frame_size > frame_capacity) return caller_fail(r, PACKET_CALLER_CAPACITY);
    word header = (word)((r->frame_size << 4) | (_message_type_packet << 2) | flags);
    byte *out = frame;
    out[0] = (byte)(header >> 8);
    out[1] = (byte)header;
    memcpy(out + sizeof(word), workspace, (size_t)group_size);
    return TRUE;
}

boolean packet_caller_decode(const struct packet_group_plan *g, size_t bound,
                             void *frame, long length, long frame_capacity,
                             void *native, long native_capacity, short expected_type,
                             short expected_class, byte expected_flags, short *version,
                             struct packet_caller_result *r)
{
    if (!caller_prepare(g, bound, r)) return FALSE;
    if (!frame || !native || !version) return caller_fail(r, PACKET_CALLER_ARGUMENT);
    if (frame_capacity < 0 || native_capacity < 0) return caller_fail(r, PACKET_CALLER_CAPACITY);
    if (expected_flags > MESSAGE_FLAG_BITS_MASK) return caller_fail(r, PACKET_CALLER_FLAGS);
    if (expected_type < 0 || expected_type >= g->type_count) return caller_fail(r, PACKET_CALLER_TYPE);
    if (length < (long)(sizeof(word) + sizeof(byte)) || length > MAXIMUM_MESSAGE_SIZE)
        return caller_fail(r, PACKET_CALLER_LENGTH);
    if (length > frame_capacity) return caller_fail(r, PACKET_CALLER_CAPACITY);
    const struct packet_array_plan *p = g->entries[expected_type].plan;
    if (!spans(native, p ? (long)p->extent : 0, frame, length, r)) return FALSE;
    r->frame_size = length;
    const byte *in = frame;
    word header = (word)(((word)in[0] << 8) | in[1]);
    if (GET_MESSAGE_SIZE(header) != length) return caller_fail(r, PACKET_CALLER_LENGTH);
    if (GET_MESSAGE_TYPE(header) != _message_type_packet) return caller_fail(r, PACKET_CALLER_TYPE);
    if (GET_MESSAGE_FLAGS(header) != expected_flags) return caller_fail(r, PACKET_CALLER_FLAGS);
    short selected = in[length - 1];
    if (selected >= g->type_count) return caller_fail(r, PACKET_CALLER_TYPE);
    if (selected != expected_type) return caller_fail(r, PACKET_CALLER_IDENTITY);
    short group_size = (short)(length - (long)sizeof(word)), type = -1;
    if (!packet_group_decode(g, bound, native, native_capacity, (byte *)frame + sizeof(word),
                             group_size, &group_size, &type, version, expected_class, &r->group))
        return caller_fail(r, PACKET_CALLER_GROUP);
    return TRUE;
}
