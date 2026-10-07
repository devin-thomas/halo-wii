#ifndef WII_PACKET_SHIM_H
#define WII_PACKET_SHIM_H
#include "cseries.h"
#include "candidate.h"
#include "memory/data_packet_groups.h"
#include "memory/data_packets.h"

void candidate_packet_verify(struct data_packet_definition *);
boolean candidate_packet_encode(struct data_packet_definition *, long, const void *,
                                void *, short *, short);
boolean candidate_packet_decode(struct data_packet_definition *, const void *, short,
                                void *, short *, short *);
void packet_reference_dispatch_decode(struct data_packet_definition *,
                                     struct data_encoding_state *, short, void *);
void packet_candidate_dispatch_decode(struct data_packet_definition *,
                                     struct data_encoding_state *, short, void *);
_Static_assert(sizeof(struct data_packet_field) == 10, "Actual five-short schema layout");
#endif
