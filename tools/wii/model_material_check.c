/* HWI-086: host check of port/wii/render/render_model_material.c (the
 * shader_model base map rules and the bounded bitmap residency of the models)
 * and hwm_texcoord_decode_le. Built and run by test_model_materials.py with
 * the strict flags of the Wii build; exits 0 when every check holds. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hwm_model.h"
#include "render_model_material.h"

static int failures;
#define CHECK(condition)                                                              \
    do {                                                                              \
        if (!(condition)) {                                                           \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition); \
            failures++;                                                               \
        }                                                                             \
    } while (0)

/* the fake GPU and file system */
static uint32_t gpu_submitted, gpu_completed;
static int waits;
static int missing_tag = -1, transient_tag = -1, invalid_tag = -1;
static int loads_seen;

static uint32_t wait_gpu(void *context)
{
    (void)context;
    waits++;
    gpu_completed = gpu_submitted;
    return gpu_completed;
}

static uint32_t file_size(void *context, long tag, long bitmap, int *found)
{
    (void)context;
    (void)bitmap;
    *found = tag != missing_tag;
    return *found ? (tag >= 100 ? 5000u : 1000u) : 0u;
}

static int file_load(void *context, int index, const struct wii_model_texture_slot *slot,
                     const struct wii_texture_cache_ticket *ticket)
{
    (void)context;
    (void)index;
    loads_seen++;
    if (slot->tag == transient_tag)
        return 1;
    if (slot->tag == invalid_tag)
        return 2;
    memset(ticket->data, (int)(slot->tag & 0xFF), ticket->bytes);
    return 0;
}

static void check_rules(void)
{
    struct wii_model_material_state s;
    wii_model_material_state(0, &s);
    CHECK(s.alpha_test && !s.blend && s.depth_write && !s.both_faces);
    wii_model_material_state(1u << WII_MODEL_SHADER_NOT_ALPHA_TESTED_BIT, &s);
    CHECK(!s.alpha_test && !s.blend && s.depth_write);
    wii_model_material_state(1u << WII_MODEL_SHADER_ALPHA_BLENDED_DECAL_BIT, &s);
    CHECK(!s.alpha_test && s.blend && !s.depth_write);
    wii_model_material_state(1u << WII_MODEL_SHADER_TWO_SIDED_BIT, &s);
    CHECK(s.both_faces && s.alpha_test);
    /* _rasterizer_model_draw reads two_sided only, not nocull_two_sided */
    wii_model_material_state(1u << WII_MODEL_SHADER_NOCULL_TWO_SIDED_BIT, &s);
    CHECK(!s.both_faces);
    /* detail_after_reflection and true atmospheric fog change no state here */
    wii_model_material_state(1u << WII_MODEL_SHADER_DETAIL_AFTER_REFLECTION_BIT |
                             1u << WII_MODEL_SHADER_TRUE_ATMOSPHERIC_FOG_BIT, &s);
    CHECK(s.alpha_test && !s.blend && s.depth_write && !s.both_faces);
    CHECK(WII_MODEL_ALPHA_REFERENCE == 0x7Fu);
}

