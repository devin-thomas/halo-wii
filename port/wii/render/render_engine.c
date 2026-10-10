/*
RENDER_ENGINE.C

The engine-side half of the Wii rasterizer's environment path (HWI-016B,
wii_render.h). Compiled as an engine unit (the engine's headers and flags),
in engine.dol only.

Each game frame (the frame hook, after the engine's own game_frame):
1. The camera, read only: the engine's deterministic camera of the local
   player's unit (director_camera_deterministic, as aim assist reads it),
   made into the window's camera as main.c's set_window_camera_values makes
   it, with the window bounds of compute_window_bounds and the rasterizer's
   clip distances. main_loop's director_update and observer_update are not
   run: the observer's camera feeds the simulation (aim assist, object
   activation, the sound environment), and the HWI-015B run never moves it
   (render_engine_player_camera). The camera effect matrix
   (player_effect_get_camera_effect_matrix: impulses and shakes) is not
   applied either: it writes the game state (player effects' timers and
   flags).
2. The engine's own visibility, as render.c's render_player_frame and
   render_window run it: structure_visibility_find_camera (the camera's
   leaf and cluster), render_camera_build_frustum_bounds and
   render_camera_build_frustum, then structure_visibility_compute (the
   portals and the cluster PVS, then each rendered cluster's subclusters'
   surfaces against its clipped frustum) into render.environment_surface_flags.
3. The draw (render_gx.c): every BSP material, in structure_render_pass's
   lightmap and material order, whose breakable surface stands
   (breakable_surface_extant), with its visible surfaces.

The engine's render globals (render.c's `render`) are written, as render.c
writes them; the game state is only read (lock_global_random_seed guards the
game's random numbers while the frame draws).

Per map (the rasterizer_initialize_for_new_map and
rasterizer_dispose_from_old_map hooks, and a BSP change seen by a frame):
the BSP's materials are described to the GX side (their shader's type, base
map and bitmap, lightmap page, surfaces) and its converted geometry and
textures are loaded.
*/

#include "cseries.h"
#include "math/real_math.h"
#include "game/game.h"
#include "game/players.h"
#include "render/render.h"
#include "render/render_cameras.h"
#include "scenario/scenario.h"
#include "structures/structure_bsp_definitions.h"
#include "structures/structure_visibility.h"
#include "camera/director.h"
#include "camera/observer.h"
#include "rasterizer/rasterizer.h"
#include "rasterizer/rasterizer_globals_internal.h"
#include "shaders/shader_definitions.h"
#include "bitmaps/bitmap_group.h"
#include "bitmaps/bitmap_group_lookup.h"
#include "physics/breakable_surfaces.h"
#include "effects/player_effects.h"

#include "wii_render.h"
#include "../engine/engine_hooks.h"
#include "../engine/real_map_scenario.h"
#include "../engine/wii_platform.h"

/* the engine's (main.c, console.c) */
void compute_window_bounds(long player_index, long num_players, rectangle2d *pixel_bounds,
	rectangle2d *safe_frame_bounds);
boolean console_is_active(void);

/* ---------- constants */

/* the engine's shader types (shaders.c, private there) */
enum
{
	_render_shader_type_environment = 3,
	_render_shader_type_model = 4,
	_render_shader_type_transparent_generic = 5,
	_render_shader_type_transparent_plasma = 10,
};

#define RENDER_MAXIMUM_MATERIALS 4096
#define RENDER_POSE_TICK_COUNT 3
#define RENDER_OVERRIDE_POSES 2
#define RENDER_SNAPSHOT_BYTES 16384

static const long render_pose_ticks[RENDER_POSE_TICK_COUNT] = { 30, 150, 300 };

/* ---------- structures */

