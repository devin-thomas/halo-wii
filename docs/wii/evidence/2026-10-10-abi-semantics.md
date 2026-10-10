# Calling, bit-field, atomic, FP-environment and math semantics on PPC (HWI-006)

This record covers the HWI-006 obligations "Calling/varargs/intrinsics/atomics" and
"Gameplay math", plus the char/bit-field assumptions the source audit
([2026-10-07 engine ABI audit](2026-10-07-engine-abi-audit.md)) left open. It
executes actual engine code on the PPC target in stock Wii-mode Dolphin and
compares it with the host. [Numeric evidence](2026-10-10-abi-semantics.json)

## What runs

`tools/wii/run_abi_semantics.py` generates two units from the source tree,
copying declarations and bodies verbatim (58 extracted pieces, each hashed in
the JSON):

- `cseries.h` types, enums, `PIN`/`FLAG` macros and `fast_ftol`;
- the actual bit-field types `animation_event_header` and
  `vector_char_difference_data` (recorded_animation_playback.c),
  `message_header_value` (message_encryption.c), the `bsp_switch_state` union
  (players.h) and `hud_nav_point_datum` (hud_nav_points.c);
- `build_message_header`, `byte_swap_message_header`, `reversible_crypt`,
  `tea_encipher`/`tea_decipher` and `message_encrypt`/`message_decrypt`;
- the five `halo_linux_Interlocked*` bodies (port/linux/src/xbox_kernel.c);
- the actual `real_math.h`, 7 `real_math.c`, 6 `matrix_math.c` and 4
  `random_math.c` bodies.

Only service macros are authored (`engine_shim.h`): counted assertions, MSVC
`__inline` linkage, the Win32 `LONG`/`WINAPI` spellings and one newlib
`#undef log2`. The generated units are byte-identical for both targets. The
authored harness (`port/wii/abi/semantics/`) adds the following:

- a separate callee translation unit, for varargs, by-value aggregates, a
  23-argument call and callbacks in both directions;
- 64-bit integer, conversion, subnormal and formatting checks;
- the shared musl maths functions (`halo_sin` and the other 11);
- explicit bit-field decoders, which are candidate boundary helpers.