static void check_texcoords(void)
{
    float rows[2][3], m[2][4], uv[2];
    wii_model_texcoord_rows(2.0f, 3.0f, rows);
    CHECK(rows[0][0] == 2.0f && rows[0][1] == 0.0f && rows[0][2] == 0.0f);
    CHECK(rows[1][0] == 0.0f && rows[1][1] == 3.0f && rows[1][2] == 0.0f);
    /* NORMSHORT2: 32767 is 1 exactly, -32767 is -1 */
    wii_model_texcoord_apply(rows, 32767, -32767, uv);
    CHECK(uv[0] == 2.0f && uv[1] == -3.0f);
    wii_model_texcoord_apply(rows, 0, 16384, uv);
    CHECK(uv[0] == 0.0f && fabsf(uv[1] - 3.0f * 16384.0f / 32767.0f) < 1e-6f);
    /* the GX matrix over the stored s16 computes the same */
    wii_model_texture_matrix(rows, m);
    CHECK(m[0][2] == 0.0f && m[1][2] == 0.0f && m[0][3] == 0.0f && m[1][3] == 0.0f);
    const int16_t samples[][2] = {{0, 0}, {32767, -32767}, {-32768, 12345}, {100, -100}};
    for (size_t i = 0; i < sizeof(samples) / sizeof(samples[0]); ++i) {
        float s = samples[i][0], t = samples[i][1];
        wii_model_texcoord_apply(rows, samples[i][0], samples[i][1], uv);
        CHECK(fabsf(m[0][0] * s + m[0][1] * t + m[0][3] - uv[0]) < 1e-5f);
        CHECK(fabsf(m[1][0] * s + m[1][1] * t + m[1][3] - uv[1]) < 1e-5f);
    }
    /* the stored texcoord: bytes 24..27, little-endian s16 */
    unsigned char vertex[HWM_SKIN_VERTEX_BYTES];
    int16_t st[2];
    memset(vertex, 0xAA, sizeof(vertex));
    vertex[24] = 0x01;
    vertex[25] = 0x80;
    vertex[26] = 0xFF;
    vertex[27] = 0x7F;
    hwm_texcoord_decode_le(vertex, st);
    CHECK(st[0] == -32767 && st[1] == 32767);
}

#define ENTRIES 4
static _Alignas(32) unsigned char memory[16384];

static uint32_t reserve_for(uint32_t arena)
{
    return wii_model_textures_table_bytes(ENTRIES) + arena;
}

static void new_frame(struct wii_model_textures *t, const long (*keys)[2], int count)
{
    for (int i = 0; i < count; ++i)
        wii_model_textures_want(t, keys[i][0], keys[i][1], 0, 0);
}

