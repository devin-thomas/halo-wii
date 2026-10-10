/* Host exporter (HWI-015B): the C type of every upstream tag schema definition.
 *
 * Built with the port/linux/game/tag_schema_*.c units and
 * tools/wii/map_layout_override.h forced in, for the upstream i686 ABI only
 * (tools/wii/map_layouts.py). It visits the definitions in the order
 * port/wii/memory_strategy/schema_export.c does (each group list's groups,
 * then every definition reachable from them, first seen first), so its
 * indices are cache_schema_tables.c's. Each line:
 *     DEF <index> <size> <name> <type spelling>
 * The two definitions the schema writes by hand (the control's, which end
 * before the tag's reserved bytes) carry no spelling; their types are named
 * here. Output: text on stdout. */
#include "cseries.h"
#include "tag_schema.h"
#include "devices/device_controls.h"

#undef printf
int printf(const char *, ...);

enum { MAXIMUM_DEFINITIONS = 4096 };

static struct tag_schema_definition const *definitions[MAXIMUM_DEFINITIONS];
static long definition_count;

static int same_text(char const *a, char const *b)
{
	while (*a && *a == *b)
	{
		a++;
		b++;
	}
	return *a == *b;
}

static long definition_id(struct tag_schema_definition const *definition)
{
	long index;

	for (index = 0; index < definition_count; index++)
	{
		if (definitions[index] == definition)
			return index;
	}
	if (definition_count >= MAXIMUM_DEFINITIONS)
		return -1;
	definitions[definition_count] = definition;
	return definition_count++;
}

static void collect(struct tag_schema_definition const *definition)
{
	long first = definition_count;
	struct tag_schema_field const *field;

	if (definition_id(definition) != first)
		return;
	for (field = definition->fields; field->type != _tag_schema_terminator; field++)
	{
		if (field->type == _tag_schema_block || field->type == _tag_schema_struct)
			collect(field->definition);
	}
}

/* the spelling stored after the name's terminator, or the hand-written
definitions' types */
static char const *type_of(struct tag_schema_definition const *definition)
{
	char const *name = definition->name;
	char const *after;
	char const *marker = "\x1fTYPE:";
	long index;

	/* (by name and size: the scenario's control placements are a definition
	named control too, written with the macro) */
	if (same_text(name, "control") &&
		definition->size == (long)offsetof(struct _control_definition, reserved88))
		return "struct _control_definition";
	if (same_text(name, "control_definition") &&
		definition->size == (long)offsetof(struct control_definition, control.reserved88))
		return "struct control_definition";
	after = name;
	while (*after)
		after++;
	after++;
	for (index = 0; marker[index]; index++)
	{
		if (after[index] != marker[index])
			return NULL;
	}
	return after + index;
}

int main(void)
{
	struct tag_schema_group const *const *list;
	struct tag_schema_group const *group;
	long index;

	for (list = tag_schema_group_lists; *list; list++)
	{
		for (group = *list; group->group_tag; group++)
		{
			if (group->definition)
				collect(group->definition);
		}
	}
	if (definition_count >= MAXIMUM_DEFINITIONS)
		return 2;
	for (index = 0; index < definition_count; index++)
	{
		char const *type = type_of(definitions[index]);

		if (!type)
			return 3;
		printf("DEF %ld %ld %s %s\n", index, definitions[index]->size, definitions[index]->name, type);
	}
	return 0;
}

/* The checks are never called; the validator's services they name are not linked. */