/* shader_environment as the Xbox rasterizer reads it
(rasterizer_xbox_environment.c, private there): the diffuse properties'
maps */
struct render_environment_diffuse
{
	word flags;
	short pad02;
	byte reserved04[0x18];
	struct tag_reference base_map;
	byte reserved2C[0x18];
	short detail_map_function;
	short pad46;
	real primary_detail_map_scale;
	struct tag_reference primary_detail_map;
	real secondary_detail_map_scale;
	struct tag_reference secondary_detail_map;
	byte reserved70[0x18];
	short micro_detail_map_function;
	short pad8A;
	real micro_detail_map_scale;
	struct tag_reference micro_detail_map;
	real_rgb_color material_color;
	byte reservedAC[0xC];
	real bump_map_scale;
	struct tag_reference bump_map;
};

struct render_shader_environment
{
	struct shader shader;
	word flags;
	short type;
	real lens_flare_spacing;
	struct tag_reference lens_flare;
	long unused[11];
	struct render_environment_diffuse diffuse;
};

typedef char render_shader_environment_diffuse_offset[
	offsetof(struct render_shader_environment, diffuse) == 0x6C ? 1 : -1];
typedef char render_shader_environment_base_map_offset[
	offsetof(struct render_shader_environment, diffuse.base_map) == 0x88 ? 1 : -1];
typedef char render_shader_environment_bump_map_offset[
	offsetof(struct render_shader_environment, diffuse.bump_map) == 0x128 ? 1 : -1];

struct render_engine_globals
{
	boolean configured;
	boolean initialized;
	char map_name[64];
	long bsp_tag_index;        /* the BSP whose resources are loaded, or NONE */
	boolean map_failed;        /* this BSP's resources failed to load: not retried */
	long material_count;
	short breakable[RENDER_MAXIMUM_MATERIALS];
	unsigned char drawn[RENDER_MAXIMUM_MATERIALS];
	struct render_camera camera;
	struct render_frustum frustum;
	struct wii_gx_view view;
	boolean view_valid;
	long load_ordinal;         /* maps loaded so far (the warm-up run is 0) */
	long next_run_pose;
	long frames;
	long camera_effect_skipped;
	boolean pose_phase;
};

/* ---------- globals */

static struct render_engine_globals render_engine;
static unsigned char render_snapshot[RENDER_SNAPSHOT_BYTES];

/* ---------- private code */

static void render_engine_clip_distances(
	void)
{
	/* rasterizer_frame_begin's defaults (rasterizer.c) */
	if (rasterizer_globals.near_clip_distance == 0.f)
		rasterizer_globals.near_clip_distance = rasterizer_global_defaults.near_clip_distance;
	if (rasterizer_globals.far_clip_distance == 0.f)
		rasterizer_globals.far_clip_distance = rasterizer_global_defaults.far_clip_distance;
	if (rasterizer_globals.first_person_weapon_near_clip_distance == 0.f)
		rasterizer_globals.first_person_weapon_near_clip_distance =
			rasterizer_global_defaults.first_person_weapon_near_clip_distance;
	if (rasterizer_globals.first_person_weapon_far_clip_distance == 0.f)
		rasterizer_globals.first_person_weapon_far_clip_distance =
			rasterizer_global_defaults.first_person_weapon_far_clip_distance;
}

/* set_window_camera_values (main.c) for one window, without the camera
effect matrix (above) */
static void render_engine_camera_from(
	struct render_camera *camera,
	real_point3d const *position,
	real_vector3d const *forward,
	real_vector3d const *up,
	real field_of_view)
{
	csmemset(camera, 0, sizeof(*camera));
	compute_window_bounds(0, 1, &camera->viewport_bounds, &camera->window_bounds);
	camera->position = *position;
	camera->forward = *forward;
	camera->up = *up;
	camera->vertical_field_of_view = 2.0f * arctangent(
		0.75f * render_camera_get_adjusted_field_of_view_tangent(field_of_view), 1.0f);
	camera->mirrored = FALSE;
	camera->z_near = rasterizer_globals.near_clip_distance;
	camera->z_far = rasterizer_globals.far_clip_distance;
}

