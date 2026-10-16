/*
 * RENDER_TEXTURE_CACHE.H
 *
 * HWI-083 (HWI-054): the environment renderer's bounded base map residency.
 * render_gx.c gives the cache at most two byte regions it carved once per
 * BSP load (the render pool's, or one heap block in the boot mode, never
 * more than the configured limit), and the memory for its entry and block
 * tables. The cache allocates nothing. It decides where each base map lives,
 * which resident maps may be evicted for a visible one, and when the GPU
 * has stopped reading a byte range so it can be reused. render_gx.c reads,
 * checks and publishes a texture's content. Strict C11 with no libogc, so
 * tools/wii tests it on the host (texture_cache_check.c).
 *
 * The rules:
 * - An entry is drawn only once it is RESIDENT. A reservation (LOADING) is
 *   published only after its content has been checked, and only in the
 *   generation that made it (a BSP reset since makes the ticket stale).
 * - Every draw that binds an entry records the draw's GPU sequence (used).
 *   An entry is evicted only by a reservation that reuses its bytes, and a
 *   reservation that evicts an entry whose sequence the GPU has not completed
 *   calls the wait callback (GX_DrawDone) before it returns any bytes. No
 *   byte the GPU may still read is ever handed out.
 * - Only an entry nobody wanted for keep_frames frames can be evicted
 *   (never one visible this frame). Among the windows that fit, the cache
 *   takes the one whose evicted entries were wanted least recently.
 * - A missing file, a file larger than every region (oversize) or invalid
 *   content is refused for the BSP. A failed or short read is retried after
 *   a backoff, at most WII_TEXTURE_CACHE_MAX_FAILURES times.
 */
#ifndef WII_RENDER_TEXTURE_CACHE_H
#define WII_RENDER_TEXTURE_CACHE_H

#include <stdint.h>

#define WII_TEXTURE_CACHE_REGIONS 2
#define WII_TEXTURE_CACHE_ALIGN 32u
#define WII_TEXTURE_CACHE_MAX_FAILURES 3
#define WII_TEXTURE_CACHE_RETRY_FRAMES 120u

enum wii_texture_cache_state
{
	_wii_texture_cache_absent,    /* not resident; may be loaded */
	_wii_texture_cache_loading,   /* bytes reserved, content not yet checked: never drawn */
	_wii_texture_cache_resident,  /* checked and published: drawn */
	_wii_texture_cache_failed,    /* a read failed; retried from retry_frame */
	_wii_texture_cache_refused    /* never loaded for this BSP (refusal says why) */
};

enum wii_texture_cache_refusal
{
	_wii_texture_cache_refusal_none,
	_wii_texture_cache_refusal_missing,   /* no file staged */
	_wii_texture_cache_refusal_oversize,  /* larger than every region */
	_wii_texture_cache_refusal_invalid,   /* the content failed its checks */
	_wii_texture_cache_refusal_failures,  /* WII_TEXTURE_CACHE_MAX_FAILURES failed reads */
	NUMBER_OF_WII_TEXTURE_CACHE_REFUSALS
};

enum wii_texture_cache_result
{
	_wii_texture_cache_ok,
	_wii_texture_cache_no_space,      /* not without evicting a wanted entry */
	_wii_texture_cache_not_loadable   /* resident, loading, refused, backing off or out of range */
};

struct wii_texture_cache_entry
{
	long tag, bitmap;
	uint32_t bytes;            /* the file's */
	uint32_t span;             /* bytes reserved: bytes, aligned */
	long coverage;             /* static surfaces: the prefill's order and demand's tie-break */
	int state, refusal, failures;
	int region;                /* while loading or resident */
	uint32_t offset;
	unsigned char *data;
	unsigned long demand;      /* this frame's visible surfaces */
	uint32_t last_wanted;      /* the last frame with demand (0: never) */
	uint32_t skip_frame;       /* no space this frame: not offered again until the next */
	uint32_t retry_frame;
	uint32_t fence;            /* the last GPU sequence that bound it */
};

/* a region's placed bytes, by offset: a loading or resident entry's */
struct wii_texture_cache_block
{
	int entry;
	uint32_t offset, span;
};

struct wii_texture_cache_region
{
	unsigned char *base;
	uint32_t bytes;
	struct wii_texture_cache_block *blocks;
	int block_count;
};

