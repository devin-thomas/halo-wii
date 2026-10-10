/* HMA1 model animation loader (HWI-008E).
 *
 * Container (tools/wii/content_animation.py): section 1 ANIMATIONS (80-byte
 * records), 2 FRAME_INFO (f32), 3 DEFAULTS, 4 FRAMES and 5 COMPRESSED (bytes,
 * every field big-endian). One animation graph tag per file.
 *
 * The engine reads an animation's frame info, defaults, frames and
 * compressed block as native shorts, longs and floats straight out of its
 * tag data (source/models/model_animations.c). The decoded form is exactly
 * that: the same byte layout as the Xbox data, every field in native order.
 *
 *   frame info  f32 per frame: dx, dy (type 1); dx, dy, dyaw (2); dx, dy, dz,
 *               dyaw (3)
 *   defaults    per node without the flag: rotation 4 x s16, translation
 *               3 x f32, scale f32
 *   frames      per frame, per node with the flag: the same three
 *   compressed  the block with every array at its original offset (the
 *               11-word header, node headers, u16 keyframe frame indices,
 *               6-byte rotations, f32 translations and scales)
 *
 * Decision: compressed animations stay compressed in memory and are decoded
 * at playback by the engine's own keyframe code, as on the Xbox. Expanding
 * them at load would cost frame count x frame size per animation
 * (hma_graph.expanded_bytes reports it). Arrays inside a compressed block keep
 * their original offsets, so some 4-byte arrays sit at 2-byte offsets; the
 * playback reader must load them alignment-safely. Every block starts 4-byte
 * aligned in the decoded memory. */
#ifndef HALO_WII_HMA_GRAPH_H
#define HALO_WII_HMA_GRAPH_H

#include "content_sections.h"

#define HMA_ANIMATION_FORMAT "HhhhhhHHIIIIIIIiIIIIIIII"
#define HMA_MAX_NODES 64u
#define HMA_COMPRESSED_BIT 1u
#define HMA_COMPRESSED_HEADER_BYTES 44u
#define HMA_KEYFRAME_COUNT_BITS 12u

struct hma_record {
    uint16_t index;
    int16_t type, frame_count, frame_size, frame_info_type, node_count;
    uint16_t flags, pad;
    uint32_t node_list_checksum;
    uint32_t translation_flags[2], rotation_flags[2], scale_flags[2];
    int32_t compressed_data_offset;
    /* (byte offset, bytes) into FRAME_INFO, DEFAULTS, FRAMES, COMPRESSED */
    uint32_t frame_info_offset, frame_info_bytes, default_offset, default_bytes;
    uint32_t frame_offset, frame_bytes, compressed_offset, compressed_bytes;
};

struct hma_animation {
    struct hma_record record;
    const float *frame_info;          /* frame_info_bytes / 4 values */
    const unsigned char *defaults;    /* default_bytes */
    const unsigned char *frames;      /* frame_bytes */
    const unsigned char *compressed;  /* compressed_bytes, 4-byte aligned */
};

struct hma_graph {
    uint32_t animation_count, compressed_count;
    struct hma_animation *animations;
    uint64_t expanded_bytes;          /* frames x frame size of the compressed animations */
    struct content_arena arena;
};

/* Validates the container and every animation (index, pad, node count,
 * frame info type, frame size against the node flags, the four data ranges in
 * order and tiling their sections, frame info, default and frame sizes, and
 * for a compressed animation the block: header, node headers and arrays that
 * cover it exactly) and decodes into owned memory. */
enum content_error hma_load(const unsigned char *data, uint32_t bytes, struct hma_graph *out);
void hma_release(struct hma_graph *graph);

/* SHA-256 of the decoded graph, every field little-endian: per animation its
 * record (HMA_ANIMATION_FORMAT), frame info, defaults, frames and compressed
 * block. Without the records this is the Xbox source data, so the host can
 * tie it to the converter's source hash. */
void hma_digest(const struct hma_graph *graph, unsigned char digest[32]);

#endif
