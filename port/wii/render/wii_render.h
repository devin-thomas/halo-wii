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
  pool_max=<bytes>     the render pool's largest size (HWI-030, below;
                       default: what the heap has above the keeps)
  pool_keep=<mem1>,<mem2>  heap bytes the pool leaves the engine in each
                       bank (defaults below)
  geometry_check=0|1   compare the drawn geometry, as the tag slot holds it,
                       with the content generation's HWL1 file, read in
                       chunks, at every BSP load (default 1)
  models=0|1           HWI-082: draw the objects' models (default 1; 0 is
                       the environment-only frame of before)
  model_skin_bytes=<bytes>  HWI-082: the models' skinning buffer, from the
                       BSP's store (default WII_RENDER_DEFAULT_MODEL_SKIN_BYTES)
  first_person=0|1     HWI-084: draw the first-person weapon and hands
                       (default 1, while models are drawn)
  first_person_store_pose=0|1  HWI-084: keep each drawn frame's first-person
                       orientations as the view model's last pose, which its
                       state changes blend from (default 1)
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

Geometry (HWI-030): GX draws each BSP material's vertices where the staged
map holds them, in the tag slot: the compressed environment vertex
(position F32 at 0, texcoord F32 at 24, stride 32) and the lightmap vertex
(texcoord S16 at 4, stride 8), as map_stage.py converts them to big-endian,
and the BSP's surfaces (three u16 each). Nothing is copied; the HWL1 file
(the same bytes, from the content pipeline) is only compared.

Textures (HWI-030): one render pool per map, taken from the heap at the
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
	/* HWI-030: the material's vertices in the tag slot (its
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
	/* HWI-030: the full draw's visible triangles of environment materials
	with a base map: drawn with it resident, or lightmap-only (not resident) */
	unsigned long textured_triangles, base_missing_triangles;
	/* the samples that show an environment material with a resident base map */
	int textured_samples;
	/* HWI-082: the samples the object models changed over the full draw, and
	the model parts drawn, refused and their triangles */
	int object_samples;
	unsigned long model_parts, model_parts_refused, model_triangles;
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

/* ---------- HWI-082: object models

render_objects_engine.c (engine side) finds the objects in the rendered
clusters as render_objects' find_rendered_objects does, with marks of its
own (the engine's object marker writes each object's magic_number), right
after each frame's visibility: wii_render_objects_collect. Then, from
inside each GX draw, after the environment, the GX side asks it to draw one
pass (wii_gx_objects_proc): it walks the objects as render_object_list does
(children too; the local player's unit in first person is not drawn), and
for each model render_model would draw (detail cutoff, level of detail,
region permutations), sets the model's node matrices (wii_gx_model_begin:
the object's node matrices times each node's runtime_default_inverse_matrix,
as the vertex shader's constants) and hands each of its parts of that pass
to the GX side (wii_gx_model_part_draw), which skins it on the CPU from the
tag slot into a bounded buffer (hwm_skin_decode_le, hwm_skin_apply) and
draws it. Only the game state is read: no render state cache
(cached_render_state_index), no lighting cache, no camera effects.

Material (diagnostic, labelled): no textures, no shader; one fixed
directional light over the skinned normal. Solid parts opaque grey, the
alpha-blended decal parts of model shaders light blue without depth writes,
transparent parts a 50% magenta blend without depth writes, drawn after
every opaque part in object order (unsorted). */

enum wii_gx_model_pass
{
	_wii_gx_model_pass_solid,
	_wii_gx_model_pass_decal,
	_wii_gx_model_pass_transparent,
	NUMBER_OF_WII_GX_MODEL_PASSES
};

#define WII_GX_MODEL_MAXIMUM_NODES 64
#define WII_RENDER_DEFAULT_MODEL_SKIN_BYTES 524288UL

struct wii_gx_model_part
{
	/* the part's vertex buffer as the tag slot holds it: vertex_count compressed
	model vertices (type 5, 32 bytes, little-endian as staged) */
	const void *vertices;
	long vertex_count;
	int vertex_type;
	/* its triangle buffer: index_count u16 (big-endian: map_stage converts
	model indices), a precompiled strip (1) or a triangle list (0) */
	const void *indices;
	long index_count;
	int strip;
	int shader_type;
	int two_sided;          /* shader_model's two-sided flag: both faces drawn */
};

typedef void (*wii_gx_objects_proc)(int pass);

/* engine side */
void wii_render_objects_collect(void);
void wii_render_objects_draw(int pass);
/* RENDER_OBJECTS: the engine side's counts (when: a label) */
void wii_render_objects_report(const char *when);
/* GX side: the engine side's pass drawer; NULL draws no objects */
void wii_gx_set_objects_proc(wii_gx_objects_proc proc);
/* one model's node matrices (node_count, 1..WII_GX_MODEL_MAXIMUM_NODES, rows
as hwm_model.h's palette); valid until wii_gx_model_end. 1 if accepted */
int wii_gx_model_begin(const float (*palette)[3][4], int node_count);
/* one part, skinned and drawn now (or refused, counted and logged); 1 if drawn */
int wii_gx_model_part_draw(const struct wii_gx_model_part *part);
void wii_gx_model_end(void);

