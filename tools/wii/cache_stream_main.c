/* Provisional MEM2 reservations and raw-tag streaming; no engine pool load. */
#include "cache_arena_plan.h"
#include "cache_arena_fixture.h"
#include "cache_address_probe.h"
#include "cache_address_fixture.h"
#include "cache_address_owned.h"
#include "cache_stream_io.h"
#include "cache_stream_fixture.h"
#include "cache_probe_payload.h"
#include "../../port/linux/include/halo_port_capacity.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef CACHE_BSP_RESIDENCY
#include "cache_bsp_probe.h"
#include "cache_bsp_fixture.h"
#endif
#ifdef CACHE_WIDGET_GRAPH
#include "cache_widget_probe.h"
#include "cache_widget_fixture.h"
#endif
#ifdef CACHE_BSP_RESIDENCY
#define CACHE_REPORT_KIND "BSP"
#define CACHE_REPORT_PATH "sd:/halo-wii-memory/cache-bsp.log"
#elif defined(CACHE_WIDGET_GRAPH)
#define CACHE_REPORT_KIND "WIDGET"
#define CACHE_REPORT_PATH "sd:/halo-wii-memory/cache-widget.log"
#else
#define CACHE_REPORT_KIND "STREAM"
#define CACHE_REPORT_PATH "sd:/halo-wii-memory/cache-stream.log"
#endif
#ifdef GEKKO
#include <gccore.h>
#include <fat.h>
#include <errno.h>
#include <sys/stat.h>
#include <malloc.h>
#endif

enum { TAG_SLOT, STATE_SLOT, SOUND_SLOT, INDEX_SLOT, IO0_SLOT, IO1_SLOT,
#ifdef CACHE_WIDGET_GRAPH
       WIDGET_WORKSPACE_SLOT, WIDGET_SERIAL_SLOT,
#endif
#ifdef CACHE_BSP_RESIDENCY
       BSP_STAGE_SLOT, BSP_SERIAL_SLOT, BSP_CONTROL_SLOT,
#endif
       SLOT_COUNT };
typedef char verify_stream_index_words[sizeof(struct cache_address_instance) == 32 ? 1 : -1];
#define RESERVE_BYTES ((size_t)0x200000)
#define HOST_SPAN_BYTES ((size_t)54368224)

static unsigned char *slot_view(const struct cache_arena_owner *owner, size_t index, size_t size)
{
    struct cache_arena_handle handle;
    struct cache_arena_result status;
    unsigned char *view = NULL;
    if (!cache_arena_owner_handle(owner, index, &handle, &status) ||
        !cache_arena_owner_resolve(owner, &handle, 0, size, &view, &status)) return NULL;
    return view;
}

static int decode_index(const struct cache_arena_owner *owner)
{
    unsigned char *tags = slot_view(owner, TAG_SLOT, CACHE_PRIVATE_SIZE);
    unsigned char *index = slot_view(owner, INDEX_SLOT, CACHE_PRIVATE_COUNT * 32u);
    if (!tags || !index) return 1;
    struct cache_address_region region = {tags, CACHE_PRIVATE_SIZE, UINT32_C(0x803a6000)};
    struct cache_address_header header;
    struct cache_address_result status;
    struct cache_address_span table;
    if (!cache_address_decode_header(&region, &header, &status) || header.tag_count != CACHE_PRIVATE_COUNT ||
        !cache_address_resolve(&region, header.instances_address, header.tag_count, 32, &table, &status) ||
        cache_probe_crc32(tags + table.offset, table.length) != CACHE_PRIVATE_TABLE_CRC) return 1;
    unsigned scenario_found = 0;
    for (size_t i = 0; i < CACHE_PRIVATE_COUNT; ++i) {
        struct cache_address_instance value;
        struct cache_address_span name, root;
        if (!cache_address_decode_instance(tags, region.size, table.offset + i * 32, &value, &status) ||
            (value.datum & 65535u) != i ||
            !cache_address_resolve(&region, value.name_address, 1, 1, &name, &status) ||
            !memchr(tags + name.offset, 0, region.size - name.offset)) return 1;
        if (value.root_address) {
            if (!cache_address_resolve(&region, value.root_address, 1, 1, &root, &status)) return 1;
        } else if (value.group != UINT32_C(0x73627370)) return 1;
        if (i == (header.scenario_datum & 65535u)) {
            if (value.datum != header.scenario_datum || value.group != UINT32_C(0x73636e72)) return 1;
            scenario_found = 1;
        }
        memcpy(index + i * 32, &value, sizeof(value));
        uint32_t words[8] = {value.group, value.parent[0], value.parent[1], value.datum,
                             value.name_address, value.root_address, value.unused[0], value.unused[1]};
        unsigned char serialized[32];
        for (unsigned word = 0; word < 8; ++word)
            for (unsigned byte = 0; byte < 4; ++byte)
                serialized[word * 4 + byte] = (unsigned char)(words[word] >> (byte * 8));
        if (memcmp(serialized, tags + table.offset + i * 32, 32)) return 1;
    }
    return !scenario_found;
}

