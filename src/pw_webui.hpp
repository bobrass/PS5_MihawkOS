// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// MihawkOS's WebUI (port 6767), the part that needs no network: a request in, a response out (src/pw_webui_http.cpp
// carries them over libmicrohttpd; ps5/src/proton_webui.cpp starts it in the launcher; tests/test_pw_webui.cpp).
//
// The owner asked for it on 2026-10-26, modelled on PS5_RetroArch's WebUI: the library, its entries edited, the
// launcher's settings, the logs, then media, forwarders and saves. It runs only while the launcher shows (no memory
// during a game). Safety is PS5_RetroArch's: the glue answers only requests addressed to the console's own address,
// every write carries the per-start token from GET /api/status, and every file it touches is under data_root, by
// names it validated (no "..", no hidden part, no symlink). Writes are atomic (a temporary file, fsync, rename).
//
// Routes (JSON in and out; an error is {"error": "..."} with its status):
//   GET  /api/status                 the token, the title's version, what the page needs to start
//   GET  /api/library                the launcher's own list (proton_read_games): each entry and its complaint
//   POST /api/library/game           create or replace games/<id>/game.json (keys the page does not edit are kept)
//   POST /api/library/delete         remove the entry games/<id>/ (never the program's own files)
//   GET  /api/browse?prefix=&path=   folders and .exe files under a prefix's drive_c, for the executable picker
//   GET  /api/settings, POST /api/settings   Game Mode, Controller, Logging, every game listed, mouse buttons
//   GET  /api/logs, GET /api/log?name=       the logs folder; one log's text, addresses masked
//   GET  /api/upload/status?prefix=&path=    how much of a file is there (its .part while it arrives), to resume
//   PUT  /api/upload?prefix=&path=&offset=&size=   (src/pw_webui_http.cpp streams it) a file under C:\Games, in pieces
//   POST /api/media/search           {name}: the store's matches (Steam's public store search), app id and name
//   GET  /api/media/thumb?appid=     a match's small capsule, fetched by the console once and kept (the page shows only
//                                    its own origin's images)
//   POST /api/media/apply            {id, appid}: the entry's art (covers/<id>-header.img, -portrait.img) and its
//                                    details (games/<id>/meta.json: description, developers, publishers, release, genres)
//   GET  /api/media/cover?id=&kind=  header or portrait; PUT the same with an image (JPEG or PNG, up to 4 MiB): your own
//   POST /api/media/remove           {id}: the entry's art and details removed
//   GET  /api/files?prefix=&path=    folders and files under Games or users of a prefix's drive_c (saves: users/Mihawk)
//   GET  /api/prefixes               the prefixes (folders of prefixes/ with a drive_c)
//   GET  /api/file?prefix=&path=     one file under Games or users, as a download
//   GET  /api/zip?prefix=&path=      (src/pw_webui_http.cpp streams it) that folder as a ZIP, stored, built as it goes
//   GET  /api/forwarders             the home screen tiles (PPSA99300-99399) and whether ShadowMountPlus answers
//   POST /api/forwarders             {game, name, icon}: a tile that starts MihawkOS into that entry (forwarder/): its own
//                                    title folder, registered through the user's ShadowMountPlus (127.0.0.1:10101)
//   GET  /api/forwarders/icon?id=    a tile's icon
//   POST /api/forwarders/delete      {id}: out of ShadowMountPlus's list and uninstalled; refused while an app runs (the
//                                    launcher is one), it is marked and finished by pw_webui_forwarders_finish
#include <functional>
#include <map>
#include <string>
#include <vector>

struct PwWebuiGame {
    std::string id, name, detail, executable, arguments, directory, prefix, complaint;
    std::vector<std::string> env;
    bool desktop = true, launcher = true, x86_32 = false;
};

struct PwWebuiChoice {
    std::string id, name;
};

