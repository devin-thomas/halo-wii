# Bounded MEM2 owner and raw UI tag streaming

Source `bc90b840f83dee35cea91cb27a801a413f09036a` passes the authored host and emulated PPC
owner/streaming diagnostic. Companion [JSON](2026-10-07-owned-cache-stream.json)
records hashes, compiler identity, static frames and exact reports. The prior
[address gate](2026-10-07-owned-cache-address.md), ABI gaps and failed shutdown
observations remain preserved. HWI-005/006 stay BLOCKED; HWI-007/008 stay TODO.

## Budget and lifetime

The provisional profile preserves source capacities: 22 MiB tags, current
20 MiB state, 4 MiB sound, 65,535 index records at 32 bytes, two 64 KiB IO buffers
and a provisional 2 MiB reserve. State and sound are canary placeholders; no
engine pools are initialized. All charges derive from the actual span address
and alignment. Data ends at 50,462,720 bytes and the contiguous reservation
charges 52,559,872. Alternating a second placement 4,096 bytes higher produces
a maximum arena charge of 52,563,968 in the 54,368,224-byte MEM2 span, leaving
1,804,256 bytes. Textures, actual engine metadata, nested conversion workspace
and full system/stack peaks are unresolved; this is not a final memory strategy.

The checked planner commits canonical plans atomically. An owner retains its
caller span and generation; handles belong to that originating persistent
owner control, whose lifetime must outlive all handle use. Views are discarded
before release/rebind. Immutable control and single-caller, nonconcurrent use
are preconditions. Failed stream reads can leave partial/full written backing
owned until explicit release; sticky errors require an explicit reset.

Sixteen load/decode/release cycles across two placements reject 15 stale
handles after rebind and 16 after release; final generation is 32. The full
reserve and state/sound edge canaries survive. MEM2 low/high bounds restore
exactly. Heap in-use samples are 783,272 before and after; heap arena grows
815,296 to 852,160, so all heap state, peak RAM and fragmentation are unqualified.
Compiler static frames exclude library and complete call-chain stack peaks.

A controlled test divides the owned span with two 64 KiB guard blocks. Its
54,237,152 aggregate free bytes exceed the request, while the largest of three
free intervals is 18,122,720; all three contiguous requests reject without
guard damage. This proves the stated interval constraint, not fragmentation
of the system allocator or an engine workload.

## Actual streaming and execution

An exclusive new private SD file was staged using Debian mtools 4.0.48-1 while
Dolphin was stopped. Read-back matches 1,642,244 bytes / SHA256
`a1d9e65575ce3617d91a4312fe4dcd2bfeb62634590f5f686540376e316b59f4`. All 12 existing files survive staging and
the runtime unchanged; the only added runtime file is this diagnostic's log.
Private game bytes, source paths and generated binaries remain outside Git.
The generated header contains numeric identity goldens and an SD path; the
streaming build embeds no private raw byte array.

Each actual read uses 26 alternating synchronous 64 KiB chunks and 27 fread
calls including EOF checking. Raw CRC `e22586e4` and table CRC `2e540905` match.
Three deliberate trailing/short/CRC inputs reject as expected. All 983 records
decode explicit little-endian words, preserve numeric metadata, resolve bounded
NUL names and one-byte root spans, and check the full scenario datum/group.
Lossless numeric reserialization matches the input table. Root sizes, nested
blocks, BSP bodies, resources and canonical Xbox checksums remain unvalidated.
No asynchronous IO, durable save or production loading is demonstrated.

Host and PPC each pass 1,916 address cases / 8,437 checks and, in these runs,
1,407 arena cases / 2,981 checks with zero failures/aborts. Arena fixture counts
can depend on actual alignment. The host stream fixture passes 60 cases /
958 checks; PPC explicitly skips that tmpfile fixture. Actual PPC SD tests cover
success and the three stated deliberate errors; runtime IO failure injection
is not performed. Eight strict C units compile without diagnostics per target.

PPC build `969c6457481c2908`, DOL SHA256
`56d3fe8ab9ef1b9b9d5e4e411f17e5aaa5cc858b1baaff1a1ce821b03f864787`. One stock Dolphin 2609 / D3D / existing single-core
profile run completes BEGIN/END result 0 and exits naturally 0 in
7.578 seconds, without timeout. Settings SHA256 remains unchanged.
Guest LF SHA256 `8d52d9da878934e1379f789eec69807adef3711af9b001a01608256657df253d`.
Earlier host teardown faults and all ABI qualification gaps remain open;
physical Wii, engine pool allocation, nested conversion and gameplay are untested.

Next bounded preparation is a typed nested UI root graph with explicit
ownership/relocation limits, followed by actual BSP residency/lifetime proof.
This diagnostic is isolated from the engine's production paths.

Independent read-only review passes 206 scoped checks with zero unresolved
findings. Source/build/artifact/compiler/SDK identities, six-line numeric header,
absence of the entire raw UI blob from the DOL, actual FAT input/12 prior files,
guest transcript, settings and lifetime/budget arithmetic agree. No second
target run, full-image/whole-map rehash or inflation was performed; before/after
hash equality does not exclude transient external writers. All diagnostic and
memory limits above remain unchanged.
