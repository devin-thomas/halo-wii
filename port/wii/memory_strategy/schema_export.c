/* Host exporter (HWI-007): flattens the upstream tag validator's schema
 * (port/linux/game/tag_schema_*.c) into plain data for the portable graph walker.
 *
 * Built only for the upstream i686 ABI (tools/wii/export_tag_schema.py), so every
 * offset and size is the compiler's view of the engine's own structures, which is
 * the Xbox cache layout. The checks (_tag_schema_check) are exported as markers
 * and never called. Output: C source on stdout. */
#include "cseries.h"
#include "tag_schema.h"
#include "rasterizer/rasterizer_geometry.h"
#include "structures/structure_bsp_definitions.h"
#include "scenario/scenario_definitions.h"

/* (the game's string services are not linked here) */
static int same_name(char const *a, char const *b)
{
	while (*a && *a == *b)
	{
		a++;
		b++;
	}
	return *a == *b;
}

/* tag_schema_models.c's model part (models.c's), whose buffers its check
(model_geometry_part_check) reads: the same layout, checked by size */
struct schema_export_model_geometry_part
{
	unsigned long flags;
	short shader_index;
	char previous_part_index;
	char next_part_index;
	short centroid_primary_node_index;
	short centroid_secondary_node_index;
	real centroid_primary_node_weight;
	real centroid_secondary_node_weight;
	real_point3d centroid;
	struct tag_block uncompressed_vertices;
	struct tag_block compressed_vertices;
	struct tag_block triangles;
	struct triangle_buffer triangle_buffer;
	struct vertex_buffer vertex_buffer;
};
typedef char schema_export_model_geometry_part_size[
	sizeof(struct schema_export_model_geometry_part) == 0x68 ? 1 : -1];

/* Pointer fields the schema's checks read (and the validator's checks
null or keep) rather than its typed fields: the vertex and index buffers of
model parts (model_geometry_part_check) and bsp materials
(structure_vertex_buffer_check), and where a scenario's structure bsp loads
in the tag cache (its reference's base address, which the loader reads). */
static struct
{
	char const *definition;
	long offset;
} const supplementary_pointers[] =
{
	{"model_geometry_part", offsetof(struct schema_export_model_geometry_part, triangle_buffer.base_address)},
	{"model_geometry_part", offsetof(struct schema_export_model_geometry_part, triangle_buffer.hardware_format)},
	{"model_geometry_part", offsetof(struct schema_export_model_geometry_part, vertex_buffer.base_address)},
	{"model_geometry_part", offsetof(struct schema_export_model_geometry_part, vertex_buffer.hardware_format)},
	{"structure_material", offsetof(struct structure_material, vertices.base_address)},
	{"structure_material", offsetof(struct structure_material, vertices.hardware_format)},
	{"structure_material", offsetof(struct structure_material, lightmap_vertices.base_address)},
	{"structure_material", offsetof(struct structure_material, lightmap_vertices.hardware_format)},
	{"structure_bsp_reference", offsetof(struct scenario_structure_bsp_reference, base_address)},
};

#undef printf
#undef fprintf
int printf(const char *, ...);

enum { MAXIMUM_DEFINITIONS = 4096, MAXIMUM_LISTS = 1024 };

static struct tag_schema_definition const *definitions[MAXIMUM_DEFINITIONS];
static long definition_count;
static unsigned long const *lists[MAXIMUM_LISTS];
static long list_offsets[MAXIMUM_LISTS];
static long list_count, list_words;

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

static long list_id(unsigned long const *groups)
{
	long index;
	long length = 0;

	if (!groups)
		return -1;
	for (index = 0; index < list_count; index++)
	{
		if (lists[index] == groups)
			return list_offsets[index];
	}
	if (list_count >= MAXIMUM_LISTS)
		return -2;
	while (groups[length])
		length++;
	lists[list_count] = groups;
	list_offsets[list_count] = list_words;
	list_words += length + 1;
	return list_offsets[list_count++];
}

/* every definition reachable from the groups, in first-seen order */
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
		else if (field->type == _tag_schema_reference || field->type == _tag_schema_tag_index)
			list_id(field->definition);
	}
}

