# Original probe startup watchpoint capture

One corrected stock/original-probe attempt qualifies its debugger setup and
captures the watched four-byte word changing from zero to 32,348. It reaches
the 64-stop cap before shutdown, then uses explicit owned-process forced cleanup.
There is no natural Dolphin exit or new guest report. [Numeric evidence](2026-10-08-shutdown-startup-capture.json)

The early init file runs before symbols load. Exact live Windows/GDB relocation,
five instruction prefixes, committed writable image page, zero word/guard,
hardware watchpoint and first instrumented PE-entry marker are verified.
The startup metadata span is 20 bytes; the hardware watch remains four bytes.
At 4.197 seconds, the known reset-entry address sees zero; accessor entry follows
at 4.242 seconds. At 4.306 seconds the hardware change sees 32,348, with post-PC
RVA `0x8d15e2` on the same debugger thread. Export names attached by GDB are
unqualified nearest labels and do not identify the source owner.

A bounded independent original-byte review identifies the preceding instruction
at RVA `0x8d15df`: `MOV DWORD [RSI+0x20],ECX`. All fifteen possible preceding starts
within the architectural instruction-length limit yield exactly one complete
instruction ending at the hardware trap PC. Saved ECX matches the new word.
The narrow forward review uses one new unwind fragment, bringing the combined
count to 22 of 24, and 29 selected outcomes within the 32-instruction bound.
It retains six numerical caller RVAs without decoding their bodies. RSI was
not captured; there is no independently measured effective address, complete
entry CFG, all-writers proof, source-owner or final shutdown-writer claim. The
following store was still pending at the capture and is not claimed executed.

Sixty subsequent SIGSEGV stops at PCs outside the main image consume the rest
of the cap. Those addresses do not establish JIT, benign/fatal exception semantics
or cause. Helper return, registered callback and fatal comparison were not
captured. The debugger lifecycle ends after 14.943 seconds, within the 120-second
bound. GDB exit zero after cleanup does not qualify natural Dolphin exit.

Fresh SD backup/source/after hashes, sentinel count 2 and original log agree;
the SD and required three configs/input identities remain unchanged. The host
profile adds sixteen WiiSession files and modifies existing TimePlayed.ini;
the other twenty-four prior files remain unchanged. These host metadata changes
are retained, without restoration or deletion. Independent saved-runtime review
passes 52 predicates and writer review passes 62; neither repeats the launch.

The [earlier setup failure](2026-10-08-shutdown-debugger-setup.md) and all prior
runtime failures remain. This gate does not qualify shutdown, ABI, native geometry,
rendering/gameplay or physical Wii; HWI-005/006 remain BLOCKED. No further launch
occurs as part of this evidence gate.