static void check_residency(void)
{
    static struct wii_model_textures t;
    CHECK(wii_model_textures_table_bytes(2) == 0);
    CHECK(!wii_model_textures_init(&t, memory, reserve_for(16), ENTRIES, 8, 2, 1u << 20, 2, 0, wait_gpu, NULL));
    CHECK(!wii_model_textures_ready(&t));
    CHECK(wii_model_textures_want(&t, 1, 0, 0, 0) == -1);
    CHECK(!wii_model_textures_init(&t, memory + 8, reserve_for(3072), ENTRIES, 8, 2, 1u << 20, 2, 0, wait_gpu, NULL));
    CHECK(wii_model_textures_init(&t, memory, reserve_for(3072), ENTRIES, 8, 2, 1u << 20, 2, 0, wait_gpu, NULL));
    CHECK(t.arena_bytes == 3072u);
    /* keys: one slot each, the same key the same slot */
    CHECK(wii_model_textures_want(&t, 1, 0, 0, 0) == 0);
    CHECK(wii_model_textures_want(&t, 1, 0, 0, 0) == 0);
    CHECK(wii_model_textures_want(&t, 2, 0, 0, 0) == 1);
    CHECK(wii_model_textures_want(&t, 1, 1, 0, 0) == 2);
    CHECK(wii_model_textures_want(&t, 3, 0, 0, 0) == 3);
    CHECK(wii_model_textures_want(&t, 4, 0, 0, 0) == -1);
    CHECK(t.stats.table_full == 1);
    CHECK(!wii_model_textures_resident(&t, 0));
    /* the first update looks every key up, then loads two (the most wanted first) */
    CHECK(wii_model_textures_update(&t, file_size, file_load, NULL) == 2);
    CHECK(t.stats.looked_up == 4 && t.stats.deferred == 1);
    CHECK(wii_model_textures_resident(&t, 0) && wii_model_textures_resident(&t, 1));
    CHECK(!wii_model_textures_resident(&t, 2) && !wii_model_textures_resident(&t, 3));
    CHECK(wii_texture_cache_check(&t.cache));
    /* the next frame: the third fits; the fourth would evict a texture wanted last frame: no */
    const long all[4][2] = {{1, 0}, {2, 0}, {1, 1}, {3, 0}};
    new_frame(&t, all, 4);
    CHECK(wii_model_textures_update(&t, file_size, file_load, NULL) == 1);
    CHECK(wii_model_textures_resident(&t, 2) && !wii_model_textures_resident(&t, 3));
    CHECK(t.cache.stats.evictions == 0 && t.cache.stats.no_space >= 1);
    /* every resident texture bound by GPU sequence 7, not complete */
    gpu_submitted = 7;
    for (int i = 0; i < 3; ++i)
        wii_model_textures_used(&t, i, 7);
    new_frame(&t, all, 4);
    CHECK(wii_model_textures_update(&t, file_size, file_load, NULL) == 0);
    /* only the fourth is wanted from now: after keep_frames one is evicted, after the GPU wait */
    const long fourth[1][2] = {{3, 0}};
    int published = 0;
    for (int frame = 0; frame < 4 && !published; ++frame) {
        new_frame(&t, fourth, 1);
        published = wii_model_textures_update(&t, file_size, file_load, NULL);
        if (!published)
            CHECK(waits == 0);
    }
    CHECK(published == 1 && wii_model_textures_resident(&t, 3));
    CHECK(waits == 1 && gpu_completed == 7);
    CHECK(t.cache.stats.evictions == 1);
    CHECK(wii_model_textures_resident(&t, 0) + wii_model_textures_resident(&t, 1) +
          wii_model_textures_resident(&t, 2) == 2);
    CHECK(wii_texture_cache_check(&t.cache));

    /* a new generation: missing, transient, invalid and oversize files */
    const uint32_t generation = t.cache.generation;
    CHECK(wii_model_textures_init(&t, memory, reserve_for(3072), ENTRIES, 8, 4, 1u << 20, 2, gpu_completed, wait_gpu,
                                  NULL));
    CHECK(t.cache.generation != generation && t.count == 0);
    missing_tag = 9;
    transient_tag = 8;
    invalid_tag = 7;
    const long bad[4][2] = {{9, 0}, {8, 0}, {7, 0}, {100, 0}};
    new_frame(&t, bad, 4);
    loads_seen = 0;
    CHECK(wii_model_textures_update(&t, file_size, file_load, NULL) == 0);
    CHECK(t.stats.missing == 1 && t.stats.oversize == 1 && t.stats.load_failed == 1 && t.stats.invalid == 1);
    CHECK(loads_seen == 2);
    CHECK(t.cache.entries[0].state == _wii_texture_cache_refused);
    CHECK(t.cache.entries[1].state == _wii_texture_cache_failed);
    CHECK(t.cache.entries[2].state == _wii_texture_cache_refused &&
          t.cache.entries[2].refusal == _wii_texture_cache_refusal_invalid);
    CHECK(t.cache.entries[3].refusal == _wii_texture_cache_refusal_oversize);
    /* the failed read backs off: not read again at once */
    new_frame(&t, bad, 4);
    CHECK(wii_model_textures_update(&t, file_size, file_load, NULL) == 0);
    CHECK(loads_seen == 2);
    CHECK(wii_texture_cache_check(&t.cache));

    /* the bytes of a frame: one load may pass the bound alone, the next waits */
    missing_tag = transient_tag = invalid_tag = -1;
    CHECK(wii_model_textures_init(&t, memory, reserve_for(3072), ENTRIES, 8, 4, 1500, 2, gpu_completed, wait_gpu,
                                  NULL));
    const long two[2][2] = {{20, 0}, {21, 0}};
    new_frame(&t, two, 2);
    CHECK(wii_model_textures_update(&t, file_size, file_load, NULL) == 1);
    CHECK(t.stats.deferred == 1);
    new_frame(&t, two, 2);
    CHECK(wii_model_textures_update(&t, file_size, file_load, NULL) == 1);
    CHECK(wii_model_textures_resident(&t, 0) && wii_model_textures_resident(&t, 1));
    wii_model_textures_reset(&t);
    CHECK(!wii_model_textures_ready(&t) && wii_model_textures_want(&t, 20, 0, 0, 0) == -1);
}

int main(void)
{
    check_rules();
    check_texcoords();
    check_residency();
    if (failures) {
        fprintf(stderr, "%d checks failed\n", failures);
        return 1;
    }
    puts("MODEL_MATERIAL_CHECK ok");
    return 0;
}
