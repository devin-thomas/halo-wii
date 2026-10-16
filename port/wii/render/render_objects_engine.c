/*
RENDER_OBJECTS_ENGINE.C

The engine side of the Wii rasterizer's object models (HWI-082,
wii_render.h). Compiled as an engine unit (the engine's headers and flags),
in engine.dol only, beside render_engine.c.

What wii_objects.c and models.c do for a frame, read only:
- find_rendered_objects: the collideable then the noncollideable objects of
  each rendered cluster whose render bounding sphere its frustum holds
  (structure_visibility_find_objects), at most MAXIMUM_RENDERED_OBJECTS. The
  engine's own marker (object_marker_begin, object_mark_function) writes the
  game state (global_object_marker, each object's magic_number), so the
  marks here are a bit vector of this unit's, by absolute object index.
- render_object_list: each object, then its children, by next_object_index;
  the local player's unit and its children are not drawn when the camera is
  in its first person (object_is_first_person_camera), nor invisible
  objects' models (their children are).
- render_model: the level of detail in pixels
  (render_frustum_sphere_diameter_in_pixels; REAL_MAX for a cinematic's
  movie star), the detail cutoff, the geometry of each region's permutation
  at that level of detail, each part whose shader is valid for models and
  that is not stripped, in its pass: transparent shaders' parts last, the
  alpha-blended decals of model shaders between. The node matrices are the
  object's (object_get_node_matrices: as the game computed them, with their
  scale; while main_loop draws, the ports' interpolated pose) times each
  node's runtime_default_inverse_matrix, as render_model's relative node
  matrices, then as the vertex shader's constants
  (rasterizer_xbox.c's node_matrix_constants).
Not used: object_get_cached_render_lighting and object_get_cached_render_state
(they write cached_render_state_index and the cache), the camera effect
matrix (it writes player effects), shadows, widgets, model effects (active
camouflage, modifiers), change colours and function values. A model or
part outside what is drawn here is refused and counted, never drawn another
way.
HWI-084: render_model's walk (detail level, region permutations, each part
in its pass, the node matrices once a model has a part in the pass) is
shared with the first-person weapon (render_first_person_engine.c), through
render_models_engine.h.
*/

#include "cseries.h"
#include "math/real_math.h"
#include "game/players.h"
#include "render/render.h"
#include "render/render_cameras.h"
#include "render/render_cameras_internal.h"
#include "objects/objects.h"
#include "objects/object_definitions.h"
#include "models/model_definitions.h"
#include "shaders/shader_definitions.h"
#include "shaders/shaders.h"
#include "camera/director.h"
#include "camera/camera_scripting.h"
#include "cutscene/cinematics.h"
#include "rasterizer/rasterizer_geometry.h"
#include "tag_files/tag_files.h"
#include "bitmaps/bitmap_group.h"
#include "bitmaps/bitmap_group_lookup.h"

#include "wii_render.h"
#include "render_models_engine.h"
#include "../engine/wii_platform.h"

/* (structure_visibility.c's; wii_objects.c declares it so too) */
short structure_visibility_find_objects(
	long *object_indices,
	short maximum_object_count,
	long (*get_first_object_function)(long *reference_index, short cluster_index),
	long (*get_next_object_function)(long *reference_index),
	void (*get_bounding_sphere_function)(long object_index, real_point3d *center, real *radius),
	boolean (*unmarked_function)(long object_index),
	boolean (*mark_function)(long object_index));

/* ---------- constants */

enum
{
	RENDER_OBJECTS_DETAIL_LEVELS = 5,
	/* models.c's (private there) */
	RENDER_OBJECTS_PART_STRIPPED_BIT = 0,
	RENDER_OBJECTS_SHADER_TYPE_MODEL = 4,
	RENDER_OBJECTS_SHADER_MODEL_TWO_SIDED_BIT = 1,
	RENDER_OBJECTS_SHADER_MODEL_DECAL_BIT = 3,
	/* rasterizer_set_texture(0, 0, ...): the base map bitmap type, 2D */
	RENDER_OBJECTS_BITMAP_TYPE_2D = 0,
	/* NUMBER_OF_SHADER_ANIMATION_SOURCES of rasterizer_xbox_models.c */
	RENDER_OBJECTS_SHADER_ANIMATION_SOURCES = 5,
	/* object children deeper than this are not walked (counted): the
	engine's own walk recurses without a bound */
	RENDER_OBJECTS_MAXIMUM_DEPTH = 32,
};

