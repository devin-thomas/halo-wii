# Concurrent Dolphin lanes (HWI-073)

The Dolphin runner now owns the Dolphin it launched by PID, and it can run a
lane beside other lanes. It no longer needs the whole host.

Proof runs used the same DOL and input in 2 lanes on Titan and 3 lanes on
Research. In every run, guest output was byte-identical to exclusive runs on
both hosts. Every teardown check named only the lane's own process tree. A
forced stop in one lane left the other lanes running and passing.

Throughput rose 1.96× on Titan and 2.91× on Research. The Dolphin time of each
run grew by only 1.6 % and 2.8 %.

[Numeric evidence](2026-10-11-concurrent-dolphin-lanes.json)

## What changed

- `tools/wii/run_dolphin.py` (commit `1751aa76`):
  - Ownership is PID-scoped. The runner tracks the Dolphin it launched and the
    descendants it observes. On Windows it reads a Toolhelp process snapshot;
    on macOS it reads `ps`. Either is polled every 2 s.
  - The teardown check looks only for that tree.
  - A forced stop ends the root through its own process handle, then each
    observed descendant by explicit PID. The runner refuses to stop any PID
    outside the tree.
  - Exclusive mode remains the default. `--allow-concurrent` runs a lane
    beside others.
  - Each run records `host_shared`, `teardown_check`, `own_pid`,
    `own_tree_observed`, `own_tree_alive_after`, and the number of other
    Dolphin processes seen at start, at most and at end.
  - `dolphin_processes_after` keeps its old host-wide meaning.
- OS stability stays host-wide. It is marked `scope: host_wide` with
  `shared_with_concurrent_lanes`. Fault events that carry a process ID are
  attributed `own` or `other`; the rest are `unattributed`.