static void print_tag(unsigned long tag)
{
	printf("0x%08lxu", tag);
}

int main(void)
{
	struct tag_schema_group const *const *list;
	struct tag_schema_group const *group;
	long index, field_total = 0, group_total = 0;

	for (list = tag_schema_group_lists; *list; list++)
	{
		for (group = *list; group->group_tag; group++)
		{
			if (group->definition)
				collect(group->definition);
		}
	}
	if (definition_count >= MAXIMUM_DEFINITIONS || list_count >= MAXIMUM_LISTS)
		return 2;

	printf("/* Generated by tools/wii/export_tag_schema.py from port/linux/game/tag_schema_*.c\n"
		" * (upstream i686 ABI offsets). Do not edit. */\n");
	printf("#include \"cache_schema_tables.h\"\n\n");

	printf("const struct cache_schema_field cache_schema_fields[] = {\n");
	for (index = 0; index < definition_count; index++)
	{
		struct tag_schema_field const *field;

		printf("\t/* %ld %s */\n", index, definitions[index]->name);
		for (field = definitions[index]->fields; field->type != _tag_schema_terminator; field++)
		{
			long link = -1;

			if (field->type == _tag_schema_block || field->type == _tag_schema_struct)
				link = definition_id(field->definition);
			else if (field->type == _tag_schema_reference || field->type == _tag_schema_tag_index)
				link = list_id(field->definition);
			printf("\t{%d, %d, %d, %d, %ld, %ld, %ld, %ld, %ld},\n", field->type, field->flags, field->size,
				field->count, field->offset, field->maximum, field->target_level, field->target_offset, link);
			field_total++;
		}
	}
	printf("};\n\n");

	printf("const struct cache_schema_definition cache_schema_definitions[] = {\n");
	{
		long first = 0;

		for (index = 0; index < definition_count; index++)
		{
			struct tag_schema_field const *field;
			long count = 0;

			for (field = definitions[index]->fields; field->type != _tag_schema_terminator; field++)
				count++;
			printf("\t{%ld, %ld, %ld}, /* %s */\n", definitions[index]->size, first, count, definitions[index]->name);
			first += count;
		}
	}
	printf("};\n\n");

	printf("const uint32_t cache_schema_group_lists[] = {\n");
	for (index = 0; index < list_count; index++)
	{
		unsigned long const *groups = lists[index];

		printf("\t");
		for (; *groups; groups++)
		{
			print_tag(*groups);
			printf(", ");
		}
		printf("0,\n");
	}
	printf("};\n\n");

	/* the validator's lookup order: the first list naming a group wins */
	printf("const struct cache_schema_group cache_schema_groups[] = {\n");
	for (list = tag_schema_group_lists; *list; list++)
	{
		for (group = *list; group->group_tag; group++)
		{
			printf("\t{");
			print_tag(group->group_tag);
			printf(", {");
			print_tag(group->parent_group_tags[0]);
			printf(", ");
			print_tag(group->parent_group_tags[1]);
			printf("}, %ld},\n", group->definition ? definition_id(group->definition) : -1L);
			group_total++;
		}
	}
	printf("};\n\n");

	printf("const struct cache_schema_pointer cache_schema_pointers[] = {\n");
	for (index = 0; index < (long)NUMBEROF(supplementary_pointers); index++)
	{
		long definition;

		for (definition = 0; definition < definition_count; definition++)
		{
			if (same_name(definitions[definition]->name, supplementary_pointers[index].definition))
				break;
		}
		if (definition == definition_count)
			return 3;
		printf("\t{%ld, %ld}, /* %s */\n", definition, supplementary_pointers[index].offset,
			supplementary_pointers[index].definition);
	}
	printf("};\n\n");

	printf("const struct cache_schema_counts cache_schema_counts = {%ld, %ld, %ld, %ld, %ld};\n", field_total,
		definition_count, list_words, group_total, (long)NUMBEROF(supplementary_pointers));
	return 0;
}

/* The checks are never called; the validator's services they name are not linked. */