/* ---------- structures */

/* models.c's (private there) */
struct render_objects_model_geometry
{
	byte reserved[0x24];
	struct tag_block parts;
};

struct render_objects_model_geometry_part
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

struct render_objects_shader_reference
{
	struct tag_reference shader;
	short permutation_index;
	word pad;
	long unused[3];
};

/* HWI-086: shader_model_definition of rasterizer_xbox_models.c (the
offsets it asserts, asserted again below) */
struct render_objects_shader_model
{
	struct shader shader;
	word flags;
	short type;
	byte reserved04[0xC];
	real translucency;
	byte reserved14[0x10];
	short change_color_source;
	byte reserved26[0x1E];
	word self_illumination_flags;
	short pad46;
	short self_illumination_color_source;
	short self_illumination_animation_function;
	real self_illumination_animation_period;
	real_rgb_color self_illumination_animation_color_lower_bound;
	real_rgb_color self_illumination_animation_color_upper_bound;
	byte reserved68[0xC];
	real map_u_scale;
	real map_v_scale;
	struct tag_reference base_map;
	byte reserved8C[8];
	struct tag_reference multipurpose_map;
	byte reservedA4[8];
	short detail_function;
	short detail_mask;
	real detail_map_scale;
	struct tag_reference detail_map;
	real detail_map_v_scale;
	byte reservedC8[0xC];
	struct shader_texture_animation texture_animation;
	byte reserved10C[8];
	real reflection_falloff_distance;
	real reflection_cutoff_distance;
	real perpendicular_brightness;
	real_rgb_color perpendicular_tint_color;
	real parallel_brightness;
	real_rgb_color parallel_tint_color;
	struct tag_reference reflection_cube_map;
};

/* a cache map's vertex and index buffer descriptor (the XDK's D3DResource:
Common, Data, Lock): Data the buffer's payload, which the Wii's staging
relocates into the tag slot */
struct render_objects_buffer_descriptor
{
	unsigned long common;
	unsigned long data;
	unsigned long lock;
};

typedef char render_objects_geometry_size[sizeof(struct render_objects_model_geometry) == 0x30 ? 1 : -1];
typedef char render_objects_part_size[sizeof(struct render_objects_model_geometry_part) == 0x68 ? 1 : -1];
typedef char render_objects_part_triangles[
	offsetof(struct render_objects_model_geometry_part, triangle_buffer) == 0x44 ? 1 : -1];
typedef char render_objects_part_vertices[
	offsetof(struct render_objects_model_geometry_part, vertex_buffer) == 0x54 ? 1 : -1];
typedef char render_objects_shader_reference_size[sizeof(struct render_objects_shader_reference) == 0x20 ? 1 : -1];
typedef char render_objects_pointer_size[sizeof(void *) == sizeof(unsigned long) ? 1 : -1];
typedef char render_objects_shader_model_flags[offsetof(struct render_objects_shader_model, flags) == 0x28 ? 1 : -1];
typedef char render_objects_shader_model_change_color[
	offsetof(struct render_objects_shader_model, change_color_source) == 0x4C ? 1 : -1];
typedef char render_objects_shader_model_self_illumination[
	offsetof(struct render_objects_shader_model, self_illumination_animation_color_upper_bound) == 0x84 ? 1 : -1];
typedef char render_objects_shader_model_map_scale[
	offsetof(struct render_objects_shader_model, map_u_scale) == 0x9C ? 1 : -1];
typedef char render_objects_shader_model_base_map[
	offsetof(struct render_objects_shader_model, base_map.index) == 0xB0 ? 1 : -1];
typedef char render_objects_shader_model_multipurpose_map[
	offsetof(struct render_objects_shader_model, multipurpose_map.index) == 0xC8 ? 1 : -1];
typedef char render_objects_shader_model_detail_map[
	offsetof(struct render_objects_shader_model, detail_map.index) == 0xE8 ? 1 : -1];
