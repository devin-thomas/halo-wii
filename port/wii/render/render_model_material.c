/*
 * RENDER_MODEL_MATERIAL.C
 *
 * HWI-086: the shader_model base map material rules and the bounded
 * bitmap residency of the models (render_model_material.h).
 */
#include <string.h>

#include "render_model_material.h"

void wii_model_material_state(unsigned shader_flags, struct wii_model_material_state *out)
{
    const int decal = (shader_flags >> WII_MODEL_SHADER_ALPHA_BLENDED_DECAL_BIT) & 1u;
    const int not_alpha_tested = (shader_flags >> WII_MODEL_SHADER_NOT_ALPHA_TESTED_BIT) & 1u;
    /* D3DRS_ALPHATESTENABLE = !alpha_blended_decal && !not_alpha_tested;
    D3DRS_ALPHABLENDENABLE = alpha_blended_decal; D3DRS_ZWRITEENABLE =
    !alpha_blended_decal; the second draw of a two-sided shader with
    D3DCULL_CW */
    out->alpha_test = !decal && !not_alpha_tested;
    out->blend = decal;
    out->depth_write = !decal;
    out->both_faces = (int)((shader_flags >> WII_MODEL_SHADER_TWO_SIDED_BIT) & 1u);
}

void wii_model_texcoord_rows(float u_scale, float v_scale, float rows[2][3])
{
    /* shader_texture_animation_evaluate with every animation input 0: the
    angle is 0 (cosine 1, sine 0), u_input = -r_center.x, v_input =
    -r_center.y, so the offsets are -r_center + r_center = 0 */
    rows[0][0] = u_scale;
    rows[0][1] = 0.0f;
    rows[0][2] = 0.0f;
    rows[1][0] = 0.0f;
    rows[1][1] = v_scale;
    rows[1][2] = 0.0f;
}

void wii_model_texture_matrix(const float rows[2][3], float matrix[2][4])
{
    for (int r = 0; r < 2; ++r) {
        matrix[r][0] = rows[r][0] / WII_MODEL_NORMSHORT_SCALE;
        matrix[r][1] = rows[r][1] / WII_MODEL_NORMSHORT_SCALE;
        matrix[r][2] = 0.0f;
        matrix[r][3] = rows[r][2];
    }
}

void wii_model_texcoord_apply(const float rows[2][3], int16_t s, int16_t t, float out[2])
{
    const float u = (float)s / WII_MODEL_NORMSHORT_SCALE, v = (float)t / WII_MODEL_NORMSHORT_SCALE;
    for (int r = 0; r < 2; ++r)
        out[r] = rows[r][0] * u + rows[r][1] * v + rows[r][2];
}

/* ---------- residency */

static uint32_t align32(uint32_t value)
{
    return (value + (WII_TEXTURE_CACHE_ALIGN - 1u)) & ~(WII_TEXTURE_CACHE_ALIGN - 1u);
}

static int entries_usable(int entries)
{
    return entries >= WII_MODEL_TEXTURES_MINIMUM_ENTRIES && entries <= WII_MODEL_TEXTURES_MAXIMUM_ENTRIES;
}

static uint32_t entry_table_bytes(int entries)
{
    return align32((uint32_t)((size_t)entries * sizeof(struct wii_texture_cache_entry)));
}

static uint32_t block_table_bytes(int entries)
{
    return align32((uint32_t)((size_t)WII_TEXTURE_CACHE_REGIONS * (size_t)wii_texture_cache_blocks_per_region(entries) *
                              sizeof(struct wii_texture_cache_block)));
}

uint32_t wii_model_textures_table_bytes(int entries)
{
    if (!entries_usable(entries))
        return 0;
    return entry_table_bytes(entries) + block_table_bytes(entries) +
           align32((uint32_t)((size_t)entries * sizeof(struct wii_model_texture_slot)));
}

void wii_model_textures_reset(struct wii_model_textures *t)
{
    /* the cache keeps its generation number: the next init is newer, and
    every ticket of this one is stale */
    uint32_t generation = t->cache.generation;
    memset(t, 0, sizeof(*t));
    t->cache.generation = generation;
}

