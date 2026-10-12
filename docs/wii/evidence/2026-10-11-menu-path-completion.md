# Menu path completion: script threads, the campaign from the menus, the interface's drawing (HWI-033)

HWI-028 (formerly HWI-016D) booted through the original menus into
split-screen Prisoner, with the game state equal to the i686 host on every
screen except for the script threads, and left four gaps: the campaign
could not be reached from the menus, a "UI bitmap" was missing from the
converter generation, the menus' plasma effect was not composited, and the
min/max blends were drawn as alpha blends. This record closes them, with
two cold launches each of the campaign path and the split-screen path on
two emulator hosts, all replayed on the i686 host.
[Numeric evidence](2026-10-11-menu-path-completion.json)

**Result:**

- **Script threads:** the whole game state, script threads included, equals
  the host on all 22 split-screen screens (HWI-028: 6 of 22) and on every
  campaign screen. Their storage already held the same values at the same
  offsets on both byte orders; the digest is what changed (below).
- **The campaign from the menus:** Campaign, the new profile, the
  difficulty (Normal); the engine loads a10 through its own path and draws
  its environment through HWI-030's path from its first frame. The whole
  state equals the host after each of a10's first 900 ticks, at the
  environment's first frame and at its 3,800th, through the script's BSP
  switches 0â†’7â†’1â†’8.
- **Found and fixed on the way:** entering a10 from the menus overflowed
  libogc's 128 KiB main stack (peak 155,952 bytes) into the boot driver's
  state; the main thread now has 256 KiB. The "missing UI bitmap" was
  Prisoner's lightmap page, asked for under the menus' map name in the frame
  the map changed; the converter had it. A campaign map's shared textures
  broke `boot_stage.py`.
- **Drawing:** the plasma effect is composited by a TEV program that
  follows the Xbox's register combiners; min and max blends are exact,
  through an EFB copy and TEV compares. A GX self-test of 112 authored
  cases matches the models on both hosts, EFB for EFB.
- **Regressions:** Prisoner direct-load `127656ef` / whole state
  `7eb53576`; a10 `f752db70` at 30 Hz, 60 Hz and retrace pacing;
  HWI-015A `c1ad6ae6`.
- **Hosts:** two cold launches of each path on Titan (D3D) and Research
  (Vulkan) from a clean commit, `source_dirty=false`: guest pass,
  persistence passed, host exit `0x0`, no OS events. One open runner
  finding: under HWI-077's focus-safe runner the pad read as unplugged at
  a10 tick about 260 in both Titan campaign attempts (the host replay
  followed it; the earlier runner never showed it). Physical Wii untested.

## Build