typedef char render_objects_shader_model_texture_animation[
	offsetof(struct render_objects_shader_model, texture_animation) == 0xFC ? 1 : -1];
typedef char render_objects_shader_model_reflection_cube_map[
	offsetof(struct render_objects_shader_model, reflection_cube_map.index) == 0x170 ? 1 : -1];

struct render_objects_globals
{
	short object_count;
	long object_indices[MAXIMUM_RENDERED_OBJECTS];
	unsigned long marks[(MAXIMUM_OBJECTS_PER_MAP + 31) / 32];
	float palette[WII_GX_MODEL_MAXIMUM_NODES][3][4];
	/* this launch's counts */
	unsigned long frames, objects_found, objects_found_max, models_drawn, lod_skipped, first_person_skipped;
	unsigned long node_block_short, data_errors, depth_skipped, frames_at_capacity;
	boolean reported;
};

/* ---------- globals */

static struct render_objects_globals wii_objects;

/* ---------- private code */

static boolean render_objects_unmarked(
	long object_index)
{
	long absolute = DATUM_INDEX_TO_ABSOLUTE_INDEX(object_index);

	return absolute >= 0 && absolute < MAXIMUM_OBJECTS_PER_MAP &&
		!(wii_objects.marks[absolute >> 5] & (1UL << (absolute & 31)));
}

static boolean render_objects_mark(
	long object_index)
{
	long absolute = DATUM_INDEX_TO_ABSOLUTE_INDEX(object_index);

	if (absolute < 0 || absolute >= MAXIMUM_OBJECTS_PER_MAP ||
		(wii_objects.marks[absolute >> 5] & (1UL << (absolute & 31))))
	{
		return FALSE;
	}
	wii_objects.marks[absolute >> 5] |= 1UL << (absolute & 31);

	return TRUE;
}

/* wii_objects.c's object_is_first_person_camera */
static boolean render_objects_first_person_camera(
	long object_index)
{
	long player_index = local_player_get_player_index(render.local_player_index);
	long unit_index = player_index == NONE ? NONE : player_get(player_index)->unit_index;

	return (unit_index == object_index &&
		director_get_perspective(render.local_player_index) == _director_perspective_first_person) ||
		scripted_camera_object_is_first_person_camera(object_index);
}

struct render_objects_palette_context
{
	long object_index;
	struct object_datum const *object;
	struct model const *model;
};

/* render_model's relative node matrices, as the vertex shader's constants:
the object's node matrices (a wii_render_model_palette_proc) */
static int render_objects_palette(
	void *context,
	float (*palette)[3][4])
{
	struct render_objects_palette_context const *objects = (struct render_objects_palette_context const *)context;
	short node_count = (short)MIN(objects->model->nodes.count, WII_GX_MODEL_MAXIMUM_NODES);
	real_matrix4x3 const *node_matrices;
	short node_index;

	if (node_count <= 0)
		return 0;
	/* the object's node matrices hold the model's nodes (an object of another
	model's skeleton is not drawn) */
	if (objects->object->object.node_matrices.size < (long)(node_count * sizeof(real_matrix4x3)))
	{
		wii_objects.node_block_short++;
		return 0;
	}
	node_matrices = object_get_node_matrices(objects->object_index);
	for (node_index = 0; node_index < node_count; node_index++)
	{
		wii_render_model_node_constants(&node_matrices[node_index],
			TAG_BLOCK_GET_ELEMENT(&objects->model->nodes, node_index, struct model_node), palette[node_index]);
	}

	return node_count;
}

/* the pass render_model_parts draws a part's shader in, or NONE */
static short render_objects_part_pass(
	struct shader *shader,
	unsigned long part_flags,
	boolean *two_sided)
{
	short type = shader->base.type;

	*two_sided = FALSE;
	if (!shader_type_is_valid_for_model(type) || TEST_FLAG(part_flags, RENDER_OBJECTS_PART_STRIPPED_BIT))
		return NONE;
	if (shader_type_is_transparent(type))
		return _wii_gx_model_pass_transparent;
	if (type == RENDER_OBJECTS_SHADER_TYPE_MODEL)
	{
		word flags = ((struct render_objects_shader_model *)shader)->flags;

		*two_sided = TEST_FLAG(flags, RENDER_OBJECTS_SHADER_MODEL_TWO_SIDED_BIT) ? TRUE : FALSE;
		if (TEST_FLAG(flags, RENDER_OBJECTS_SHADER_MODEL_DECAL_BIT))
			return _wii_gx_model_pass_decal;
	}

	return _wii_gx_model_pass_solid;
}