struct wii_texture_cache_ticket
{
	int entry;
	uint32_t generation;
	unsigned char *data;
	uint32_t bytes;
};

struct wii_texture_cache_stats
{
	unsigned long reserves, publishes, abandons, stale, evictions, waits, no_space;
	unsigned long long bytes_published;
	uint32_t used, used_peak;  /* reserved and resident spans */
};

/* blocks until the GPU has completed every sequence submitted; returns the
sequence now complete */
typedef uint32_t (*wii_texture_cache_wait_proc)(void *context);

struct wii_texture_cache
{
	struct wii_texture_cache_entry *entries;
	int entry_count;
	struct wii_texture_cache_region regions[WII_TEXTURE_CACHE_REGIONS];
	int block_capacity;        /* per region */
	uint32_t generation;       /* kept across init: each init is a new one */
	uint32_t frame;            /* starts at 1 */
	uint32_t completed;        /* the GPU sequence known complete */
	wii_texture_cache_wait_proc wait;
	void *wait_context;
	struct wii_texture_cache_stats stats;
};

/* the block table each region needs for entry_count entries */
int wii_texture_cache_blocks_per_region(int entry_count);

/* a new generation over the caller's memory: entry_count entries,
WII_TEXTURE_CACHE_REGIONS * blocks_per_region blocks, and the regions (base
32-byte aligned; a region of 0 bytes is unused). completed: the GPU sequence
already complete. 0 if the arguments are unusable (the cache is then empty) */
int wii_texture_cache_init(struct wii_texture_cache *cache, struct wii_texture_cache_entry *entries, int entry_count,
	struct wii_texture_cache_block *blocks, int blocks_per_region, unsigned char *const base[WII_TEXTURE_CACHE_REGIONS],
	const uint32_t bytes[WII_TEXTURE_CACHE_REGIONS], uint32_t completed, wii_texture_cache_wait_proc wait,
	void *wait_context);

/* entry index's key and file: refused as missing (found 0) or oversize */
void wii_texture_cache_entry_set(struct wii_texture_cache *cache, int index, long tag, long bitmap, int found,
	uint32_t bytes, long coverage);

/* the next frame: every entry's demand back to 0 */
void wii_texture_cache_frame_begin(struct wii_texture_cache *cache);
/* entry index is visible this frame on this many surfaces (0 adds nothing) */
void wii_texture_cache_want(struct wii_texture_cache *cache, int index, unsigned long surfaces);
/* the loadable entry with the most demand this frame (then coverage), or -1 */
int wii_texture_cache_next_wanted(const struct wii_texture_cache *cache);

/* bytes for entry index. may_evict: entries not wanted for keep_frames
frames may be evicted. On ok, the ticket's bytes are writable (the GPU no
longer reads them) and the entry is loading */
enum wii_texture_cache_result wii_texture_cache_reserve(struct wii_texture_cache *cache, int index, int may_evict,
	uint32_t keep_frames, struct wii_texture_cache_ticket *ticket);
/* the ticket's content was checked: the entry is resident. 0 (the bytes
given back) if the ticket is stale */
int wii_texture_cache_publish(struct wii_texture_cache *cache, const struct wii_texture_cache_ticket *ticket);
/* the ticket's load failed: transient (a failed or short read, retried after
a backoff) or not (refused as invalid). Its bytes are given back */
void wii_texture_cache_abandon(struct wii_texture_cache *cache, const struct wii_texture_cache_ticket *ticket,
	int transient);

int wii_texture_cache_resident(const struct wii_texture_cache *cache, int index);
/* a draw of GPU sequence `sequence` bound entry index */
void wii_texture_cache_used(struct wii_texture_cache *cache, int index, uint32_t sequence);
/* the GPU completed sequence (after a GX_DrawDone) */
void wii_texture_cache_gpu_done(struct wii_texture_cache *cache, uint32_t sequence);

/* every invariant (blocks ordered, aligned, inside their region and
disjoint; each loading or resident entry exactly one block and no other entry
any; spans counted): 1 if it holds */
int wii_texture_cache_check(const struct wii_texture_cache *cache);

const char *wii_texture_cache_refusal_name(int refusal);

#endif
