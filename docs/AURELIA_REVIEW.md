# Aurelia review, and where the store-client sign-in belongs (2026-10-24)

Question from the owner: review the Aurelia implementation; the store-client sign-in should be part of Aurelia, not the launcher menu; if
Aurelia is not the best approach, suggest another. Method: ten read-only agents (six reviewers of the repository, four researchers of upstream
and of Valve's published material), one design judge, and two skeptics for each of six central claims (23 agents, no console access).
Everything below is from that run; "verified" means a skeptic checked it against the files, logs or public pages. The Subscriber Agreement
points are summaries, not legal advice.

## Verdict on Aurelia

- **Sound as the library account and installer.** Pinned v0.1.40, a Rust Steam-protocol client, no CEF. On record: a QR sign-in in 43 s, a 59-game
  list, two complete 2 GB installs, 29 launched runs at about +110 MiB counted memory (277-290 MiB peak through an install), no guard action, every
  session that reached its launcher stage returned with status 0 (76 records).
- **Not a Steamworks runtime.** A game that calls `SteamAPI_Init` needs the official client running as the same user and signed in to an account that
  owns the app (Valve's SDK page; verified). Aurelia has no client interface and no emulation; its own wiki says the in-Wine client keeps a separate
  sign-in. In the 2026-10-23 runs Isaac, with only Aurelia and SteamCMD signed in, started `steam.exe steam://run/250900` itself in the same tick
  (`klog/isaac-steamapi2-title.log:90-91`).
- **Single maintainer, pre-1.0, fast-moving**, no checksums for its releases (our pin has one). Running the pinned GPL-3.0 binary as a separate
  process is fine for this repository's licence.

## Defects in the title's Aurelia driver (all fixable on the host)

Status 2026-10-24: defect 1 is fixed (the parser reads one game at a time; the page's cap is 1024) and tested with a 300-game library; defect 3's page
searches the unfiltered catalog now; the sign-in session can be cancelled by holding OPTIONS for 2 s (the client's window, not the Aurelia QR screen);
defects 2 (the Aurelia QR screen has no cancel or failure reasons), 4 and 5's plan correction (done) remain as listed.


1. **Libraries of 96 or more games never appear.** `TOKENS 4096` (`src/pw_aurelia.c:10`); the parse error becomes -1 and the page stays empty.
   The 59-game capture needs 2,538 tokens (43 per game), so the ceiling is 95. Reproduced on the host.
2. **No way out of the sign-in screen**: no cancel, no timeout message, no phone-free path (the Circle button is read only on the grid and on the
   SteamCMD form). Failure reasons are dropped by the parser.
3. **The R2 page's actions are dead unless `library-show-all` exists**: `aureliaProfile()` and `aureliaInstalled()` search the filtered list
   (`examples/ps5proton/ps5proton.cpp:141-174`). The page was committed "built, not yet run" and its side panel reads the wrong index (read, not run).
4. **Installed games are registered by a PC-side FTP script** (`tools/register-aurelia-games.py`) that can overwrite the working `250900` entry;
   nothing in the title does it after an install.
5. **The plan's "install left no files" conclusion is unsupported** (verified): a bare-path launch gets the prefix `probe` (`src/pw_wine_launch.c:20`)
   while the check listed the `mihawk` prefix, and later logs show the official client launching Isaac from `Games\Aurelia`. Aurelia's install did
   work; docs/PLAN.md around line 5938 is corrected below.

## What "sign-in as part of Aurelia" can and cannot mean

- **Cannot:** one scan that also signs the official client in. No supportable way exists to seed the client's remembered sign-in from another
  client's (verified): it keeps a DPAPI-protected token in `local.vdf` (under Wine, Wine's own format keyed on the Wine user), Aurelia's token is
  minted as a different platform type, and writing credentials into the client's files is outside what Valve documents. Valve documents no import.
- **Cannot:** be drawn by the title. The official client's QR and account page are drawn by its own browser window (verified from the pictures and
  the helper's command line); the title never obtains the QR link. Aurelia draws its own QR only for its own account.
- **Can:** the Aurelia page owns the entry point, status and retry of the official client's one-time install and sign-in (design B below). The user
  then scans twice, in one framed flow, and the launcher's library list no longer carries a Steam row.

## Candidates (scores 1-5, 5 best; effort 5 = least)

| Design | Sign-in | Games | Memory | Maintain | Legal | Effort |
| --- | --- | --- | --- | --- | --- | --- |
| A. Aurelia only | 5 | 2 | 5 | 3 | 3 | 5 |
| **B. Aurelia page hosts the client's install and sign-in; Steamworks games run through the client headless** | 3 | 4 | 3 | 3 | 4 | 3 |
| C. one Aurelia sign-in seeds the client (rejected: DPAPI store, different token type, eviction risk, terms) | 5 | 2 | 3 | 1 | 2 | 1 |
| D. SteamCMD behind the Aurelia UI (rejected: third sign-in, typed password written to disk, one deciding test failed) | 2 | 1 | 3 | 2 | 3 | 2 |
| E. a native in-title client, replacing Aurelia | 5 | 2 | 5 | 2 | 2 | 1 |

**Recommendation: B.** It is the only design with a console-proven Steamworks game (Isaac: about 61 fps, peak 680 of 870 MiB), it reuses the
finished engine (`pw_steam`, the download thread, `PROTON_SESSION_FOLLOWS`, the watcher, the hand-off), and the client step is lazy and optional:
users who only want games with no Steamworks calls never pay for it. First use costs the Aurelia scan (about 45 s) plus, once, the client step:
2-3 taps, one scan, about 6-7 minutes of which about 5.5 are the client updating itself, peak 577 (guard sample 589) of 870 MiB. Afterwards a
Steamworks game starts the client headless with no taps.

## Corrections to what the repository says

- docs/STEAM.md: the sign-in window's steady figure is 566-577 MiB with a guard-sampled peak of 589, not "peak 577"; the updater was still
  showing at 300 s (the picture at 320 s is a black window); the 503 MiB figure belongs to the signed-in client's library window, not the
  sign-in page (214 MiB in its largest process).
- docs/PLAN.md "install left no files": see defect 5. docs/PLAN.md also still calls the SteamCMD question open: the test was run three times and
  Isaac fell back to the official client each time, but SteamCMD never completed its login in those runs (it sat at the prompt), so what is shown is
  that this setup failed, not that SteamCMD could never host a client interface.

## Not determined (needs a console run, an owner step, or a human reading)

- A real QR sign-in in the official client followed by a headless logon has never been run; the Isaac runs used a client signed in earlier.
  Neither the 20 s stop after the saved sign-in nor the memory right after approval is measured. This is the first thing to run.
- Whether a second sign-in on the same account evicts Aurelia's session or the reverse.
- Whether `SteamAPI_Init` returned true in the Isaac runs and what launched `steam.exe` in the standalone ones; offline mode; DRM-wrapped games.
- Whether Valve still honours `-silent -no-browser` in later client builds (the community notice is relayed, Valve's wiki was unreachable).
- Where Aurelia's `session.json` lives under Wine and whether it is encrypted; how Aurelia fails on a full disk or a dropped network.
- The terms questions rest on summaries; the Subscriber Agreement and the SteamCMD licence need a human reading.

## Safety fact for the memory experiments

The only console-level damage in the records came from running the official client with the guard off (`PROTON_SYSTEM_MEMORY_LIMIT=0`, the standard
PS5, 2026-10-21): RSS 1863 MiB and the CPU page-table pool at 7784 of 8192, then the console dropped off the network; a second run with the pool
raised panicked at 7757 of 8192 (docs/HYPERVISOR.md). A run past the guard needs a brake on the page-table pool as well as on resident memory.
The Pro's pool is 9216; the Steam runs print it in the `VM Stats` line (`page table CPU a/9216`).

## Built since this review (2026-10-24, uncommitted)

The Accounts rows on the Aurelia page (design B), the Steam row gone from the library list, the portable `pw_steam_step`, the sign-in watcher's
cancel, cap and logon-line observation, the runner's `--real` mode. A diff review (13 agents) found three high defects before the first run (the panel
order hid the Accounts panel, the Accounts list was never styled so it drew in the screen's corner, a compile error) and six medium ones; all are fixed.

## Build plan (owner decisions first)

0. Host only: parse libraries of any size (count tokens first), typed errors and failure reasons, cancel and timeout on the sign-in screen, the
   Aurelia page searching the unfiltered list, and a `hidden` key in `game.json` instead of `library-show-all` for service entries.
1. One console run with the owner's phone: a real QR sign-in on the existing option, recording the memory trace across the approval and whether the
   next headless start logs on (harness fails unless `Client sign-in: saved` and a logon verdict appear).
2. Host only: a `pw_steam_step(facts)` state function and entry generation in code (the `250900` entry from the appmanifest).
3. Move the client row from the launcher's library into an "Accounts" block of the Aurelia page; delete the library row and its branches.
4. A session screen per kind in the title, ending on a verdict (not a 20 s guess), handing back to the Aurelia page.
5. Installed games become playable without the PC script; then the commit gate (patch 2072 touches every prefix's first start).