#ifdef CACHE_WIDGET_GRAPH
static int widget_roundtrip(FILE *report, const struct cache_arena_owner *owner,
                            size_t workspace_bytes, unsigned cycle, struct cache_widget_graph *graph)
{
    struct cache_arena_handle tag;
    struct cache_arena_result arena_status;
    struct cache_widget_result status;
    void *workspace = slot_view(owner, WIDGET_WORKSPACE_SLOT, workspace_bytes);
    void *serialized = slot_view(owner, WIDGET_SERIAL_SLOT, CACHE_WIDGET_SERIALIZED_BYTES);
    if (!workspace || !serialized || !cache_arena_owner_handle(owner, TAG_SLOT, &tag, &arena_status)) {
        fprintf(report, "WIDGET FAIL live workspace/serialization/tag handles\n"); return 1;
    }
    if (!cache_widget_graph_decode(owner, &tag, CACHE_PRIVATE_SIZE, UINT32_C(0x803a6000),
            CACHE_WIDGET_ROOT_DATUM, workspace, workspace_bytes, CACHE_WIDGET_NODE_COUNT, graph, &status)) {
        fprintf(report, "WIDGET FAIL decode cycle=%u error=%s offset=%lu node=%lu required=%lu\n",
                cycle, cache_widget_error_name(status.error), (unsigned long)status.offset,
                (unsigned long)status.node_index, (unsigned long)status.required);
        return 1;
    }
    if (graph->count != CACHE_WIDGET_NODE_COUNT || graph->serialized_size != CACHE_WIDGET_SERIALIZED_BYTES) {
        fprintf(report, "WIDGET FAIL graph count or serialization extent\n"); return 1;
    }
    size_t elements = 0;
    for (size_t i = 0; i < graph->count; ++i) {
        struct cache_widget_projection node;
        if (!cache_widget_graph_node(graph, i, &node, &status)) {
            fprintf(report, "WIDGET FAIL node use error=%s\n", cache_widget_error_name(status.error)); return 1;
        }
        for (unsigned kind = 0; kind < CACHE_WIDGET_BLOCKS; ++kind)
            for (int32_t j = 0; j < node.blocks[kind].count; ++j) {
                struct cache_widget_element element;
                if (!cache_widget_graph_element(graph, i, (enum cache_widget_block_kind)kind,
                                               (size_t)j, &element, &status)) {
                    fprintf(report, "WIDGET FAIL element use error=%s\n", cache_widget_error_name(status.error)); return 1;
                }
                ++elements;
            }
    }
    size_t used = 0;
    if (!cache_widget_graph_serialize(graph, serialized, CACHE_WIDGET_SERIALIZED_BYTES, &used, &status) ||
            used != CACHE_WIDGET_SERIALIZED_BYTES || cache_probe_crc32(serialized, used) != CACHE_WIDGET_SERIALIZED_CRC32) {
        fprintf(report, "WIDGET FAIL lossless serialization or CRC\n"); return 1;
    }
    if (cycle == 0) fprintf(report, "WIDGET GRAPH nodes=%lu elements=%lu serialized_bytes=%lu crc32=%08lx workspace=%lu projection_bytes=%lu scope=partial_typed_representative_graph\n",
        (unsigned long)graph->count, (unsigned long)elements, (unsigned long)used,
        (unsigned long)cache_probe_crc32(serialized, used), (unsigned long)workspace_bytes,
        (unsigned long)sizeof(struct cache_widget_projection));
    return 0;
}
#endif

