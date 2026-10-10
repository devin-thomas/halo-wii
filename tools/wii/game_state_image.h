#ifndef WII_GAME_STATE_IMAGE_H
#define WII_GAME_STATE_IMAGE_H
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* HWI-007 game-state ownership prototype (proof ladder 4).
 *
 * The upstream game state is one region the engine allocates from in order
 * (game_state_malloc), whose saved image is the region's raw bytes: native
 * pointers included (each data array's elements, the object pool's blocks and
 * the element fields that reference them). Upstream accepts an image only at
 * the address it was taken at (game_state_image_accept).
 *
 * Here every census allocation (game_state_census_table.c: the measured
 * upstream counts and sizes) is made in a caller-provided region with the
 * actual engine units source/memory/data.c and memory_pool.c, representative
 * player/object/actor/prop state is created and updated through the engine's
 * own calls, and the state is saved as a canonical little-endian image with no
 * native pointer in it: the pool blocks are persisted in order with owners
 * (allocation, element, field), and element pointer fields as block ordinals.
 * Restore rebuilds the region at any address and rebinds every pointer.
 *
 * Bounds: element and global payloads other than the registered pointer
 * fields are persisted as 32-bit words (the prototype writes only 32-bit
 * fields; real engine globals need per-field encoders). The lruv cache and
 * GPU (decal vertex) allocations hold procedures and GPU data; they are
 * rebuilt empty, not persisted. */

enum gs_kind { GS_DATA = 1, GS_POOL = 2, GS_BYTES = 3, GS_GPU = 4, GS_LRUV = 5 };

struct gs_census_row {
    const char *name;
    int kind;
    long count, element_size, bytes_each, multiplicity;
};
extern const struct gs_census_row gs_census_rows[];
extern const unsigned gs_census_row_count;
extern const unsigned long gs_census_cpu_requested, gs_census_gpu_requested;
extern const unsigned long gs_census_data_array_header, gs_census_memory_pool_header;

#define GS_MAX_ALLOCATIONS 96
#define GS_MAX_STALE 64

enum gs_error {
    GS_OK, GS_ARGUMENT, GS_CAPACITY, GS_ENGINE, GS_LAYOUT, GS_FULL, GS_IO, GS_MAGIC, GS_CHECKSUM,
    GS_TRUNCATED, GS_MISMATCH, GS_COUNTS, GS_OWNER, GS_STATE
};

struct gs_allocation {
    int kind;
    size_t offset, size;      /* region offset and bytes, as game_state_malloc charges them */
    long count, element_size; /* a data array's maximum count and element size; a pool's bytes */
    const char *name;
};

struct gs_region {
    unsigned char *base;
    size_t size, cpu_size, gpu_size, cpu_used, gpu_used;
    uint32_t allocation_checksum;
    unsigned count;
    struct gs_allocation allocations[GS_MAX_ALLOCATIONS];
    int players, objects, pool, actors, props;
    uint32_t step;
    unsigned stale_count;
    uint32_t stale[GS_MAX_STALE]; /* deleted object datum indices of the last update */
    int live;
};

struct gs_result {
    enum gs_error error;
    size_t offset; /* image byte offset, or allocation index */
};

struct gs_report {
    uint32_t step, players, objects, actors, props, blocks;
    uint32_t pool_used, pool_free, object_identifier_seed, player_identifier_seed;
    uint32_t image_bytes, image_crc, stale_checked, stale_rejected, compactions_moved;
};

const char *gs_error_name(enum gs_error);
/* Lays out every census allocation in region [base, base+size): the CPU part from the start, the GPU part
 * down from base+cpu_size+gpu_size, as game_state.c does; data arrays and the pool through the engine. Each
 * data array is made valid (data_make_valid) as at map start. */
int gs_build(struct gs_region *, void *base, size_t size, size_t cpu_size, size_t gpu_size, struct gs_result *);
/* Representative state through engine calls: step 0 creates players, their bipeds, weapons, vehicles,
 * scenery, actors and props; each later step deletes and makes objects (new identifiers at old indices),
 * moves pool blocks (free, reallocate, compact) and updates fields. Deleted datum indices are checked
 * stale (datum_try_and_get NULL) after the step. */
int gs_update(struct gs_region *, struct gs_result *);
/* Canonical image to file (or only measured when file is NULL); crc/bytes always computed. */
int gs_save(const struct gs_region *, FILE *, struct gs_report *, struct gs_result *);
/* Restore an image into a region just built by gs_build (any base). Rejects a damaged, truncated or
 * inconsistent image; on rejection the region must be rebuilt. */
int gs_restore(struct gs_region *, FILE *, struct gs_result *);
/* The engine's seed for an array named name, as data_delete_all computes it (identifier_seed) and as the
 * unported copy into a native short gives it on this target (native_seed). */
void gs_identifier_seeds(const char *name, uint16_t *identifier_seed, uint16_t *native_seed);
size_t gs_describe(const struct gs_report *, char *text, size_t capacity);
#endif
