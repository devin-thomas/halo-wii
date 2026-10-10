# PPC semantic parity rules adopted and re-proved (HWI-006, ADR-018)

The [ABI semantics record](2026-10-10-abi-semantics.md) measured four PPC
target differences in actual engine code and one newlib header collision. ADR-018
turned them into rules. This record adopts those rules in the shared engine
source and the Wii build definitions, then reruns the same semantics fixture on
the host and on the PPC in stock Wii-mode Dolphin. The earlier failing results
stay in that record unchanged. [Numeric evidence](2026-10-10-abi-parity-adoption.json)

## Result

| | Host | PPC (Dolphin, two launches) |
|---|---|---|
| Source | clean commit `360b0c06` | clean commit `360b0c06` |
| Compiler | MSYS2 GCC 16.2.0 (x86_64-w64-mingw32) | devkitPPC GCC 16.1.0 |
| Build ID | `9e52a32a284ac798` | `ef10ed0bb5c6643d` |
| Binary SHA-256 | exe `24c474f1…21ce` | DOL `052fbf204c4a470ca0e6acd616848e4d9743231721119e7c4f7a4683a1054c56` |
| Report | 240 checks, 0 failures, exit 0 | 240 checks, 0 failures (0 contract, 0 engine assumption), `result=0` |
| Run | — | host exit 0x0, not forced, 4.5 s each; identical report SHA-256 `3dca21a9…504c` |

**Comparison:** 328 ids compared, 320 identical, **0 unexplained**, 5 classified
and 3 target-specific. Before adoption the same comparison had 41 CHECK, 5 OBS and
20 MATH differences.

Three ids are target-specific and are not compared: the pointer width, a
pointer-valued varargs check, and the raw FPSCR before the start, which is `0x4`
on the Wii and 0 on the host.

The 5 classified differences are each pinned to the measured values. The
comparer classifies one only when its exact values and preconditions hold; any
other difference stays unexplained.

| Id | Host / PPC | Classification | Consumer impact |
|---|---|---|---|
| `engine.layout.players_switch.state_byte_2_12` | `c2` / `2c` | In-memory bit-field layout (allowed) | None. The byte alias is never read or written. The fields are used by name and round-trip identically. |
| `engine.layout.animation_header.byte_2_5` | `16` / `85` | In-memory bit-field layout | None. Stream bytes are now decoded explicitly into this struct. |
| `engine.layout.hud_nav_point.unit_bytes` | `5d00` / `d500` | In-memory bit-field layout | None. The datum is runtime-only. |
| `float.signaling_nan_widened_bits` | `7ff8…` / `7ff0…` | Signalling-NaN widening: x86 quiets, PPC `lfs` preserves | None known. IEEE arithmetic never produces a signalling NaN. |
| `musl.atan.f64` | raw digest differs in 1 block | The same signalling-NaN case: `atan` returns its NaN argument unchanged | The NaN-canonical and narrowed digests are identical (required by the classification). |

Everything else is identical on both targets:

- all 30 engine and gameplay MATH lines (60 across the two passes before) and 35
  of the 36 musl lines;
- the 8 subnormal contract checks that failed before;
- every char, wchar, decoder and playback check;
- the message ciphertext bytes and the generated-message digest;
- the recorded-animation playback digests.

## Rules as adopted

1. **Floating point.** `port/wii/runtime/runtime_start.c` provides
   `wii_runtime_start`, which clears FPSCR[NI] with `mtfsb0 29` and records the
   control word before and after. The fixture calls it as the first statement of
   `main`. Measured: `0x4` before, `0x0` after, still 0 after all engine code.
   No other current Wii main runs engine code. FPSCR is per-thread context in
   libogc, so every engine thread must call it at entry (HWI-015).
2. **Signed char.** `tools/wii/build.py` defines `ENGINE_SEMANTIC_FLAGS` and
   `tools/wii_build.py` defines `ENGINE_CFLAGS = CFLAGS + ENGINE_SEMANTIC_FLAGS`
   for future engine units. The set contains `-fsigned-char`, `-fshort-wchar`,
   `-fno-builtin-<wide>`, `-fno-strict-aliasing`, `-fwrapv` and
   `-fno-delete-null-pointer-checks`. The fixture's three engine units
   (generated engine code, engine probe, maths corpus) use it. The authored
   diagnostics, the probe, the GX scene and the geometry view keep `CFLAGS`.
3. **Bit-fields at external boundaries.**
   - *Recorded-animation header.* The new `recorded_animation_decode_event_header`
     takes the kind from bits 0-1 and the type from bits 2-7. A byte delta
     follows, or a little-endian word delta. Truncation returns 0 and leaves
     the outputs untouched. `recorded_animation_apply_event_stream` passes
     the decoded in-memory copy to the apply procs, and keeps its range
     assertions.
   - *Network message header.* `message_encrypt` and `message_decrypt` now use
     `GET_MESSAGE_FLAGS` and `GET_MESSAGE_SIZE`, the same shifts that
     `build_message_header` encodes. The bit-field union is removed.
     - A size below the 2-byte header is left untouched. Before, it wrapped
       to 0xFFFF blocks.
     - TEA blocks and the remainder's key bytes are loaded and stored as
       little-endian. This gives the Xbox ciphertext on a big-endian host and
       removes the misaligned word access.
   - *Players BSP-switch byte.* Not serialized at any external boundary, so it
     is unchanged. `-fsigned-char` fixes the NONE readback, and the field round
     trips are contracts.
4. **wchar_t.** The audit is in the next section. The rule is adopted as
   `-fshort-wchar` with a mechanical gate.