static boolean render_objects_colour_nonzero(
	real_rgb_color const *colour)
{
	return colour->red != 0.0f || colour->green != 0.0f || colour->blue != 0.0f;
}

/* HWI-086: what _rasterizer_model_draw binds and computes for the base map
of a shader_model (stage 0), and the features none of this draws */
static void render_objects_part_material(
	struct model const *model,
	struct shader *shader,
	short shader_permutation_index,
	struct wii_gx_model_material *material)
{
	struct render_objects_shader_model const *shader_model;
	struct shader_texture_animation const *animation;

	material->base = _wii_gx_model_base_not_model_shader;
	material->base_tag = -1;
	if (shader->base.type != RENDER_OBJECTS_SHADER_TYPE_MODEL)
		return;
	shader_model = (struct render_objects_shader_model const *)shader;
	material->shader_flags = shader_model->flags;
	material->base = _wii_gx_model_base_default;
	if (shader_model->base_map.index != NONE)
	{
		long bitmap_count = bitmap_group_get(shader_model->base_map.index)->bitmaps.count;

		if (bitmap_count > 0)
		{
			/* rasterizer_set_texture: bitmap_sequence_index % bitmap_count */
			long bitmap_index = shader_permutation_index % bitmap_count;
			struct bitmap_data *bitmap = bitmap_index >= 0 ?
				bitmap_group_try_and_get_bitmap(shader_model->base_map.index, (short)bitmap_index) : NULL;

			if (!bitmap)
				material->base = _wii_gx_model_base_bad_bitmap;
			else if (bitmap->type != RENDER_OBJECTS_BITMAP_TYPE_2D)
				material->base = _wii_gx_model_base_not_2d;
			else
			{
				material->base = _wii_gx_model_base_bitmap;
				material->base_tag = DATUM_INDEX_TO_ABSOLUTE_INDEX(shader_model->base_map.index);
				material->base_bitmap = (int)bitmap_index;
				material->base_width = bitmap->width;
				material->base_height = bitmap->height;
			}
		}
	}
	/* the scales shader_texture_animation_evaluate is given: the model
	base_map_scale times the shader map scales (render_model_material.h: the
	rows of a static animation) */
	material->texcoord[0][0] = model->base_map_scale.i * shader_model->map_u_scale;
	material->texcoord[0][1] = 0.0f;
	material->texcoord[0][2] = 0.0f;
	material->texcoord[1][0] = 0.0f;
	material->texcoord[1][1] = model->base_map_scale.j * shader_model->map_v_scale;
	material->texcoord[1][2] = 0.0f;
	animation = &shader_model->texture_animation;
	material->features = 0;
	if (shader_model->detail_map.index != NONE)
		material->features |= WII_GX_MODEL_FEATURE_DETAIL_MAP;
	if (shader_model->multipurpose_map.index != NONE)
		material->features |= WII_GX_MODEL_FEATURE_MULTIPURPOSE_MAP;
	if (shader_model->reflection_cube_map.index != NONE)
		material->features |= WII_GX_MODEL_FEATURE_REFLECTION_CUBE_MAP;
	if (render_objects_colour_nonzero(&shader_model->self_illumination_animation_color_lower_bound) ||
		render_objects_colour_nonzero(&shader_model->self_illumination_animation_color_upper_bound))
	{
		material->features |= WII_GX_MODEL_FEATURE_SELF_ILLUMINATION;
	}
	if (shader_model->change_color_source > 0 &&
		shader_model->change_color_source < RENDER_OBJECTS_SHADER_ANIMATION_SOURCES)
	{
		material->features |= WII_GX_MODEL_FEATURE_CHANGE_COLOR;
	}
	if (animation->u_scale != 0.0f || animation->v_scale != 0.0f || animation->r_scale != 0.0f)
		material->features |= WII_GX_MODEL_FEATURE_TEXTURE_ANIMATION;
}