/* The local player's camera, read only. main_loop moves the observer every
frame (director_update, observer_update); the HWI-015B run, and its i686
reference, never do, and the observer's camera is read by the simulation
(aim_assist.c, players.c's activation PVS, scenario.c's sound environment,
game_sound.c's listener): moving it here changed the simulation digest. So
the frame's camera is the engine's deterministic one for the player's unit
(director_camera_deterministic: the head marker and the aiming vector, as
aim assist reads it, or the following camera in a third-person seat), with
the observer's field of view; before the unit exists, the observer's camera
as it stands. Neither writes anything. */
static void render_engine_player_camera(
	struct render_camera *camera)
{
	struct observer_result const *observer = observer_get_camera(0);
	long player_index = local_player_get_player_index(0);
	struct player_datum *player = player_index != NONE ? player_try_and_get(player_index) : NULL;

	if (player && player->unit_index != NONE)
	{
		real_point3d position;
		real_vector3d forward;
		real_vector3d left;
		real_vector3d up;

		director_camera_deterministic(player->unit_index, &position, &forward);
		normalize3d(&forward);
		up = *global_up3d;
		cross_product3d(&up, &forward, &left);
		if (normalize3d(&left) == 0.0f)
			left = *global_left3d;
		cross_product3d(&forward, &left, &up);
		normalize3d(&up);
		render_engine_camera_from(camera, &position, &forward, &up, observer->field_of_view);
	}
	else
	{
		render_engine_camera_from(camera, &observer->position, &observer->forward, &observer->up,
			observer->field_of_view);
	}
}

/* render_player_frame's and render_window's visibility for the camera; the
GX view from the engine's frustum */
static void render_engine_visibility(
	struct render_camera *camera)
{
	real_rectangle2d frustum_bounds;
	struct render_frustum *frustum = &render_engine.frustum;
	struct wii_gx_view *view = &render_engine.view;
	real n, f;
	short row;

	structure_visibility_find_camera(camera);
	render_camera_build_frustum_bounds(camera, &frustum_bounds);
	render_camera_build_frustum(camera, &frustum_bounds, frustum, TRUE);
	render.local_player_index = 0;
	render.window_index = 0;
	render.camera = *camera;
	render.frustum = *frustum;
	structure_visibility_compute();

	/* the GX position matrix: the frustum's world_to_view (matrix4x3:
	x' = s(forward.i x + left.i y + up.i z) + position.x ...) */
	for (row = 0; row < 3; row++)
	{
		real s = frustum->world_to_view.scale;

		view->world_to_view[row][0] = s * (&frustum->world_to_view.forward.i)[row];
		view->world_to_view[row][1] = s * (&frustum->world_to_view.left.i)[row];
		view->world_to_view[row][2] = s * (&frustum->world_to_view.up.i)[row];
		view->world_to_view[row][3] = (&frustum->world_to_view.position.x)[row];
	}
	/* the projection (render_camera_build_frustum's, D3D row vectors) as a GX
	perspective: x and y scales and centres as the engine's; depth from the
	camera's near and far planes into GX's [-w, 0] */
	n = camera->z_near;
	f = camera->z_far;
	csmemset(view->projection, 0, sizeof(view->projection));
	view->projection[0][0] = frustum->projection_matrix[0][0];
	view->projection[0][2] = frustum->projection_matrix[2][0];
	view->projection[1][1] = frustum->projection_matrix[1][1];
	view->projection[1][2] = frustum->projection_matrix[2][1];
	view->projection[2][2] = -n / (f - n);
	view->projection[2][3] = -(f * n) / (f - n);
	view->projection[3][2] = -1.0f;
	view->eye[0] = camera->position.x;
	view->eye[1] = camera->position.y;
	view->eye[2] = camera->position.z;
	view->leaf_index = render.leaf_index;
	view->cluster_index = render.cluster_index;
	view->rendered_clusters = render.rendered_cluster_count;
	view->visible_surfaces = render.environment_surface_count;
}

static void render_engine_materials_drawn(
	void)
{
	long index;

	for (index = 0; index < render_engine.material_count; index++)
	{
		render_engine.drawn[index] = (unsigned char)breakable_surface_extant(render_engine.breakable[index]);
	}
}

