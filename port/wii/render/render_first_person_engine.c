/*
RENDER_FIRST_PERSON_ENGINE.C

The engine side of the Wii rasterizer's first-person weapon (HWI-084,
wii_render.h). Compiled as an engine unit (the engine's headers and flags),
in engine.dol only, beside render_objects_engine.c.

What render.c does for the first-person weapon in a window, and where:
- render_window, before render_objects: first_person_weapon_render_update
  (its visibility, then first_person_weapon_build_node_matrices: the pose
  from the view model's animation state, in world space from the window's
  camera). Here wii_render_first_person_collect, right after the objects'
  collect, which follows the frame's camera and visibility: the engine's
  first_person_weapon_render_pose (interface/first_person_weapons.c) runs
  the same orientation code into a copy, and the graph's node matrices from
  render.camera (the frame's camera, as render_window sets it).
- render_objects: first_person_weapon_draw, before the objects
  (draw_first_person_weapon_first), with the first-person near and far clip
  distances (rasterizer_set_frustum_z) and the stencil written, which the
  world drawn after is rejected by. Here the first-person passes, after the
  objects' (render_models_gx.c), with the first-person clip distances in the
  front slice of the depth range: the weapon is over the world as there.
  Each model as render_model draws it: level of detail 0 pixels, the default
  region permutations (every region's 0), each part in its pass, the node
  matrices the model's own (model_remap_node_matrices_to_match_animation_graph:
  model node m takes graph node table[m]) times each model node's
  runtime_default_inverse_matrix: the weapon's first-person model, then the
  first-person interface's hands (game globals), each with its own table.

Node domains, explicit: the pose's node matrices are the animation graph's
nodes (node_count, 1..64); each model's table maps the model's nodes (its
tag's count, 1..WII_GX_MODEL_MAXIMUM_NODES) into them, every entry checked
below node_count before anything is drawn; the palette handed to the GX
side is in the model's node domain, which the parts' vertices index (node
byte / 3, hwm_model.h: a vertex of full weight leaves its second node byte
unread; any other consumed node at or past the model's count refuses the
part). A model whose count or any entry is outside these is refused whole
and counted, never drawn with other nodes. The world weapon's model is never
drawn here.

State: first_person_weapon_render_pose writes the view model's
node_orientations on a drawn frame only (store_pose: the map run's and the
boot mode's frames, not the pose checks), as the render update does at that
point: its last pose drawn, which its next state change blends from. The
director's perspective cache (director_get_perspective, a static of
director.c, not the game state; the objects' collect reads it too) is the
only other write. Nothing else: no lighting cache, no effects, no sounds, no
random numbers.

Pointers: nothing is kept across frames but the collected pose (a copy) and
model tag indices; every frame's collect replaces them, and the map's end
(wii_render_first_person_forget) drops them, so a switched or dropped weapon,
a dead player or a zoom draws nothing stale.
*/

#include "cseries.h"
#include "math/real_math.h"
#include "render/render.h"
#include "render/render_cameras.h"
#include "models/model_definitions.h"
#include "rasterizer/rasterizer.h"
#include "rasterizer/rasterizer_globals_internal.h"
#include "interface/first_person_weapons.h"

#include "wii_render.h"
#include "render_models_engine.h"
#include "../engine/wii_platform.h"

/* ---------- constants */

enum
{
	_first_person_model_weapon = 0,
	_first_person_model_hands,
	NUMBER_OF_FIRST_PERSON_MODELS,

	/* transitions logged (RENDER_FIRST_PERSON_STATE): then counted only */
	FIRST_PERSON_STATES_LOGGED = 64,
};

enum first_person_model_check
{
	_first_person_model_ok = 0,
	_first_person_model_none,        /* no model, or its remapping table is not valid (not drawn, as the engine) */
	_first_person_model_node_count,  /* the model's nodes are not 1..WII_GX_MODEL_MAXIMUM_NODES */
	_first_person_model_remap,       /* an entry is not one of the pose's nodes */
	_first_person_model_detail,      /* below its detail cutoff at 0 pixels */
	NUMBER_OF_FIRST_PERSON_MODEL_CHECKS
};

