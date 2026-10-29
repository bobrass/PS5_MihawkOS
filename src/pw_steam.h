/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PW_STEAM_H
#define PW_STEAM_H

/* The launcher's Steam option: the official client, downloaded from Valve's own servers when the user asks, installed
 * silently into the Windows prefix, started once for the user to sign in, and from then on started in the background
 * to run the games that need it. Nothing of Valve's is in this repository or in a release: the installer is fetched
 * over the title's own TLS (proton_fetch.h, certificates and host name checked) and run as the user would run it.
 *
 * This file is the part with no console in it, host-tested by tests/test_pw_steam.c:
 *   - what a downloaded file must look like before it is run as a program;
 *   - whether the client holds a remembered sign-in (its loginusers.vdf), and that user's display name;
 *   - the process rules that keep the client inside the console's memory (docs/PLAN.md, "Steam's client and Isaac on
 *     the Pro": patch 2070's WINE_PS5_PROCESS_RULES), kept in one list so a rule is read, changed and tested here;
 *   - the catalog entries (game.json) the launcher writes for the installer run and for the client.
 *
 * The client's own window is the heaviest thing it does (its browser reached 503 MiB in one process on the PS5 Pro, a
 * guard end at 44 s), so the option asks for it once, to sign in, and ends that session when the sign-in is saved
 * (PROTON_STEAM_SIGNIN_WATCH, native/proton_wine.cpp); games then start it headless. */

#include <stddef.h>
#include <stdint.h>
#include "pw_wine_profile.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Valve's installer, from its content network (the address its download page links), and the older host as the
 * second try. Both are HTTPS; the title checks the certificate and the name. */
#define PW_STEAM_INSTALLER_URL "https://cdn.cloudflare.steamstatic.com/client/installer/SteamSetup.exe"
#define PW_STEAM_INSTALLER_URL_FALLBACK "https://steamcdn-a.akamaihd.net/client/installer/SteamSetup.exe"

/* Where things live on the console (the launcher's own prefix, the one every game of the desktop shares). */
#define PW_STEAM_PREFIX "mihawk"
#define PW_STEAM_PREFIX_DRIVE_C "/data/ps5proton/prefixes/" PW_STEAM_PREFIX "/drive_c"
#define PW_STEAM_EXE_REL "/drive_c/Program Files (x86)/Steam/steam.exe"   /* inside a prefix folder */
#define PW_STEAM_EXE_PATH PW_STEAM_PREFIX_DRIVE_C "/Program Files (x86)/Steam/steam.exe"
#define PW_STEAM_LOGINUSERS_REL "/drive_c/Program Files (x86)/Steam/config/loginusers.vdf"   /* inside a prefix folder */
#define PW_STEAM_CONNECTION_LOG_REL "/drive_c/Program Files (x86)/Steam/logs/connection_log.txt"   /* inside a prefix folder */
/* The client's token, which keeps it signed in; never read, only its time (native/proton_wine.cpp: the client is ended once it
 * is written after a sign-in). Inside a prefix folder. */
#define PW_STEAM_LOCAL_VDF_REL "/drive_c/users/" PW_WINE_PROFILE_USER "/AppData/Local/Steam/local.vdf"
#define PW_STEAM_LOGINUSERS_PATH PW_STEAM_PREFIX_DRIVE_C "/Program Files (x86)/Steam/config/loginusers.vdf"
#define PW_STEAM_DOWNLOAD_DIR "/data/ps5proton/downloads"
#define PW_STEAM_INSTALLER_PATH PW_STEAM_DOWNLOAD_DIR "/SteamSetup.exe"
/* The same file as the Windows side sees it (Z: is the console's root). */
#define PW_STEAM_INSTALLER_WINDOWS_PATH "Z:\\data\\ps5proton\\downloads\\SteamSetup.exe"
#define PW_STEAM_CLIENT_WINDOWS_PATH "C:\\Program Files (x86)\\Steam\\steam.exe"
#define PW_STEAM_CLIENT_WINDOWS_DIR "C:\\Program Files (x86)\\Steam"

/* A test's own prefix (the owner's trial of the Steam guide in a fresh one, 2026-10-26): /data/ps5proton/steam-prefix
 * names it, so the option installs and signs in there and never touches the desktop's prefix. Only a plain word is
 * taken: 1..32 of a-z, 0-9 and '-'. Copies it into out (cap bytes) and returns 1, else 0 (the option keeps
 * PW_STEAM_PREFIX). Text may end in a newline. */
#define PW_STEAM_PREFIX_HOOK "/data/ps5proton/steam-prefix"
int pw_steam_prefix_word(const char *text, size_t length, char *out, size_t cap);

/* The catalog entries the launcher writes under /data/ps5proton/games/<id>/game.json. */
#define PW_STEAM_ENTRY_INSTALL "steam-setup"
#define PW_STEAM_ENTRY_CLIENT "steam"

/* The installer is about 2 MB. Under 512 KiB or over 32 MiB is not it (an error page, a redirect that was not followed,
 * or something else). */
enum { PW_STEAM_INSTALLER_MIN = 512 * 1024, PW_STEAM_INSTALLER_MAX = 32 * 1024 * 1024 };