static int render_engine_frontfacing(
	const float eye[3],
	const float p0[3],
	const float p1[3],
	const float p2[3])
{
	struct render_camera camera;
	real_point3d points[3];

	csmemset(&camera, 0, sizeof(camera));
	camera.position.x = eye[0];
	camera.position.y = eye[1];
	camera.position.z = eye[2];
	points[0].x = p0[0]; points[0].y = p0[1]; points[0].z = p0[2];
	points[1].x = p1[0]; points[1].y = p1[1]; points[1].z = p1[2];
	points[2].x = p2[0]; points[2].y = p2[1]; points[2].z = p2[2];

	return render_camera_triangle_frontfacing(&camera, &points[0], &points[1], &points[2]) ? 1 : 0;
}

/* the BSP's materials described to the GX side, and its resources loaded */
static boolean render_engine_map_load(
	long bsp_tag_index)
{
	struct structure_bsp *structure_bsp = global_structure_bsp_get();
	long material_index = 0;
	long material_total = 0;
	short lightmap_index;
	boolean ok;

	for (lightmap_index = 0; lightmap_index < structure_bsp->lightmaps.count; lightmap_index++)
	{
		struct structure_lightmap *lightmap = TAG_BLOCK_GET_ELEMENT(&structure_bsp->lightmaps, lightmap_index,
			struct structure_lightmap);

		material_total += lightmap->materials.count;
	}
	if (material_total > RENDER_MAXIMUM_MATERIALS)
	{
		wii_log("RENDER_FAIL map_load bsp_tag=%ld materials=%ld maximum=%d\n", bsp_tag_index, material_total,
			RENDER_MAXIMUM_MATERIALS);
		return FALSE;
	}
	if (!wii_gx_map_begin((unsigned long)DATUM_INDEX_TO_ABSOLUTE_INDEX(bsp_tag_index), material_total,
		structure_bsp->surfaces.count))
		return FALSE;
	render_engine.material_count = material_total;
	ok = TRUE;
	for (lightmap_index = 0; ok && lightmap_index < structure_bsp->lightmaps.count; lightmap_index++)
	{
		struct structure_lightmap *lightmap = TAG_BLOCK_GET_ELEMENT(&structure_bsp->lightmaps, lightmap_index,
			struct structure_lightmap);
		short index;

		for (index = 0; ok && index < lightmap->materials.count; index++, material_index++)
		{
			struct structure_material *material = TAG_BLOCK_GET_ELEMENT(&lightmap->materials, index,
				struct structure_material);
			struct shader *shader = shader_definition_get(material->shader.index);
			struct wii_gx_material description;

			csmemset(&description, 0, sizeof(description));
			description.shader_type = shader->base.type;
			description.lightmap_page = structure_bsp->lightmap_group.index == NONE ? -1 : lightmap->bitmap_index;
			description.base_tag = -1;
			description.first_surface = material->first_surface_index;
			description.surface_count = material->surface_count;
			if (shader->base.type == _render_shader_type_environment)
			{
				struct render_shader_environment *environment = (struct render_shader_environment *)shader;

				description.kind = _wii_gx_material_environment;
				if (environment->diffuse.base_map.index != NONE)
				{
					struct bitmap_group *group = bitmap_group_get(environment->diffuse.base_map.index);

					if (group->bitmaps.count > 0)
					{
						struct bitmap_data *bitmap;

						description.base_tag = DATUM_INDEX_TO_ABSOLUTE_INDEX(environment->diffuse.base_map.index);
						description.base_bitmap = material->permutation_index % group->bitmaps.count;
						bitmap = bitmap_group_try_and_get_bitmap(environment->diffuse.base_map.index,
							(short)description.base_bitmap);
						description.base_width = bitmap ? bitmap->width : 0;
						description.base_height = bitmap ? bitmap->height : 0;
					}
				}
				description.detail_maps = (environment->diffuse.primary_detail_map.index != NONE) +
					(environment->diffuse.secondary_detail_map.index != NONE) +
					(environment->diffuse.micro_detail_map.index != NONE);
				description.bump_map = environment->diffuse.bump_map.index != NONE;
			}
			else if (shader->base.type >= _render_shader_type_transparent_generic &&
				shader->base.type <= _render_shader_type_transparent_plasma)
			{
				description.kind = _wii_gx_material_placeholder_transparent;
			}
			else
			{
				description.kind = _wii_gx_material_placeholder_other;
			}
			render_engine.breakable[material_index] = material->breakable_surface_index;
			ok = wii_gx_material_set(material_index, &description);
		}
	}
	if (ok)
	{
		ok = wii_gx_textures_load(structure_bsp->lightmap_group.index == NONE ? -1 :
			(long)DATUM_INDEX_TO_ABSOLUTE_INDEX(structure_bsp->lightmap_group.index));
	}
	wii_log("RENDER_BSP load=%ld bsp_tag=%lu clusters=%ld cluster_portals=%ld surfaces=%ld lightmaps=%ld materials=%ld "
		"weather_palette=%ld fog_palette=%ld lens_flares=%ld detail_object_data=%ld ok=%d\n", render_engine.load_ordinal,
		(unsigned long)DATUM_INDEX_TO_ABSOLUTE_INDEX(bsp_tag_index), structure_bsp->clusters.count,
		structure_bsp->cluster_portals.count,
		structure_bsp->surfaces.count, structure_bsp->lightmaps.count, material_total,
		structure_bsp->weather_palette.count, structure_bsp->fog_palette.count, structure_bsp->lens_flares.count,
		structure_bsp->detail_object_data.count, ok);
	if (!ok)
	{
		wii_gx_map_end();
		render_engine.material_count = 0;
	}

	return ok;
}