#ifdef CACHE_BSP_RESIDENCY
static int bsp_prepare(FILE *report, const struct cache_arena_owner *owner, const char *path,
                       unsigned cycle, struct cache_bsp_control **control_output)
{
    struct cache_arena_handle tag, stage, io0, io1;
    struct cache_arena_result arena_status;
    struct cache_stream_result stream_status = {0};
    struct cache_bsp_result status;
    struct cache_bsp_reference reference;
    struct cache_bsp_control *control = (void *)slot_view(owner, BSP_CONTROL_SLOT, sizeof(*control));
    if (!control || !cache_arena_owner_handle(owner, TAG_SLOT, &tag, &arena_status) ||
            !cache_arena_owner_handle(owner, BSP_STAGE_SLOT, &stage, &arena_status) ||
            !cache_arena_owner_handle(owner, IO0_SLOT, &io0, &arena_status) ||
            !cache_arena_owner_handle(owner, IO1_SLOT, &io1, &arena_status)) {
        fprintf(report, "BSP FAIL preparation handles\n"); return 1;
    }
    memset(control, 0, sizeof(*control));
    if (!cache_bsp_select(owner, &tag, CACHE_PRIVATE_SIZE, CACHE_BSP_GOLDEN_DECLARED_MAP_BYTES,
                          CACHE_BSP_GOLDEN_BSP_ORDINAL, &reference, &status) ||
            reference.datum != CACHE_BSP_GOLDEN_DATUM || reference.address != CACHE_BSP_GOLDEN_ENCODED_BASE ||
            reference.file_size != CACHE_BSP_GOLDEN_FILE_BYTES || reference.rounded_bytes != CACHE_BSP_GOLDEN_ROUNDED_RESERVATION_BYTES) {
        fprintf(report, "BSP FAIL selected reference cycle=%u error=%s\n", cycle, cache_bsp_error_name(status.error));
        return 1;
    }
    FILE *input = fopen(path, "rb");
    if (!input) { fprintf(report, "BSP FAIL sidecar open\n"); return 1; }
    int result = !cache_stream_read(input, owner, &stage, &io0, &io1, CACHE_BSP_GOLDEN_FILE_BYTES,
                                    CACHE_BSP_GOLDEN_SERIALIZED_CRC32, &stream_status);
    if (result) fprintf(report, "BSP FAIL sidecar read error=%s bytes=%lu\n",
                        cache_stream_error_name(stream_status.error), (unsigned long)stream_status.bytes);
    if (fclose(input)) { fprintf(report, "BSP FAIL sidecar close\n"); result = 1; }
    if (result) return 1;
    unsigned char *destination = NULL;
    unsigned char *source = slot_view(owner, BSP_STAGE_SLOT, CACHE_BSP_GOLDEN_FILE_BYTES);
    if (!source || !cache_arena_owner_resolve(owner, &tag, reference.slot_offset,
                                             reference.rounded_bytes, &destination, &arena_status)) {
        fprintf(report, "BSP FAIL rounded residency span\n"); return 1;
    }
    /* All input rewrites precede immutable widget/BSP view acquisition. */
    memcpy(destination, source, CACHE_BSP_GOLDEN_FILE_BYTES);
    if (cycle == 0) fprintf(report, "BSP INPUT bytes=%lu crc32=%08lx chunks=%lu fread_calls=%lu slot_offset=%lu rounded=%lu unread_gap=%lu\n",
        (unsigned long)stream_status.bytes, (unsigned long)stream_status.actual_crc32,
        (unsigned long)stream_status.chunks, (unsigned long)stream_status.read_calls,
        (unsigned long)reference.slot_offset, (unsigned long)reference.rounded_bytes,
        (unsigned long)(reference.slot_offset - CACHE_PRIVATE_SIZE));
    *control_output = control;
    return 0;
}