/* render_model and render_model_parts for one object and one pass */
static void render_objects_model(
	long object_index,
	struct object_datum const *object,
	int pass)
{
	struct object_definition *definition = object_definition_get(object->definition_index);
	long model_index = definition->object.model.index;
	struct render_objects_palette_context context;
	real level_of_detail_pixels;
	int detail_level;

	if (model_index == NONE)
		return;
	/* object_get_level_of_detail_pixels */
	if (cinematic_in_progress() && TEST_FLAG(object->object.flags, _object_movie_star_bit))
		level_of_detail_pixels = REAL_MAX;
	else
		level_of_detail_pixels = render_frustum_sphere_diameter_in_pixels(&render.frustum,
			&object->object.bounding_sphere_center, object->object.bounding_sphere_radius);
	detail_level = wii_render_model_detail_level(model_index, level_of_detail_pixels);
	if (detail_level < 0)
	{
		wii_objects.lod_skipped += pass == _wii_gx_model_pass_solid;
		return;
	}
	context.object_index = object_index;
	context.object = object;
	context.model = model_definition_get(model_index);
	/* (render_model_parts reads each permutation as a char: NONE is none) */
	if (wii_render_model_parts(model_index, (signed char const *)object->object.region_permutations,
		MAXIMUM_REGIONS_PER_OBJECT, object->object.forced_shader_permutation_index, detail_level, pass,
		render_objects_palette, &context, &wii_objects.data_errors) > 0)
	{
		wii_objects.models_drawn += pass == _wii_gx_model_pass_solid;
	}
}

/* ---------- the model walk shared with the first-person weapon (render_models_engine.h) */

void wii_render_model_node_constants(
	real_matrix4x3 const *node_matrix,
	struct model_node const *node,
	float (*constants)[4])
{
	real_matrix4x3 relative;

	matrix4x3_multiply(node_matrix, &node->runtime_default_inverse_matrix, &relative);
	constants[0][0] = relative.scale * relative.forward.i;
	constants[0][1] = relative.scale * relative.left.i;
	constants[0][2] = relative.scale * relative.up.i;
	constants[0][3] = relative.position.x;
	constants[1][0] = relative.scale * relative.forward.j;
	constants[1][1] = relative.scale * relative.left.j;
	constants[1][2] = relative.scale * relative.up.j;
	constants[1][3] = relative.position.y;
	constants[2][0] = relative.scale * relative.forward.k;
	constants[2][1] = relative.scale * relative.left.k;
	constants[2][2] = relative.scale * relative.up.k;
	constants[2][3] = relative.position.z;
}

int wii_render_model_detail_level(
	long model_index,
	real level_of_detail_pixels)
{
	struct model *model = model_definition_get(model_index);
	short detail_level;

	if (!(level_of_detail_pixels >= model->detail_cutoff_pixels[0]))
		return -1;
	detail_level = RENDER_OBJECTS_DETAIL_LEVELS - 1;
	while (detail_level > 0 && level_of_detail_pixels < model->detail_cutoff_pixels[detail_level])
		detail_level--;

	return detail_level;
}

