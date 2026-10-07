#ifndef WII_MEMORY_CANDIDATE_H
#define WII_MEMORY_CANDIDATE_H
#include "shim.h"

/* Diagnostic adapter only; the actual engine reference remains linked. */
boolean candidate_encode_memory(struct data_encoding_state *, const void *, short, long);
boolean candidate_encode_integer(struct data_encoding_state *, long, long);
void *candidate_decode_memory(struct data_encoding_state *, short, long);
byte candidate_decode_byte(struct data_encoding_state *);
short candidate_decode_short(struct data_encoding_state *);
long candidate_decode_long(struct data_encoding_state *);
__int64 candidate_decode_int64(struct data_encoding_state *);
long candidate_decode_integer(struct data_encoding_state *, long);
int wii_memory_candidate_subset(FILE *, int);
int wii_memory_candidate_edges(FILE *, int);
#endif
