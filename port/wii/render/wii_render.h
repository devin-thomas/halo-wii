/*
WII_RENDER.H

The Wii rasterizer's environment path (HWI-016B): the loaded map's structure
BSP drawn through GX from the engine's own render camera, every game_frame.

Three parts, one interface (C types only, so each side includes it beside
its own headers):

- render_engine.c, an engine unit (the engine's headers and flags): the
  rasterizer's start-up the Wii build had skipped, its per-map hooks, and
  each frame's camera (director_update, observer_update, the camera values
  main.c builds) and visibility (structure_visibility_find_camera and
  structure_visibility_compute: the BSP's clusters, portals and PVS); then
  what each BSP material is (its shader's type, base map, lightmap page,
  breakable surface) and which of its surfaces are visible.
- render_gx.c, a driver unit (libogc): the converted geometry (HWL1) and
  textures (HWT1) staged on the card, GX state, the draw, EFB samples, the
  frame copy, timings and memory.
- the driver (port/wii/engine/wii_engine_main.c) sets video and GX up,
  registers the hooks with the engine-side driver (engine_hooks.h), and asks
  for the fixed-pose checks after the map run.

Configuration: WII_ENGINE_ROOT/render.txt, optional, "key=value" lines:
  render=off           the HWI-015B build: no rasterizer (it reports itself
                       unsupported), nothing drawn
  mode=poses_only      skip the map run; load the map once and run the
                       fixed-pose checks (frame dumps stay short)
  base_budget=<bytes>  base map residency budget (default below)
*/

#ifndef __HALO_WII_RENDER_H
#define __HALO_WII_RENDER_H

/* the provisional residency budget for environment base maps, pending the
Wii texture cache (HWI-032): bytes of HWT1 texture data */
#define WII_RENDER_DEFAULT_BASE_BUDGET 2097152UL

/* ---------- driver (wii_engine_main.c) */

struct wii_render_video
{
	void *mode;      /* GXRModeObj * */
	void *xfb[2];    /* the driver's two external framebuffers */
};

/* reads render.txt, sets the GX state up; 1 if the rasterizer is enabled */
int wii_render_configure(const struct wii_render_video *video);
int wii_render_enabled(void);
int wii_render_poses_only(void);
/* the engine-side hooks (render_engine.c), registered with engine_hooks.c */
void wii_render_register_hooks(void);
/* the staged map (map.txt's scenario path, levels\...\<name>) */
void wii_render_set_map(const char *scenario_path);
/* load the map once, run the scripted player at 30 Hz with the frames
drawn, and check fixed poses: three of the engine's own camera (ticks 30,
150, 300) and two labelled diagnostic overrides. 1 if every check passed */
int wii_render_pose_phase(const char *scenario_path);
/* the totals over the launch (RENDER_TOTALS) */
void wii_render_report(void);

/* ---------- engine side -> GX side */

enum wii_gx_material_kind
{
	_wii_gx_material_environment,          /* base map x lightmap through TEV */
	_wii_gx_material_placeholder_transparent, /* labelled placeholder */
	_wii_gx_material_placeholder_other,    /* labelled placeholder */
	NUMBER_OF_WII_GX_MATERIAL_KINDS
};

struct wii_gx_material
{
	int kind;
	int shader_type;          /* the engine's _shader_type_* */
	long lightmap_page;       /* the lightmap bitmap's index, or -1 */
	long base_tag;            /* the base map's bitmap tag index, or -1 */
	int base_bitmap;          /* its bitmap: permutation % bitmap count */
	int base_width, base_height; /* the engine's bitmap_data, checked against the HWT1 */
	long first_surface, surface_count;
	int detail_maps;          /* not drawn (counted): detail, micro detail */
	int bump_map;             /* not drawn (counted) */
};

struct wii_gx_view
{
	float world_to_view[3][4]; /* the engine's frustum, as a GX position matrix */
	float projection[4][4];    /* GX_PERSPECTIVE */
	float eye[3];
	long leaf_index, cluster_index;
	long rendered_clusters, visible_surfaces;
	unsigned long visibility_us; /* camera and visibility, CPU */
};

/* per map: HWL1 geometry of the BSP whose tag index this is; 1 on success */
int wii_gx_map_begin(unsigned long bsp_tag_index, long material_count, long surface_count);
int wii_gx_material_set(long index, const struct wii_gx_material *material);
/* the lightmap pages and the base maps that fit the budget, by surface
coverage; 1 on success (base maps left out are reported, not failed) */
int wii_gx_textures_load(long lightmap_tag_index);
void wii_gx_map_end(void);
int wii_gx_map_ready(void);

/* one game frame: every visible surface (surface_flags: the engine's bit
vector, bit i of word i/32) of every material whose breakable surface stands
(material_drawn), opaque first; then the frame copy. With sample, the EFB is
sampled first (a 16x12 grid): its CRC and covered samples */
void wii_gx_frame(const struct wii_gx_view *view, const unsigned long *surface_flags,
	const unsigned char *material_drawn, int sample, unsigned long *crc, int *covered);

/* the engine's rule (render_camera_triangle_frontfacing), for the facing
classification of the pose checks */
typedef int (*wii_gx_frontfacing_proc)(const float eye[3], const float p0[3], const float p1[3], const float p2[3]);

struct wii_gx_pose_result
{
	unsigned long crc_full, crc_lightmap_only, crc_base_only;
	int samples, covered, distinct, differs_lightmap_only, differs_base_only;
	int facing_front[3], facing_back[3]; /* classified, cull none/back/front */
	unsigned long draw_us;
};

void wii_gx_pose_capture(const struct wii_gx_view *view, const unsigned long *surface_flags,
	const unsigned char *material_drawn, wii_gx_frontfacing_proc frontfacing, int present_frames,
	struct wii_gx_pose_result *out);
#endif
