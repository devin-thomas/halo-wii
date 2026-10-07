# Running and deploying

**No Wii executable is included with these planning documents.** Begin with an asset-free diagnostic built by the implemented probe target. Game data import, package paths and loader behavior must be implemented and validated before a game-run recipe is marked verified.

## Dolphin loop

Use a pinned Dolphin build and launch the Wii DOL/ELF through the options that build actually supports. Record emulator version, configuration, controller mapping, video/backend settings and binary hash. Confirm the diagnostic's visible build identifier; do not mistake a stale executable or savestate for the new build. Run save/persistence checks from cold application starts, not only savestates.

A Dolphin pass supports emulator development. It is not a physical-Wii pass, especially for memory/cache behavior, storage, real controllers and sensors, networking or performance. Keep [status](STATUS.md) scoped to the tested feature and binary.

## Physical console loop

Use an already working Homebrew Channel setup. Inventory the actual Wii model, controller ports, storage and network access before changing anything. These documents do not authorize installing channels/IOS, updating firmware, formatting media or replacing an existing homebrew setup.

1. Start Homebrew Channel and confirm its network-ready state and the intended console's local address.
2. Select an already-built diagnostic, verify its hash and send it with `wiiload`.
3. Confirm its build identifier on the console, exercise the scenario, save/retrieve the log, and return cleanly to HBC.
4. Repeat, including a cold boot. Record what still needs physical button input.

The documented underlying interface is `WIILOAD=tcp:host` followed by `wiiload <executable>`; see [Wiiload](https://wiibrew.org/wiki/Wiiload). It is executable loading while the receiver is available, not SSH, a generic filesystem service, remote power control or recovery from every crash.

A helper is provided at [tools/wii](../../tools/wii/README.md). Run it with an explicit console address and executable path. Its success means the sending process succeeded, **not** that the console ran the scenario correctly.

## Data and evidence are separate

Large source/converted game data must not be embedded in every executable delivery. Stage initial assets using a verified supported medium and path. Subsequently use a separately reviewed LAN-only, project-scoped transfer/log mechanism. Never assume wiiload makes the whole SD/USB filesystem remotely accessible.

The implementation must preserve the previous valid cache/save during interrupted updates, validate a project sentinel file's round-trip hash, retrieve run logs without swapping media, and continue gameplay if a log receiver is absent. Identify the exact mechanism before claiming this loop works. Keep unrelated applications and saves untouched.

## Exit and recovery

Provide a tested safe return path, reset/power callbacks, controller-disconnect handling and bounded diagnostics. A failed network launch may require returning physically to HBC. Do not retry against arbitrary LAN devices, open router ports or weaken the main network's security to hide a configuration failure.
