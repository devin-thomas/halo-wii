/*
RENDER_MODELS_ENGINE.H

HWI-084: the engine side's model walk (render_objects_engine.c), shared by
the objects (HWI-082) and the first-person weapon (render_first_person_engine.c).
Engine units only (the engine's types); the GX side is wii_render.h's.

wii_render_model_detail_level: render_model's detail cutoff and level of
detail for a size in pixels (-1: below the cutoff, not drawn).
wii_render_model_parts: render_model_parts for one pass of one model at
that level: each region's permutation (permutations[region], a char as the
engine reads it: NONE none; permutation_count of them), its geometry, and
each part whose shader is valid for models and not stripped, in its pass
(transparent shaders' last, model shaders' alpha-blended decals between).
The node matrices come from palette, once the model has a part in the pass:
it fills palette rows (the vertex shader's constants: a node matrix times
its node's runtime_default_inverse_matrix, wii_render_model_node_constants)
for the model's nodes in order and returns their count (1 up to
WII_GX_MODEL_MAXIMUM_NODES; anything else refuses the model). Returns the
parts handed to the GX side (wii_gx_model_part_draw: drawn or refused there,
counted), or -1 if the skeleton or node matrices were refused. Unsupported
skeleton counts and bad region, geometry and shader indices count in
data_errors (in the solid pass only).
*/

#ifndef __HALO_WII_RENDER_MODELS_ENGINE_H
#define __HALO_WII_RENDER_MODELS_ENGINE_H

struct model_node;

typedef int (*wii_render_model_palette_proc)(void *context, float (*palette)[3][4]);

void wii_render_model_node_constants(
	real_matrix4x3 const *node_matrix,
	struct model_node const *node,
	float (*constants)[4]);
int wii_render_model_detail_level(
	long model_index,
	real level_of_detail_pixels);
long wii_render_model_parts(
	long model_index,
	signed char const *permutations,
	short permutation_count,
	int detail_level,
	int pass,
	wii_render_model_palette_proc palette,
	void *context,
	unsigned long *data_errors);

#endif