typedef char first_person_pose_nodes[
	FIRST_PERSON_WEAPON_RENDER_POSE_MAXIMUM_NODES <= WII_GX_MODEL_MAXIMUM_NODES ? 1 : -1];

/* ---------- structures */

struct first_person_model
{
	long model_index;
	short node_count;
	short const *table;
	int detail_level;
	short check;
	boolean drawable;
};

struct render_first_person_globals
{
	/* the frame's, from its collect to the next (or the map's end) */
	boolean pending;
	struct first_person_weapon_render_pose pose;
	struct first_person_model models[NUMBER_OF_FIRST_PERSON_MODELS];
	struct wii_gx_first_person_view view;
	/* this launch's counts */
	unsigned long collects, disabled, stored, clip_refused, pending_frames;
	unsigned long results[NUMBER_OF_FIRST_PERSON_WEAPON_RENDER_POSE_RESULTS];
	unsigned long checks[NUMBER_OF_FIRST_PERSON_MODELS][NUMBER_OF_FIRST_PERSON_MODEL_CHECKS];
	unsigned long models_drawn[NUMBER_OF_FIRST_PERSON_MODELS], palette_refused[NUMBER_OF_FIRST_PERSON_MODELS];
	unsigned long data_errors;
	/* the transitions logged */
	short last_result;
	long last_weapon_index;
	long states_logged;
	boolean reported;
};

/* ---------- globals */

static struct render_first_person_globals wii_first_person;
static signed char const first_person_default_permutations[MAXIMUM_REGIONS_PER_MODEL] = { 0 };

static char const *const first_person_result_names[NUMBER_OF_FIRST_PERSON_WEAPON_RENDER_POSE_RESULTS] =
{
	"drawn", "no_local_player", "no_unit", "not_first_person", "zoomed", "no_weapon", "weapon_gone",
	"no_animations", "no_state_animation", "node_count",
};

static char const *const first_person_check_names[NUMBER_OF_FIRST_PERSON_MODEL_CHECKS] =
{
	"ok", "none", "node_count", "remap", "detail",
};

/* ---------- private code */

/* the model's node matrices in its own node domain: the pose's graph node
table[m] for model node m, times model node m's runtime_default_inverse_matrix
(a wii_render_model_palette_proc) */
static int render_first_person_palette(
	void *context,
	float (*palette)[3][4])
{
	struct first_person_model const *model = (struct first_person_model const *)context;
	struct model *definition = model_definition_get(model->model_index);
	short node_index;

	for (node_index = 0; node_index < model->node_count; node_index++)
	{
		wii_render_model_node_constants(&wii_first_person.pose.node_matrices[model->table[node_index]],
			TAG_BLOCK_GET_ELEMENT(&definition->nodes, node_index, struct model_node), palette[node_index]);
	}

	return model->node_count;
}

/* what first_person_weapon_draw would draw of one model, checked whole */
static short render_first_person_model_check(
	struct first_person_model *model,
	long model_index,
	short model_node_count,
	short const *table)
{
	short node_index;

	model->model_index = model_index;
	model->node_count = model_node_count;
	model->table = table;
	model->detail_level = -1;
	model->drawable = FALSE;
	if (model_index == NONE)
		return model->check = _first_person_model_none;
	if (model_node_count < 1 || model_node_count > WII_GX_MODEL_MAXIMUM_NODES ||
		model_node_count > FIRST_PERSON_WEAPON_RENDER_POSE_MAXIMUM_NODES)
	{
		return model->check = _first_person_model_node_count;
	}
	for (node_index = 0; node_index < model_node_count; node_index++)
	{
		if (table[node_index] < 0 || table[node_index] >= wii_first_person.pose.node_count)
			return model->check = _first_person_model_remap;
	}
	/* render_model's level of detail for the first-person call (0 pixels) */
	model->detail_level = wii_render_model_detail_level(model_index, 0.0f);
	if (model->detail_level < 0)
		return model->check = _first_person_model_detail;
	model->drawable = TRUE;

	return model->check = _first_person_model_ok;
}

