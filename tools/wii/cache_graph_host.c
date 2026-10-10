/* Host reference for the HWI-007 whole-map graph walk (cache_schema_graph.c).
 * Loads owned tags and one BSP as the Wii guest does (arena plan, streamed reads,
 * cache_bsp_select), then prints the canonical report: the encoded walk, and where
 * native pointers fit 32 bits, the relocated native walk too.
 * Usage: cache_graph_host <tags.bin> <bsp.bin> <map_bytes> <bsp_ordinal> [placements]
 * Private inputs stay outside Git; the report carries counts and digests only. */
#include "cache_arena_plan.h"
#include "cache_bsp_probe.h"
#include "cache_schema_graph.h"
#include "cache_stream_io.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { TAG_SLOT, IO0_SLOT, IO1_SLOT, SLOT_COUNT };

static uint32_t file_crc(FILE *file, long *size)
{
    unsigned char buffer[65536];
    uint32_t crc = UINT32_MAX;
    size_t got;
    *size = 0;
    while ((got = fread(buffer, 1, sizeof(buffer), file)) > 0) {
        for (size_t i = 0; i < got; ++i) {
            crc ^= buffer[i];
            for (int bit = 0; bit < 8; ++bit)
                crc = (crc >> 1) ^ (UINT32_C(0xEDB88320) & (0u - (crc & 1u)));
        }
        *size += (long)got;
    }
    rewind(file);
    return ~crc;
}

