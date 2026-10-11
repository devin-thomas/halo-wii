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
  base_limit=<count>   at most this many base maps resident, in coverage
                       order (comparisons of texture profiles at equal
                       residency; default: no limit)
  pool_max=<bytes>     the render pool's largest size (HWI-016E, below;
                       default: what the heap has above the keeps)
  pool_keep=<mem1>,<mem2>  heap bytes the pool leaves the engine in each
                       bank (defaults below)
  geometry_check=0|1   compare the drawn geometry, as the tag slot holds it,
                       with the content generation's HWL1 file, read in
                       chunks, at every BSP load (default 1)
  run_pose_ticks=<t>,...  the map run's sampled frames (game ticks; default
                       30,150,300)
  poses=<scenario path> [ticks=<t>,...] [switch_bsp=<t>:<i>,...] [overrides=each]
                       a fixed-pose phase on that map: the engine's camera at
                       those ticks (default 30,150,300); switch_bsp has the
                       script runtime switch BSPs as the map run's does;
                       overrides=each adds the two labelled diagnostic
                       cameras of the BSP at every tick (otherwise once, at
                       the end); one line each, in order (default: one, on
                       the plan's first map)

Geometry (HWI-016E): GX draws each BSP material's vertices where the staged
map holds them, in the tag slot: the compressed environment vertex
(position F32 at 0, texcoord F32 at 24, stride 32) and the lightmap vertex
(texcoord S16 at 4, stride 8), as map_stage.py converts them to big-endian,
and the BSP's surfaces (three u16 each). Nothing is copied; the HWL1 file
(the same bytes, from the content pipeline) is only compared.

Textures (HWI-016E): one render pool per map, taken from the heap at the
map's first BSP load in two parts (MEM1's arena above its keep, MEM2's
above the arena reserve and its keep) and given back when the map unloads;
each BSP load fills it again: the material tables, every lightmap page,
then base maps by coverage while they fit both the base budget and the
pool. Nothing else is allocated per BSP.
*/

#ifndef __HALO_WII_RENDER_H
#define __HALO_WII_RENDER_H

/* the provisional residency budget for environment base maps, pending the
Wii texture cache (HWI-032): bytes of HWT1 texture data. 3 MiB holds all ten
of Prisoner's base maps in gx-splitalpha-v1 (HWI-008F) */
#define WII_RENDER_DEFAULT_BASE_BUDGET 3145728UL

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
/* the staged map (a scenario path, levels\...\<name>: render/<name>/...);
each BSP load sets it again from the loaded scenario's own name */
void wii_render_set_map(const char *scenario_path);
/* each pose phase (render.txt's poses lines; by default one, on
scenario_path, the plan's first map): load the map once, run the scripted
player at 30 Hz with the frames drawn, and check fixed poses: the engine's
own camera at the phase's ticks (default 30, 150, 300) and two labelled
diagnostic overrides. 1 if every check passed */
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
	int environment_type;     /* shader_environment's type: normal 0, blended 1,
	                          blended base specular 2 */
	int base_alpha_used;      /* the shader reads the base map's alpha (environment
	                          types blended and blended base specular: it blends
	                          the detail maps, and is the specular mask) */
	/* HWI-016E: the material's vertices in the tag slot (its
	compressed_vertex_data: vertex_count compressed environment vertices,
	32 bytes each, then lightmap_vertex_count lightmap vertices, 8 bytes
	each, big-endian as staged), and its vertex buffers' types */
	const void *vertex_data;
	long vertex_data_bytes;
	long vertex_count, lightmap_vertex_count;
	int vertex_type, lightmap_vertex_type;
	long uncompressed_bytes;  /* the uncompressed vertex data: none is staged */
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

/* per BSP: the BSP whose tag index this is, its materials and its surfaces
block (surface_count of three u16 vertex indices, local to each material,
in the tag slot); 1 on success. The render pool is taken at a map's first */
int wii_gx_map_begin(unsigned long bsp_tag_index, long material_count, long surface_count, const void *surfaces);
int wii_gx_material_set(long index, const struct wii_gx_material *material);
/* the lightmap pages and the base maps that fit the budget, by surface
coverage; 1 on success (base maps left out are reported, not failed) */
int wii_gx_textures_load(long lightmap_tag_index);
void wii_gx_map_end(void);
/* the map unloads: the render pool goes back to the heap */
void wii_gx_pool_release(void);
/* the boot mode (HWI-016D): each texture its own heap block (the menus'
texture cache shares the heap), no pool */
void wii_gx_heap_textures(int on);
int wii_gx_map_ready(void);
/* render.txt's run_pose_ticks (count at most max) and the pose phases */
#define WII_RENDER_MAXIMUM_POSE_TICKS 16
#define WII_RENDER_MAXIMUM_POSE_SWITCHES 12
struct wii_render_pose_plan
{
	const char *map;             /* the scenario path; NULL: the plan's first map */
	long ticks[WII_RENDER_MAXIMUM_POSE_TICKS];
	int tick_count;
	long switch_ticks[WII_RENDER_MAXIMUM_POSE_SWITCHES];
	int switch_indices[WII_RENDER_MAXIMUM_POSE_SWITCHES];
	int switch_count;
	int overrides_each;
};
int wii_render_run_pose_ticks(long *ticks, int max);
int wii_render_pose_phase_count(void);
void wii_render_pose_plan_get(int phase, struct wii_render_pose_plan *plan);

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
	/* the base maps' alpha as TEV combines it (CMPR colour's map with its
	alpha plane, or RGBA8's own alpha), shown as grey: its CRC and the
	samples below 255 */
	unsigned long crc_base_alpha;
	int base_alpha_below_255;
	int samples, covered, distinct, differs_lightmap_only, differs_base_only;
	int facing_front[3], facing_back[3]; /* classified, cull none/back/front */
	unsigned long draw_us;
};

void wii_gx_pose_capture(const struct wii_gx_view *view, const unsigned long *surface_flags,
	const unsigned char *material_drawn, wii_gx_frontfacing_proc frontfacing, int present_frames,
	struct wii_gx_pose_result *out);
/* ---------- HWI-016D: the boot mode (render_ui.c)

wii_render_environment_boot: the rasterizer's start-up for the boot mode;
the per-frame hook then stands aside. wii_render_environment_draw: the
loaded scenario's environment from a window's camera (struct render_camera
const *, main_loop's own), into the EFB without the frame copy; 1 if drawn.
wii_gx_environment_draw: its GX half. */
void wii_render_environment_boot(void);
int wii_render_environment_draw(const void *render_camera);
void wii_gx_environment_draw(const struct wii_gx_view *view, const unsigned long *surface_flags,
	const unsigned char *material_drawn);

#endif