/* the projection: the frame's x and y (render_camera_hack_frustum_z keeps
them), depth from the first-person near and far clip distances
(rasterizer_set_frustum_z's) in the front depth slice
(render_first_person_projection.c); then the frame's again */
static boolean render_first_person_projection(
	struct wii_gx_view const *view)
{
	csmemcpy(wii_first_person.view.restore, view->projection, sizeof(wii_first_person.view.restore));

	return wii_gx_first_person_projection(view->projection, rasterizer_globals.first_person_weapon_near_clip_distance,
		rasterizer_globals.first_person_weapon_far_clip_distance, WII_GX_FIRST_PERSON_DEPTH_SLICE,
		wii_first_person.view.projection) ? TRUE : FALSE;
}

static void render_first_person_log_state(
	short result)
{
	struct first_person_weapon_render_pose const *pose = &wii_first_person.pose;
	boolean drawn = result == _first_person_weapon_render_pose_ok;

	if (result == wii_first_person.last_result && (!drawn || pose->weapon_index == wii_first_person.last_weapon_index))
		return;
	wii_first_person.last_result = result;
	wii_first_person.last_weapon_index = drawn ? pose->weapon_index : NONE;
	if (wii_first_person.states_logged >= FIRST_PERSON_STATES_LOGGED)
		return;
	wii_first_person.states_logged++;
	wii_log("RENDER_FIRST_PERSON_STATE frame=%lu state=%s weapon=%08lx graph=%ld graph_nodes=%d weapon_model=%ld "
		"weapon_nodes=%d weapon_check=%s hands_model=%ld hands_nodes=%d hands_check=%s\n", wii_first_person.collects,
		first_person_result_names[result], (unsigned long)pose->weapon_index, pose->animation_graph_index,
		pose->node_count, pose->weapon_model_index, pose->weapon_model_node_count,
		drawn ? first_person_check_names[wii_first_person.models[_first_person_model_weapon].check] : "-",
		pose->hands_model_index, pose->hands_model_node_count,
		drawn ? first_person_check_names[wii_first_person.models[_first_person_model_hands].check] : "-");
}

static void render_first_person_model_draw(
	short which,
	int pass)
{
	struct first_person_model *model = &wii_first_person.models[which];
	long handed;

	if (!model->drawable)
		return;
	/* (the render_model calls of first_person_weapon_draw force shader permutation 0: none) */
	handed = wii_render_model_parts(model->model_index, first_person_default_permutations, MAXIMUM_REGIONS_PER_MODEL,
		0, model->detail_level, pass, render_first_person_palette, model, &wii_first_person.data_errors);
	if (handed > 0)
		wii_first_person.models_drawn[which] += pass == _wii_gx_model_pass_solid;
	else if (handed < 0)
		wii_first_person.palette_refused[which] += pass == _wii_gx_model_pass_solid;
}

/* ---------- public code */

void wii_render_first_person_collect(
	struct wii_gx_view const *view,
	int store_pose)
{
	struct first_person_weapon_render_pose *pose = &wii_first_person.pose;
	boolean store = store_pose && wii_gx_first_person_store_pose() ? TRUE : FALSE;
	short which;
	short result;

	/* nothing of the last frame's is drawn from here on */
	wii_first_person.pending = FALSE;
	wii_gx_first_person_view_set(NULL);
	for (which = 0; which < NUMBER_OF_FIRST_PERSON_MODELS; which++)
		wii_first_person.models[which].drawable = FALSE;
	wii_first_person.collects++;
	if (!wii_gx_first_person_enabled())
	{
		wii_first_person.disabled++;
		return;
	}
	result = first_person_weapon_render_pose(render.local_player_index, &render.camera, store, pose);
	if (result < 0 || result >= NUMBER_OF_FIRST_PERSON_WEAPON_RENDER_POSE_RESULTS)
		return;
	wii_first_person.results[result]++;
	if (result != _first_person_weapon_render_pose_ok)
	{
		render_first_person_log_state(result);
		return;
	}
	wii_first_person.stored += store;
	wii_first_person.checks[_first_person_model_weapon][render_first_person_model_check(
		&wii_first_person.models[_first_person_model_weapon], pose->weapon_model_index,
		pose->weapon_model_node_count, pose->weapon_node_remapping_table)]++;
	wii_first_person.checks[_first_person_model_hands][render_first_person_model_check(
		&wii_first_person.models[_first_person_model_hands], pose->hands_model_index,
		pose->hands_model_node_count, pose->hands_node_remapping_table)]++;
	render_first_person_log_state(result);
	if (!wii_first_person.models[_first_person_model_weapon].drawable &&
		!wii_first_person.models[_first_person_model_hands].drawable)
	{
		return;
	}
	if (!render_first_person_projection(view))
	{
		wii_first_person.clip_refused++;
		return;
	}
	wii_gx_first_person_view_set(&wii_first_person.view);
	wii_first_person.pending = TRUE;
	wii_first_person.pending_frames++;
}

