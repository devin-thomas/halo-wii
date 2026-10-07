#ifndef WII_PACKET_CALLER_REFERENCE_H
#define WII_PACKET_CALLER_REFERENCE_H
#include "packet_caller_policy.h"
#include "bungie_net/common/message_header.h"
struct packet_caller_union_observation {
    long initialized_value, after_write_value;
    short initial_encoded, after_write_encoded;
    byte bytes[4];
};
void packet_caller_reference_network_union(short, struct packet_caller_union_observation *);
void packet_caller_reference_key_union(short, struct packet_caller_union_observation *);
void *caller_reference_allocate(long, boolean, const char *, long);
#endif