static void render_engine_map_unload(
	void)
{
	if (render_engine.bsp_tag_index != NONE)
		wii_gx_map_end();
	render_engine.bsp_tag_index = NONE;
	render_engine.material_count = 0;
	render_engine.view_valid = FALSE;
}

/* the BSP drawn now has its resources loaded (a BSP switch reloads) */
static boolean render_engine_map_current(
	void)
{
	long bsp_tag_index;

	if (global_scenario_index == NONE || !global_structure_bsp_get())
		return FALSE;
	bsp_tag_index = global_structure_bsp_tag_index_get();
	if (bsp_tag_index == NONE)
		return FALSE;
	if (bsp_tag_index != render_engine.bsp_tag_index)
	{
		render_engine_map_unload();
		if (render_engine.map_failed)
			return FALSE;
		if (!render_engine_map_load(bsp_tag_index))
		{
			render_engine.map_failed = TRUE;
			return FALSE;
		}
		render_engine.bsp_tag_index = bsp_tag_index;
		render_engine.next_run_pose = 0;
	}

	return wii_gx_map_ready();
}

/* ---------- the hooks (engine_hooks.h) */

static int render_engine_rasterizer_initialize(
	void)
{
	/* what the environment path needs of _rasterizer_initialize: the frame
	and screen bounds (compute_window_bounds) and the clip distances */
	rasterizer_globals.reserved04.screen_bounds.x0 = 0;
	rasterizer_globals.reserved04.screen_bounds.y0 = 0;
	rasterizer_globals.reserved04.screen_bounds.x1 = 640;
	rasterizer_globals.reserved04.screen_bounds.y1 = 480;
	rasterizer_globals.reserved04.frame_bounds = rasterizer_globals.reserved04.screen_bounds;
	render_engine_clip_distances();
	render_engine.initialized = TRUE;
	render_engine.bsp_tag_index = NONE;
	/* not made: rasterizer_initialize's game-state allocation of the model
	ambient reflection tint (rasterizer.c; cinematics write it, models read
	it, both test it for NULL), which would move every later game-state
	allocation from the i686 host reference's; models are not drawn here */
	wii_log("RENDER_INIT rasterizer=wii_gx_environment near=%.6f far=%.6f frame=%d,%d,%d,%d "
		"model_ambient_reflection_tint=not_allocated\n", (double)rasterizer_globals.near_clip_distance,
		(double)rasterizer_globals.far_clip_distance, rasterizer_globals.reserved04.frame_bounds.x0,
		rasterizer_globals.reserved04.frame_bounds.y0, rasterizer_globals.reserved04.frame_bounds.x1,
		rasterizer_globals.reserved04.frame_bounds.y1);
	/* the render subsystems this rasterizer does not draw, each reported */
	wii_unsupported("render", "objects and models (render_objects)");
	wii_unsupported("render", "first-person weapons");
	wii_unsupported("render", "sky (render_sky)");
	wii_unsupported("render", "decals (rasterizer_decals_draw)");
	wii_unsupported("render", "particles, particle systems, contrails, weather");
	wii_unsupported("render", "effects, lights, lens flares, shadows");
	wii_unsupported("render", "water, fog (atmospheric, planar, screen)");
	wii_unsupported("render", "environment detail maps, bump maps, specular, reflections, self-illumination");
	wii_unsupported("render", "transparent environment shaders (labelled placeholders)");
	wii_unsupported("render", "mirrors, HUD, interface, widgets, menus, text");
	wii_unsupported("render", "camera effect matrix (writes player effects state)");

	return TRUE;
}

