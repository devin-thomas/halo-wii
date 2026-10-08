# Original probe debugger setup attempt

One bounded stock 2609/original-probe attempt reached a library-load all-thread
stop. Its controller then rejected section bounds and terminated only its owned
inferior. Setup remains unqualified: no watched-word read, hardware watchpoint,
lifecycle breakpoint or lifecycle event occurred. GDB exited 0 after cleanup;
this is not a natural Dolphin exit. [Numeric evidence](2026-10-08-shutdown-debugger-setup.json)

The original source/build/DOL/movie identities from the
[closure gate](2026-10-07-original-probe-closure.md) were retained and verified.
Windows module metadata and GDB section starts agreed at the actual relocated
base. The controller incorrectly compared GDB's executable VirtualSize against
raw file padding: 16,315,328 displayed bytes versus 16,315,392 raw bytes, a 64-byte
overreach. Its same rule would also reject `.data`: GDB displays 760,832 initialized
bytes, while the PE virtual extent is 22,960,376 bytes including a 22,199,544-byte
zero-filled tail. These are controller comparisons, not debugger incapability
or an observed shutdown cause. The initial inline `-iex` command also reports an
expression error before symbols load; later file commands disable auto-load,
so successful early inline delivery is not claimed.

The debugger lifecycle lasted 1.067 seconds. A fresh stopped SD backup exactly matches
the original and post-cleanup image hash; the authored sentinel remains 2 and
the log prefix is preserved. No SD file or profile file changed. Exact stock
executable, original DOL/movie and three configuration hashes remain unchanged.
All Dolphin processes are confirmed absent before post-run reads. Independent
saved-record review passes 35 predicates; it does not repeat the launch or read
the SD. Raw source/argv/output and the failed comparisons remain preserved.

No retry occurs in this gate. The [static lifecycle scope](2026-10-07-shutdown-symbol-scope.md)
still does not establish an executed callback/reset/join, last writer, source
owner or cause. Hardware coverage, unmodified cold-process exit, ABI, engine
geometry/rendering/gameplay and physical Wii acceptance remain open.