Every unit builds with `-std=c11 -O2 -Wall -Wextra -Werror -ffp-contract=off`.
Engine units add the other ports' `-fno-strict-aliasing -fwrapv
-fno-delete-null-pointer-checks`. The generated engine unit has two recorded
per-unit exceptions, `-Wno-type-limits` and `-Wno-unused-parameter`. Unchanged
musl uses `-w`, as on every port.

The engine probe is built twice: as configured, and with `-fsigned-char` as a
candidate. The whole run then repeats in a second pass with IEEE subnormals
enabled (PPC `FPSCR[NI]` cleared). In that pass every contract check is
recorded as a candidate.

## Commands and identities (clean commit `76ca89e1`)

```text
python tools/wii/run_abi_semantics.py --cc <msys2 ucrt64 gcc.exe> --output .local/abi-sem-final-host
python tools/wii/run_abi_semantics.py --cc <devkitPPC powerpc-eabi-gcc.exe> --wii-devkitpro <devkitPro> --output .local/abi-sem-final-ppc
python run_fixture.py <abi_semantics.dol> <new dir> halo-wii-memory/abi-semantics.log 300   (twice, Dolphin lock held)
python tools/wii/compare_abi_semantics.py <host-report.txt> <guest-report.txt> --json <out>
```

| | Host | PPC |
|---|---|---|
| Compiler | MSYS2 GCC 16.2.0 (x86_64-w64-mingw32) | devkitPPC GCC 16.1.0 |
| Build ID | `15a77af5b249e739` | `4d814d554d228d4b` |
| Binary SHA-256 | exe `4b4d90eb…e6e9` | DOL `3c3554ab52efc46762eadbe24984de9d027513d2c9ccc833f697498cc16f7054` |
| Result | 397 checks, 0 failures, exit 0 | 397 checks, 41 failures (8 contract, 33 engine assumption, 0 candidate) |
| Run | — | Two runs: host exit 0x0, not forced (5.3 s and 4.8 s); identical report SHA-256 `edb2a404…a98e` |

Comparison: 554 ids, 483 identical, 41 CHECK, 5 OBS and 20 MATH differences.
The pointer-valued ids are excluded as target-specific.

## Results

### Proven equal on host and PPC

Counts are pass-1 checks. All pass on both targets, except the subnormal
cases listed in Finding 1, which pass in the IEEE pass.

| Area | Checks |
|---|---|
| Varargs: the render_debug pattern (pointers, `short`/`boolean` read as `int`, `real` read as `double`) for ±0, max, Inf and a quiet NaN payload; a 20-argument mixed `int`/`long`/`long long`/`double`/pointer list that spills to the stack; `va_copy` | 40 (2 subnormal) |
| By-value 12-, 16-, 4-, 3- and 52-byte aggregates and a padded mixed struct. A 23-argument call (10 int, 10 float/double, 3 `long long`, a struct) echoed bit-exact | 32 |
| Callbacks: hs_typecasting-shaped `long(*)(long)` table, create_thread-shaped entry, float callbacks in both directions, libc `qsort`/`bsearch` calling back | 8 |
| `__sync` interlocked bodies: increment/decrement/exchange/exchange-add/CAS hit and miss return contracts, `0x7fffffff` wrap, 32-bit LONG | 13 |
| 64-bit divide/remainder/multiply/shift (PPC libgcc helpers); `-fwrapv` long overflow; arithmetic right shift of negatives | 9 |
| Subnormal arithmetic, `sqrt`/`sqrtf` (newlib software sqrt on the 750), `floor`/`ceil` signed zero, divide by zero | 14 (4 subnormal) |
| Conversions: int→float ties, u32→float, float→u32, int64→double, double→float ties to even, subnormals and overflow | 11 (2 subnormal) |
| `fast_ftol` (`__builtin_rint`) half-to-even at ±0.5/1.5/2.5, 2^23-0.5, ±2^31 edges | 17 |
| TEA reference vector `41ea3a0a 94baa940` through the actual `tea_encipher`/`tea_decipher`; `reversible_crypt` vector | 4 |
| `vsnprintf` integer/string/float formatting, truncation through a variadic function pointer | 5 |
| Explicit bit-field helpers: valid kinds, every truncation and range reject with untouched output, exhaustive 256×4 animation-header, 65,536 message-word and 256 switch-byte round trips | 27 |

The engine maths is bit-identical in both passes. All 60 engine and gameplay
MATH lines match:

- vectors: `magnitude3d`, `normalize3d`, dot, cross, `perpendicular3d`,
  `rotate_vector_about_axis`, `angle_between_vectors3d`;
- quaternions: multiply, interpolate, normalize, transform;
- matrices: from/to quaternion, multiply, inverse, transform point/vector;
- scalars: the trigonometric inlines, `arctangent`, `signed_angular_difference`,
  `square_root` and `fast_ftol` (2,048 cases);
- the random generators over 65,536 draws each.

Each vector, quaternion and matrix routine runs 512 cases. The gameplay
replicas also match:

- `game_time_update` accumulation: 20,000 frames for each of 60 Hz, 59.94 Hz and
  jittered frame times;
- the spawn rating `pow(random, 0.5)` and best-pick sequence: 4,096 rounds;
- the vehicle flip `cos` threshold.

### Finding 1: libogc starts with FPSCR[NI] set (subnormals flushed)

`FPSCR` at `main` is `0x00000004`, so non-IEEE mode is on. Subnormal results
become zero. Subnormal operands with normal results are kept: both
`input_scaled_to_normal` checks pass. This produces all 8 contract failures:

- double→float to 2^-149 and 0.75×2^-149;
- `FLT_MIN/2`, `2^-149×2` and `3×2^-149×0.5`, and `DBL_MIN/2`;
- a subnormal `real` passed through varargs, where the narrowing `(float)`
  flushes it.

With NI set, 19 of the 36 musl MATH lines differ (asin, atan, atan2, exp, pow,
sin, tan). In 18 of them the classification counts match flushing exactly: the
PPC's zero count rises by the number of subnormal results the host has. For
example, `exp` narrowed to `real` has 78 subnormal results on the host and 0
on the PPC, and its zero count rises from 148 to 226. `atan2` loses 26 the same
way. The 19th line, `atan.f64`, has equal counts on both targets; it is the
NaN-payload difference that remains in the IEEE pass. NI is the only change
between the passes, and the 18 lines become identical once it is cleared.

Clearing NI (candidate) makes all 151 checks pass on the PPC. 35 of 36 musl
lines then become identical. The remaining one is `atan.f64`, a NaN-payload
difference described under Finding 5; its NaN-canonical line matches.

The other native ports keep IEEE subnormals, and the port requires
bit-identical system-link simulation (`port/include/halo_math.h`). So the Wii
build must decide this explicitly. The measured candidate is to clear
`FPSCR[NI]` at startup.

### Finding 2: plain `char` is unsigned on PPC EABI

`CHAR_MIN` is 0 on the PPC. Every other port has a signed char: x86 natively,
and Android through the arm64_32 Darwin ABI. Measured effects on actual
declarations:

- `vector_char_difference_data.delta_yaw` from stream byte `f6` reads 246, not
  -10 (recorded-animation playback deltas).
- `players.h` `char local_player_triggered_switch : 4` stores NONE and reads
  back 15. The players.c test `!= _local_player_triggered_switch_none` is then
  always true, so the BSP-switch recursion timer runs every tick. This changes
  gameplay state.

The `-fsigned-char` candidate passes all 5 char checks on both targets. It
does not change bit-field allocation.

### Finding 3: bit-fields are allocated MSB-first on PPC

The same declarations give different bits on the PPC. Both the baseline and
the signed-char variant fail all 13 bit-field checks:

- `animation_event_header` from tag byte `0d` reads time_delta 0 and
  event_type 13, where the Xbox reads 1 and 3. Encoding (2, 5) gives `85`, not
  `16`. Recorded-animation streams authored on the Xbox mis-decode.
- `message_header_value` from LE wire `34 12` decodes type 3 and size 0x412,
  where the Xbox decodes type 1 and size 0x123. On the same target, a header
  built by `build_message_header` with shifts and masks reads back through the
  bit-fields as size 0x20E, type 0 and flags 0, not 0x20, 3 and 2.
- The actual `message_encrypt` therefore misreads a 20-byte message as 332
  bytes and rewrites 312 bytes past it. In this run they landed inside a
  canary-bounded buffer. `message_decrypt` reads flags 0, skips decryption and
  the round trip fails. No current caller of these two functions was found in
  `source/` or `port/`.
- The players `bsp_switch_state` byte for (2, 12) is `2c`, not `c2`. This is
  game-state layout. The `hud_nav_point_datum` unit bytes are `d500`, not
  `5d00`; that datum is runtime-only and its values read back correctly.

The explicit LSB-first decoders in `bitfield_boundary.c` are the candidate.
They give the Xbox values on both targets and reject truncated or out-of-range
input. They are not wired into the engine.

### Finding 4: `wchar_t` is 4 bytes on PPC

The engine assumes 2 bytes, for player profiles and UTF-16 strings. This is
already listed as an open gap; it is now measured.

### Finding 5: other measured differences (observations)

- Widening a signalling-NaN float: x86 quiets it (`7ff82468a…`), but PPC `lfs`
  keeps it signalling (`7ff02468a…`). This difference is the only one left in
  the IEEE pass (`atan` returns NaN inputs unchanged).
- `message_encrypt` ciphertext bytes differ, as expected from Findings 2 and 3
  plus the native-endian TEA words.
- The NaN generated by 0/0 reads `ffc00000` on both. On the PPC this is
  Dolphin's value. Real Gekko hardware is expected to generate a positive
  default NaN, so this is not hardware evidence.
- `vsnprintf` decimal halves and exponent spelling (`0.12|0|2|0.2|1.500000e+00`)
  are identical on UCRT and newlib.

### Compile-time finding

newlib's `<math.h>` defines `log2` as a function-like macro (`log(x)/_M_LN2`).
That macro collides with `port/include/halo_math.h` under `-Werror`. The Wii
game `math.h` must `#undef log2` before renaming it.