void wii_render_first_person_draw(
	int pass)
{
	if (!wii_first_person.pending)
		return;
	/* first_person_weapon_draw's order: the weapon, then the hands */
	render_first_person_model_draw(_first_person_model_weapon, pass);
	render_first_person_model_draw(_first_person_model_hands, pass);
	if (pass == NUMBER_OF_WII_GX_MODEL_PASSES - 1 && !wii_first_person.reported)
	{
		wii_first_person.reported = TRUE;
		wii_render_first_person_report("first");
	}
}

void wii_render_first_person_forget(
	void)
{
	short which;

	wii_first_person.pending = FALSE;
	wii_gx_first_person_view_set(NULL);
	for (which = 0; which < NUMBER_OF_FIRST_PERSON_MODELS; which++)
		wii_first_person.models[which].drawable = FALSE;
}

void wii_render_first_person_report(
	char const *when)
{
	unsigned long const *r = wii_first_person.results;
	unsigned long const *w = wii_first_person.checks[_first_person_model_weapon];
	unsigned long const *h = wii_first_person.checks[_first_person_model_hands];

	wii_log("RENDER_FIRST_PERSON when=%s enabled=%d store_pose=%d collects=%lu disabled=%lu drawn_frames=%lu "
		"stored=%lu no_local_player=%lu no_unit=%lu not_first_person=%lu zoomed=%lu no_weapon=%lu weapon_gone=%lu "
		"no_animations=%lu no_state_animation=%lu graph_node_count=%lu clip_refused=%lu "
		"weapon_models_solid_pass=%lu weapon_none=%lu weapon_node_count=%lu weapon_remap=%lu weapon_detail=%lu "
		"weapon_palette_refused=%lu hands_models_solid_pass=%lu hands_none=%lu hands_node_count=%lu hands_remap=%lu "
		"hands_detail=%lu hands_palette_refused=%lu data_errors=%lu state_writes=%s\n", when,
		wii_gx_first_person_enabled(), wii_gx_first_person_store_pose(), wii_first_person.collects,
		wii_first_person.disabled, wii_first_person.pending_frames, wii_first_person.stored,
		r[_first_person_weapon_render_pose_no_local_player], r[_first_person_weapon_render_pose_no_unit],
		r[_first_person_weapon_render_pose_not_first_person], r[_first_person_weapon_render_pose_zoomed],
		r[_first_person_weapon_render_pose_no_weapon], r[_first_person_weapon_render_pose_weapon_gone],
		r[_first_person_weapon_render_pose_no_animations], r[_first_person_weapon_render_pose_no_state_animation],
		r[_first_person_weapon_render_pose_node_count], wii_first_person.clip_refused,
		wii_first_person.models_drawn[_first_person_model_weapon], w[_first_person_model_none],
		w[_first_person_model_node_count], w[_first_person_model_remap], w[_first_person_model_detail],
		wii_first_person.palette_refused[_first_person_model_weapon],
		wii_first_person.models_drawn[_first_person_model_hands], h[_first_person_model_none],
		h[_first_person_model_node_count], h[_first_person_model_remap], h[_first_person_model_detail],
		wii_first_person.palette_refused[_first_person_model_hands], wii_first_person.data_errors,
		wii_gx_first_person_store_pose() ? "view_model_node_orientations_on_drawn_frames" : "none");
}
