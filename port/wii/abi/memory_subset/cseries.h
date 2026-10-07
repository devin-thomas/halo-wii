#ifndef WII_PACKET_CSERIES_SERVICES_H
#define WII_PACKET_CSERIES_SERVICES_H
/* Authored diagnostic services for the actual packet metadata header. */
#include "shim.h"
#include <stdarg.h>
#define __CSERIES_H
#define NONE -1
#define MAXIMUM_STRING_SIZE 0x2000
#define cseries_match_assert match_assert
long packet_strnlen(const char *, long);
char *csstrncpy(char *, const char *, unsigned long);
char *csstrcpy(char *, const char *);
#endif
