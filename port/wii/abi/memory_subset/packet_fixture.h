#ifndef WII_PACKET_FIXTURE_H
#define WII_PACKET_FIXTURE_H
#include <stdio.h>
int wii_packet_compare(FILE *report, int collect_failures);
int wii_packet_policy_subset(FILE *report, int collect_failures);
#endif