static void render_engine_rasterizer_dispose(
	void)
{
	render_engine_map_unload();
	render_engine.initialized = FALSE;
}

static void render_engine_new_map(
	void)
{
	render_engine.map_failed = FALSE;
	render_engine.load_ordinal++;
}

static void render_engine_old_map(
	void)
{
	render_engine_map_unload();
}

static void render_engine_frame(
	float dt)
{
	unsigned long long started;
	boolean sample = FALSE;
	unsigned long crc = 0;
	int covered = 0;

	if (!render_engine.initialized)
		return;
	/* (a map's resources load in its first frame: timed by the load, not
	the frame) */
	if (!render_engine_map_current())
		return;
	started = wii_time_microseconds();
	lock_global_random_seed();
	render_engine_clip_distances();
	render_engine_player_camera(&render_engine.camera);
	if (local_player_get_player_index(0) != NONE &&
		!console_is_active() && !game_time_get_paused() &&
		director_get_perspective(0) != _director_perspective_neutral)
	{
		render_engine.camera_effect_skipped++;
	}
	render_engine_visibility(&render_engine.camera);
	render_engine_materials_drawn();
	render_engine.view_valid = TRUE;
	render_engine.view.visibility_us = (unsigned long)(wii_time_microseconds() - started);
	if (render_engine.next_run_pose < RENDER_POSE_TICK_COUNT &&
		game_time_get() >= render_pose_ticks[render_engine.next_run_pose])
	{
		sample = TRUE;
	}
	wii_gx_frame(&render_engine.view, render.environment_surface_flags, render_engine.drawn, sample, &crc, &covered);
	unlock_global_random_seed();
	if (sample)
	{
		wii_log("RENDER_RUN_POSE load=%ld tick=%ld game_time=%ld crc=%08lx covered=%d leaf=%ld cluster=%ld "
			"clusters=%ld surfaces=%ld eye=%.4f,%.4f,%.4f\n", render_engine.load_ordinal,
			render_pose_ticks[render_engine.next_run_pose], game_time_get(), crc, covered, render_engine.view.leaf_index,
			render_engine.view.cluster_index, render_engine.view.rendered_clusters,
			render_engine.view.visible_surfaces, (double)render_engine.view.eye[0], (double)render_engine.view.eye[1],
			(double)render_engine.view.eye[2]);
		render_engine.next_run_pose++;
	}
	render_engine.frames++;
}

/* ---------- the pose checks */

static unsigned long render_engine_state_digest(
	void)
{
	unsigned long crc = 0;
	long index;

	for (index = 0; index < real_map_allocation_count(); index++)
	{
		crc = crc * 31UL + real_map_digest_bytes(real_map_allocation_address(index),
			real_map_allocation_bytes(index));
	}

	return crc;
}