### HS cell representation (priority 3): source audit only

The typecasting writers store through the cell's first address:

- `*(boolean *)&result`: hs_runtime.c:1668, 1684, 2009, 2061, 2117;
- `*(short *)&result`: hs_runtime.c:1742, 1764.

Some readers instead convert the cell by value:

- `(short)value`: hs_runtime.c:1596, 1640-1651, 1885; hs_compile.c:1393;
- `(real)(short)arguments[n]`: hs_runtime.c:2050;
- `(boolean)value`: hs_runtime.c:1570.

On the PPC a first-address `short` store lands in bits 16-31, but a value read
takes bits 0-15. A short produced by `hs_real_to_short` and then compared by
the short inequality path, for example, reads indeterminate bytes. These
bodies read indeterminate stack bytes by design, so executing them would not
give a deterministic canonical result; they were not executed. The existing
24-check cell model remains the executed evidence. The
HS upper-bit/save policy is still open.

## Not established

- Real Gekko hardware: NI behaviour, NaN generation, denormal performance with
  NI clear. This is emulator evidence only.
- Concurrent atomics. Only single-thread return contracts were executed; no
  preemption inside a reservation was tested.
- Out-of-range float→integer conversions (undefined in C; x87 and `fctiwz`
  differ). These were not executed.
- Production use: no engine code was changed. The `-fsigned-char`, NI-clear
  and bit-field decoders remain candidates.
- Original Xbox/MSVC execution. References are authored from MSVC/x86
  semantics, and the host stands in for little-endian LSB-first behaviour.
- Executed HS cell consumers and the save policy.
