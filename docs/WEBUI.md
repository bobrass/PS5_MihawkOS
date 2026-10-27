# The WebUI

A web page the MihawkOS launcher serves on port 6767, for a browser on the same home network: the library and its
entries, a game folder sent to the console, art and details from the Steam store, home screen tiles that start one
game, the saves of a prefix, the launcher's settings, and the logs. Its design is PS5_RetroArch's WebUI (NOTICE.md),
in the guided setup's Frutiger Aero look and its ten languages.

## Opening it

The launcher shows the address and a QR code under the library's panel, and again, larger, in **Settings > Web UI**.
Scan the code with a phone, or type the address (`http://<console address>:6767`) into a browser on the same network.

The page runs only while the launcher is on screen. It starts in the launcher's `prepare()` once `/data` is granted, and
it ends with the launcher's process when a game or a Mihawk OS session starts (`sceSystemServiceLoadExec`), so it holds
no memory and no thread while a game runs. A change made in the page reaches the launcher at once: it counts the
changes (`proton_webui_changes()`) and reloads its settings and library when the count moves.

## Who it is for

The average player, not a developer: the owner judged the first version (2026-10-26) too technical, as it put prefixes,
entry ids and environment words up front. Every page now guides one task in plain words, says what each choice does
and what to pick when unsure, and keeps the technical fields (the program's path, arguments, start folder, prefix,
entry id, OpenGL, more environment) under a game's "Advanced settings". A change to the page's words must keep that.

## Pages