long wii_render_model_parts(
	long model_index,
	signed char const *permutations,
	short permutation_count,
	short forced_shader_permutation_index,
	int detail_level,
	int pass,
	wii_render_model_palette_proc palette,
	void *context,
	unsigned long *data_errors)
{
	struct model *model = model_definition_get(model_index);
	short region_count = (short)MIN(model->regions.count, permutation_count);
	short region_index;
	long handed = 0;
	int node_count;
	boolean begun = FALSE;

	if (detail_level < 0 || detail_level >= RENDER_OBJECTS_DETAIL_LEVELS)
		return 0;
	/* Refuse an unsupported skeleton whole. The object's palette clamps
	its local work to the buffer capacity; that must not make a larger
	model appear to be a supported, partially populated skeleton. */
	if (model->nodes.count < 1 || model->nodes.count > WII_GX_MODEL_MAXIMUM_NODES)
	{
		*data_errors += pass == _wii_gx_model_pass_solid;
		return -1;
	}
	for (region_index = 0; region_index < region_count; region_index++)
	{
		struct model_region *region = TAG_BLOCK_GET_ELEMENT(&model->regions, region_index, struct model_region);
		signed char permutation_index = permutations[region_index];
		struct model_region_permutation *permutation;
		struct render_objects_model_geometry *geometry;
		short geometry_index;
		short part_index;

		if (permutation_index == NONE)
			continue;
		if (!VALID_INDEX(permutation_index, region->permutations.count))
		{
			*data_errors += pass == _wii_gx_model_pass_solid;
			continue;
		}
		permutation = TAG_BLOCK_GET_ELEMENT(&region->permutations, permutation_index, struct model_region_permutation);
		geometry_index = permutation->geometry_indices[detail_level];
		if (geometry_index == NONE)
			continue;
		if (!VALID_INDEX(geometry_index, model->geometries.count))
		{
			*data_errors += pass == _wii_gx_model_pass_solid;
			continue;
		}
		geometry = TAG_BLOCK_GET_ELEMENT(&model->geometries, geometry_index, struct render_objects_model_geometry);
		for (part_index = 0; part_index < geometry->parts.count; part_index++)
		{
			struct render_objects_model_geometry_part *part = TAG_BLOCK_GET_ELEMENT(&geometry->parts, part_index,
				struct render_objects_model_geometry_part);
			struct render_objects_shader_reference *reference;
			struct render_objects_buffer_descriptor const *vertex_descriptor;
			struct render_objects_buffer_descriptor const *index_descriptor;
			struct wii_gx_model_part description;
			boolean two_sided;
			short part_pass;

			if (!VALID_INDEX(part->shader_index, model->shaders.count))
			{
				*data_errors += pass == _wii_gx_model_pass_solid;
				continue;
			}
			reference = TAG_BLOCK_GET_ELEMENT(&model->shaders, part->shader_index,
				struct render_objects_shader_reference);
			if (reference->shader.index == NONE)
				continue;
			part_pass = render_objects_part_pass(shader_definition_get(reference->shader.index), part->flags,
				&two_sided);
			if (part_pass != pass)
				continue;
			if (!begun)
			{
				begun = TRUE;
				/* the node matrices, once the model has a part in this pass */
				node_count = palette(context, wii_objects.palette);
				if (node_count < 1 || node_count > WII_GX_MODEL_MAXIMUM_NODES ||
					!wii_gx_model_begin((const float (*)[3][4])wii_objects.palette, node_count))
				{
					return -1;
				}
			}
			vertex_descriptor = (struct render_objects_buffer_descriptor const *)part->vertex_buffer.hardware_format;
			index_descriptor = (struct render_objects_buffer_descriptor const *)part->triangle_buffer.hardware_format;
			csmemset(&description, 0, sizeof(description));
			description.vertices = vertex_descriptor ? (const void *)vertex_descriptor->data : NULL;
			description.vertex_count = part->vertex_buffer.count;
			description.vertex_type = part->vertex_buffer.type;
			description.indices = index_descriptor ? (const void *)index_descriptor->data : NULL;
			description.strip = part->triangle_buffer.type == _triangle_buffer_type_precompiled_strip;
			description.index_count = part->triangle_buffer.type == _triangle_buffer_type_precompiled_strip ?
				(part->triangle_buffer.count > 0 ? part->triangle_buffer.count + 2 : 0) :
				part->triangle_buffer.type == _triangle_buffer_type_triangles ? 3 * part->triangle_buffer.count : 0;
			description.shader_type = shader_definition_get(reference->shader.index)->base.type;
			description.two_sided = two_sided;
			/* HWI-086: the permutation render_model_parts passes (the forced one, when not 0) */
			render_objects_part_material(model, shader_definition_get(reference->shader.index),
				forced_shader_permutation_index ? forced_shader_permutation_index : reference->permutation_index,
				&description.material);
			/* (the GX side checks the pointers lie in the tag slot's tag data, and
			refuses whatever is not its one vertex profile) */
			wii_gx_model_part_draw(&description);
			handed++;
		}
	}
	if (begun)
		wii_gx_model_end();

	return handed;
}

