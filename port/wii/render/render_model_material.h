/*
 * RENDER_MODEL_MATERIAL.H
 *
 * HWI-086 (HWI-054): the common shader_model base map material of the
 * object and first-person models, and the bounded residency of its bitmaps.
 * Strict C11 with no libogc and no engine header, so tools/wii tests it on
 * the host (test_model_materials.py); render_models_gx.c applies it through
 * GX and render_objects_engine.c reads the shader the rules come from.
 *
 * The rules are rasterizer_xbox_models.c's _rasterizer_model_draw for a
 * shader_model (type 4) without a model effect:
 * - stage 0 is the base map: rasterizer_set_texture(0, 2D, ..., base_map,
 *   permutation) (bitmap = permutation % bitmap count; a base map of NONE, no
 *   bitmaps or not 2D binds the globals' default texture instead), wrapped
 *   in u and v, linear magnification, minification and mip filters.
 * - its texcoord is the vertex's NORMSHORT2 (vertex_shader_declarations
 *   [0x78 / 4], register 4, type 0x21: s / 32767, t / 32767, then 0 and 1)
 *   through the two rows shader_texture_animation_evaluate writes into
 *   vertex constants 1 and 2 with the scales model.base_map_scale times
 *   shader map_u_scale and map_v_scale. With no texture animation (its u, v
 *   and rotation scales all 0) those rows are exactly (u_scale, 0, 0) and
 *   (0, v_scale, 0): the r_center terms cancel and the angle is 0.
 * - the output alpha is the base map's (the final combiner's G is T0's
 *   alpha). Colour writes are RGB only.
 * - alpha test (D3DRS_ALPHAFUNC GREATER, the device's default from
 *   rasterizer_xbox.c, D3DRS_ALPHAREF 0x7F) unless the shader is an
 *   alpha-blended decal or has not_alpha_tested.
 * - an alpha-blended decal (models.c draws it in the decal pass) blends
 *   source alpha over inverse source alpha and writes no depth; other parts
 *   write depth. Depth test LESSEQUAL either way.
 * - D3DCULL_CCW (GX_CULL_BACK, as the environment); a two-sided shader draws
 *   the part again with D3DCULL_CW: both faces (GX_CULL_NONE here).
 */
#ifndef WII_RENDER_MODEL_MATERIAL_H
#define WII_RENDER_MODEL_MATERIAL_H

#include <stdint.h>

#include "render_texture_cache.h"

/* shader_model flags (rasterizer_xbox_models.c) */
#define WII_MODEL_SHADER_DETAIL_AFTER_REFLECTION_BIT 0
#define WII_MODEL_SHADER_TWO_SIDED_BIT 1
#define WII_MODEL_SHADER_NOT_ALPHA_TESTED_BIT 2
#define WII_MODEL_SHADER_ALPHA_BLENDED_DECAL_BIT 3
#define WII_MODEL_SHADER_TRUE_ATMOSPHERIC_FOG_BIT 4
#define WII_MODEL_SHADER_NOCULL_TWO_SIDED_BIT 5

#define WII_MODEL_ALPHA_REFERENCE 0x7Fu   /* D3DRS_ALPHAREF; passes when alpha > it */
#define WII_MODEL_NORMSHORT_SCALE 32767.0f

struct wii_model_material_state
{
    int alpha_test;           /* alpha > WII_MODEL_ALPHA_REFERENCE, else discarded */
    int blend;                /* source alpha, inverse source alpha */
    int depth_write;
    int both_faces;           /* two-sided: GX_CULL_NONE (else GX_CULL_BACK) */
};

void wii_model_material_state(unsigned shader_flags, struct wii_model_material_state *out);

/* the texture rows of a static shader_model (no texture animation): u row
then v row over (s, t, 1), s and t already normalised */
void wii_model_texcoord_rows(float u_scale, float v_scale, float rows[2][3]);
/* GX's 2x4 texture matrix over the stored s16 texcoord: the rows with the
NORMSHORT2 normalisation (1 / 32767) folded in, z column 0 */
void wii_model_texture_matrix(const float rows[2][3], float matrix[2][4]);
/* the same in C, for one stored texcoord (what GX computes) */
void wii_model_texcoord_apply(const float rows[2][3], int16_t s, int16_t t, float out[2]);

