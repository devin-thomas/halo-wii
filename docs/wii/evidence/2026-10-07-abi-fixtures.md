# Synthetic C boundary results

Source `87a6c1b2a812460a262713069c32c49253fb89e8`, clean input tree. PPC build ID
`ef3cae24af674f90`; [build manifest](2026-10-07-abi-build.json) and
[host compiler/report](2026-10-07-abi-host.json) retain exact hashes and flags.
This is independent preparatory work while HWI-005 host teardown is blocked.

```powershell
python tools/wii/run_abi_host.py --cc C:/msys64/ucrt64/bin/gcc.exe
python configure.py --wii --wii-devkitpro C:/dev/resources/toolchains/devkitpro --wii-probe-frames 300
ninja wii_probe
```

The helper supplies the selected host compiler's DLL path only to its child
processes. Initial direct host GCC invocation failed without that runtime DLL
path; the helper resolved it and executed the compiled fixture with exit0.
PPC ran in the stock isolated Dolphin profile from the separate runtime record.
Its emulated SD report was read after the process exited. Running the host helper
with `--compare .local/abi-ppc.txt` passed the exact canonical/arithmetic comparison.

PPC output:

```text
ABI CANONICAL checks=17 address=803a6010 offset=16 float=3fc00000 layout=0,4,8/12 varargs=ok datum=abcd,1234
ABI ARITHMETIC separate=00000000 sum1000=43a6aac4
ABI TARGET pointer_bytes=4 int_bytes=4 double_bytes=8
```

The first two records match host execution. Host pointer_bytes=8; PPC=4;
int_bytes=4 and double_bytes=8 on both. Seventeen checks cover explicit unaligned
little-endian decoding, binary32 bit conversion, Xbox integer address/range
translation, array counts, truncated/overflowing inputs, field offsets, indirect
function/struct return ABI, varargs promotions, signed zero and datum halves.
No native struct file casts, blanket packing or blanket byte swapping were added.
Invalid spans return false to checked callers; fixture failures name the case.

Measured arithmetic: separate multiply/subtract yields binary32 `00000000` and
1000 additions of binary32(1/3) yield `43a6aac4` with contraction disabled. These
two scalar fixtures are not evidence of engine-wide numerical equivalence.
Next work must audit actual engine intrinsics/calling boundaries, unaligned uses,
math-library/transcendental behavior and integration assumptions before HWI-006
closes. The guest record ends `abi=0`; Dolphin still aborts during host teardown.
HWI-006 remains incomplete, and physical Wii is untested.

Host evidence helper refinement `ac4aa3f9` invalidates old successful evidence before rerunning. An intentional mismatch rejected with exit2 and comparison_failed state, followed by a valid comparison pass. Host and PPC use the same C fixture input hashes; the host record additionally identifies this newer evidence helper revision.
