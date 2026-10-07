# Bounded stock Dolphin shutdown diagnosis

Result: **blocked**. This extends the [guest/runtime record](2026-10-07-dolphin-probe.md),
without changing its acceptance result. The installed stock Dolphin2609 executable
SHA256 remains `1ed5e3f63f08b672133dade2c570e87ea4fa7fac2f6a65a118cc300a18dc0dac`.
The isolated profile, normal memory/speed and disabled cheats/overclock remain
the same. Analytics was disabled in that profile before the visible GUI checks.
No existing user profile, NAND, security setting or SDK was changed.

## Executed comparisons

| Scenario | Observation | Acceptance limit |
|---|---|---|
| Unmodified official libogc application example, non-batch D3D GUI | A first window-close request stopped rendering/emulation; the process remained in Qt's game-list event loop. GDB attachment showed `QEventDispatcherWin32`, `QEventLoop::exec`, `QCoreApplication::exec`. | Emulation stop observed; application termination was not tested. The exact owned process was stopped after bounded inspection. |
| Authored 17-check probe, non-batch D3D GUI | After guest auto-exit, a window-close request terminated the process with `-1073740791` / `0xc0000409`. | Normal application close does not clear the runtime gate. |
| Same authored probe, batch D3D under installed GDB11.2 | Main thread stopped at `ucrtbase!terminate`; its callers include `_execute_onexit_table`. Two corrected traces caught this class. | Narrows the failure to static/CRT exit handling; exact object remains unidentified. |

Probe input identity: source `87a6c1b2a812460a262713069c32c49253fb89e8`,
build ID `ef3cae24af674f90`, DOL SHA256
`5107247a7210f5ba407bf83e471e56823bc0b028def5c2ebb85f02ad083c87b6`.
Official example DOL SHA256
`8a808fde0cb4fcefe58fdb0b2dabc3187ea8c81713f2df3c6e1333ef0da52270`.
This example scans Wii Remote Home; it does not scan GameCube Start. A Start
movie therefore cannot establish the official example's exit behavior.

The initial debugger launch stopped on a first-chance CPU/JIT access fault.
That stop was not treated as the shutdown cause. Corrected commands passed
`SIGSEGV` through to Dolphin and set pending breakpoints on `ucrtbase!abort`,
`ucrtbase!_invoke_watson` and `ucrtbase!terminate`. GDB auto-loading was disabled,
pagination was disabled and frame arguments were suppressed. All-thread stacks
were bounded to 12 frames. Debugger quit terminated only its owned inferior.
No diagnostic Dolphin or GDB process remains.

Sanitized executed argument vector (local paths substituted):

```text
gdb -batch -nx -q -ix <early.gdb> -x <crash.gdb> --args Dolphin.exe --user <isolated-profile> --batch --video_backend D3D --exec probe.dol
```

Raw traces stay outside Git. Trace SHA256:

- First corrected fatal trace: `ecbd9d7f163d8ff1a97aa3354194d1c04c2a3314b7ffc03ff0d9e7136553f635`.
- Trace with disassembly/module ranges: `5f47e66cd7b401fcfd2cecf1669a4629f7dd76227b00b0ea86f9bbea8bcde7df`.
- Official example idle GUI trace: `4167f9b720d18a52669a925429a81f9c2dafb5a09b1f029525c86625d766bfa1`.

## Portable call-site evidence

The loaded executable base was determined from its PE sections. The main-thread
return address after the terminate call is executable RVA `0x00f8e5c0`;
the enclosing exit callback starts near RVA `0x00f8e570`. An offline disassembly
of this exact executable, at preferred base `0x140000000`, confirms:

```text
140f8e5b1: cmpl $0x0,0xfc1b90(%rip)  # global preferred VA 0x141f50148
140f8e5b8: je 0x140f8e5c1
140f8e5ba: call *0x9690(%rip)          # import preferred VA 0x140f97c50
140f8e5c0: int3
140f8e5c1: add $0x28,%rsp
140f8e5c5: ret
```

Executed read-only inspection:

```text
objdump -d --start-address=0x140f8e570 --stop-address=0x140f8e5d0 Dolphin.exe
```

The stripped executable's nearest `hid_write` label is not a symbol for this
callback. Other nearest exported labels in the backtrace likewise do not identify
the actual private function. A still-joinable static thread fits the observed
termination class, but this is an **inference**, not an identified owner.
A surviving thread named resource-worker also does not prove ownership.

Source inspection of stock2609's
[CustomResourceManager](https://github.com/dolphin-emu/dolphin/blob/2609/Source/Core/VideoCommon/Resources/CustomResourceManager.cpp)
and [WorkQueueThread](https://github.com/dolphin-emu/dolphin/blob/2609/Source/Core/Common/WorkQueueThread.h)
does not resolve that identity: resource shutdown resets a worker, while its
queue destructor also shuts down/joins. No source patch or stock binary change
was made on that hypothesis.

## Remaining recovery

Obtain matching symbols/map for this exact stock executable, or use a separately
authorized symbol-bearing build of the same revision. Resolve callback
RVA `0x00f8e570` and its global at RVA `0x01f50148`, then inspect that owner's
shutdown ordering. A real application close of the unmodified official example
is still needed to separate probe-specific behavior from a shared host issue.
Do not repeat the completed backend/profile matrix without new evidence.

The combined source-fetch and second official-example GUI launch was **not
executed**: automatic approval review returned `blocked by policy`, with no
detailed reason. The earlier development2609-69 comparison also remains
unexecuted. Neither rejected launch was retried through a workaround.
Read-only stock source inspection continued separately.

Guest clean return, emulator stop and host process termination remain distinct.
Physical Wii and Halo gameplay are untested. The expanded 24-check fixture was
compiled afterward and was not run in Dolphin during this diagnosis.
