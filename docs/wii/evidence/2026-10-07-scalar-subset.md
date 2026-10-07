# Actual scalar encoder/decoder subset

Source `6b3701b0` adds an isolated harness for 11 actual engine functions from
`source/memory/data_encoding.c` and `source/memory/byte_swapping.c`. This is
independent preparation while clean Dolphin shutdown remains blocked.

The runner extracts the named function definitions and the source SWAP8 macro,
uses the actual memory headers, and records source/function/macro hashes. It
changes only MSVC `ui64` literal suffix spelling to standard C `ULL` in the
generated translation unit. Original engine source is not edited. Generated
source, objects and executables remain ignored local artifacts.

The authored shim retains unsigned-byte boolean, 16-bit short and 32-bit long
types with static assertions. Its memory services use libc memcpy/memset;
assertions terminate immediately. Those services omit engine service assertions,
and engine release assertions may continue. Current valid fixture inputs avoid
those differences; this is not a complete engine/platform build.

Executed on a clean committed subset input tree:

```powershell
python tools/wii/run_memory_subset.py --cc C:/msys64/ucrt64/bin/gcc.exe
python tools/wii/run_memory_subset.py --cc C:/dev/resources/toolchains/devkitpro/devkitPPC/bin/powerpc-eabi-gcc.exe --compile-only --output .local/wii-memory-ppc
```

Host GCC 16.2 executed with exit 0:

```text
MEMORY SUBSET checks=128 offsets=8 wire=5a1234123456781122334455667788 mutation=native
```

Sixteen assertions at each offset0-7 cover actual byte/short/long/int64 scalar
encoding and decoding, integer-width dispatch, exact wire bytes, cursor/overflow
state, native byte mutation and neighboring canaries. Truncated read/write and
sticky-overflow cases compare the entire 40-byte storage snapshot, not just the
first byte. Golden wire bytes are independent of round-trip results: x86
encoding of 5a/1234/12345678/1122334455667788 is the big-endian record above.

The first host attempt with all warnings as errors failed on five existing
negative-sizeof selectors: this LLP64 compiler has 64-bit size_t and 32-bit long.
For this host only, `-Wno-error=overflow` retains those warnings and permits the
compiler's documented conversion to -2/-4/-8; no source rewrite hides them.
Other warnings remain errors. The [host manifest](2026-10-07-scalar-subset-host.json)
retains all five compiler diagnostics, flags and actual results.

Official devkitPPC GCC 16.1 compiled both generated engine and harness objects
with warnings as errors and no diagnostics. Their headers are checked for
ELF32, big endian and PowerPC machine identity. See the
[PPC object manifest](2026-10-07-scalar-subset-ppc.json). Function/input hashes
match the host run, and generated translation-unit hashes agree.
Independent review found no actionable defects; its suggested full-buffer
truncation snapshots were incorporated before the recorded runs.

## Limits and next proof

The subset has **not executed on PPC**. Existing typed unaligned accesses and
unconditional byte reversal remain source-faithful. Host success does not prove
defined alignment behavior, and target object compilation does not prove wire
agreement. On a big-endian target, unconditional reversal after a native copy
is expected to disagree with the x86 golden bytes; that is a source-backed
prediction, not an observed PPC result. Test the actual target path before
selecting explicit endian/alignment boundary changes.

Selected state buffer arithmetic uses actual byte pointers. Wider engine GNU
void-pointer arithmetic, packets/structure arrays, saves, cache relocation,
intrinsics, atomics and gameplay math remain outside this subset. HWI-005 and
HWI-006 remain open; no Halo data, gameplay or physical Wii pass is implied.
