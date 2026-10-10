# Memory strategy diagnostic (HWI-007)

`ninja wii_memory_strategy` builds `memory_strategy.dol`. It measures the Wii
memory map, checks what fits of the current upstream reservations, and
exercises the relocation strategy on a real tag graph and on the game state.
The repository contains no game data; the graph phase needs a private manifest
on the SD card.

## What one launch does

1. Clears FPSCR NI before any engine code (ADR-018) and records FPSCR.
2. Logs the arenas at entry, the linked image bounds, the GX FIFO and both
   framebuffers, and the heap. It then plans the upstream reservations against
   the actual arenas: tag cache 22 MiB, game state 20 MiB, sound cache 4 MiB,
   and the Xbox (22 MiB) and desktop (256 MiB) texture caches.
3. Runs four load/use/unload cycles. Placements alternate by 4 KiB. Each
   cycle:
   - plans MEM2 (tag slot, state slot, sound slot, two 64 KiB IO buffers,
     2 MiB reserve);
   - builds every upstream game-state allocation in the state slot. The
     allocations come from `tools/wii/game_state_census_table.c`, and are made
     with the engine's own `data.c` and `memory_pool.c`. The cycle restores
     the previous image (previous cycle or previous launch), updates the
     state, and saves it as a pointer-free image;
   - with a manifest: streams the tags and one BSP, walks the whole tag graph
     through the upstream validator schema, relocates every pointer field the
     walk named to its MEM2 address, and walks again through the native
     pointers. Both reports must equal the host reference;
   - releases the plan. A handle taken in the cycle must then reject, and the
     MEM2 arena must be restored.
4. Last cycle only: corrupts one block address and one tag-table datum, and
   expects each walk to reject at that offset. Then checks the restored bytes
   walk cleanly, and rejects damaged state images (truncated, flipped bit,
   bad magic, empty).
5. Reports the main-stack peak, from paint below the entry frame. Then probes
   the heap last, because the probe moves the arenas.

## Files

- `sd:/halo-wii-memory-strategy/strategy.txt` (optional): `key value` lines.
  Every key is required exactly once: `tag_file`, `bsp_file`, `tag_bytes`,
  `tag_crc32`, `bsp_bytes`, `bsp_crc32`, `map_bytes`, `bsp_ordinal`,
  `graph_digest`, `edge_digest`, `pointer_fields`, `null_fields`,
  `reachable`, `residual`. The expected values come from the host reference
  (`tools/wii/cache_graph_host.c`, built for i686).
- `strategy.log`: the report.
- `state.bin`: the last saved game-state image.
- `runs.txt`: the launch counter.

## Host references

| Reference | Build | What it runs |
|---|---|---|
| `tools/wii/cache_graph_host.c` | i686 or x86-64 | The same graph walk. Relocation needs 32-bit pointers. |
| `tools/wii/game_state_host.c` | i686 only, with the engine units | The same game-state cycle sequence. |

The tables these use are generated:

| Script | Writes | Built from |
|---|---|---|
| `tools/wii/export_tag_schema.py` | `tools/wii/cache_schema_tables.c` | `port/linux/game/tag_schema_*.c` with clang for i686 (WSL) |
| `tools/wii/game_state_census.py --emit-c` | `tools/wii/game_state_census_table.c` | Each allocation site's own count and size expressions, evaluated by the compiler |

## Limits

- One map's tags and one BSP only. Other maps and BSPs are not walked.
- The walk proves the pointer fields the validator schema names, and the
  buffer pointers its checks read. Address-like words outside those fields
  are counted (`residual`), not classified one by one. Some are datums or
  data; some are pointers in blocks the game never reads.
- No tag field is byte-swapped. The relocated graph mixes native pointers
  with little-endian numbers, which proves addressing only, not the endian
  conversion.
- The game-state payloads are representative values written by the
  prototype. They are not real engine objects. The lruv cache and the GPU
  decal vertices are rebuilt empty instead of being persisted.
- The texture and sound caches are reservations only. No textures or
  sounds are decoded.
- Emulator only. Physical Wii is untested.
