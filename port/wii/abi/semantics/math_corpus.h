#ifndef WII_ABI_MATH_CORPUS_H
#define WII_ABI_MATH_CORPUS_H
#include "report.h"

/* Deterministic corpus digests of the shared musl maths functions, actual
   engine real/matrix/random routines and gameplay expressions. Digests are
   compared host vs PPC; there is no authored oracle for these values. */
void wii_abi_math_corpus(struct wii_abi_report *report);

#endif