/* ---------- the models' bitmaps: one bounded reserve

One block the caller carves once per BSP (from the same store as the
skinning buffer), holding the entry and block tables and the texture
arena, given to a texture cache of its own: model bitmaps never share an
entry, a key or an arena with the environment's lightmap pages or base
maps. Slots are keyed by (bitmap tag, bitmap) as the engine names them; a
key seen in a draw is wanted (demand), its file is looked for at the next
update (at most stats_per_frame new keys), and the most wanted absent
textures load there within loads_per_frame and frame_bytes (one larger
texture may load alone), evicting only entries nobody wanted for keep_frames
frames. The cache waits for the GPU before it reuses any byte a draw may
still read. Nothing is allocated after init. */

#define WII_MODEL_TEXTURES_MINIMUM_ENTRIES 4
#define WII_MODEL_TEXTURES_MAXIMUM_ENTRIES 1024

struct wii_model_texture_slot
{
    long tag, bitmap;
    int width, height;        /* the engine's bitmap_data, checked against the file */
    int looked_up;            /* the file was looked for (the cache entry is set) */
};

/* the file's size; *found 0 if it is not staged */
typedef uint32_t (*wii_model_texture_size_proc)(void *context, long tag, long bitmap, int *found);
/* the ticket's bytes read and checked for slot index: 0 ok, 1 a failed or
short read (retried after the cache's backoff), 2 invalid content (refused) */
typedef int (*wii_model_texture_load_proc)(void *context, int index, const struct wii_model_texture_slot *slot,
    const struct wii_texture_cache_ticket *ticket);

struct wii_model_textures_stats
{
    unsigned long wanted, table_full, looked_up, missing, oversize, loads, load_failed, invalid, deferred;
    unsigned long long loaded_bytes;
};

struct wii_model_textures
{
    struct wii_texture_cache cache;
    struct wii_model_texture_slot *slots;
    int capacity, count;
    int stats_per_frame, loads_per_frame;
    uint32_t frame_bytes, keep_frames;
    uint32_t arena_bytes, table_bytes;
    struct wii_model_textures_stats stats;
};

/* the tables' bytes for entries slots (32-byte aligned) */
uint32_t wii_model_textures_table_bytes(int entries);
/* a new generation over memory (32-byte aligned, bytes of it): the tables,
then the arena. 0 if unusable (memory too small for the tables and one
32-byte block, or entries outside the bounds): textures is then empty */
int wii_model_textures_init(struct wii_model_textures *textures, void *memory, uint32_t bytes, int entries,
    int stats_per_frame, int loads_per_frame, uint32_t frame_bytes, uint32_t keep_frames, uint32_t completed,
    wii_texture_cache_wait_proc wait, void *wait_context);
/* forgets everything (the GPU must read none of it any more) */
void wii_model_textures_reset(struct wii_model_textures *textures);
int wii_model_textures_ready(const struct wii_model_textures *textures);

/* a draw wants (tag, bitmap): its slot, or -1 (the table is full, or not
ready). Records one unit of demand */
int wii_model_textures_want(struct wii_model_textures *textures, long tag, long bitmap, int width, int height);
int wii_model_textures_resident(const struct wii_model_textures *textures, int index);
/* GPU sequence `sequence` bound slot index; the GPU completed sequence */
void wii_model_textures_used(struct wii_model_textures *textures, int index, uint32_t sequence);
void wii_model_textures_gpu_done(struct wii_model_textures *textures, uint32_t sequence);
/* before a frame's draws: new keys looked for, the most wanted loaded,
then the next frame begins (the demand cleared). Returns the textures
published (the caller invalidates GX's texture cache if any) */
int wii_model_textures_update(struct wii_model_textures *textures, wii_model_texture_size_proc size,
    wii_model_texture_load_proc load, void *context);

#endif
