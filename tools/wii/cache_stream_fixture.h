#ifndef WII_CACHE_STREAM_FIXTURE_H
#define WII_CACHE_STREAM_FIXTURE_H
#include <stdio.h>
/* Host tmpfile tests only. GEKKO reports an explicit not-executed scope. */
int wii_cache_stream_fixture(FILE *report, int collect_failures);
#endif