| Page | What it does | Files |
| --- | --- | --- |
| My games | Every game with its picture and its state in plain words (ready to play, or needs attention with the launcher's reason under "Why?"). Folders in `C:\Games` no game uses are listed as "Copied, but not set up yet". A game opens its settings: name and second line; its program, chosen in a picker that opens the game's folder and marks a suggested `.exe` (not an installer or uninstaller); its picture and details; "How it plays": Game Mode and controller, each "Same as Settings" unless the game needs another; "Where to find it": MihawkOS's game list (unticked: `"launcher": false`) and the desktop shortcut; "Advanced settings". Unknown keys of `game.json` are kept. | `games/<id>/game.json` |
| Picture and details (in a game's settings) | Looks the game up in the Steam store by its name (a new game's lookup starts by itself); the chosen match's header and portrait art and its details (release, developers, genres, description) are saved; or the player's own JPEG or PNG. A new game is saved first. The launcher draws the header art in its library. | `covers/<id>-header.img`, `covers/<id>-portrait.img`, `games/<id>/meta.json`, thumbnails in `caches/webui/` |
| Add a game | Three steps: choose the game's folder on the computer; copy it into `C:\Games\<folder>` of Mihawk OS's prefix, 8 MiB at a time, with speed and time left (a stopped copy carries on where the PS5's copy ends; hidden files are left out); set it up, in the game's settings with its program suggested and its picture looked up. It ends with where to start the game and a way to put it on the home screen. | `prefixes/mihawk/drive_c/Games/` |
| Home screen | Says whether ShadowMountPlus answers and what to do if not; makes a tile that opens MihawkOS straight into one game (`profile=<id>`), named by the player, its icon drawn from the game's art (its tall cover or wide banner; filled, fitted whole over a blurred copy, stretched or at its own size; moved and zoomed) and the MihawkOS mark; lists the tiles and removes them; a tile appears on, or leaves, the home screen a few seconds after MihawkOS closes. | `/data/homebrew/PPSA99300` to `PPSA99399` |
| Saves | The save folders that exist in the prefix's user folder (Saved Games, Documents, AppData's Roaming, LocalLow and Local), each downloaded as a ZIP named from `C:\` (`users/<user>/...`); a ZIP put back, read in the browser (stored or deflated), each file to where it came from, refused whole when any entry does not name its place; the folder browsed, a file or a folder downloaded. | `prefixes/<prefix>/drive_c/users/` |
| Settings | Controller (with what each choice means), Game Mode, 120 Hz output, theme and music, mouse buttons, detailed logs: the same files and the same save code as the launcher's Settings. | `controller.txt`, `game-mode.txt`, `high-refresh.txt`, `logging.txt`, `controls.txt`, the title's `hui/theme.txt` and `hui/music.txt` |
| Help | What to do when something goes wrong, the bug report (a ZIP of every log, the last 512 KiB of each with addresses masked, and the version, runtime and settings), the logs themselves and the version. | read only |

When the PS5 does not answer (MihawkOS closed for a game), a banner says so and how to come back. A write refused for
an old token (MihawkOS started again since the page opened) takes the new token and is sent once more.

## A game's own settings

A game's Game Mode and controller are words of its `game.json` "env", which the page shows as two choices:
`WINE_PS5_GAME_MODE=1` or `=0` (`src/pw_game_mode.h`) and `WINE_PS5_CONTROLLER=xbox-360`, `xbox-one` or `dualsense`
(`src/pw_controller.h`); none means the player's setting. Both hold wherever the game starts:

- **From the launcher or a tile** (the game's own session): the session reads them after the game's environment
  (`native/proton_wine.cpp`) before the broker starts.
- **From the Mihawk OS desktop**: the title gives each new program its library entry's words (`title_application_lookup`,
  `src/pw_app_environment.c`), which add `WINE_PS5_HID=1` for a DualSense and `=0` otherwise, since each program runs
  its own HID stack (Wine patch 0870). Wine tells the title each helper's Game Mode before it runs
  (`PW_CHILD_NOTICE_GAME_MODE`, `wine/ps5/pw_wine_process.c`), and its windows take the screen or not by that choice
  (`pw_window_direct_set_process`, `src/pw_window_direct.h`).
- **The programs a game starts** (a launcher that starts the game) inherit the three words (`wine/ps5/pw_wine_inherit.h`).

120 Hz output is one setting for the whole console (`src/pw_high_refresh.h`, `high-refresh.txt`), as the display's rate
is the title's once per session: off, the session asks for 60 Hz unless the game's own `PROTON_120HZ` says otherwise.

## Who owns what

| Part | File | Host-tested by |
| --- | --- | --- |
| Routes, checks, files (portable) | `src/pw_webui.hpp`, `src/pw_webui.cpp`: `pw_webui_handle(context, request) → response` | `tests/test_pw_webui.cpp` |
| HTTP (libmicrohttpd), token, body limits, uploads, ZIP streaming | `src/pw_webui_http.hpp`, `src/pw_webui_http.cpp` | `tests/test_webui_http.py` (the real server on the PC) |
| The page | `webui/index.html`, `webui/app.css`, `webui/app.js`, `webui/i18n.js` | `tests/test_webui_i18n.py`; in a browser through `tools/run-webui-host.sh` |
| The launcher's side: start, address, ShadowMountPlus, the store fetch | `ps5/src/proton_webui.{h,cpp}`, `examples/ps5proton/ps5proton.cpp` (`drawWebuiBlock`, `reloadFromWebui`, `coverTexture`) | the console |
| The tile app and its launch payload | `forwarder/`, `tools/build-forwarder.sh` (copied into the title's `forwarder/` by `ps5/tools/link-title.sh`) | the console |
| The server library | `tools/build-webui-http.sh`: GNU libmicrohttpd 1.0.10, pinned by SHA-256, static, no TLS | `tests/test_webui_http.py` |

`tools/run-webui-host.sh` serves the page on the PC from sample data (`WEBUI_HOST_FAKE_SMP=1` answers as a
ShadowMountPlus that takes every tile), for checks in a browser at `http://127.0.0.1:6767`.

## Safety

The page is open to the home network only, as PS5_RetroArch's is:

- **The console's own origin.** A request whose `Host` is not the address and port the browser reached the console
  at, or whose `Origin` is another site, is refused (403) before its body is read: another web page in the player's browser cannot reach it.
- **A token on every write.** `GET /api/status` gives a token made at the launcher's start; every other method needs it
  in `X-Mihawk-Token`. A page from an earlier start is told to reload.
- **Strict headers.** `Content-Security-Policy: default-src 'self'; img-src 'self' data:`, `nosniff`, `no-referrer`,
  `no-store`. The page has no inline script or style.
- **Fixed files.** Only the page's own files are served (`pw_webui_asset`); everything else is an API route.
- **Paths.** A path part is never empty, `..`, hidden, or holds `/`, `\` or `:`; a symbolic link is never followed.
  Writes go to `C:\Games\<folder>` or a folder of `C:\users\<user>` only.
- **Atomic writes.** Every file is written to a temporary, synced and renamed; an upload grows `<file>.part` and is
  renamed when whole.
- **Bounded bodies.** 64 KiB by default, 4 MiB for a picture, 3 MiB for a tile; an upload piece streams straight to
  its file (a file of up to 64 GiB). A refused request's body is drained (up to 16 MiB) so the browser reads the answer.
- **Addresses masked.** Logs leave through the page with every IPv4 address replaced by `<address>`. The launcher
  shows the console's address on screen only; it never goes to a log (AGENTS.md rule 5).

## Tiles

A tile's modes are an app's (`tile_modes`): its programs 0755 (a tile whose `eboot.bin` was 0644 did not start, "Can't
start the game or app"; the base PS5, 2026-10-26: `[Syscore App] error=0x80aa001a errno=13`), its folders 0755, its
picture, `param.json` and `forward.txt` 0644 (the first tile, so, showed its picture on the home screen; every tile
made 0777 and 0666 after it showed none). The launcher mends a tile with other modes at its next start
(`pw_webui_forwarders_finish`). Whatever else the page makes under `/data` (an entry, a copied game, a picture) is open
to every process as the FTP server's copies are (`make_dir`, `write_file`): a game's helpers do not all run as the
title's user. The player chooses the picture (cover or banner) and how it fills the square (Fill, Fit, Stretch, Actual
size), and moves and zooms it (dragged, a zoom slider); the page draws the icon so, a 24-bit RGB PNG.

A tile is a title of its own, `PPSA99300` to `PPSA99399` (`kFirstTile`, `kLastTile`), clear of MihawkOS (`PPSA99170`),
its memory probe (`PPSA99171`, the tester probe title) and PS5_RetroArch's tiles, whose range begins at `PPSA99170`
and so holds both of MihawkOS's: PS5_RetroArch's own rule is to skip them (a change in its repository). Its folder holds `eboot.bin` (the tile app, 10.8 KB, the same
for every tile), `sce_sys/param.json` and `icon0.png`, `launch.elf` and `forward.txt` (`profile=<entry id>`). Started,
the app writes its title id into the slot of `launch.elf` (`MIHAWK-FORWARDER-TILE-ID-SLOT`) and sends it to the ELF
loader on `127.0.0.1:9021`; the payload reads that tile's `forward.txt` and starts MihawkOS with it
(`sceLncUtilLaunchApp`, `sceSystemServiceLaunchApp` as the fallback). No file is written for the hand-over, so two tiles
cannot race.

The page names ShadowMountPlus in its own words; the server's reasons under them never do, as the publication check reads
a binary that holds the name as a copy of the app, which MihawkOS never ships (`tools/publication-check.py`).

ShadowMountPlus registers a tile (`POST 127.0.0.1:10101/api/v1/manual/add`) and installs it with a scan; it removes
it with `/api/v1/games/uninstall`. Neither happens while a game or app runs, and MihawkOS is one: a tile installed while
MihawkOS ran reached the home screen without its picture (the base PS5, 2026-10-26: the system held the picture in
`/user/appmeta/<tile>/`, yet the home screen drew none, whatever the id, content id, PNG or modes), as PS5_RetroArch's
tiles appear "once RetroArch closes". So the page only registers a new tile and marks it `install.pending`, or marks a
removed one `remove.pending`, and sends the tiles' worker (`forwarder/worker.c`, the title folder's
`forwarder/worker.elf`) to the ELF loader: a payload that waits while a big app runs, then installs each marked tile
(registers it again and asks a scan, every 30 s, until the game list shows it installed) or uninstalls it and deletes
its folder, and ends when none waits, or after a day (one at a time: the one holding 127.0.0.1:6768; its record:
`logs/tile-worker.log`). The launcher sends it again at its start while a tile waits.

## Languages

The page is in the guided setup's ten languages (`src/pw_setup.c`), chosen from the flag at the right of its header (the
setup's own flags, `ps5/assets/flags`, served as `/flags/<flag>.svg`): the one chosen there (kept in that
browser), else the console's (the guided setup's choice, in `GET /api/status`), else English. `webui/i18n.js` holds
each language's text keyed by the English; `t('...')` in `app.js` and the page's own text nodes are looked up there,
and the server's complete messages are too.

`tests/test_webui_i18n.py` holds them together: every English string the page, the script or the server can show has
its text in every language, with the same `{0}`, `{1}` values. A string added without its translations fails the
host tests; `python3 tests/test_webui_i18n.py --list` prints what a translator is given.

## Routes

| Route | Does |
| --- | --- |
| `GET /api/status` | Version, runtime, the token, the setup's language |
| `GET /api/library` | The launcher's list, each with `entry`, `cover` (its time) and `meta` |
| `POST /api/library/game`, `POST /api/library/delete` | An entry saved (merged into its `game.json`) or removed (its files stay) |
| `GET /api/browse` | Folders and `.exe` files of a prefix's `C:\Games`, for the program picker |
| `GET`, `POST /api/settings` | The settings and their choices; every key checked before any is saved |
| `GET /api/logs`, `GET /api/log`, `GET /api/report` | The logs, one log (masked), the bug report ZIP |
| `GET /api/upload/status`, `PUT /api/upload` | How much of a file the console has; one piece at an offset |
| `GET /api/prefixes`, `GET /api/files`, `GET /api/file`, `GET /api/zip` | Prefixes; a folder of `C:\Games` or `C:\users`; one file; a folder as a ZIP (stored, streamed, under 65000 files and 4 GiB) |
| `POST /api/media/search`, `GET /api/media/thumb`, `POST /api/media/apply` | The store's matches, their thumbnails, one applied |
| `GET`, `PUT /api/media/cover`, `POST /api/media/remove` | The art of an entry: read, the player's own, removed |
| `GET`, `POST /api/forwarders`, `POST /api/forwarders/delete`, `GET /api/forwarders/icon` | Tiles: listed with ShadowMountPlus's state, made, removed, their icon |

## What is proven

On the PC (2026-10-26): `tests/test_pw_webui.cpp` (routes, refusals, settings, entries, media with a canned store,
tiles with a canned ShadowMountPlus, saves, the report) and `tests/test_webui_http.py` (7 tests on the real server:
origin, token, body limit, an upload in pieces resumed, a folder ZIP with every CRC checked and a save put back, the
bug report, the page's headers) pass, with `tests/test_webui_i18n.py` (every string in every language) and the host tests
of a game's own settings (`test_pw_window_direct`, `test_pw_wine_inherit`, `test_pw_app_environment`,
`test_pw_high_refresh`). Every page was used in a browser against `tools/run-webui-host.sh`, at a computer's width and
a phone's (375 px): a folder copied and set up in three steps (its program suggested past its uninstaller, its picture
looked up in the real Steam store), a game's own Game Mode and controller saved and shown again, a tile made, a save
folder backed up and put back, a deflated ZIP from another tool put back, one without places refused before any write,
the 120 Hz setting saved.

Not set from the page: the language of Windows (the guided setup's, which also sets the prefix's locale), and free
space (the title cannot read it: `statfs` is not importable in a title process; `ps5/src/proton_install.cpp`).

On the base PS5 (2026-10-26, the owner): the launcher showed the QR code under the library's panel and in Settings >
Web UI. The first page from the console stayed loading: every file over 32 KiB stopped after its first 33 KB (curl from
the PC: `/app.js`, `/i18n.js`, `/inter.ttf` 33,1xx bytes in 20 s, smaller files whole in 10 ms), as MHD sent a file
descriptor with sendfile, which the console's kernel stops; files are now read in blocks (`respond_file`), and
`tests/test_webui_http.py` refuses a descriptor response. The PC's sendfile works, so no host test could show it.

With files read in blocks, the owner used the page from a phone ("It's amazing"), then found three things: a game set up
on the page was not in the launcher's list (an entry of Mihawk OS's prefix was listed only with library-show-all; a
per-game box, off for entries made before it, still left the owner's game out: now every entry is listed unless its
`game.json` says `"launcher": false`); its home screen tile did not start ("Can't start the
game or app": the kernel log said `error=0x80aa001a errno=13`, the tile's eboot.bin was 0644: now 0777, as the title's
own, and old tiles are mended); and a tile removed on the page stayed on the home screen until deleted there
(ShadowMountPlus uninstalls nothing while MihawkOS runs: now the tiles' worker finishes it after MihawkOS closes).
The owner asked for the language flags PS5_RetroArch's page has: the guided setup's own, now the header's picker.
Then tiles reached the home screen without their picture, though the PS5 had copied it (`/user/appmeta/<tile>/icon0.png`,
the page's PNG, valid): the kernel log said `The size of content id is too small (UP9000-PPSA99301_00-MIHAWKTILE99301,
35)` from PlayGo and the installer (0x80b21166). A content id is 36 characters, its label 16: now `MIHAWKTILE0` and the
number, as PS5_RetroArch's `FORWARDER00` and the number. With that fixed, a new tile under `PPSA99300` (installed, deleted
and installed again several times that day) still showed no picture; the first tile ever under it had. Now tiles take
ids in turn (`tiles-next.txt` in the data folder), so an id just removed is not taken again soon, and the page writes
the icon as a 24-bit RGB PNG, as the system's own and PS5_RetroArch's are (the canvas's PNG is 32-bit RGBA): checked
in a browser, pixel for pixel. Still no picture (`PPSA99303`, never used before, RGB): the owner asked why not simply do as
PS5_RetroArch does. It never installs a tile while RetroArch runs; ours installed every tile at once, MihawkOS running.
Now ours installs once MihawkOS has closed (the tiles' worker), and its modes are the first tile's (0755, 0644). The
owner then made a tile, closed MihawkOS, and it reached the home screen with its picture, positioned as chosen on the
page ("It works as expected now").

Not yet run on the console after those fixes: a setting or an entry reloaded
by the launcher, the store through the title's own HTTPS, a tile made, started and removed, a game's own Game Mode and
controller from the launcher and from the desktop, and the 120 Hz setting.
