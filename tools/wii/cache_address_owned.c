/* Diagnostic ownership/index interpretation only; no nested tag conversion. */
#include "cache_address_owned.h"
#include "cache_address_probe.h"
#include <stdlib.h>
#include <string.h>
#ifdef GEKKO
#include <malloc.h>
#endif

uint32_t cache_probe_crc32(const void *data, size_t size)
{
    const unsigned char *bytes = data;
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < size; ++i) {
        crc ^= bytes[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (UINT32_C(0xedb88320) & (0u - (crc & 1u)));
    }
    return ~crc;
}

static void put_le(unsigned char *out, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i) out[i] = (unsigned char)(value >> (8 * i));
}

int cache_address_owned(FILE *report, const unsigned char *bytes, size_t size,
                        uint32_t expected_crc, uint32_t expected_count, uint32_t expected_table_crc)
{
    struct cache_address_region region = {bytes, size, UINT32_C(0x803a6000)};
    unsigned failures = 0;
    size_t maximum_owned_bytes = 0;
    uint32_t table_crc = 0;
    unsigned native_word = 0;
    if (size >= sizeof(native_word)) memcpy(&native_word, bytes, sizeof(native_word));
    if (fprintf(report, "OWNED INPUT bytes=%lu crc32=%08lx tags=%lu native_first_word=%08x\n",
                (unsigned long)size, (unsigned long)cache_probe_crc32(bytes, size),
                (unsigned long)expected_count, native_word) < 0) return 1;
    for (unsigned cycle = 0; cycle < 8; ++cycle) {
        struct cache_address_graph graph = {0};
        struct cache_address_result status = {0};
        struct cache_address_reference reference;
        if (!cache_address_graph_load(&region, &graph, &status)) {
            fprintf(report, "OWNED FAIL load cycle=%u error=%s offset=%lu instance=%lu\n", cycle,
                    cache_address_error_name(status.error), (unsigned long)status.offset,
                    (unsigned long)status.instance_index);
            return 1;
        }
        if (graph.count != expected_count || graph.size != size) ++failures;
        size_t saved_size = 0;
        const unsigned char *saved = cache_address_graph_bytes(&graph, &saved_size);
        if (saved == NULL || saved == bytes || saved_size != size ||
            cache_probe_crc32(saved, saved_size) != expected_crc || memcmp(saved, bytes, size)) ++failures;
        unsigned char *serialized = malloc(graph.count * 32);
        if (serialized == NULL) {
            fprintf(report, "OWNED FAIL table serialization allocation\n");
            cache_address_graph_unload(&graph);
            return 1;
        }
        for (size_t i = 0; i < graph.count; ++i) {
            const struct cache_address_instance *entry = &graph.instances[i];
            uint32_t words[8] = {entry->group, entry->parent[0], entry->parent[1], entry->datum,
                                 entry->name_address, entry->root_address, entry->unused[0], entry->unused[1]};
            for (unsigned j = 0; j < 8; ++j) put_le(serialized + i * 32 + j * 4, words[j]);
            if (!cache_address_graph_lookup(&graph, entry->datum, entry->group, &reference, &status) ||
                reference.instance.datum != entry->datum || reference.name.length == 0 ||
                graph.bytes[reference.name.offset + reference.name.length - 1] != 0) ++failures;
        }
        table_crc = cache_probe_crc32(serialized, graph.count * 32);
        if (table_crc != expected_table_crc) ++failures;
        maximum_owned_bytes = graph.size + graph.count * sizeof(*graph.instances) + graph.count * 32;
#ifdef GEKKO
        if (cycle == 0) {
            struct mallinfo heap = mallinfo();
            fprintf(report, "HEAP owned_live arena=%d in_use=%d free=%d releasable_top=%d payload_usable=%lu\n",
                    heap.arena, heap.uordblks, heap.fordblks, heap.keepcost,
                    (unsigned long)(malloc_usable_size(graph.bytes) + malloc_usable_size(graph.instances) +
                                    malloc_usable_size(serialized)));
        }
#endif
        free(serialized);
        if (cycle == 0 && fprintf(report, "OWNED GRAPH storage=%p index=%p count=%lu peak_payload=%lu table_crc32=%08lx\n",
                                 (void *)graph.bytes, (void *)graph.instances, (unsigned long)graph.count,
                                 (unsigned long)maximum_owned_bytes, (unsigned long)table_crc) < 0) ++failures;
        cache_address_graph_unload(&graph);
        if (graph.bytes != NULL || graph.instances != NULL || graph.size != 0 || graph.count != 0) ++failures;
        if (cache_address_graph_lookup(&graph, region.encoded_base, 0, &reference, &status)) ++failures;
    }
    if (cache_probe_crc32(bytes, size) != expected_crc) ++failures;
#ifdef GEKKO
    struct mallinfo heap = mallinfo();
    fprintf(report, "HEAP owned_released arena=%d in_use=%d free=%d releasable_top=%d\n",
            heap.arena, heap.uordblks, heap.fordblks, heap.keepcost);
#endif
    if (fprintf(report, "OWNED SUMMARY cycles=8 tags=%lu table_crc32=%08lx peak_payload=%lu failures=%u\n",
                (unsigned long)expected_count, (unsigned long)table_crc,
                (unsigned long)maximum_owned_bytes, failures) < 0) return 1;
    return failures != 0;
}