static int bsp_roundtrip(FILE *report, const struct cache_arena_owner *owner,
                         struct cache_bsp_control *control, unsigned cycle,
                         struct cache_bsp_view *last, unsigned *child_release, unsigned *child_rebind)
{
    struct cache_arena_handle tag;
    struct cache_arena_result arena_status;
    struct cache_bsp_result status;
    struct cache_bsp_view old = {0};
    void *serialized = slot_view(owner, BSP_SERIAL_SLOT, CACHE_BSP_GOLDEN_SERIALIZED_BYTES);
    if (!serialized || !cache_arena_owner_handle(owner, TAG_SLOT, &tag, &arena_status)) {
        fprintf(report, "BSP FAIL roundtrip handles\n"); return 1;
    }
    for (unsigned load = 0; load < 2; ++load) {
        struct cache_bsp_view view;
        struct cache_bsp_reference reference;
        struct cache_bsp_header header;
        struct cache_address_span root, gap;
        if (!cache_bsp_bind(control, owner, &tag, CACHE_PRIVATE_SIZE, CACHE_BSP_GOLDEN_DECLARED_MAP_BYTES,
                            CACHE_BSP_GOLDEN_BSP_ORDINAL, &view, &status)) {
            fprintf(report, "BSP FAIL bind cycle=%u load=%u error=%s offset=%lu\n", cycle, load,
                    cache_bsp_error_name(status.error), (unsigned long)status.offset); return 1;
        }
        if (load) {
            if (cache_bsp_get_root(&old, &root, &status) || status.error != CACHE_BSP_STATE) {
                fprintf(report, "BSP FAIL child rebind invalidation\n"); return 1;
            }
            ++*child_rebind;
        }
        if (!cache_bsp_lookup(&view, &reference, &status) || reference.datum != CACHE_BSP_GOLDEN_DATUM ||
                reference.scenario_datum != CACHE_BSP_GOLDEN_SCENARIO_DATUM ||
                !cache_bsp_get_header(&view, &header, &status) || header.root_address != CACHE_BSP_GOLDEN_ROOT_ADDRESS ||
                header.vertex_count != CACHE_BSP_GOLDEN_VERTEX_COUNT || header.index_count != CACHE_BSP_GOLDEN_INDEX_COUNT ||
                !cache_bsp_get_root(&view, &root, &status) || root.length != CACHE_BSP_ROOT_BYTES ||
                root.offset != CACHE_BSP_GOLDEN_ROOT_ADDRESS - CACHE_BSP_TAG_BASE) {
            fprintf(report, "BSP FAIL header/root/full datum use\n"); return 1;
        }
        for (unsigned kind = 0; kind < 2; ++kind) {
            size_t count = (size_t)(kind ? header.index_count : header.vertex_count);
            for (size_t at = 0; at < count; ++at) {
                struct cache_bsp_descriptor descriptor;
                if (!cache_bsp_get_descriptor(&view, kind, at, &descriptor, &status)) {
                    fprintf(report, "BSP FAIL descriptor use\n"); return 1;
                }
            }
        }
        if (reference.slot_offset > CACHE_PRIVATE_SIZE &&
                (cache_bsp_resolve(&view, CACHE_BSP_TAG_BASE + CACHE_PRIVATE_SIZE, 1, 1, &gap, &status) ||
                 status.error != CACHE_BSP_SPAN)) {
            fprintf(report, "BSP FAIL unread gap became addressable\n"); return 1;
        }
        size_t used = 0;
        if (!cache_bsp_serialize(&view, serialized, CACHE_BSP_GOLDEN_SERIALIZED_BYTES, &used, &status) ||
                used != CACHE_BSP_GOLDEN_SERIALIZED_BYTES || cache_probe_crc32(serialized, used) != CACHE_BSP_GOLDEN_SERIALIZED_CRC32) {
            fprintf(report, "BSP FAIL lossless inspection serialization\n"); return 1;
        }
        if (cycle == 0 && load == 0) fprintf(report, "BSP VIEW datum=%08lx root_offset=%lu root_bytes=%lu vertex_descriptors=%ld index_descriptors=%ld serialized_bytes=%lu crc32=%08lx control_bytes=%lu view_bytes=%lu scope=opaque_root_extent_not_geometry\n",
            (unsigned long)reference.datum, (unsigned long)root.offset, (unsigned long)root.length,
            (long)header.vertex_count, (long)header.index_count, (unsigned long)used,
            (unsigned long)cache_probe_crc32(serialized, used), (unsigned long)sizeof(*control),
            (unsigned long)sizeof(view));
        old = view;
        if (load == 0) {
            if (!cache_bsp_unload(control, &status) || cache_bsp_get_root(&view, &root, &status) ||
                    status.error != CACHE_BSP_STATE) {
                fprintf(report, "BSP FAIL child unload invalidation\n"); return 1;
            }
            ++*child_release;
        }
        /* The second child stays live until parent release; stale use must
         * reject the copied parent pin before touching released control bytes. */
        *last = view;
    }
    return 0;
}
#endif

static int controlled_fragmentation(FILE *report, uintptr_t begin, uintptr_t end,
                                    const struct cache_arena_request *requests)
{
    const size_t guard = 65536;
    size_t capacity = end - begin;
    size_t first = (capacity / 3) & ~(size_t)31;
    size_t second = (capacity * 2 / 3) & ~(size_t)31;
    if (first + guard > second || second + guard > capacity) return 1;
    uintptr_t starts[3] = {begin, begin + first + guard, begin + second + guard};
    size_t lengths[3] = {first, second - first - guard, capacity - second - guard};
    struct cache_arena_plan full;
    struct cache_arena_result status;
    if (!cache_arena_plan_build(requests, SLOT_COUNT, (void *)begin, capacity, RESERVE_BYTES, &full, &status)) return 1;
#ifdef GEKKO
    SYS_SetArena2Lo((void *)end);
#endif
    memset((void *)(begin + first), 0x6d, guard);
    memset((void *)(begin + second), 0x9b, guard);
    unsigned rejected = 0;
    size_t largest = 0;
    for (unsigned i = 0; i < 3; ++i) {
        struct cache_arena_plan candidate;
        if (lengths[i] > largest) largest = lengths[i];
        if (!cache_arena_plan_build(requests, SLOT_COUNT, (void *)starts[i], lengths[i],
                                    RESERVE_BYTES, &candidate, &status) && status.error == CACHE_ARENA_CAPACITY) ++rejected;
    }
    unsigned failures = rejected != 3 || capacity - 2 * guard < full.required;
    for (size_t i = 0; i < guard; ++i)
        if (((unsigned char *)begin)[first + i] != 0x6d || ((unsigned char *)begin)[second + i] != 0x9b) { ++failures; break; }
    fprintf(report, "STREAM FRAGMENT controlled_intervals=3 guard_bytes=%lu aggregate_free=%lu largest_free=%lu requested=%lu rejected=%u failures=%u scope=owned_intervals_not_system_heap\n",
            (unsigned long)(2 * guard), (unsigned long)(capacity - 2 * guard), (unsigned long)largest,
            (unsigned long)full.required, rejected, failures);
#ifdef GEKKO
    if ((uintptr_t)SYS_GetArena2Lo() != end || (uintptr_t)SYS_GetArena2Hi() != end) {
        fprintf(report, "STREAM FAIL fragmentation arena mutation; refusing restore\n"); return 1;
    }
    SYS_SetArena2Lo((void *)begin);
#endif
    return failures != 0;
}