int wii_model_textures_init(struct wii_model_textures *t, void *memory, uint32_t bytes, int entries,
                            int stats_per_frame, int loads_per_frame, uint32_t frame_bytes, uint32_t keep_frames,
                            uint32_t completed, wii_texture_cache_wait_proc wait, void *wait_context)
{
    unsigned char *base = memory;
    wii_model_textures_reset(t);
    const uint32_t tables = wii_model_textures_table_bytes(entries);
    if (!base || ((uintptr_t)base & (WII_TEXTURE_CACHE_ALIGN - 1u)) || !tables || !wait ||
        bytes < tables + WII_TEXTURE_CACHE_ALIGN || stats_per_frame < 1 || loads_per_frame < 1)
        return 0;
    const uint32_t entry_bytes = entry_table_bytes(entries), block_bytes = block_table_bytes(entries);
    struct wii_texture_cache_entry *entry_table = (struct wii_texture_cache_entry *)(void *)base;
    struct wii_texture_cache_block *block_table = (struct wii_texture_cache_block *)(void *)(base + entry_bytes);
    struct wii_model_texture_slot *slots =
        (struct wii_model_texture_slot *)(void *)(base + entry_bytes + block_bytes);
    unsigned char *const region_base[WII_TEXTURE_CACHE_REGIONS] = {base + tables, NULL};
    const uint32_t region_bytes[WII_TEXTURE_CACHE_REGIONS] = {(bytes - tables) & ~(WII_TEXTURE_CACHE_ALIGN - 1u), 0};
    if (!wii_texture_cache_init(&t->cache, entry_table, entries, block_table,
                                wii_texture_cache_blocks_per_region(entries), region_base, region_bytes, completed,
                                wait, wait_context)) {
        wii_model_textures_reset(t);
        return 0;
    }
    memset(slots, 0, (size_t)entries * sizeof(*slots));
    t->slots = slots;
    t->capacity = entries;
    t->count = 0;
    t->stats_per_frame = stats_per_frame;
    t->loads_per_frame = loads_per_frame;
    t->frame_bytes = frame_bytes;
    t->keep_frames = keep_frames;
    t->arena_bytes = region_bytes[0];
    t->table_bytes = tables;
    return 1;
}

int wii_model_textures_ready(const struct wii_model_textures *t)
{
    return t->slots != NULL && t->capacity > 0;
}

int wii_model_textures_want(struct wii_model_textures *t, long tag, long bitmap, int width, int height)
{
    if (!wii_model_textures_ready(t) || tag < 0 || bitmap < 0)
        return -1;
    int index = -1;
    for (int i = 0; i < t->count; ++i)
        if (t->slots[i].tag == tag && t->slots[i].bitmap == bitmap) {
            index = i;
            break;
        }
    if (index < 0) {
        if (t->count == t->capacity) {
            t->stats.table_full++;
            return -1;
        }
        /* a new key: its cache entry stays refused (missing, from init)
        until the update looks for its file */
        index = t->count++;
        t->slots[index].tag = tag;
        t->slots[index].bitmap = bitmap;
        t->slots[index].width = width;
        t->slots[index].height = height;
        t->slots[index].looked_up = 0;
    }
    t->stats.wanted++;
    wii_texture_cache_want(&t->cache, index, 1);
    return index;
}

int wii_model_textures_resident(const struct wii_model_textures *t, int index)
{
    return wii_model_textures_ready(t) && wii_texture_cache_resident(&t->cache, index);
}

void wii_model_textures_used(struct wii_model_textures *t, int index, uint32_t sequence)
{
    if (wii_model_textures_ready(t))
        wii_texture_cache_used(&t->cache, index, sequence);
}

void wii_model_textures_gpu_done(struct wii_model_textures *t, uint32_t sequence)
{
    if (wii_model_textures_ready(t))
        wii_texture_cache_gpu_done(&t->cache, sequence);
}

int wii_model_textures_update(struct wii_model_textures *t, wii_model_texture_size_proc size,
                              wii_model_texture_load_proc load, void *context)
{
    int published = 0, looked = 0, loads = 0;
    uint32_t bytes = 0;
    if (!wii_model_textures_ready(t) || !size || !load)
        return 0;
    /* the files of new keys, at most stats_per_frame; their demand carried over */
    for (int i = 0; i < t->count && looked < t->stats_per_frame; ++i) {
        struct wii_model_texture_slot *slot = &t->slots[i];
        if (slot->looked_up)
            continue;
        const unsigned long demand = t->cache.entries[i].demand;
        int found = 0;
        const uint32_t file_bytes = size(context, slot->tag, slot->bitmap, &found);
        wii_texture_cache_entry_set(&t->cache, i, slot->tag, slot->bitmap, found, file_bytes, 0);
        wii_texture_cache_want(&t->cache, i, demand);
        slot->looked_up = 1;
        looked++;
        t->stats.looked_up++;
        t->stats.missing += t->cache.entries[i].refusal == _wii_texture_cache_refusal_missing;
        t->stats.oversize += t->cache.entries[i].refusal == _wii_texture_cache_refusal_oversize;
    }
    /* the most wanted loadable textures, within the frame bounds */
    for (;;) {
        struct wii_texture_cache_ticket ticket;
        const int e = wii_texture_cache_next_wanted(&t->cache);
        if (e < 0)
            break;
        const uint32_t need = t->cache.entries[e].bytes;
        if (loads >= t->loads_per_frame || (loads && bytes + need > t->frame_bytes)) {
            t->stats.deferred++;
            break;
        }
        if (wii_texture_cache_reserve(&t->cache, e, 1, t->keep_frames, &ticket) != _wii_texture_cache_ok)
            continue;  /* no room this frame: the cache skips it and offers the next */
        loads++;
        bytes += need;
        const int result = load(context, e, &t->slots[e], &ticket);
        if (result == 0) {
            if (wii_texture_cache_publish(&t->cache, &ticket)) {
                published++;
                t->stats.loads++;
                t->stats.loaded_bytes += need;
            }
        } else {
            wii_texture_cache_abandon(&t->cache, &ticket, result == 1);
            if (result == 1)
                t->stats.load_failed++;
            else
                t->stats.invalid++;
        }
    }
    wii_texture_cache_frame_begin(&t->cache);
    return published;
}
