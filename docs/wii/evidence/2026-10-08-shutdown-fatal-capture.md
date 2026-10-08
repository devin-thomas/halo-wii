# Original probe fatal shutdown chronology

One instrumented stock/original-probe run reaches the onexit callback, fatal
comparison and verified terminate CALL site with the watched word still 8,724.
Windows independently reports natural process completion with status
`0xc0000409`; no forced cleanup occurs. This is a fatal exit, not a successful
host shutdown. [Numeric evidence](2026-10-08-shutdown-fatal-capture.json)

The early debugger file disables auto-load before symbols. Live signal policy,
ASLR/sections, six original instruction prefixes, committed writable image page,
zero word/guard and four-byte hardware watch placement qualify. SIGSEGV uses
nostop/noprint/PASS: delivery is preserved, but individual occurrences are not
logged or classified as handled, benign or JIT. SIGABRT/SIGILL use stop/print/PASS.

One loader setup stop is separate from eight structured snapshots and a terminal
exit notification. The approved bound is one setup plus at most 64 snapshots,
120 seconds total and 15 seconds per stopped phase. The run takes 16.391 seconds.
Entry sees zero; startup reset/accessor entries at 4.264/4.324 seconds still see
zero. The hardware watch changes from zero to 8,724 at 4.386 seconds, post-PC
RVA `0x8d15e2`, matching the previously bounded store instruction. The callback
at `0xf8e570`, fatal comparison at `0xf8e5b1` and terminate CALL at `0xf8e5ba`
follow at 14.237/14.319/14.384 seconds with that same word. No shutdown reset-entry
or join-helper return snapshot occurs. This trace does not prove all-thread
hardware coverage, all writers, a final shutdown writer or source ownership.

At 14.396 seconds GDB reports an unknown signal named `?` at Windows abort.
The saved stack includes terminate, the main-image return at `0xf8e5c0` and
Windows onexit-table frames. The signal is not relabeled SIGABRT. The retained
read-only owned-process handle confirms `0xc0000409` after completion; GDB exits
zero. Nearest hid export labels do not establish the source owner.

The guest appends one 254-byte report with END at 116 frames/57 ticks and
connected/activity/storage all one; sentinel advances from two to three. Prior
log bytes and all other SD files remain. Fresh stopped SD and all existing
profile files have verified byte backups; exact inputs and three required
configs remain unchanged. Dolphin automatically moves sixteen WiiSession files
to WiiSession.backup and updates TimePlayed.ini: 41 files remain, with sixteen
added paths, sixteen removed paths, one modified file and twenty-four other
prior files unchanged. Those automatic changes remain, without manual
restoration or deletion.

The controller returns Python exit one with the retained failure
`Unexpected SD file change`: FAT inventory names use uppercase PROBE.LOG and
SENTINEL.TXT while its exact allowlist uses lowercase. Independent saved-only
review checks the two expected paths by casefold with no name collisions,
unchanged other files, log prefix and single sentinel increment. This narrowly
qualifies preservation; it does not erase the failed controller result, patch
its source or repeat the run.

Independent saved chronology and preservation reviews pass 119 and 218 checks.
These checks validate retained evidence, not additional target executions.

The [earlier startup capture](2026-10-08-shutdown-startup-capture.md) and setup
failure remain. HWI-005/006 remain BLOCKED. This diagnostic does not qualify
original unmodified cold success, ABI, native geometry, GX, gameplay or physical
Wii acceptance. No further target launch is part of this gate.