/* render_object_list for one pass */
static void render_objects_list(
	long object_index,
	int pass,
	short depth)
{
	if (depth >= RENDER_OBJECTS_MAXIMUM_DEPTH)
	{
		wii_objects.depth_skipped += pass == _wii_gx_model_pass_solid;
		return;
	}
	while (object_index != NONE)
	{
		struct object_datum *object = object_get(object_index);

		if (!render_objects_first_person_camera(object_index) || render.camera.mirrored)
		{
			if (!TEST_FLAG(object->object.flags, _object_invisible_bit))
				render_objects_model(object_index, object, pass);
			if (object->object.first_child_object_index != NONE)
				render_objects_list(object->object.first_child_object_index, pass, (short)(depth + 1));
		}
		else
		{
			wii_objects.first_person_skipped += pass == _wii_gx_model_pass_solid;
		}
		object_index = object->object.next_object_index;
	}
}

/* ---------- public code */

void wii_render_objects_collect(
	void)
{
	short index;

	wii_objects.object_count = structure_visibility_find_objects(wii_objects.object_indices,
		MAXIMUM_RENDERED_OBJECTS, cluster_get_first_collideable_object, cluster_get_next_collideable_object,
		object_get_render_bounding_sphere, render_objects_unmarked, render_objects_mark);
	wii_objects.object_count += structure_visibility_find_objects(
		&wii_objects.object_indices[wii_objects.object_count],
		(short)(MAXIMUM_RENDERED_OBJECTS - wii_objects.object_count), cluster_get_first_noncollideable_object,
		cluster_get_next_noncollideable_object, object_get_render_bounding_sphere, render_objects_unmarked,
		render_objects_mark);
	/* the marks are cleared for the next frame (only those set) */
	for (index = 0; index < wii_objects.object_count; index++)
	{
		long absolute = DATUM_INDEX_TO_ABSOLUTE_INDEX(wii_objects.object_indices[index]);

		wii_objects.marks[absolute >> 5] &= ~(1UL << (absolute & 31));
	}
	wii_objects.frames++;
	wii_objects.objects_found += wii_objects.object_count;
	if ((unsigned long)wii_objects.object_count > wii_objects.objects_found_max)
		wii_objects.objects_found_max = wii_objects.object_count;
	if (wii_objects.object_count == MAXIMUM_RENDERED_OBJECTS)
		wii_objects.frames_at_capacity++;
}

void wii_render_objects_draw(
	int pass)
{
	short index;

	for (index = 0; index < wii_objects.object_count; index++)
	{
		long object_index = wii_objects.object_indices[index];
		struct object_datum *object = object_get(object_index);

		/* render_object: an invisible object without children draws nothing
		(widgets, which it would light, are not drawn) */
		if (TEST_FLAG(object->object.flags, _object_invisible_bit) && object->object.first_child_object_index == NONE)
			continue;
		render_objects_list(object_index, pass, 0);
	}
	if (pass == NUMBER_OF_WII_GX_MODEL_PASSES - 1 && !wii_objects.reported && wii_objects.object_count > 0)
	{
		wii_objects.reported = TRUE;
		wii_render_objects_report("first");
	}
}

void wii_render_objects_report(
	char const *when)
{
	wii_log("RENDER_OBJECTS when=%s frames=%lu found_now=%d found=%lu found_max=%lu at_capacity=%lu maximum=%d "
		"models_solid_pass=%lu lod_skipped=%lu first_person_skipped=%lu node_block_short=%lu data_errors=%lu depth_skipped=%lu "
		"marks=render_local state_writes=none\n", when, wii_objects.frames, wii_objects.object_count,
		wii_objects.objects_found, wii_objects.objects_found_max, wii_objects.frames_at_capacity,
		MAXIMUM_RENDERED_OBJECTS, wii_objects.models_drawn, wii_objects.lod_skipped,
		wii_objects.first_person_skipped, wii_objects.node_block_short, wii_objects.data_errors,
		wii_objects.depth_skipped);
}
