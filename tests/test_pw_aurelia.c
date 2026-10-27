/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The lines below were captured from aurelia 0.1.40 (the Pro console and this host, 2026-10-22), so a change in the
 * client's JSON breaks the test the same way it breaks the launcher. */
#include "../src/pw_aurelia.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    PwAureliaEvent event;
    PwAureliaGame games[4];
    const char *library =
        "[{\"app_id\": 105600, \"name\": \"Terraria\", \"playtime_forever_minutes\": 119, "
        "\"local_manifest_ids\": {}, \"update_available\": false},"
        " {\"app_id\": 1245620, \"name\": \"ELDEN RING\", \"playtime_forever_minutes\": 0, "
        "\"local_manifest_ids\": {}, \"update_available\": true}]";

    assert(pw_aurelia_parse_event("{\"event\":\"qr_challenge\",\"url\":\"https://s.team/q/1/1803213555507675877\"}\n",
                                  &event) == 1);
    assert(event.kind == PW_AURELIA_EVENT_QR_CHALLENGE);
    assert(!strcmp(event.url, "https://s.team/q/1/1803213555507675877"));
    /* Captured on the Pro (2026-10-22): the phone read the code, and the account's confirmation follows. */
    assert(pw_aurelia_parse_event("{\"event\":\"qr_scanned\"}\n", &event) == 1);
    assert(event.kind == PW_AURELIA_EVENT_QR_SCANNED);
    /* An install's progress, in the client's own shape: it was unreachable in the parser until 2026-10-22 (the
     * event-string branch returned before its check), so every one was logged as a line the client does not know. */
    assert(pw_aurelia_parse_event("{\"event\":\"progress\",\"state\":\"downloading\",\"percent\":29.6,"
                                  "\"bytes_downloaded\":609705868,\"total_bytes\":2000000000}\n", &event) == 1);
    assert(event.kind == PW_AURELIA_EVENT_PROGRESS && event.percent == 29 && !strcmp(event.state, "downloading"));

    assert(pw_aurelia_parse_event("{\"error\":\"not logged in \xe2\x80\x94 run `aurelia login` first\","
                                  "\"hint\":\"run `aurelia login`\",\"type\":\"auth_required\"}",
                                  &event) == 1);
    assert(event.kind == PW_AURELIA_EVENT_AUTH_REQUIRED);
    assert(strstr(event.error, "logged in"));
    assert(!strcmp(event.hint, "run `aurelia login`"));

    assert(pw_aurelia_parse_event("{\"event\":\"result\",\"ok\":true}", &event) == 1);
    assert(event.kind == PW_AURELIA_EVENT_RESULT && event.result_ok == 1);
    assert(pw_aurelia_parse_event("{\"event\":\"result\",\"ok\":false}", &event) == 1);
    assert(event.kind == PW_AURELIA_EVENT_RESULT && !event.result_ok);
    /* The install's result, captured from aurelia 0.1.40 after the 2D game reached 100% (2026-10-22): no "ok" field, the
     * success is "status":"installed". Until now it was read as a failure and the screen showed an error. */
    assert(pw_aurelia_parse_event("{\"app_id\":250900,\"dlc\":false,\"event\":\"result\",\"status\":\"installed\","
                                  "\"steam_restart_required\":false,\"steam_restarted\":false}", &event) == 1);
    assert(event.kind == PW_AURELIA_EVENT_RESULT && event.result_ok && !strcmp(event.status, "installed"));
    assert(pw_aurelia_parse_event("{\"error\":\"download failed\"}", &event) == 1);
    assert(event.kind == PW_AURELIA_EVENT_ERROR && !strcmp(event.error, "download failed"));

    assert(pw_aurelia_parse_event("wine: a human line on stderr", &event) == 0);
    assert(pw_aurelia_parse_event("", &event) == 0);
    assert(pw_aurelia_parse_event("[]", &event) == 0);

    assert(pw_aurelia_parse_library(library, games, 4) == 2);
    assert(games[0].app_id == 105600 && !strcmp(games[0].name, "Terraria") && games[0].playtime_minutes == 119 &&
           !games[0].update_available);
    assert(games[1].app_id == 1245620 && !strcmp(games[1].name, "ELDEN RING") && games[1].update_available);
    /* Signed in, list --json is pretty-printed, and the overlay accumulates it into one document before parsing
     * (native/proton_wine.cpp aurelia_feed; the documents below are shaped like the console's 2026-10-22 answer). */
    {
        const char *pretty_document =
            "[\n  {\n    \"active_branch\": \"public\",\n    \"app_id\": 761890,\n    \"name\": \"Songs of Silence\",\n"
            "    \"update_available\": false\n  },\n  {\n    \"app_id\": 1145360,\n    \"name\": \"Another Game\",\n"
            "    \"update_available\": true\n  }\n]";
        assert(pw_aurelia_parse_library(pretty_document, games, 4) == 2);
        assert(games[0].app_id == 761890 && !strcmp(games[0].name, "Songs of Silence"));
        assert(games[1].app_id == 1145360 && games[1].update_available);
    }
    /* A library of 300 games, shaped like the console's pretty list (cover URLs, branches, braces inside names): the first
     * version parsed the whole document with 4096 tokens and failed from 96 games on. */
    {
        static char big[300 * 400 + 16];
        static PwAureliaGame many[512];
        size_t used = 0;
        used += (size_t)snprintf(big + used, sizeof(big) - used, "[\n");
        for (int i = 0; i < 300; i++)
            used += (size_t)snprintf(big + used, sizeof(big) - used,
                "%s  {\n    \"active_branch\": \"public\",\n    \"app_id\": %d,\n    \"name\": \"Game {%d} \\\"x\\\"\",\n"
                "    \"cover\": \"https://cdn.example/%d/library_600x900.jpg\",\n    \"branches\": [\"public\", \"beta\"],\n"
                "    \"playtime_forever_minutes\": %d,\n    \"update_available\": %s\n  }",
                i ? ",\n" : "", 1000 + i, i, i, i * 3, i % 7 == 0 ? "true" : "false");
        snprintf(big + used, sizeof(big) - used, "\n]");
        assert(pw_aurelia_parse_library(big, many, 512) == 300);
        assert(many[0].app_id == 1000 && many[299].app_id == 1299 && many[299].playtime_minutes == 897);
        assert(!strcmp(many[5].name, "Game {5} \\\"x\\\"") && many[7].update_available && !many[8].update_available);
        assert(pw_aurelia_parse_library(big, many, 10) == 10);              /* the cap still holds */
        big[strlen(big) - 4] = 0;                                           /* cut inside the last object */
        assert(pw_aurelia_parse_library(big, many, 512) == -1);
    }
    assert(pw_aurelia_parse_library("[{\"app_id\":1,\"name\":\"A\"}", games, 4) == -1);   /* no closing bracket */
    assert(pw_aurelia_parse_library("[{\"app_id\":1,\"name\":\"A\"},]", games, 4) == 1);
    assert(pw_aurelia_parse_library("[{\"name\":\"no id\"},{\"app_id\":2,\"name\":\"B\"}]", games, 4) == 1 && games[0].app_id == 2);
    assert(pw_aurelia_parse_library("[]", games, 4) == 0);
    assert(pw_aurelia_parse_library("{}", games, 4) == -1);
    /* A library longer than the buffer keeps the first entries and reports only what it wrote. */
    assert(pw_aurelia_parse_library(library, games, 1) == 1);

    assert(pw_aurelia_parse_libraries("{\"libraries\": []}") == 0);
    assert(pw_aurelia_parse_libraries("[]") == -1);

    printf("pw_aurelia: %d games parsed from the captured document\n", 2);
    return 0;
}