5. **Shared maths header.** `port/include/halo_math.h` now `#undef`s each of its
   12 renamed names before defining it. The fixture's own `#undef log2`
   workaround was removed, and the PPC build compiles with `-Werror`.

## wchar_t audit

- `wchar_t` appears 586 times in 47 engine files, with 398 `L"…"` literals.
  Engine text is UTF-16 code units.
- The engine calls 42 C-library wide functions. Most are wrappers in
  `source/text/unicode.c` (105 call sites); the rest are in
  `source/interface/ui_widget.c` (`wcschr`, `wcslen`, `_wcsnicmp`) and
  `source/cseries/cseries.c` (`towlower`). The list is in the JSON.
- So bare `-fshort-wchar` against newlib is **not** safe: those calls would
  reach newlib's 4-byte implementations.
- A separate 16-bit type would rewrite those declarations and literals and
  diverge from every other port. Linux and Android already compile with
  `-fshort-wchar` and a replacement `<wchar.h>` (`port/linux/include/wchar.h`,
  `port/linux/src/msvc_wide.c`) that sends every wide name to 16-bit code.
- Adopted: the same model on Wii, enforced by `engine_wide_references`, which
  runs `nm -u` on every engine object and fails the build on any C-library
  wide symbol.
- Gate result: the fixture's engine objects reference none on either target.
  A negative control, a PPC object calling `wcslen` and `towlower`, is
  reported.
- Remaining for HWI-015: build `unicode.c`, `ui_widget.c` and `cseries.c`
  against the 16-bit replacement header, and pass the gate.

## What executes

The generator now also extracts the actual
`recorded_animation_apply_event_stream`, its decoder, its `event_data_sizes`
table and its declarations. The authored glue is a traced apply-proc stub
that consumes each event's data as the real procs do, plus a counting
`recorded_animation_stream_damaged`. On both targets the fixture runs:

- all 256 header bytes and every truncation (448 cases), which must reject;
- an Xbox-authored stream with kind 1, 2 (byte) and 3 (LE word) deltas;
- exact finish, every truncation, and an unknown event type;
- 4,096 generated streams, with result and trace digests;
- `message_encrypt`/`message_decrypt` on a 20-byte message: no byte changes
  past it, and the header and round trip are correct;
- undersized headers (sizes 0 and 1), which must be left untouched;
- the maximum 0xFFF size, and 512 generated messages (digest);
- the earlier char, wchar, interlocked, `fast_ftol`, varargs, callback,
  64-bit, conversion and maths checks.

## Host regression (x86 behaviour unchanged)

`run_abi_semantics.py --regression-base 7a7168ca` extracts the pre-adoption
`message_encrypt`, `message_decrypt` and `recorded_animation_apply_event_stream`
from that commit. It renames them `original_*` and runs them against the
current bodies on x86:

| Case | Cases | Mismatches |
|---|---|---|
| Encrypt and decrypt, every size 2..0xFFF, random flags, type, key, payload and canary | 8,188 | 0 |
| Playback, every stream of 0 to 3 bytes at three tick budgets | 50,529,027 | 0 |
| Playback, generated streams up to ~56 bytes | 262,144 | 0 |

Mismatches compare result, ticks, stream position, damaged count, assertion
count and applied-event trace. The only intended behaviour change is excluded
and checked separately: a header whose size is 0 or 1. The original enciphers
up to 0xFFFF blocks past the message.

## Commands

```text
python tools/wii/run_abi_semantics.py --cc <msys2 ucrt64 gcc.exe> --output .local/adopt-final-host --regression-base 7a7168ca
python tools/wii/run_abi_semantics.py --cc <devkitPPC powerpc-eabi-gcc.exe> --wii-devkitpro <devkitPro> --output .local/adopt-final-ppc
python dolphin_lock.py acquire hwi-006 3600
python run_fixture.py <abi_semantics.dol> <new dir> halo-wii-memory/abi-semantics.log 300   (twice)
python dolphin_lock.py release hwi-006
python tools/wii/compare_abi_semantics.py <host-report.txt> <guest-report.txt> --json <out>
```

Equivalent local CI steps all passed:

- 270 `tools/wii` unit tests, on Python 3.14 and 3.11;
- the five host fixtures;
- `configure.py --wii` and ninja for the three Wii targets;
- `package_artifacts.py stage` and `inspect --require-clean`.

`tools/test_wii_build.py` has one pre-existing failure that is independent of
this change: a linked worktree has a `.git` file, not a directory. The
Linux, Windows and Android builds were not run locally.

## Not established

- **Hardware.** No real Gekko run: NI clearing, the cost of denormals with
  NI clear (HWI-040/041) and the generated NaN bits are emulator evidence only.
- **Threads.** Only the main thread's FPSCR is covered.
- **Full engine units.** No Wii engine build exists yet (HWI-015). The flags
  and the gate are exercised only by the fixture's engine units.
- **Recorded-animation payloads.** Event data after the header is still read
  native-endian: control flags, throttle, short deltas and unit control.
  `byte_swap_recording_stream` is a no-op, and v1 streams read a native word
  delta. This is a stream-conversion obligation, not a bit-field one.
- **Message header wire order.** `byte_swap_message_header` swaps
  unconditionally, so a big-endian host puts the header on the wire
  little-endian. This is tracked by the packet/caller records and is
  unchanged here.
- **Message encryption in use.** It has no caller, and no exchange with a
  real peer was executed.
- **Unchanged from the previous record.** Concurrent atomics and
  out-of-range float-to-integer conversions.
- **Script cells and saves.** The HS cell upper-bit/save policy and its
  executed consumers remain open (HWI-006/HWI-024).