static long render_engine_allocation(
	char const *name)
{
	long index;

	for (index = 0; index < real_map_allocation_count(); index++)
	{
		if (!csstrcmp(real_map_allocation_name(index), name))
			return index;
	}

	return NONE;
}

/* does the camera effect matrix write the game state at this pose? (it is
restored at once: a diagnosis, not part of the frame) */
static int render_engine_camera_effect_writes(
	void)
{
	long index = render_engine_allocation("player effects");
	unsigned long bytes;
	unsigned long seed;
	unsigned long before, after;
	real_matrix4x3 matrix;

	if (index == NONE)
		return -1;
	bytes = real_map_allocation_bytes(index);
	if (bytes > RENDER_SNAPSHOT_BYTES)
		return -1;
	csmemcpy(render_snapshot, real_map_allocation_address(index), bytes);
	seed = *get_global_local_random_seed_address();
	before = render_engine_state_digest();
	player_effect_get_camera_effect_matrix(0, &matrix);
	after = render_engine_state_digest();
	csmemcpy(real_map_allocation_address(index), render_snapshot, bytes);
	*get_global_local_random_seed_address() = seed;

	return before != after;
}

static int render_engine_capture(
	char const *name,
	boolean diagnostic_override)
{
	struct wii_gx_pose_result result;
	unsigned long before, after;
	int camera_effect_writes;
	int ok;

	before = render_engine_state_digest();
	lock_global_random_seed();
	render_engine_visibility(&render_engine.camera);
	render_engine_materials_drawn();
	wii_gx_pose_capture(&render_engine.view, render.environment_surface_flags, render_engine.drawn,
		render_engine_frontfacing, 3, &result);
	unlock_global_random_seed();
	after = render_engine_state_digest();
	camera_effect_writes = diagnostic_override ? -1 : render_engine_camera_effect_writes();
	/* lit and textured pixels, and culling as qualified: under GX_CULL_BACK
	no sample shows a back face, under GX_CULL_FRONT none a front face */
	ok = before == after && result.covered > 0 && result.differs_lightmap_only > 0 &&
		result.differs_base_only > 0 && result.facing_back[1] == 0 && result.facing_front[1] > 0 &&
		result.facing_front[2] == 0;
	wii_log("RENDER_POSE name=%s camera=%s eye=%.4f,%.4f,%.4f leaf=%ld cluster=%ld clusters=%ld surfaces=%ld "
		"crc_full=%08lx crc_lightmap_only=%08lx crc_base_only=%08lx samples=%d covered=%d distinct=%d "
		"differs_lightmap_only=%d differs_base_only=%d facing_none=%d/%d facing_back=%d/%d facing_front=%d/%d "
		"draw_us=%lu state_before=%08lx state_after=%08lx state_unchanged=%d camera_effect_writes=%d pass=%d\n",
		name, diagnostic_override ? "diagnostic_override" : "engine_unit_deterministic",
		(double)render_engine.view.eye[0], (double)render_engine.view.eye[1], (double)render_engine.view.eye[2],
		render_engine.view.leaf_index, render_engine.view.cluster_index, render_engine.view.rendered_clusters,
		render_engine.view.visible_surfaces, result.crc_full, result.crc_lightmap_only, result.crc_base_only,
		result.samples, result.covered, result.distinct, result.differs_lightmap_only, result.differs_base_only,
		result.facing_front[0], result.facing_back[0], result.facing_front[1], result.facing_back[1],
		result.facing_front[2], result.facing_back[2], result.draw_us, before, after, before == after,
		camera_effect_writes, ok);

	return ok;
}

