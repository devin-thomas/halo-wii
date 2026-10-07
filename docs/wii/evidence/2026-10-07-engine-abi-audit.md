# Preparatory engine ABI audit and script-cell failure fixture

Source audit base: `b5239daeb318343c9cf83695dbeaec940fe2e911`. This is source
inspection, not engine compilation or behavioral qualification. The Wii target
currently builds the probe and synthetic C fixtures only. The [earlier 17 checks](2026-10-07-abi-fixtures.md)
matched host/PPC execution. The expanded 24 checks initially passed host execution
and PPC compilation; a subsequent coordinator run now confirms matching PPC
canonical/arithmetic output. See the [scoped runtime record](2026-10-07-hs-cell-runtime.json).
Clean Dolphin shutdown and actual engine qualification remain open.

## Source-backed gaps

Paths/lines refer to the audit base above.

| Area | Actual source boundary | Required next proof |
|---|---|---|
| HS cells | `source/hs/hs_runtime.c:1668,1684,1742,1764` writes a byte/word through the first address of an uninitialized long. Whole cells are copied onward at line1914. PPC byte order changes which integer bits are written; indeterminate upper bytes also affect persistence. | Decide historical upper-bit handling per conversion using the existing source comments/reference evidence, then test actual defined conversions and saved cell bytes. |
| Serialized scalar access | `source/memory/data_encoding.c:140,432,460` uses host copies, unconditional multibyte reversal and typed reads; `source/memory/byte_swapping.c:252` casts byte offsets to wider pointers. | Test an actual mixed scalar record at offsets0-7, exact x86 wire bytes, truncation and expected input mutation. Same-target round trips cannot prove wire compatibility. |
| Packet/native layout | `source/memory/data_packets.c:264,348` uses native sizeof(long) and short header reads; `source/networking/network_messages.c:195` derives schema from native structures. | Compile one actual packet layout and compare field sizes/offsets/wire bytes; test odd offsets and malformed array counts. |
| Cache/save identity | `source/cache/cache_files.c:163,1188,1250` crosses native pointer-bearing records; `source/saved games/player_profile.h:70` embeds wchar_t and `player_profile.c:806` checksums copied profile bytes. | Assert actual fixed-width layout, alignment and UTF-16 bytes. Test authored address relocation/span rejection; authentic-file validation remains later. |
| Calls, inline linkage, atomics | `source/bungie_net/common/thread.h:24`, `port/linux/include/halo_linux_prefix.h:56`, `port/linux/src/xbox_kernel.c:432`, `tools/linux_msvc_semantics.py:2` expose callback, intrinsic and linkage assumptions. | Use actual declaration patterns across two translation units and verify exchange/add/CAS return contracts. Preserve PPC calling conventions. |
| Gameplay math | `source/cseries/cseries.h:341` emulates FISTP with builtin rint; `source/game/players.c:1308,3778` uses pow/cos decisions; `source/game/game_time.c:419` uses floor. Shared math is declared in `port/include/halo_math.h:30` and integrated by `tools/linux_build.py:500`. | Compare shared sin/cos/pow bit patterns, half ties, negative rounding and gameplay thresholds; retain contraction controls. Signed zero, denormals and invalid inputs need distinct cases. |

Linux prior art includes short wchar, disabled strict aliasing, wrapping and
disabled floating contraction. Aliasing flags do not repair unaligned access.
Its x86 architecture definitions, double alignment and register-struct-return
flags cannot be copied to PPC as a calling-convention solution. Current HS
converters have signature `long(long)` (`hs_runtime.c:384`); the Linux build
comment describing union-returning converters is stale.

## Implemented bounded fixture

`port/wii/abi/boundary.c` now provides low8/low16 replacement on a uint32_t cell.
The caller supplies the full incoming payload; unsigned masks replace only the
specified low bits. These helpers are used only by the synthetic fixture, not
the game engine. No general HS upper-bit or save-format policy was selected.

The model starts from authored payload `a1b2c3d4` and simulates first-byte/word
stores using explicit byte arrays. It avoids executing the engine's
uninitialized/aliasing operations:

| Store | Little-endian first-address result | Big-endian first-address result | Explicit low-bit replacement |
|---|---|---|---|
| byte1 | a1b2c301 | 01b2c3d4 | a1b2c301 |
| word1234 | a1b21234 | 1234c3d4 | a1b21234 |

Seven new checks cover both failure models, zero/nonzero boolean conversion,
negative/positive shorts and a representable negative real-to-short conversion.
Existing `n == 0` boolean behavior is preserved in the model. Supplied upper
bits are retained; `-2.75f` truncates to `-2`, represented in the low word as
`fffe`. Source comments explicitly describe January stack-residue behavior
reaching saved-game CRCs. Zeroing all upper bytes or changing truth behavior
would require an engine/reference decision beyond this fixture.

Executed on a clean committed input tree:

```powershell
python tools/wii/run_abi_host.py --cc C:/msys64/ucrt64/bin/gcc.exe
python configure.py --wii --wii-devkitpro C:/dev/resources/toolchains/devkitpro --wii-probe-frames 300
ninja wii_probe
```

Host output (exit0):

```text
ABI CANONICAL checks=24 address=803a6010 offset=16 float=3fc00000 layout=0,4,8/12 varargs=ok datum=abcd,1234 cells=a1b2c301,a1b2fffe
ABI ARITHMETIC separate=00000000 sum1000=43a6aac4
ABI TARGET pointer_bytes=8 int_bytes=4 double_bytes=8
```

See [host manifest](2026-10-07-hs-cell-host.json) and
[PPC compile manifest](2026-10-07-hs-cell-build.json) for exact source commit,
input/binary hashes and flags. Native GCC16.2 and official devkitPPC GCC16.1
both compile with warnings as errors and contraction disabled. Independent
read-only review found no actionable defects in the three-file C change.

The new canonical line differs from the 17-check report, so an old PPC report
cannot be used to certify these checks. HWI-005 remains blocked on host teardown;
HWI-006 remains incomplete on that prerequisite and the actual engine proofs
listed above. The subsequent 24-check guest report ends `storage=1 abi=0`, with
300 frames/150 nominal ticks; host shutdown still exits `0xc0000409`. The host
runner's [exact comparison](2026-10-07-hs-cell-host-comparison.json) passes for
the same C input hashes. This synthetic agreement does not establish save compatibility.
