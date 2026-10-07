#ifndef WII_PACKET_CALLER_POLICY_H
#define WII_PACKET_CALLER_POLICY_H
#include "packet_group_policy.h"

enum packet_caller_error {
    PACKET_CALLER_OK, PACKET_CALLER_ARGUMENT, PACKET_CALLER_LENGTH,
    PACKET_CALLER_CAPACITY, PACKET_CALLER_OVERLAP, PACKET_CALLER_TYPE,
    PACKET_CALLER_FLAGS, PACKET_CALLER_IDENTITY, PACKET_CALLER_GROUP
};
struct packet_caller_result {
    enum packet_caller_error error;
    struct packet_group_result group;
    long frame_size;
};
const char *packet_caller_error_name(enum packet_caller_error);
/* Diagnostic packet framing only: two-byte BE numeric header, type3, flags0..3.
 * Borrowed group contract and stable array policy remain unchanged. Truthful
 * native/workspace/frame extents and separate immutable control storage required.
 * Workspace retains group partial writes; frame output changes only after full
 * size/capacity checks. No long/short union reinterpretation is used. */
boolean packet_caller_encode(const struct packet_group_plan *, size_t entry_bound,
                             const void *native, long native_capacity,
                             void *workspace, long workspace_capacity,
                             void *frame, long frame_capacity, short type,
                             long version, byte flags, struct packet_caller_result *);
/* Outer size must equal the supplied frame length; extra payload before the
 * group trailer remains accepted. Expected trailer identity is checked before
 * typed native payload IO. Version/partial group effects retain group policy.
 * Header bytes are read explicitly; payload decoding may mutate wire bytes. */
boolean packet_caller_decode(const struct packet_group_plan *, size_t entry_bound,
                             void *frame, long frame_length, long frame_capacity,
                             void *native, long native_capacity, short expected_type,
                             short expected_class, byte expected_flags, short *version,
                             struct packet_caller_result *);
#endif