static void render_engine_override_camera(
	int pose)
{
	struct structure_bsp *structure_bsp = global_structure_bsp_get();
	real_rectangle3d const *bounds = &structure_bsp->world_bounds;
	real_point3d centre;
	real_point3d eye;
	real_vector3d forward;
	real_vector3d up;
	real_vector3d left;

	centre.x = (bounds->x0 + bounds->x1) * 0.5f;
	centre.y = (bounds->y0 + bounds->y1) * 0.5f;
	centre.z = (bounds->z0 + bounds->z1) * 0.5f;
	if (pose == 0)
	{
		/* above a corner, looking down across the whole BSP */
		eye.x = centre.x - (bounds->x1 - bounds->x0) * 0.45f;
		eye.y = centre.y - (bounds->y1 - bounds->y0) * 0.45f;
		eye.z = bounds->z1 + (bounds->z1 - bounds->z0) * 0.6f;
	}
	else
	{
		/* the opposite corner, at mid height */
		eye.x = centre.x + (bounds->x1 - bounds->x0) * 0.35f;
		eye.y = centre.y + (bounds->y1 - bounds->y0) * 0.35f;
		eye.z = centre.z;
	}
	vector_from_points3d(&eye, &centre, &forward);
	normalize3d(&forward);
	up = *global_up3d;
	cross_product3d(&up, &forward, &left);
	normalize3d(&left);
	cross_product3d(&forward, &left, &up);
	normalize3d(&up);
	render_engine_camera_from(&render_engine.camera, &eye, &forward, &up, DEGREES_TO_RADIANS(70.0f));
}

/* ---------- public code */

void wii_render_register_hooks(
	void)
{
	struct wii_engine_render_hooks hooks;

	csmemset(&hooks, 0, sizeof(hooks));
	if (wii_render_enabled())
	{
		hooks.rasterizer_initialize = render_engine_rasterizer_initialize;
		hooks.rasterizer_dispose = render_engine_rasterizer_dispose;
		hooks.new_map = render_engine_new_map;
		hooks.old_map = render_engine_old_map;
		hooks.frame = render_engine_frame;
	}
	render_engine.configured = TRUE;
	render_engine.bsp_tag_index = NONE;
	wii_engine_set_render_hooks(&hooks);
}

int wii_render_pose_phase(
	const char *scenario_path)
{
	struct real_map_result result;
	long frame = 0;
	long next = 0;
	int failures = 0;
	int captured = 0;
	int pose;

	if (!wii_render_enabled() || !render_engine.initialized)
		return TRUE;
	render_engine.pose_phase = TRUE;
	wii_log("RENDER_POSES begin scenario=%s\n", scenario_path);
	if (!real_map_load(scenario_path) || !real_map_begin(NULL))
	{
		wii_log("RENDER_FAIL pose_phase_load\n");
		real_map_unload();
		render_engine.pose_phase = FALSE;
		return FALSE;
	}
	/* the scripted player at 30 Hz, every frame drawn; the engine's own
	camera at three ticks */
	while (frame < 100000 && real_map_frame(1.0f / 30.0f))
	{
		frame++;
		if (next < RENDER_POSE_TICK_COUNT && game_time_get() >= render_pose_ticks[next] && render_engine.view_valid)
		{
			char name[32];

			csprintf(name, "engine_tick_%ld", render_pose_ticks[next]);
			failures += !render_engine_capture(name, FALSE);
			captured++;
			next++;
		}
	}
	if (next < RENDER_POSE_TICK_COUNT && render_engine.view_valid)
	{
		/* the last tick ends the run inside its frame */
		failures += !render_engine_capture("engine_tick_300", FALSE);
		captured++;
	}
	/* two labelled diagnostic overrides of the camera, from the BSP's bounds;
	the engine's visibility is computed from each */
	for (pose = 0; pose < RENDER_OVERRIDE_POSES && render_engine.view_valid; pose++)
	{
		render_engine_override_camera(pose);
		failures += !render_engine_capture(pose == 0 ? "diagnostic_override_high" : "diagnostic_override_mid",
			TRUE);
		captured++;
	}
	real_map_end(&result);
	real_map_unload();
	wii_log("RENDER_POSES end frames=%ld poses=%d failures=%d ticks=%ld sim=%08lx camera_effect_frames=%ld\n", frame,
		captured, failures, result.ticks, result.simulation_digest, render_engine.camera_effect_skipped);
	render_engine.pose_phase = FALSE;

	return failures == 0 && captured == RENDER_POSE_TICK_COUNT + RENDER_OVERRIDE_POSES;
}
