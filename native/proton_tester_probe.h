// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// The tester probe (title PPSA99171, docs/FLEXIBLE_MEMORY.md "The tester probe"): which memory can execute code, in the
// title and in a Sony local process, on whatever console and firmware a tester has. It settles whether "only JIT
// memory executes" (the base PS5 on 13.60) is the hardware's or the firmware's.
//
// It installs as its own title beside MihawkOS and touches nothing of MihawkOS's: it asks MihawkOS's daemon for the
// /data grant once (MihawkOS runs granted) but writes no dev.conf and nothing under /data/ps5proton. Every kernel call it
// may not have on an older firmware is looked up at run time and recorded "unavailable"; every execute attempt runs
// under a fault guard and an alarm, and code is called only where the kernel's own query says execute is held. It shows
// its verdict on the screen for a photograph, and writes it, with the figures, to its own title folder (granted:
// /data/homebrew/PPSA99171; else /app0), and to a USB stick or /download0 when that folder takes nothing.
//
// The title's first act, before any thread: MihawkOS's /data grant asked once (enum ps5_elevation_status), because a title
// runs JIT memory only when granted (fmem-tester-base-2026-10-25-01: refused without it on 13.60).
int proton_tester_probe_elevate();
// Returns the title's exit status (0: the probe ran to its end, whatever it found).
int proton_tester_probe_main(int elevation);