/* GX side, inside (render_gx.c <-> render_models_gx.c): render.txt's models
and model_skin_bytes; the skinning buffer (from the BSP's store, given back
at its end: wii_gx_models_unbind waits for GX first); one draw of every
pass with the view loaded, returning the parts drawn; the totals */
void wii_gx_models_configure(int enabled, unsigned long skin_bytes);
int wii_gx_models_enabled(void);
unsigned long wii_gx_models_skin_bytes(void);
void wii_gx_models_bind(void *buffer, unsigned long bytes);
void wii_gx_models_unbind(void);
unsigned long wii_gx_models_draw(void);
void wii_gx_models_last(unsigned long *parts, unsigned long *refused, unsigned long *triangles);
void wii_gx_models_report(void);

/* ---------- HWI-084: the first-person weapon

What render.c's render_window and render_objects do for it, in the same
frame as the objects (render_first_person_engine.c, engine side):
- first_person_weapon_render_update's pose and visibility, at its point
  (after the frame's camera and visibility, with the objects' collect):
  wii_render_first_person_collect, through the engine's
  first_person_weapon_render_pose (interface/first_person_weapons.c): the
  view model's state animation, overlays and interpolation, as the engine
  computes them, into the animation graph's node matrices from the frame's
  camera. On a drawn frame (store_pose) the orientations are kept as the
  view model's last pose drawn, which its next state change blends from (a
  presentation field of a frame-coupled allocation; nothing else of the game
  state is written; pose checks store nothing).
- first_person_weapon_draw: the weapon's first-person model, then the
  first-person interface's hands, each through its own node remapping table
  (model node -> graph node; every entry checked, a model of more than
  WII_GX_MODEL_MAXIMUM_NODES nodes or with a bad entry refused, counted),
  drawn as render_model draws them (level of detail 0 pixels, every region's
  permutation 0, each part in its pass) through the objects' model walk
  (render_models_engine.h) and the GX side's parts: the first-person passes
  (wii_render_first_person_draw), after the objects', in the diagnostic
  material, with the projection of the first-person near and far clip
  distances (rasterizer_set_frustum_z's) in the front depth slice.
Not drawn: the weapon's and hands' shaders, lighting
(object_get_cached_render_lighting writes the render state cache), change
colours, function values and active camouflage; first-person effects and
particles (the view model's visible flag, which starts them, is not
written); the sway inputs the render update writes (render facing, the
facing of the last frame), so the turning and moving sway stay at rest. */

/* the first-person passes' depth: the front 1/32 of the range (24 bits) */
#define WII_GX_FIRST_PERSON_DEPTH_SLICE (1.0f / 32.0f)

struct wii_gx_first_person_view
{
	float projection[4][4];  /* GX_PERSPECTIVE: the weapon's */
	float restore[4][4];     /* the frame's, loaded again after */
};

/* the weapon's projection from the frame's (GX_PERSPECTIVE rows): x and y
kept, depth from the first-person near and far clip distances into the
front slice [0, slice] of the depth range (render_first_person_projection.c);
1 if the distances and slice are usable (0 < near < far, 0 < slice <= 1) */
int wii_gx_first_person_projection(const float frame[4][4], float near_distance, float far_distance, float slice,
	float out[4][4]);

typedef void (*wii_gx_first_person_proc)(int pass);

/* engine side: the frame's pose (view: the frame's GX view, after its
visibility; store_pose: a drawn frame, not a pose check), one pass of its
draw, and forget (the map's end, or a diagnostic camera: nothing collected
is drawn after it) */
void wii_render_first_person_collect(const struct wii_gx_view *view, int store_pose);
void wii_render_first_person_draw(int pass);
void wii_render_first_person_forget(void);
/* RENDER_FIRST_PERSON: the engine side's counts */
void wii_render_first_person_report(const char *when);

/* GX side (render_models_gx.c): render.txt's first_person=0|1 (default 1:
drawn while models are) and first_person_store_pose=0|1 (default 1), as
read_config hands it lines (1 if the line was one of these) */
int wii_gx_models_config_line(const char *line);
int wii_gx_first_person_enabled(void);
int wii_gx_first_person_store_pose(void);
void wii_gx_set_first_person_proc(wii_gx_first_person_proc proc);
/* the frame's first-person projection (NULL: no first-person weapon this
frame); copied, valid until the next call */
void wii_gx_first_person_view_set(const struct wii_gx_first_person_view *view);
/* the last draw's first-person parts (included in wii_gx_models_last's) */
void wii_gx_first_person_last(unsigned long *parts, unsigned long *refused, unsigned long *triangles);

#endif
