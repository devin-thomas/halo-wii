#ifndef WII_ABI_ENGINE_PROBE_H
#define WII_ABI_ENGINE_PROBE_H
#include "report.h"

/* engine_probe.c built with the port's engine flags as-is */
void wii_abi_engine_probe_baseline(struct wii_abi_report *report);
/* the same unit built with -fsigned-char (candidate build flag) */
void wii_abi_engine_probe_signed_char(struct wii_abi_report *report);

#endif
