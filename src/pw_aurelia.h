/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PW_AURELIA_H
#define PW_AURELIA_H
#include <stddef.h>
#include <stdint.h>

/* The Aurelia client (Drackrath/Aurelia, GPL-3.0; the pinned release binary comes from tools/fetch-aurelia.sh): a
 * headless Steam client that speaks Steam's protocols with no CEF and no WebView, driven here as a backend over its
 * --json lines. Measured on the Pro (firmware 10.01, 2026-10-22): login by QR, list, install and play all work
 * headless; the official client's CEF was what cost the console a gigabyte of helpers, ~8,000 shared sections and the
 * black windows. This module only reads what Aurelia says: the launcher draws the UI and starts the game itself.
 * tests/test_pw_aurelia.c holds the lines captured from aurelia 0.1.40.
 *
 *   login --qr --json   {"event":"qr_challenge","url":"https://s.team/q/1/<id>"}   re-emitted as the code rotates,
 *                       then {"event":"qr_scanned"} once the phone reads it, then the account's confirmation
 *   signed out          {"error":"not logged in - run `aurelia login` first","hint":"run `aurelia login`",
 *                        "type":"auth_required"}   (the error text is UTF-8, an em dash in 0.1.40)
 *   list --json         [{"app_id":105600,"name":"Terraria","playtime_forever_minutes":119,
 *                         "local_manifest_ids":{},"update_available":false}]
 *   libraries --json    {"libraries":[...]}
 *   an install          progress lines, then {"event":"result",...}
 *
 * A line that is not one of these is not an error: aurelia also prints human text on stderr, which the caller may
 * show as a status line. Nothing here allocates; every buffer is in the struct, so a launcher loop can parse on the
 * spot. */

typedef enum {
    PW_AURELIA_EVENT_NONE = 0,
    PW_AURELIA_EVENT_QR_CHALLENGE,  /* show url as a QR code; the phone scans it */
    PW_AURELIA_EVENT_QR_SCANNED,    /* {"event":"qr_scanned"}: the phone read it; the app confirms next */
    PW_AURELIA_EVENT_AUTH_REQUIRED, /* not signed in: offer the login */
    PW_AURELIA_EVENT_RESULT,        /* a command finished; result_ok says whether it did */
    PW_AURELIA_EVENT_ERROR,         /* an error that is not about the session */
    PW_AURELIA_EVENT_SIGNED_IN,     /* {"account":"...","logged_in":true}: the QR was approved */
    PW_AURELIA_EVENT_PROGRESS,      /* an install or update: {"state":"downloading","percent":29.6,...} */
} PwAureliaEventKind;

typedef struct PwAureliaEvent {
    PwAureliaEventKind kind;
    char url[512];   /* QR challenge URL */
    char error[256]; /* the error text (UTF-8 copied as-is) */
    char hint[128];  /* the error's "hint", when there is one */
    int result_ok;   /* RESULT: the command's "ok", or its success "status" when "ok" is absent */
    char status[32];  /* RESULT: "installed", "success", ... when the client reports one (an install does) */
    char account[64]; /* SIGNED_IN: the account the client signed in to */
    int percent;      /* PROGRESS: 0..100, or -1 */
    char state[32];   /* PROGRESS: "downloading", "installing", ... */
} PwAureliaEvent;

typedef struct PwAureliaGame {
    uint32_t app_id;
    char name[128];
    uint32_t playtime_minutes;
    int update_available;
} PwAureliaGame;

/* One JSON line (the newline may be present). 1 when it is a recognised event, 0 for anything else. */
int pw_aurelia_parse_event(const char *line, PwAureliaEvent *out);
/* A `list --json` document: up to cap games, the count written, or -1 when it is not the expected array. */
int pw_aurelia_parse_library(const char *json, PwAureliaGame *games, size_t cap);
/* A `libraries --json` document: the number of library folders, or -1 when it is not that document. */
int pw_aurelia_parse_libraries(const char *json);

#endif