- The rules are in
  [BUILDING.md](../BUILDING.md#concurrent-dolphin-lanes-hwi-073), including
  the honest-timing rule: a shared-host timing is never compared with an
  exclusive one.

## Method

- **Guest:** `gx_materials.dol`, SHA-256 `194c63ae…1e19`, build
  `6958875983abf052`, source `4a4aa208`.
  - Scenario `gx_materials`, with `--efb-access`.
  - Authored DTM from `gx-materials.json`, movie SHA-256 `cd06b35c…befc`.
  - Two cold launches per lane, each lane in its own new stock-limit profile.
  - No frame dumps.
- **Titan:** Windows 11 Pro 10.0.26200, i7-12700 (20 logical CPUs), RTX
  4090, D3D. Dolphin.exe `1ed5e3f6…0dac`.
- **Research:** macOS 27.0 on Apple M5 Max (18 cores), Vulkan. Dolphin 2609
  `b3c369bc…3ccf`.
- **Batches:** each was one acquisition of the live single-slot lock as owner
  `hwi-073`, so no other agent's Dolphin shared the host. The lock was
  released after every batch, with a 30 s gap for queued agents.
  - **Exclusive:** the lanes ran one after another in default mode.
  - **Concurrent:** the lanes started together with `--allow-concurrent`.
    Each lane took a slot of a `.local` copy of the new counted-slot lock
    through `locked_run_v2.sh`. Each record carries its `lock_slot`.
  - **Forced:** the same as concurrent, but the last lane had `--timeout 6`.
- **Wall time:** batch start to the last lane's exit. It includes each lane's
  profile setup and its 10 s event-log settle.

## Guest identity

The runner reads back the guest log (`halo-wii-gxm/materials.log`) and the
run counter after every launch. Across all 26 completed runs (both hosts, all
batches, every lane), each launch number produced exactly one set of bytes:

| Launch | Guest log SHA-256 | Counter SHA-256 | Runs |
|---|---|---|---|
| 1 | `b66f7922…6e0c` | `899715aa…5787` | 13 |
| 2 | `2d5b5cbe…6318` | `773b5336…e01d` | 13 |

Every run passed its guest and persistence checks:

- 49 EFB checks × 4 load cycles, with cycle CRC `53843a35`.
- 17 of 17 deliberate invalid cases detected.
- Zero heap growth.
- The counter advanced across cold processes, and the prior log was
  preserved.

The guest log includes the guest's own microsecond timings. Those are
identical too.

The two force-stopped launches were excluded on purpose: they never wrote a
log.

## Teardown checks and forced stops

| Batch | Runs | `teardown_check` | Other Dolphins seen (start, max, end) |
|---|---|---|---|
| Titan exclusive | 4 | all `own_process_tree_exited` | 0, 0, 0 every run |
| Titan concurrent | 4 | all `own_process_tree_exited` | max 1 every run; `host_shared: true` |
| Titan forced | 3 | all `own_process_tree_exited` | see below |
| Research exclusive | 6 | all `own_process_tree_exited` | 0, 0, 0 every run |
| Research concurrent | 6 | all `own_process_tree_exited` | max 2 every run; `host_shared: true` |
| Research forced | 5 | all `own_process_tree_exited` | see below |

**Process trees.** In every run Dolphin's observed tree was the Dolphin
process alone (`own_tree_observed` has one PID). No run left a process of
its own behind.

**Host-wide count.** In concurrent runs `dolphin_processes_after` was 1 or 2
when another lane was still running. The PID-scoped check still passed. The
old host-wide check would have failed such a run, or a runner that killed by
image name would have stopped the other lane.

**Titan forced batch.** Lane 2 hit its 6 s timeout and was stopped:

- outcome `forced_stop_timeout`, exit `0x1`, `forced_stop_scope:
  own_process_tree`;
- its teardown check passed with `dolphin_processes_after: 1`.

Lane 1's Dolphin was running at that moment. It was not touched: lane 1 went
on to pass both runs with the bytes above.

**Research forced batch.** Lane 3 was stopped at 6 s by SIGKILL (exit
`0xfffffff7`). `dolphin_processes_after` was 2: lanes 1 and 2 were both
running. Both passed both runs.

As before, a forced stop is a guest and persistence failure for that lane
(`forced_stop_before_natural_exit`). It is never accepted as completion.

**OS stability.** No instability events were observed in any batch window on
either host, and no Dolphin fault events occurred. Concurrent records carry
`shared_with_concurrent_lanes: true`.

## Throughput

| Host | Mode | Runs | Wall (s) | s per run | Runs per hour | Dolphin time per run, mean (range) |
|---|---|---|---|---|---|---|
| Titan | exclusive, 2 lanes in sequence | 4 | 86.0 | 21.5 | 167.4 | 15.63 s (15.20–16.59) |
| Titan | 2 concurrent lanes | 4 | 43.9 | 11.0 | 328.3 | 15.88 s (15.33–16.19) |
| Research | exclusive, 3 lanes in sequence | 6 | 106.6 | 17.8 | 202.7 | 12.16 s (12.08–12.39) |
| Research | 3 concurrent lanes | 6 | 36.6 | 6.1 | 589.7 | 12.50 s (12.12–13.15) |

Each concurrent Dolphin run took 1.016× (Titan) and 1.028× (Research) its
exclusive time. Per the honest-timing rule, the concurrent per-run times are
shared-host timings. They are listed here only to measure the cost of sharing
the host. They are not exclusive measurements of this DOL.

**Limits.**

- This DOL runs for 12–16 s. The fixed per-lane overhead (profile, 10 s
  event-log settle) is a large share of the exclusive wall time, so the batch
  ratios mix overlapped overhead with overlapped emulation.
- For long runs the gain approaches the lane count only if Dolphin's per-run
  time holds at that lane count. That was shown here only for this light
  DOL.
- a10 uses 2–3 cores per Dolphin. Its concurrency was **not measured**: the
  optional a10-length test was skipped because the live lanes were in use by
  HWI-030, HWI-033 and HWI-034.

## Counted-slot lock (prepared, not deployed)

The coordinator lock is replaced by counted slots, but has not been deployed.

- **Slots:** `dolphin.lock` stays slot 0, in the same location and with the
  same `owner.txt` first line. Further slots are `dolphin.lock.1`,
  `dolphin.lock.2` and so on.
- **Slot count:** `DOLPHIN_LOCK_SLOTS` or `dolphin_lock.conf`; the default is
  1.
- **Queue:** waiters queue oldest first, using tickets that a dead waiter
  cannot block.
- **`status`:** lists the holders. A dead holder PID is reported as possibly
  stale and is never released automatically.

Its 17 unit tests pass on Windows (Python 3.14, 3.11) and on Research (3.12).
They include:

- six real processes racing for 2 slots, never more than 2 holders;
- a lock held by the v1 script staying valid;
- v1's own release still working on a slot 0 written by the new lock.

The live lock files are unchanged. The proof batches used a `.local` copy.

Physical Wii: untested.