struct PwWebuiContext {
    std::string data_root;   // /data/ps5proton: the settings files, games/, prefixes/, logs/
    std::string title_root;  // the title folder: webui/ (the page itself) and runtime-stamp
    std::string token;       // every write must carry it (X-Mihawk-Token)
    std::string version;     // the title's version, shown in the page
    std::function<std::vector<PwWebuiGame>()> games;  // the launcher's own list
    std::function<void(const char *what)> changed;    // a setting or an entry changed: the launcher reloads it
    // GET url into body, at most limit bytes; false and why when it fails (the title's proton_fetch: certificates checked;
    // a test's canned answers). Unset: media cannot be fetched.
    std::function<bool(const std::string &url, size_t limit, std::string &body, std::string &why)> fetch;
    std::string homebrew_root = "/data/homebrew";   // where the tiles' title folders go
    // POST body (JSON) to the user's ShadowMountPlus at route; reply: its JSON. False and why when it does not answer.
    std::function<bool(const std::string &route, const std::string &body, std::string &reply, std::string &why)> shadowmount;
    // Sends the tiles' worker (forwarder/worker.c) to the console's ELF loader: a tile is installed or uninstalled only
    // once no game or app runs, and MihawkOS is one, so each finishes a few seconds after MihawkOS closes. False and why
    // when it could not be sent.
    std::function<bool(std::string &why)> tile_worker;

    // The launcher's themes and music pieces, kept by id in the title folder's hui/theme.txt and hui/music.txt (as
    // ps5/ui/kit.cpp and examples/ps5proton/launcher_music.hpp keep them; the first of each is what an unknown id means,
    // and the music can also be "off"). Empty: the page offers neither.
    std::vector<PwWebuiChoice> themes, music;
};

struct PwWebuiRequest {
    std::string method, path, body, token;
    std::map<std::string, std::string> args;
};

struct PwWebuiResponse {
    unsigned status = 200;
    std::string type = "application/json";
    std::string body;
    std::string file;  // when set: serve this file (a page asset) instead of body
    std::string download;  // when set: the name it is saved under (Content-Disposition: attachment)
};

PwWebuiResponse pw_webui_handle(const PwWebuiContext &context, const PwWebuiRequest &request);
// At the launcher's start: the tiles marked for removal whose app ShadowMountPlus no longer has are removed. Each step
// is said through say (for the title's log).
void pw_webui_forwarders_finish(const PwWebuiContext &context, const std::function<void(const std::string &)> &say);
// The page's files: the path asked for ("/" is index.html) to its file under title_root (webui/, the icon, the launcher's
// Inter), or "" when it is not one of them.
std::string pw_webui_asset(const PwWebuiContext &context, const std::string &path, std::string *type);
// A name the WebUI accepts for an entry, a prefix or a folder part: 1-64 of A-Z a-z 0-9 _ - (ids), or any printable part
// without a leading dot, "/", "\\" or ":" (folder parts, up to 255 bytes).
bool pw_webui_id(const std::string &id);
bool pw_webui_part(const std::string &part);
// Where an upload goes: path (Games/<folder>/...) under prefix's drive_c, every part validated, the folders made as
// needed, none a symlink. absolute: the file; false and why when it is refused.
bool pw_webui_upload_path(const PwWebuiContext &context, const std::string &prefix, const std::string &path,
                          std::string *absolute, std::string *why);
// What a folder's ZIP holds: each file under prefix's drive_c/path (Games/... or users/...), its name in the ZIP (its
// path from C:\, "users/<user>/...", so a backup put back lands where it was), size and time; false and why when the
// path is refused or the ZIP would be too large (65000 files, 4 GiB: no ZIP64).
// One file of a ZIP: read from path, or, when path is empty, text itself (a bug report's masked logs).
struct PwWebuiZipFile { std::string name, path; unsigned long long size; long long mtime; std::string text; };
bool pw_webui_zip_plan(const PwWebuiContext &context, const std::string &prefix, const std::string &path,
                       std::vector<PwWebuiZipFile> *files, std::string *why);
// A bug report (GET /api/report): every log the Logs page lists, its last 512 KiB with addresses masked, and about.txt
// (the version, the runtime and the settings), all held in memory, at most 16 MiB of logs.
void pw_webui_report_plan(const PwWebuiContext &context, std::vector<PwWebuiZipFile> *files);
// Text with every IPv4 address replaced by "<address>" (logs leave the console through the page).
std::string pw_webui_mask(const std::string &text);
