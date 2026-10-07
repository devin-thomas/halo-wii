# Actual scalar subset: standalone Wii execution

The source-faithful scalar subset executed all 128 checks in stock Dolphin 2609.
Sixteen checks failed: scalar and integer-dispatch wire bytes disagree with the
independent x86 golden record at every offset 0-7. The other 112 predicates pass.
The host run passes all 128. This measures an endian incompatibility; it does
not qualify the overall engine or clean Dolphin shutdown.

## Reproducible inputs and build

Source commit: `d1d84540f314e41f03b8b2acb5c142c79085141c`.
Build ID: `5b8f8ab099f896d1`.
DOL SHA-256: `bf84725b88f55e307ba5caa0731cb66f86ec20ed8dd93d82b67f0baba332d3d5`.
See the [Wii build manifest](2026-10-07-scalar-wii-build.json),
[host manifest](2026-10-07-scalar-wii-host.json) and
[runtime report](2026-10-07-scalar-wii-runtime.json).

From the repository root, using the recorded official SDK installation:

```powershell
python tools/wii/run_memory_subset.py --cc C:/msys64/ucrt64/bin/gcc.exe
python tools/wii/run_memory_subset.py --cc C:/dev/resources/toolchains/devkitpro/devkitPPC/bin/powerpc-eabi-gcc.exe --wii-devkitpro C:/dev/resources/toolchains/devkitpro --output build/wii-memory-subset
```

The runner retains the 11 extracted engine function bodies, actual memory
headers, independent golden inputs and documented authored service shims from
the [original subset](2026-10-07-scalar-subset.md). Only MSVC `ui64` literal
suffix spelling becomes standard `ULL` in ignored generated source. Host/PPC
function, macro and generated-source hashes agree. No production engine
function is rewritten or integrated by this diagnostic.

The harness now accepts a report stream. Native execution stops at a failure;
the Wii diagnostic collects failures across the independent valid inputs and
flushes wire-byte and failure records. It writes matching BEGIN/END build IDs
to `sd:/halo-wii-memory/subset.log`, checks report I/O, waits 60 video frames
and returns the fixture result. This executable is separate from the original
screen/input probe and does not load Halo assets.

Official devkitPPC GCC 16.1 compiled and linked with warnings as errors and no
diagnostics. Verified ELF32 big-endian PowerPC entry is `0x80003f00`; DOL load
sections and entry are checked. Host GCC 16.2 retains the five existing LLP64
negative-sizeof conversion warnings through its narrow host-only exception.

Independent review identified ambient GCC search overrides as a provenance
risk. The runner now clears those overrides only in its child environment,
checks the compiler against the SDK inventory and records the shared full SDK
fingerprint. An adversarial host run with a poisoned include and bogus tool
search overrides still passes all 128 checks. Three toolchain preflight tests,
the configure help path and exact old/new SDK-input enumeration equality pass.
Follow-up review found no remaining actionable defects.

## Executed stock route

Installed stock Dolphin 2609 executable SHA-256:
`1ed5e3f63f08b672133dade2c570e87ea4fa7fac2f6a65a118cc300a18dc0dac`.
The bounded launch used the existing isolated profile:

```text
Dolphin.exe --user <isolated-profile> --batch --video_backend D3D --exec memory_subset.dol
```

No profile settings changed: dual core, cheats, overclock and RAM overrides
remain off, with normal memory speed. The owned process exited after 5.584
seconds without a timeout or forced cleanup. Its host exit is unsigned
`3221226505`, signed `-1073740791`, hex `0xc0000409`: the separate known shutdown
failure persists. This is not a clean runtime pass.

Read-only SD extraction found matching BEGIN/END build `5b8f8ab099f896d1` and
the full 128-check summary. Guest transcript SHA-256 (UTF-8, LF separators and
one trailing LF):
`ceeb0fb9f209fa3fddb89c8cd4d5aefe8b47846472713bf379789157504a59eb`.
The launched DOL hash matches the clean-source build manifest.

| Measurement | Host | PPC guest |
|---|---|---|
| Executed checks | 128 | 128 |
| Failed checks | 0 | 16 |
| Wire bytes | `5a1234123456781122334455667788` | `5a3412785634128877665544332211` |

`wire_big_endian_golden` and `integer_dispatch_wire` fail at each offset 0-7.
The guest ends with:

```text
MEMORY SUBSET checks=128 offsets=8 failures=16 golden=5a1234123456781122334455667788
END MEMORY build=5b8f8ab099f896d1 result=1
```

Native copy followed by unconditional reversal produces the opposite wire
order on the big-endian target. Matching decode reversal allows round trips
to pass while the external bytes are wrong. Other tested predicates cover
round trips, cursor state, native mutation, canaries, truncated reads/writes
and sticky overflow; their full-buffer checks pass in this execution.

## Remaining boundary work

The next bounded candidate is an explicit serializer wire-order and
alignment-safe access path measured against the preserved original x86 golden
reference. Removing generic byte swaps globally would alter other boundaries
and is not justified by this result.

Original typed unaligned accesses remain in the extracted functions. Passing
emulation predicates does not establish defined C alignment behavior or
physical Wii acceptance. HS/save policies, packet/cache layouts, callbacks,
intrinsics/atomics and gameplay math require separate proofs. HWI-005 and
HWI-006 remain open; no Halo gameplay or physical Wii pass is implied.
