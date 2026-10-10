/* Host walk of one map's tag slot for the map stager (HWI-015B, tools/wii/map_stage.py).
 *
 * Reads a tag slot image (the map's tag data at offset 0 and, optionally,
 * one structure BSP at the offset it loads to), walks it through the
 * upstream validator's schema with the HWI-007 graph walker
 * (cache_schema_graph.c, the same walk that relocates on the Wii) and writes
 * what the walk visited as little-endian 32-bit records of five words:
 *     1 offset definition 0 0         a tag root, block element or BSP root
 *     2 offset size field field_at    a tag data field's bytes, and its tag_data
 *     3 offset target 0 0             a pointer field and its target's slot offset
 *     4 offset 0 0 0                  a pointer field the relocation nulls
 * then prints the walk's canonical report on stdout. Portable C11; nothing
 * here depends on the host's byte order.
 *
 * usage: map_stage_walk <slot image> <tag bytes> <bsp offset> <bsp bytes> <bsp tag index> <records out> */
#include "cache_schema_graph.h"
#include "cache_schema_tables.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct sink {
    FILE *file;
    unsigned long records;
    int failed;
};

static void put(struct sink *sink, uint32_t kind, size_t a, size_t b, size_t c, size_t d)
{
    unsigned char bytes[20];
    const uint32_t words[5] = {kind, (uint32_t)a, (uint32_t)b, (uint32_t)c, (uint32_t)d};
    for (int index = 0; index < 5; ++index)
        for (int shift = 0; shift < 4; ++shift)
            bytes[index * 4 + shift] = (unsigned char)(words[index] >> (shift * 8));
    if (fwrite(bytes, 1, sizeof bytes, sink->file) != sizeof bytes)
        sink->failed = 1;
    ++sink->records;
}

static void on_element(void *context, size_t offset, int32_t definition)
{
    put(context, 1, offset, (size_t)(uint32_t)definition, 0, 0);
}

static void on_data(void *context, size_t offset, size_t size, uint32_t field, size_t field_offset)
{
    put(context, 2, offset, size, field, field_offset);
}

static void on_pointer(void *context, size_t offset, size_t target)
{
    if (target == CACHE_GRAPH_NULLED)
        put(context, 4, offset, 0, 0, 0);
    else
        put(context, 3, offset, target, 0, 0);
}

static int number(const char *text, unsigned long *value)
{
    char *end;
    errno = 0;
    *value = strtoul(text, &end, 0);
    return !errno && end != text && !*end;
}

int main(int argc, char **argv)
{
    unsigned long tag_bytes, bsp_offset, bsp_bytes, bsp_tag_index;
    if (argc != 7 || !number(argv[2], &tag_bytes) || !number(argv[3], &bsp_offset) || !number(argv[4], &bsp_bytes) ||
        !number(argv[5], &bsp_tag_index)) {
        fprintf(stderr, "usage: map_stage_walk <slot image> <tag bytes> <bsp offset> <bsp bytes> <bsp tag index> "
                        "<records out>\n");
        return 2;
    }
    FILE *input = fopen(argv[1], "rb");
    if (!input) {
        fprintf(stderr, "cannot open the slot image\n");
        return 2;
    }
    unsigned char *slot = calloc(1, CACHE_GRAPH_CACHE_BYTES);
    size_t workspace_bytes = cache_graph_workspace_bytes(CACHE_GRAPH_CACHE_BYTES);
    void *workspace = malloc(workspace_bytes);
    struct cache_graph_report *report = calloc(1, sizeof *report);
    char *text = malloc(1 << 20);
    if (!slot || !workspace || !report || !text) {
        fprintf(stderr, "out of memory\n");
        return 2;
    }
    size_t read = fread(slot, 1, CACHE_GRAPH_CACHE_BYTES, input);
    int extra = fgetc(input);
    fclose(input);
    if (extra != EOF || read < tag_bytes) {
        fprintf(stderr, "the slot image is larger than the tag slot or shorter than its tags\n");
        return 2;
    }
    struct sink sink = {fopen(argv[6], "wb"), 0, 0};
    if (!sink.file) {
        fprintf(stderr, "cannot write the records\n");
        return 2;
    }
    const struct cache_graph_input in = {slot, tag_bytes, CACHE_GRAPH_CACHE_BYTES, CACHE_GRAPH_TAG_BASE, bsp_offset,
                                         bsp_bytes, (int32_t)(uint32_t)bsp_tag_index, 0};
    const struct cache_graph_visitor visitor = {&sink, on_element, on_data, on_pointer};
    struct cache_graph_result result;
    int ok = cache_graph_visit(&in, workspace, workspace_bytes, report, &result, &visitor);
    if (fclose(sink.file) != 0)
        sink.failed = 1;
    if (!ok) {
        printf("WALK error=%s offset=%lu tag=%ld\n", cache_graph_error_name(result.error), (unsigned long)result.offset,
               result.tag_ordinal);
        return 1;
    }
    cache_graph_describe(report, text, 1 << 20);
    fputs(text, stdout);
    printf("RECORDS %lu\n", sink.records);
    return sink.failed ? 1 : 0;
}