static int run(const char *tags_path, const char *bsp_path, uint32_t map_bytes, size_t ordinal, size_t placement,
               void *workspace, size_t workspace_bytes)
{
    size_t shift = placement * 4096;
    size_t capacity = CACHE_GRAPH_CACHE_BYTES + 2 * CACHE_STREAM_IO_BYTES + 3 * 64 + shift;
    unsigned char *storage = malloc(capacity + 64);
    if (!storage)
        return 0;
    unsigned char *span = storage + ((64 - (uintptr_t)storage % 64) % 64) + shift;
    capacity -= shift;
    const struct cache_arena_request requests[SLOT_COUNT] = {
        {CACHE_GRAPH_CACHE_BYTES, 64}, {CACHE_STREAM_IO_BYTES, 64}, {CACHE_STREAM_IO_BYTES, 64}};
    struct cache_arena_plan plan;
    struct cache_arena_owner owner = {0};
    struct cache_arena_result arena;
    struct cache_arena_handle tag, io0, io1;
    int ok = 0;
    if (!cache_arena_plan_build(requests, SLOT_COUNT, span, capacity, 0, &plan, &arena) ||
        !cache_arena_owner_bind(&owner, &plan, span, capacity, &arena) ||
        !cache_arena_owner_handle(&owner, TAG_SLOT, &tag, &arena) ||
        !cache_arena_owner_handle(&owner, IO0_SLOT, &io0, &arena) ||
        !cache_arena_owner_handle(&owner, IO1_SLOT, &io1, &arena)) {
        printf("FAIL arena %s\n", cache_arena_error_name(arena.error));
        goto done;
    }
    FILE *tags_file = fopen(tags_path, "rb"), *bsp_file = fopen(bsp_path, "rb");
    long tag_bytes = 0, bsp_bytes = 0;
    if (!tags_file || !bsp_file) {
        printf("FAIL open\n");
        goto done;
    }
    uint32_t tag_crc = file_crc(tags_file, &tag_bytes), bsp_crc = file_crc(bsp_file, &bsp_bytes);
    struct cache_stream_result stream = {0};
    struct cache_bsp_reference reference;
    struct cache_bsp_result bsp;
    unsigned char *tags;
    memset(span + plan.slots[TAG_SLOT].offset, 0xa5, CACHE_GRAPH_CACHE_BYTES);
    if (!cache_stream_read(tags_file, &owner, &tag, &io0, &io1, (size_t)tag_bytes, tag_crc, &stream) ||
        !cache_bsp_select(&owner, &tag, (size_t)tag_bytes, map_bytes, ordinal, &reference, &bsp) ||
        (long)reference.file_size != bsp_bytes) {
        printf("FAIL load\n");
        goto done;
    }
    stream = (struct cache_stream_result){0};
    if (!cache_stream_read_at(bsp_file, &owner, &tag, &io0, &io1, reference.slot_offset, (size_t)bsp_bytes, bsp_crc,
                              &stream) ||
        !cache_arena_owner_resolve(&owner, &tag, 0, CACHE_GRAPH_CACHE_BYTES, &tags, &arena)) {
        printf("FAIL bsp load\n");
        goto done;
    }
    fclose(tags_file);
    fclose(bsp_file);
    struct cache_graph_input input = {tags, (size_t)tag_bytes, CACHE_GRAPH_CACHE_BYTES, CACHE_GRAPH_TAG_BASE,
        reference.slot_offset, (size_t)bsp_bytes, (int32_t)reference.datum, 0};
    static struct cache_graph_report encoded, native;
    static char text[65536];
    struct cache_graph_result result;
    printf("LOAD placement=%zu tag_bytes=%ld bsp_offset=%zu bsp_bytes=%ld bsp_datum=%08x\n", placement, tag_bytes,
           reference.slot_offset, bsp_bytes, (unsigned)reference.datum);
    if (!cache_graph_walk(&input, workspace, workspace_bytes, &encoded, &result)) {
        printf("FAIL walk %s offset=%zu tag=%ld\n", cache_graph_error_name(result.error), result.offset,
               result.tag_ordinal);
        goto done;
    }
    cache_graph_describe(&encoded, text, sizeof(text));
    fputs(text, stdout);
    if (sizeof(void *) == 4) {
        struct cache_graph_report relocation;
        if (!cache_graph_relocate(&input, tags, workspace, workspace_bytes, &relocation, &result)) {
            printf("FAIL relocate %s offset=%zu\n", cache_graph_error_name(result.error), result.offset);
            goto done;
        }
        input.native = 1;
        if (!cache_graph_walk(&input, workspace, workspace_bytes, &native, &result)) {
            printf("FAIL native walk %s offset=%zu tag=%ld\n", cache_graph_error_name(result.error), result.offset,
                   result.tag_ordinal);
            goto done;
        }
        printf("NATIVE relocated=%u nulled=%u digest=%08x edge_digest=%08x same=%d\n", relocation.pointer_fields,
               relocation.null_fields, native.digest, native.edge_digest,
               !memcmp(&native, &encoded, sizeof(native)));
    } else {
        printf("NATIVE skipped pointer_bits=%u\n", (unsigned)(sizeof(void *) * 8));
    }
    ok = 1;
done:
    cache_arena_owner_release(&owner, &arena);
    free(storage);
    return ok;
}

int main(int argc, char **argv)
{
    if (argc < 5) {
        fprintf(stderr, "usage: %s tags.bin bsp.bin map_bytes bsp_ordinal [placements]\n", argv[0]);
        return 2;
    }
    size_t placements = argc > 5 ? strtoul(argv[5], NULL, 0) : 2;
    size_t workspace_bytes = cache_graph_workspace_bytes(CACHE_GRAPH_CACHE_BYTES);
    void *workspace = malloc(workspace_bytes);
    int ok = workspace != NULL;
    printf("BEGIN host pointer_bits=%u workspace=%zu schema_fields=%d\n", (unsigned)(sizeof(void *) * 8),
           workspace_bytes, 0);
    for (size_t placement = 0; ok && placement < placements; ++placement)
        ok = run(argv[1], argv[2], (uint32_t)strtoul(argv[3], NULL, 0), strtoul(argv[4], NULL, 0), placement,
                 workspace, workspace_bytes);
    printf("END result=%s\n", ok ? "pass" : "fail");
    free(workspace);
    return ok ? 0 : 1;
}