enum PwSteamInstallerCheck {
    PW_STEAM_INSTALLER_OK = 0,
    PW_STEAM_INSTALLER_SIZE = -1,      /* outside PW_STEAM_INSTALLER_MIN..MAX */
    PW_STEAM_INSTALLER_NOT_PE = -2,    /* does not start as a Windows program ("MZ", a valid PE header offset) */
    PW_STEAM_INSTALLER_NOT_SIGNED = -3 /* the PE has no certificate table: Valve signs its installers */
};

/* Whether `first` (the file's first bytes, at least 1 KiB are needed for the PE checks and `n` of them are given)
 * and its `total` size are a plausible signed Windows installer. This is a sanity check against a wrong download, not
 * a signature check: the transport's certificate and host name are what vouch for the file. */
int pw_steam_installer_check(const uint8_t *first, size_t n, uint64_t total);

/* Whether the client's loginusers.vdf (text, `length` bytes) holds a user whose password is remembered (a
 * "RememberPassword" of 1 and no "AllowAutoLogin" of 0): 1 and that user's PersonaName in `persona` (cap bytes, may be
 * NULL), else 0. Malformed or cut text is 0, never an overrun. */
int pw_steam_remembered_user(const char *vdf, size_t length, char *persona, size_t cap);

/* Where the client's own set-up stands, from the facts the launcher reads, as the Accounts block of the Aurelia page shows
 * it: one step, its words, and what CROSS does there. A pure function, so every state and its words are host-tested. */
typedef enum {
    PW_STEAM_STEP_NOT_INSTALLED = 0, /* CROSS downloads and installs it */
    PW_STEAM_STEP_DOWNLOADING,       /* the installer is coming from the vendor: nothing to press */
    PW_STEAM_STEP_INSTALLING,        /* downloaded: the launcher is about to restart into the silent install */
    PW_STEAM_STEP_FAILED,            /* the download failed and the client is not installed: CROSS tries again */
    PW_STEAM_STEP_SIGN_IN_NEEDED,    /* installed, no remembered sign-in: CROSS opens its window once */
    PW_STEAM_STEP_SIGNED_IN,         /* installed and remembered: games start it in the background */
} PwSteamStep;

/* `downloading`, `downloaded` and `failed` are the download thread's phase (at most one is set); installed and
 * signed_in are read from the prefix. An installed client is never "downloading": the files decide. */
PwSteamStep pw_steam_step(int installed, int signed_in, int downloading, int downloaded, int failed);
/* The row's subtitle, its badge (may be empty) and the word for what CROSS does ("" when nothing). Static strings. */
const char *pw_steam_step_subtitle(PwSteamStep step);
const char *pw_steam_step_badge(PwSteamStep step);
const char *pw_steam_step_action(PwSteamStep step);

/* The WINE_PS5_PROCESS_RULES value for the client's runs (patch 2070), and its rules one by one for a test: `out`
 * gets the whole value ("NAME|ACTION|VALUE;..."), its length is returned, 0 when `cap` is too small. The browser helper
 * runs as one process (--single-process) on every console: its own processes took the system page tables a game needs
 * (src/pw_steam.c). */
size_t pw_steam_rules(char *out, size_t cap);
unsigned pw_steam_rule_count(void);
/* Rule i's three fields (NULL past the end). */
int pw_steam_rule(unsigned i, const char **name, const char **action, const char **value);
/* A stored WINE_PS5_PROCESS_RULES value (a game.json an older build wrote) with each built-in rule whose program and
 * action it lacks appended, so a stored list cannot drop a rule this build adds (the browser's hold, patch 2104). The
 * stored rules come first and keep their values (patch 2070 takes the first match). Length, or 0 when `cap` is too small. */
size_t pw_steam_rules_merge(const char *stored, char *out, size_t cap);

/* A catalog entry (game.json) as text: name, detail, the Windows path of the program, its directory, its command line,
 * whether the Mihawk desktop lists it, an environment of NAME=VALUE words (NULL-terminated, may be NULL), and its prefix
 * (NULL: PW_STEAM_PREFIX). Strings are JSON-escaped. Returns the length, 0 when `cap` is too small or a field is
 * missing. */
size_t pw_steam_entry_json(char *out, size_t cap, const char *name, const char *detail, const char *executable,
                           const char *directory, const char *arguments, int desktop, const char *const *environment,
                           const char *prefix);

/* The two entries as text: the silent installer run and the client. Not big_picture: its own window for the sign-in (no arguments;
 * the environment carries the sign-in watch). big_picture: the signed-in client in Big Picture mode (-bigpicture), without the
 * client's check of its own executables on every start (-noverifyfiles: 15 s of each start on the Pro, steam-logs-2026-10-25's
 * bootstrap_log "Verifying all executable checksums"; an update still replaces them), the rules and the
 * window route in its environment, no watch: the session ends when the user leaves Steam. */
size_t pw_steam_installer_entry(char *out, size_t cap, const char *prefix);
size_t pw_steam_client_entry(char *out, size_t cap, int big_picture, const char *prefix);

#ifdef __cplusplus
}
#endif

#endif
