# SteamCMD runtime in the Aurelia session

## Goal and user outcome

Aurelia stays the account, library and download side. The Steam runtime becomes a second,
invisible service behind the same Aurelia session screen: the user signs in to Steam once in the
launcher UI (account, password, Steam Guard), the title runs SteamCMD headlessly, and a
Steam game starts with the Steamworks client it expects. No terminal, no `login.txt`, no
Steam client CEF (the CEF was what crashed the homebrew on memory), and no emulator or DRM
removal.

The deciding open question, proven in slice 3: does the game's
`SteamInternal_SteamAPI_Init` accept SteamCMD's client pipe? The Steam build of the 2D game
imports `steam_api.dll` and `SteamInternal_SteamAPI_Init`, and does not import
`SteamAPI_RestartAppIfNecessary`, so the game may be started by the title as its owner.

## Architecture and owners

| Concern | Owner |
| --- | --- |
| SteamCMD output -> state | new portable `src/pw_steamcmd.{h,c}`, host-tested |
| SteamCMD process and pipes | `native/proton_wine.cpp`, the session that already owns the program/output |
| Runtime setup page and status | `native/proton_loading.cpp` overlay (`proton_aurelia_*`), the Aero theme |
| Login fields and keyboard | the title's input surface (`pw_usb_input`, the pad) |
| Token persistence | the SteamCMD config (`ssfn`), never the password |
| Game prelaunch and `SteamAppId` | `ps5/src/proton_launch.{h,cpp}`, `ProtonGame`/`ProtonLaunch` |
| Library tile and install | `src/pw_aurelia.*`, the launcher's game entry |
| Proof | `tools/host-tests.sh`, `tools/run-console.py`, `evidence/`, `klog/` |

The existing Aurelia session detection (`profile == "aurelia"`) is the pattern for the
Steam runtime session (`profile == "steamcmd"`), and the existing program-output capture
(`WINE_PS5_STDOUT`, polled in the session loop) is the pipe.

## Baseline and authority refs

- Aurelia client lines and the session overlay: `src/pw_aurelia.h`,
  `native/proton_wine.cpp` (`aurelia_session`, `aurelia_poll`, `aurelia_line`).
- SteamCMD installed at `C:\Games\SteamCMD\steamcmd.exe` in the mihawk prefix; measured
  461 MiB under Wine, no CEF.
- The Steam game's executable imports: `steam_api.dll` (`SteamInternal_SteamAPI_Init`,
  `SteamAPI_RunCallbacks`, ...), no `SteamAPI_RestartAppIfNecessary`.
- The Steam client route failed: black CEF window and the homebrew crash on memory; that
  evidence retires the CEF client route.
- Aegis `ui-ux-governance` for the setup page, async states, errors and recovery.

## Compatibility boundary

The Aurelia QR login, library, downloads and the existing game catalog keep working.
The Steam runtime is additive: a new catalog entry (`steamcmd`, `desktop: false` in the
prototype) and a new session mode; the game entries gain an optional runtime association.
No game data or credentials in the repository, commits or logs; the Steam Guard token is
the only persisted secret; the password is never written by us.

## TDD route

`off` -> `skipped`. The project's host tests and console runs are the verification; the
parser gets a host test; each slice gets a console proof before it is claimed.

## Tasks

1. **Slice 1 - wrapper and parser.** `src/pw_steamcmd.{h,c}`: feed lines, emit
   `idle/update/signing-in/guard/signed-in/ready/failed` plus the update percent and the
   account. `tests/test_pw_steamcmd.c` with lines captured from the real SteamCMD. In
   `native/proton_wine.cpp`: `steamcmd_session()` (profile `steamcmd` or the path),
   `steamcmd_poll()` reading the session output, `steamcmd_line()` mapping the parser to
   `proton_aurelia_phase/status/progress/error`; the session loop calls it next to
   `aurelia_poll`; the overlay on for the runtime screen. Host test, build, deploy,
   console run `--profile steamcmd`, evidence, docs, local commit.
2. **Slice 2 - setup page.** Overlay fields for account/password/Guard, keyboard input,
   masked password, labels and focus, the login commands written to SteamCMD's stdin,
   the Steam Guard prompt state, failure and retry, cancel/back; the token persists
   across sessions. Console proof: the user logs in from the TV.
3. **Slice 3 - game prelaunch.** `ProtonGame`/`ProtonLaunch` gain the runtime
   association and `SteamAppId`; the title starts SteamCMD as a hidden helper in the same
   prefix, the game as the owner; read the Steam API result and the frame/GPU log.
   Console proof: the game runs, draws and exits back to the launcher.
4. **Slice 4 - library tile.** The game entry shows the runtime status (not configured /
   signing in / ready / failed), the install is still Aurelia's, the play goes through
   the runtime; recovery links to the setup page.
5. **Slice 5 - closeout.** Tests for the login/Guard flow, the ready state, the game's
   Steam API, the exit back to the launcher, the memory ceiling; docs/PLAN and README.

## Verification

- `bash tools/host-tests.sh` for the parser and the existing suites.
- The title build and deploy; `python3 tools/run-console.py --profile steamcmd ...` for
  slice 1, the login and game runs for slices 2-3.
- `evidence/<run>.json` and `klog/<run>.log` per slice; the memory and frame/GPU numbers
  in the report; the docs and a local commit per slice.

## Risks

- The SteamCMD client pipe may not satisfy a client game's SteamAPI (slice 3 proves it;
  if it fails, this route stops and the alternatives are recorded in docs/PLAN).
- Steam Guard rate limits and token expiry; the UI must show and recover.
- Keyboard input on the TV: the title's USB keyboard surface must reach the overlay.
- Memory: SteamCMD plus the game must stay under the ceiling; measured per slice.

## Retirement

The black SteamCMD console workflow and `login.txt` are retired in slice 2. The Steam
client CEF route is not reinstated; the `steam-client` catalog entry is retained only as
a recorded failure until the goal closes. GOG remains out of scope per the user.