| Item | Value |
|---|---|
| Source | Clean `57b4fe16` on `agent/hwi-033` (`wii` at `bae56603` merged: HWI-030, HWI-034, HWI-073), `source_dirty=false` (HWI-027 rule) |
| Build ID | `10dcb04d7f3d53dd` |
| Toolchain | devkitPPC GCC 16.1.0 |
| `engine.dol` | 4,201,280 bytes, SHA-256 `ca21c6a4c591acf48bfa9e984235ac40beacbeb03a2d44fcad14c065229ac16c` |
| `engine.elf` | 37,893,908 bytes, SHA-256 `800557308d8920c90064f79b94ffacb265e166981fecbe5cf61d7e51b15a6636` |
| Host reference | i686, Debian clang, the same commit (`run_boot_host.py`); program SHA-256 `d9fc83facd47a9d3ff48213c416a18aec137d0c70b3333713f7fe66b3d6a7746` |
| Content | HWIMAP02 stagings of `ui`, `prisoner`, `a10` (HWI-026/028); converter generation `6e7b87372c08e3fa` (UI textures, Prisoner's and a10's render files; private) |
| Artifacts | `package_artifacts.py` stage and inspect `--require-clean` on `e64d8ad4` (the same engine sources, `wii` at `7f23a821` merged: HWI-077's runner): PASS, 33 files, 0 problems, 0 findings |
| Tests | 594 (1 skipped) on Python 3.14 and 3.11 |

## 1. Script threads

**What differed.** HWI-028 found the `hs thread` allocation's bytes unequal
to the host's up to frame 4,310, and read it as a short script value stored
at the other end of its long stack cell. With the allocation dumped at
every sampled screen on the Wii and on the host (`dump_allocation`,
`compare_hs_threads.py`), every live frame of every thread on all 22
screens splits into 1-, 2- and 4-byte cells each holding the host's bytes
in the other order: the same values at the same offsets. HWI-016C's cell
fix (merged before HWI-028's evidence) already stores a short at the start
of its cell on every port. What differed was the digest: the boot digest
hashes each aligned 8-byte group of the game state as a multiset of bytes,
which is byte-order neutral for naturally aligned scalars only. A thread's
stack packs its frames at 2-byte offsets (the Xbox's layout, and the
saves'), so a long can straddle two groups and give each group different
halves in each byte order (measured: the expression index of the menus'
first frame, at stack offset 0x22).

**Which fix, and why.** Storage parity in the strict sense (the same bytes)
is not possible for native longs on two byte orders without changing every
reader, and aligning the stack's cells would change its capacity and the
saves' layout on every port. The storage is already position-identical,
which is what HWI-026's value-cell fix gave. So the digest reads the
threads by their fields (`real_map_digest_hs_threads`): each typed field as
its value (identifier, type, flags, script, sleep times, result; each live
frame's previous frame, expression, result and size, pointers into the
thread as offsets there), and each live frame's values as one group: the
multiset of their bytes, whose types only the script functions know.
Popped frames' bytes above the top frame are left out: nothing reads them
(`hs_stack_push` zeroes a frame's values). This is stronger than the map
run's one group a thread. What bytes alone cannot tell (a cell written as a
long on one port and as a short with its neighbour on the other look alike)
shows in behaviour: the threads' typed fields and the whole state compare
equal screen by screen and tick by tick.

**A second byte-order effect in the digest.** The canonical digest reads a
word lying in the game state's or the tag slot's region as a pointer
(HWI-016C). Four bytes that are no pointer can read as one in one byte order
only: 6 of a10's first 900 ticks differed this way in a development launch
(transiently: 486-487, 586-587, 836-837), while every allocation's raw
digest was equal at tick 486. The boot mode now logs both forms: canonical
(comparable across placements) and raw (the Wii against its host replay,
which maps the same addresses). `BOOT_TICK` uses the raw form.

**Proof.** Split-screen path (HWI-028's pad script, later start), all four
launches replayed on the host:

| Comparison | Result |
|---|---|
| Screens sampled at the same frames | 22 of 22 |
| Whole state equal, canonical (script threads included) | 22 of 22 |
| Whole state equal, raw | 22 of 22 |
| Allocations at the first screen / first environment frame | 77 / 77 each |
| Environment first and last frame (Prisoner) | equal |

## 2. The campaign from the menus

**The path.** A cold card: Campaign (A) â†’ the new campaign's profile is
created and saved (A) â†’ Choose Difficulty, Normal (A) â†’ the engine's own
`main_menu_switch_to_single_player` loads a10. Three presses on one pad
(`input-camp4.json`, private: 1,500 polls' wait, then A, 300, A, 300, A).
The boot mode marks the map a campaign map (its scenario's type is solo)
and labels every frame `MAP A10: CAMPAIGN - ENVIRONMENT ONLY (NO OBJECTS,
SKY, EFFECTS, HUD YET)`.

**Drawing.** a10's environment draws through HWI-030's path (geometry from
the tag slot, textures as heap blocks beside the menus), from engine frame
1,225, its first frame. The engine camera is the cinematic's: the opening
shows BSP 0 for two ticks, then BSP 7 (little of it in view, as in
HWI-030), BSP 1 from tick 1,308 and BSP 8 from tick 3,365. Objects, the sky,
effects and the HUD are not drawn. If a BSP cannot be drawn, the frame says
so under a panel and the game runs on (`BOOT_ENVIRONMENT_NOT_DRAWN`; not
seen in these launches).

| Campaign launch | Host | a10 from frame | Ticks digested / run | Every tick equal to the host | First environment frame, raw state | Frame 3,800, raw state | Result |
|---|---|---|---|---|---|---|---|
| Titan 1 (HWI-073 (before HWI-077) runner) | Titan (D3D) | 1225 | 900 / 3803 | 900 of 900 | `af67045d` | `33f6fbaf` | campaign_reached, pass |
| Titan 2 (HWI-077 focus-safe runner) | Titan (D3D) | 1225 | 900 / 3803 | 900 of 900 | `af67045d` | `77b959e2` | campaign_reached, pass |
| Research 1 (HWI-073 (before HWI-077) runner) | Research (Vulkan) | 1225 | 900 / 3803 | 900 of 900 | `af67045d` | `33f6fbaf` | campaign_reached, pass |
| Research 2 (HWI-073 (before HWI-077) runner) | Research (Vulkan) | 1225 | 900 / 3803 | 900 of 900 | `af67045d` | `33f6fbaf` | campaign_reached, pass |

Titan's second launch, under HWI-077's runner, saw the pad unplugged at scan 1,485 (a10 tick about 260; also in a first attempt, with a 12-times longer pad script); the game then followed another path, and its host replay, given the same unplug, followed it tick for tick (below, Launches). The three launches under HWI-073's runner agree with each other at every digest.

The canonical digests at those frames are `cba7e8c3` (first) and `5f41047f` (frame 3,800); the host's
canonical end digest differs by the effect above, its raw one is equal.

## 3. The "missing UI bitmap" (tag 1642, bitmap 1)

The converter did not drop it. The UI map has 983 tags; tag 1642 is
Prisoner's lightmap bitmap group, and
the generation holds both of its pages (`textures/prisoner/01642-000.hwt`,
`-001.hwt`), which the environment draws (`lightmap_pages=2
lightmap_failed=0`). The texture cache asked for page 1 while Prisoner was
loading, before the first frame of the new map; the boot mode still had
the menus' map name and looked for `render/ui/textures/01642-001.hwt`. The
hook now checks which map is loaded before answering, and the menus'
answer is given only for the menus' map. The same miss appeared for a10
(tag 3264, bitmap 7) and is gone too: no `BOOT_UI_TEXTURE_FAIL` in any
evidence launch. A fixture test (`test_content_pipeline.py`) guards the
converter's side: every index of a multi-bitmap group is planned under its
own index, in both texture profiles.

## 4. The plasma effect and min/max blends

**Plasma.** `ui_widget.c` draws a widget with two animated maps of the
plasma bitmap under its own bitmap; the Xbox rasterizer
(`rasterizer_xbox_dynavobgeom.c`) builds a register-combiner program for it
(map0_to_1 = min, map1_to_2 = alpha blend with the plasma bit). Its
semantics are those of the Linux port's NV2A translator (`nv2a_psh.c`):
with a the maps' faded alphas and b their tinted blues,
x = a1 âˆ’ a0 + Â½, y = b0 âˆ’ b1 + Â½, r = min(1, 4Â·(y â‰¥ Â½ ? x : y)Â²),
u = max(2r âˆ’ 1, 0), P = (r + uÂ²)/2; colour = PÂ·plasma fade + map 2Â·tintÂ·vertex
colour, alpha = map 2's alphaÂ·fade. The maps' texture coordinates follow the
Xbox's screen-geometry vertex shader (`vertex_shader_table[3]`, translated
by `nv2a_vsh.c`): anchored to the screen or the vertex's, times the map's
scale, plus its offset, times its texture scale. The Wii draws it as a
12-stage TEV program (15 with alpha planes): the plasma maps through a
texture swap (a, a, a, b), the subtraction with a half bias, the scaled
squares, the mux as two compares, the halved sum. `ui_combiners.py` holds
the Xbox program (an evaluator of the combiner words, which a test checks
against the rasterizer's source) and the TEV program as an 8-bit model.

**Min and max.** GX's blender has neither, and its logic operations are
bitwise. The destination is copied from the EFB (RGB8, unfiltered) to a
texture, read at the same pixel, and three TEV compare stages give
(s > d ? d : 0) + (d > s ? s : 0) + (s = d ? s : 0) (max with the operands
swapped); the result replaces the pixel. Exact on 8 bits. The copy is a
heap block taken for the draw. The menus draw no min/max quad.

**Proof (EFB).** With `selftest=1` the boot mode draws, before the engine
starts, 48 plasma cases (three authored 4Ã—4 RGBA8 textures, point sampled
at texel centres, random tints, fades and vertex colours) and 32 min plus
32 max cases over authored backgrounds, and peeks each centre pixel.
`ui_combiners.py check-log`:

| Self-test, every evidence launch | Result |
|---|---|
| Min and max: EFB against the exact per-channel result | 64 of 64 equal |
| Plasma: EFB against the TEV model | 48 of 48 equal |
| Plasma: EFB against the combiners' float result | 9 cases off by 0, 32 cases off by 1, 6 cases off by 2, 1 cases off by 3 |
| EFB CRC of the self-test frame | `b3a13762` on Titan and on Research |

Over 30,000 random cases (`test_ui_combiners.py` checks 6,000), the TEV
result is within 2 of the combiners' float result for 89% of inputs, within
4 for 97% and within 8 for 99.5%. The rest lie on the program's own
threshold y â‰¥ Â½, where one step of input precision flips the mux: the
Xbox's 9-bit combiners have such edges too, at slightly different places.

**On the menus.** On the split-screen path every screen quad but the progress bar's is a plasma quad: 61,630 of 67,818 are composited, none missing; 67 menu bitmaps load, 0 fail. HWI-028 drew the widget's own bitmap for them; where the widget's plasma fade is zero (most frames) the result is the same image, and the frame CRCs change with the manifest line's build field anyway, so the EFB proof is the self-test.

## 5. The render module's unsupported list

The boot mode now reports `render: mirrors, HUD` in place of
`mirrors, HUD, interface, widgets, menus, text`, and no longer reports the
plasma effect. The map run, which draws no interface, keeps the long form
with "(drawn in the boot mode only)". Still reported in the boot mode: the
console window, HUD and game engine overlays; objects, first-person
weapons, the sky, decals, particles, effects, water and fog, detail maps
and the rest of the shader matrix, the camera effect matrix.

## 6. The main stack

The manifest line at the bottom of every frame vanished once a10 loaded:
its string, in the boot driver's state, read empty. The boot driver's
state sits in .bss just below libogc's default main stack (128 KiB,
`s_mainStack`), with libfat's `Fsid` between them. Painted below the
driver's entry frame, the main stack reaches 155,952 bytes on the campaign
path, so it ran past its base into both. The main thread now has the
driver's own 256 KiB stack, through libogc's weak `__ppc_main_sp` (crt0
loads r1 from it; the default 128 KiB is no longer linked), and the boot
mode logs the peak:

| Path | Main stack peak |
|---|---:|
| Campaign from the menus (a10, 3,800 ticks) | 155,952 bytes |
| Split-screen Prisoner from the menus | 97,200 bytes |
| Map run (HWI-029's plan: Prisoner, a10 9,000 ticks at three cadences) | 136,760 bytes |

The map run's peak also exceeds 128 KiB with HWI-030's drawing merged. To
keep MEM1 for the menus' textures, two static 64 KiB buffers (the boot
hash chunk, the min/max copy) became heap blocks around their use; MEM1
after GX is 1,105,920 bytes, and every menu bitmap loads.

## 7. Regressions

| Check | Result |
|---|---|
| Split-screen Prisoner from the menus | above: 22 of 22 screens, environment equal, all four launches |
| Prisoner direct load (HWI-029's plan) | simulation `127656ef` (HWI-015B's classification) and `961a5ec5`; whole state at 60 Hz `7eb53576`; all 18 runs, `expect` met (Research, HWI-077 runner, build `10dcb04d7f3d53dd`) |
| a10 direct load, 9,000 ticks at 30 Hz, 60 Hz and retrace | 30hz `f752db70`, 60hz `f752db70`, vsync_measured `f752db70`: cadence-invariant |
| HWI-015A fixed-step scenario | `c1ad6ae6` |
| Load failures in the plan (6 kinds) | 6 of 6 refused as expected; `END target=engine build=10dcb04d7f3d53dd cycles=3 cadences=6 failures=0`, host exit `0x00000000`, persistence passed, 1125 s |

## 8. Launches

| Launch | Host, backend | Runner | Guest | Persistence | Host process | OS | Dolphin s (shared) | Host replay |
|---|---|---|---|---|---|---|---:|---|
| campaign 1 | Titan, D3D | HWI-073 (before HWI-077) | `campaign_reached` pass | passed | exit `0x00000000` | no instability events observed | 452.5 | 5/5 screens, 900/900 ticks, environment yes/yes |
| campaign 2 | Titan, D3D | HWI-077 focus-safe | `campaign_reached` pass | passed | exit `0x00000000` | no instability events observed | 450.9 | 5/5 screens, 900/900 ticks, environment yes/yes |
| campaign 1 | Research, Vulkan | HWI-073 (before HWI-077) | `campaign_reached` pass | passed | exit `0x00000000` | no instability events observed | 447.9 | 5/5 screens, 900/900 ticks, environment yes/yes |
| campaign 2 | Research, Vulkan | HWI-073 (before HWI-077) | `campaign_reached` pass | passed | exit `0x00000000` | no instability events observed | 447.6 | 5/5 screens, 900/900 ticks, environment yes/yes |
| split screen 1 | Titan, D3D | HWI-077 focus-safe | `environment_reached` pass | passed | exit `0x00000000` | no instability events observed | 132.2 | 22/22 screens, 0/0 ticks, environment yes/yes |
| split screen 2 | Titan, D3D | HWI-077 focus-safe | `environment_reached` pass | passed | exit `0x00000000` | no instability events observed | 131.3 | 22/22 screens, 0/0 ticks, environment yes/yes |
| split screen 1 | Research, Vulkan | HWI-073 (before HWI-077) | `environment_reached` pass | passed | exit `0x00000000` | no instability events observed | 128.4 | 22/22 screens, 0/0 ticks, environment yes/yes |
| split screen 2 | Research, Vulkan | HWI-073 (before HWI-077) | `environment_reached` pass | passed | exit `0x00000000` | no instability events observed | 128.4 | 22/22 screens, 0/0 ticks, environment yes/yes |

Guest, persistence, host and OS outcomes are recorded apart for every launch. The split-screen launches reach Prisoner at engine frame 5787 (HWI-028: 5,919; the pad script now starts 1,500 polls later, after the self-test and the hash of a10). Every launch's self-test: 112 of 112 cases as modelled, EFB CRC `b3a13762` on both backends.

Every launch ran under HWI-073's concurrent lane lock (`--allow-concurrent`);
all but Titan's second campaign and second split-screen launch recorded
`host_shared=true`, so their times compare only with runs at the same lane
count. All launches ran the same evidence `engine.dol`. The Research launches
and Titan's first campaign launch used the runner before HWI-077, which could
take window focus; the rest used HWI-077's focus- and keyboard-safe runner
(input bound to no device, its guard logged no action). Under HWI-077's
runner, Titan's campaign launches saw the pad report no controller at pad
scan 1,485, twice, which the earlier runner never showed: guest-visible
input differs there, and it is reported to the runner's owner. Apart from
that event, both launches of a path on a host gave the same EFB CRCs,
digests and frames; across hosts the game state and the self-test's EFB are
identical, and the menu and environment EFB CRCs differ by backend.

## Timings

| Step | Host | Wall |
|---|---|---|
| Wii build, full | Titan | 77 s |
| Host reference build | Titan (WSL Debian) | 151 s |
| `boot_stage.py` (ui, prisoner and a10 environments) | Titan | 6 s |
| Campaign launch (5,025 frames; 900 digested ticks) | Titan / Research | 451-453 s / 448 s Dolphin (shared lanes) |
| Split-screen launch (5,937 frames) | Titan / Research | 131-132 s / 128 s Dolphin (shared lanes) |
| Map-run regression (HWI-029's plan) | Research | 1,125 s Dolphin (shared lanes) |
| Host replay | Titan (WSL Debian) | 4.6-19.8 s |

Research load averages were 2.6 to 4.0 during the launches. All Dolphin times were taken under concurrent lanes
(`host_shared`), so they compare only with runs at the same lane count.

## Remaining work

- **Skipping a cinematic, and every checkpoint revert, halts on the Wii.**
  A button during a10's opening skips the cinematic, which reverts to the
  checkpoint the cinematic saved; `game_state_read_from_file` reads the
  saved game into a 20 MiB temporary before taking it, and the Wii's heap
  has no such block (`couldn't read saved game file (#8)`, found in a
  development launch). The same path serves a death's revert. Needs its own
  ticket (a streamed or in-place check of the saved image).
- a10 from the menus draws the environment only: objects, the sky, effects
  and the HUD are other tickets' (HWI-054 and the renderer's rest).
- Other map combinations and meters in screen quads stay approximated
  (none on the menu path).
- Sound and movies are not started in the boot mode; system link and
  internet play are absent.
- The script thread's typed cells cannot be told apart from their bytes;
  their equality rests on the behaviour comparisons above.
- No physical Wii was used.

## Privacy

The card images, staged maps, converted textures, frame dumps, review
frames and run folders are private and outside Git. This record holds
digests, counts and the authored self-test only.
