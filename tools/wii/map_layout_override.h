/* Forced into the upstream tag schema units by tools/wii/map_layouts.py
 * (HWI-015B) when it exports which C type each schema definition is. Nothing
 * else changes: tag_schema.h is included here first (its guard keeps the
 * units' own include from redefining anything), then TAG_SCHEMA_DEFINITION
 * stores its type's spelling after the name's terminator, behind a marker,
 * so the name a definition compares and prints is unchanged.
 *
 * Built only for the upstream i686 ABI, on the host; never in the game. */
#ifndef WII_MAP_LAYOUT_OVERRIDE_H
#define WII_MAP_LAYOUT_OVERRIDE_H

#include "cseries.h"
#include "tag_schema.h"

#define MAP_LAYOUT_TYPE_MARKER "\x1fTYPE:"

#undef TAG_SCHEMA_DEFINITION
#define TAG_SCHEMA_DEFINITION(name, type, fields) \
	{ #name "\0" MAP_LAYOUT_TYPE_MARKER #type, sizeof(type), (fields) }

#endif