static int stream_cycles(FILE *report, const char *path
#ifdef CACHE_BSP_RESIDENCY
                         , const char *bsp_path
#endif
                         )
{
#ifdef CACHE_BSP_RESIDENCY
    struct cache_bsp_view previous_bsp = {0};
    unsigned bsp_completed = 0, bsp_parent_rebind = 0, bsp_parent_release = 0;
    unsigned bsp_child_release = 0, bsp_child_rebind = 0;
#endif
#ifdef CACHE_WIDGET_GRAPH
    size_t workspace_bytes = 0;
    struct cache_widget_result widget_status;
    if (!cache_widget_workspace_size(CACHE_WIDGET_NODE_COUNT, &workspace_bytes, &widget_status)) {
        fprintf(report, "WIDGET FAIL workspace size error=%s\n", cache_widget_error_name(widget_status.error)); return 1;
    }
    struct cache_widget_graph previous_graph = {0};
    unsigned widget_completed = 0, widget_stale_rebind = 0, widget_stale_release = 0;
#endif
    const struct cache_arena_request requests[SLOT_COUNT] = {
        {0x1600000, 64}, {HALO_PORT_GAME_STATE_SIZE, 32}, {0x400000, 32},
        {65535u * 32u, 32}, {CACHE_STREAM_IO_BYTES, 64}, {CACHE_STREAM_IO_BYTES, 64}
#ifdef CACHE_WIDGET_GRAPH
        , {workspace_bytes, 64}, {CACHE_WIDGET_SERIALIZED_BYTES, 64}
#endif
#ifdef CACHE_BSP_RESIDENCY
        , {CACHE_BSP_GOLDEN_FILE_BYTES, 64}, {CACHE_BSP_GOLDEN_SERIALIZED_BYTES, 64}, {sizeof(struct cache_bsp_control), 64}
#endif
    };
    struct cache_arena_owner owner = {0};
    struct cache_arena_handle previous = {0};
    unsigned failures = 0, expected_errors = 0, completed = 0, stale_rebind = 0, stale_release = 0;
    size_t maximum_charge = 0, maximum_io_chunks = 0;
    fprintf(report, "STREAM REQUEST tags=%lu state=%lu sound=%lu max_index=%lu io_each=%lu reserve=%lu state_sound=canary_placeholders_not_engine_pools\n",
            (unsigned long)requests[TAG_SLOT].size, (unsigned long)requests[STATE_SLOT].size,
            (unsigned long)requests[SOUND_SLOT].size, (unsigned long)requests[INDEX_SLOT].size,
            (unsigned long)CACHE_STREAM_IO_BYTES, (unsigned long)RESERVE_BYTES);
#ifdef CACHE_WIDGET_GRAPH
    fprintf(report, "WIDGET REQUEST workspace=%lu serialized=%lu node_workload_capacity=%lu source_block_maxima=64,32,32,32,32 source_depth=32\n",
            (unsigned long)workspace_bytes, (unsigned long)CACHE_WIDGET_SERIALIZED_BYTES,
            (unsigned long)CACHE_WIDGET_NODE_COUNT);
#endif
#ifdef CACHE_BSP_RESIDENCY
    fprintf(report, "BSP REQUEST stage=%lu serialized=%lu control=%lu dynamic_projection=0 resident_bytes_already_in_tag_slot=%lu source_references_max16\n",
        (unsigned long)CACHE_BSP_GOLDEN_FILE_BYTES, (unsigned long)CACHE_BSP_GOLDEN_SERIALIZED_BYTES,
        (unsigned long)sizeof(struct cache_bsp_control), (unsigned long)CACHE_BSP_GOLDEN_FILE_BYTES);
#endif
#ifdef GEKKO
    uintptr_t initial_lo = (uintptr_t)SYS_GetArena2Lo();
    uintptr_t initial_hi = (uintptr_t)SYS_GetArena2Hi();
    fprintf(report, "STREAM ARENAS mem1_lo=%p mem1_hi=%p mem1_bytes=%lu mem2_lo=%p mem2_hi=%p mem2_bytes=%lu\n",
            SYS_GetArena1Lo(), SYS_GetArena1Hi(), (unsigned long)SYS_GetArena1Size(),
            (void *)initial_lo, (void *)initial_hi, (unsigned long)SYS_GetArena2Size());
    struct mallinfo before = mallinfo();
    fprintf(report, "STREAM HEAP before in_use=%d arena=%d free=%d\n", before.uordblks, before.arena, before.fordblks);
#else
    unsigned char *storage = malloc(HOST_SPAN_BYTES + 4096);
    if (!storage) { fprintf(report, "STREAM FAIL host capacity allocation\n"); return 1; }
    uintptr_t initial_lo = (uintptr_t)storage;
    uintptr_t initial_hi = initial_lo + HOST_SPAN_BYTES;
#endif
    failures += (unsigned)controlled_fragmentation(report, initial_lo, initial_hi, requests);
    for (unsigned cycle = 0; cycle < 16 && !failures; ++cycle) {
        uintptr_t begin = initial_lo + (cycle & 1u ? 4096u : 0u);
        struct cache_arena_plan plan;
        struct cache_arena_result arena_status;
        if (!cache_arena_plan_build(requests, SLOT_COUNT, (void *)begin, initial_hi - begin,
                                    RESERVE_BYTES, &plan, &arena_status)) {
            fprintf(report, "STREAM FAIL plan cycle=%u error=%s required=%lu\n", cycle,
                    cache_arena_error_name(arena_status.error), (unsigned long)arena_status.required);
            ++failures; break;
        }
#ifdef GEKKO
        SYS_SetArena2Lo((void *)(begin + plan.required));
#endif
        if (!cache_arena_owner_bind(&owner, &plan, (void *)begin, initial_hi - begin, &arena_status)) {
            fprintf(report, "STREAM FAIL owner bind\n"); ++failures;
#ifdef GEKKO
            SYS_SetArena2Lo((void *)initial_lo);
#endif
            break;
        }
        unsigned char *bad_view = NULL;
#ifdef CACHE_BSP_RESIDENCY
        struct cache_bsp_view bsp_view = {0};
        struct cache_bsp_control *bsp_control = NULL;
        struct cache_bsp_result bsp_status;
        if (cycle) {
            struct cache_address_span stale;
            if (cache_bsp_get_root(&previous_bsp, &stale, &bsp_status) || bsp_status.error != CACHE_BSP_STATE) {
                fprintf(report, "BSP FAIL parent rebind invalidation\n"); ++failures;
            } else ++bsp_parent_rebind;
        }
#endif
        if (cycle) {
            if (cache_arena_owner_resolve(&owner, &previous, 0, 1, &bad_view, &arena_status) ||
                arena_status.error != CACHE_ARENA_HANDLE) ++failures;
            else ++stale_rebind;
        }
#ifdef CACHE_WIDGET_GRAPH
        struct cache_widget_graph graph = {0};
        if (cycle) {
            struct cache_widget_projection stale;
            if (cache_widget_graph_node(&previous_graph, 0, &stale, &widget_status) ||
                    widget_status.error != CACHE_WIDGET_STATE) {
                fprintf(report, "WIDGET FAIL stale graph rebind rejection error=%s\n",
                        cache_widget_error_name(widget_status.error)); ++failures;
            } else ++widget_stale_rebind;
        }
#endif
        struct cache_arena_handle tag = {0}, io0 = {0}, io1 = {0};
        if (!cache_arena_owner_handle(&owner, TAG_SLOT, &tag, &arena_status) ||
            !cache_arena_owner_handle(&owner, IO0_SLOT, &io0, &arena_status) ||
            !cache_arena_owner_handle(&owner, IO1_SLOT, &io1, &arena_status)) {
            fprintf(report, "STREAM FAIL live handles\n"); ++failures;
        }
        /* Entire requested backing is owned, but engine pools remain canary placeholders. */
        memset((void *)begin, 0xa5, plan.required);
        FILE *input = fopen(path, "rb");
        struct cache_stream_result stream_status = {0};
        if (!input) { fprintf(report, "STREAM FAIL open owned raw input\n"); ++failures; }
        else {
            if (!cache_stream_read(input, &owner, &tag, &io0, &io1, CACHE_PRIVATE_SIZE,
                                    CACHE_PRIVATE_CRC, &stream_status)) {
                fprintf(report, "STREAM FAIL read cycle=%u error=%s bytes=%lu\n", cycle,
                        cache_stream_error_name(stream_status.error), (unsigned long)stream_status.bytes);
                ++failures;
            } else {
                if (decode_index(&owner)) {
                    fprintf(report, "STREAM FAIL index identity/span or numeric table validation cycle=%u\n", cycle);
                    ++failures;
                }
                if (stream_status.chunks > maximum_io_chunks) maximum_io_chunks = stream_status.chunks;
                if (cycle == 0) fprintf(report, "STREAM IO bytes=%lu chunks=%lu fread_calls=%lu crc32=%08lx\n",
                                        (unsigned long)stream_status.bytes, (unsigned long)stream_status.chunks,
                                        (unsigned long)stream_status.read_calls, (unsigned long)stream_status.actual_crc32);
            }
            if (cycle == 0) {
                const size_t lengths[] = {CACHE_PRIVATE_SIZE - 1u, CACHE_PRIVATE_SIZE + 1u, CACHE_PRIVATE_SIZE};
                const enum cache_stream_error errors[] = {CACHE_STREAM_TRAILING, CACHE_STREAM_SHORT, CACHE_STREAM_CRC};
                for (unsigned test = 0; test < 3; ++test) {
                    rewind(input); stream_status = (struct cache_stream_result){0};
                    int ok = cache_stream_read(input, &owner, &tag, &io0, &io1, lengths[test],
                                                test == 2 ? CACHE_PRIVATE_CRC ^ 1u : CACHE_PRIVATE_CRC, &stream_status);
                    if (ok || stream_status.error != errors[test]) ++failures;
                    else ++expected_errors;
                }
            }
            if (fclose(input)) { fprintf(report, "STREAM FAIL input close\n"); ++failures; }
        }
#ifdef CACHE_BSP_RESIDENCY
        if (!failures && (bsp_prepare(report, &owner, bsp_path, cycle, &bsp_control) ||
                bsp_roundtrip(report, &owner, bsp_control, cycle, &bsp_view, &bsp_child_release, &bsp_child_rebind))) ++failures;
        else if (!failures) ++bsp_completed;
#endif
#ifdef CACHE_WIDGET_GRAPH
        /* Acquire immutable typed views only after the deliberate IO rewrites. */
        if (!failures) {
            unsigned char *raw = slot_view(&owner, TAG_SLOT, CACHE_PRIVATE_SIZE);
            if (!raw || cache_probe_crc32(raw, CACHE_PRIVATE_SIZE) != CACHE_PRIVATE_CRC ||
                    widget_roundtrip(report, &owner, workspace_bytes, cycle, &graph)) ++failures;
            else ++widget_completed;
        }
#endif
        for (size_t index = STATE_SLOT; index <= SOUND_SLOT; ++index) {
            unsigned char *view = slot_view(&owner, index, requests[index].size);
            if (!view || view[0] != 0xa5 || view[requests[index].size - 1] != 0xa5) ++failures;
        }
        for (size_t i = plan.reserve_offset; i < plan.required; ++i)
            if (((unsigned char *)begin)[i] != 0xa5) { ++failures; break; }
        size_t charge = begin - initial_lo + plan.required;
        if (charge > maximum_charge) maximum_charge = charge;
        if (cycle < 2) fprintf(report, "STREAM PLAN placement=%u base=%p data_end=%lu reserve=%lu required=%lu arena_charge=%lu remaining=%lu\n",
                              cycle, (void *)begin, (unsigned long)plan.data_end, (unsigned long)plan.reserve_size,
                              (unsigned long)plan.required, (unsigned long)charge, (unsigned long)(initial_hi - initial_lo - charge));
        previous = tag;
#ifdef CACHE_WIDGET_GRAPH
        previous_graph = graph;
#endif
#ifdef CACHE_BSP_RESIDENCY
        previous_bsp = bsp_view;
#endif
        if (!cache_arena_owner_release(&owner, &arena_status)) {
            fprintf(report, "STREAM FAIL release; backing retained\n");
            return 1;
        }
        if (cache_arena_owner_resolve(&owner, &previous, 0, 1, &bad_view, &arena_status) ||
            arena_status.error != CACHE_ARENA_STATE) ++failures;
        else ++stale_release;
#ifdef CACHE_BSP_RESIDENCY
        if (bsp_view.owner) {
            struct cache_address_span stale;
            if (cache_bsp_get_root(&bsp_view, &stale, &bsp_status) || bsp_status.error != CACHE_BSP_STATE) {
                fprintf(report, "BSP FAIL parent release invalidation\n"); ++failures;
            } else ++bsp_parent_release;
        }
#endif
#ifdef CACHE_WIDGET_GRAPH
        if (graph.count) {
            struct cache_widget_projection stale;
            if (cache_widget_graph_node(&graph, 0, &stale, &widget_status) ||
                    widget_status.error != CACHE_WIDGET_STATE) {
                fprintf(report, "WIDGET FAIL stale graph release rejection error=%s\n",
                        cache_widget_error_name(widget_status.error)); ++failures;
            } else ++widget_stale_release;
        }
        cache_widget_graph_release(&graph);
#endif
#ifdef GEKKO
        if ((uintptr_t)SYS_GetArena2Lo() != begin + plan.required || (uintptr_t)SYS_GetArena2Hi() != initial_hi) {
            fprintf(report, "STREAM FAIL arena changed; refusing restore\n"); return 1;
        }
        SYS_SetArena2Lo((void *)initial_lo);
#endif
        ++completed;
    }
#ifdef GEKKO
    struct mallinfo after = mallinfo();
    fprintf(report, "STREAM HEAP released in_use=%d arena=%d free=%d\n", after.uordblks, after.arena, after.fordblks);
    fprintf(report, "STREAM ARENAS released mem2_lo=%p mem2_hi=%p mem2_bytes=%lu\n",
            SYS_GetArena2Lo(), SYS_GetArena2Hi(), (unsigned long)SYS_GetArena2Size());
#else
    free(storage);
#endif
#ifdef CACHE_WIDGET_GRAPH
    fprintf(report, "WIDGET SUMMARY cycles=%u stale_rebind=%u stale_release=%u workspace_bytes=%lu serialized_bytes=%lu failures=%u unknown_bytes=opaque_preserved full_UI_graph=not_qualified\n",
            widget_completed, widget_stale_rebind, widget_stale_release, (unsigned long)workspace_bytes,
            (unsigned long)CACHE_WIDGET_SERIALIZED_BYTES, failures);
#endif
#ifdef CACHE_BSP_RESIDENCY
    fprintf(report, "BSP SUMMARY cycles=%u child_loads=%u child_release_STATE=%u child_rebind_STATE=%u parent_release_STATE=%u parent_rebind_STATE=%u serialized_bytes=%lu failures=%u gap_addressable=0 geometry=not_qualified\n",
        bsp_completed, bsp_completed * 2u, bsp_child_release, bsp_child_rebind, bsp_parent_release,
        bsp_parent_rebind, (unsigned long)CACHE_BSP_GOLDEN_SERIALIZED_BYTES, failures);
#endif
    fprintf(report, "STREAM SUMMARY cycles=%u tags=%lu max_charge=%lu chunks=%lu expected_errors=%u stale_rebind=%u stale_release=%u generation=%llu failures=%u\n",
            completed, (unsigned long)CACHE_PRIVATE_COUNT, (unsigned long)maximum_charge, (unsigned long)maximum_io_chunks,
            expected_errors, stale_rebind, stale_release, (unsigned long long)owner.generation, failures);
    return failures != 0;
}

int main(int argc, char **argv)
{
    FILE *report = stdout;
    const char *path;
#ifdef CACHE_BSP_RESIDENCY
    const char *bsp_path;
#endif
#ifdef GEKKO
    (void)argc; (void)argv;
    VIDEO_Init(); GXRModeObj *mode = VIDEO_GetPreferredMode(NULL);
    if (!mode) { fprintf(stderr, "STREAM video mode unavailable\n"); return 2; }
    void *framebuffer = SYS_AllocateFramebuffer(mode);
    if (!framebuffer) { fprintf(stderr, "STREAM framebuffer unavailable\n"); return 2; }
    framebuffer = MEM_K0_TO_K1(framebuffer);
    console_init(framebuffer, 20, 20, mode->fbWidth, mode->xfbHeight, mode->fbWidth * VI_DISPLAY_PIX_SZ);
    VIDEO_Configure(mode); VIDEO_SetNextFramebuffer(framebuffer); VIDEO_SetBlack(false); VIDEO_Flush();
    VIDEO_WaitVSync(); if (mode->viTVMode & VI_NON_INTERLACE) VIDEO_WaitVSync();
    if (!fatInitDefault()) { fprintf(stderr, "STREAM SD unavailable\n"); return 2; }
    if (mkdir("sd:/halo-wii-memory", 0777) && errno != EEXIST) { fprintf(stderr, "STREAM log directory failed\n"); return 2; }
    report = fopen(CACHE_REPORT_PATH, "a");
    if (!report) { fprintf(stderr, "STREAM log open failed\n"); return 2; }
    path = CACHE_STREAM_FILENAME;
#ifdef CACHE_BSP_RESIDENCY
    bsp_path = CACHE_BSP_FILENAME;
#endif
#else
    if (argc !=
#ifdef CACHE_BSP_RESIDENCY
        3
#else
        2
#endif
        ) { fprintf(stderr, "STREAM requires private raw input paths\n"); return 2; }
    path = argv[1];
#ifdef CACHE_BSP_RESIDENCY
    bsp_path = argv[2];
#endif
#endif
    if (fprintf(report, "BEGIN %s build=%s\n", CACHE_REPORT_KIND, CACHE_PROBE_BUILD_ID) < 0 || fflush(report)) return 2;
    int result = wii_cache_address_fixture(report, 1);
    result |= wii_cache_arena_fixture(report, 1);
    result |= wii_cache_stream_fixture(report, 1);
#ifdef CACHE_WIDGET_GRAPH
    result |= wii_cache_widget_fixture(report, 1);
#endif
#ifdef CACHE_BSP_RESIDENCY
    result |= wii_cache_bsp_fixture(report, 1);
#endif
    result |= stream_cycles(report, path
#ifdef CACHE_BSP_RESIDENCY
                            , bsp_path
#endif
                            );
    if (fprintf(report, "END %s build=%s result=%d\n", CACHE_REPORT_KIND, CACHE_PROBE_BUILD_ID, result) < 0 || fflush(report) || ferror(report)) return 2;
#ifdef GEKKO
    if (fclose(report)) return 2;
    for (unsigned i = 0; i < 60; ++i) VIDEO_WaitVSync();
#endif
    return result;
}
