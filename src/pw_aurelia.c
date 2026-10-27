/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pw_aurelia.h"
#include "pw_jsmn.h"

#include <string.h>

/* A signed-in list --json is a pretty-printed array whose objects carry cover URLs and branches: hundreds of tokens
 * for a dozen games. The array is shared, as the caller's one thread (native/proton_wine.cpp holds its lock around
 * the call) parses one document at a time. */
#define TOKENS 4096

static jsmntok_t shared_tokens[TOKENS];

static int parse_n(const char *json, size_t length, jsmntok_t *tokens)
{
    jsmn_parser parser;

    jsmn_init(&parser);
    return jsmn_parse(&parser, json, length, tokens, TOKENS);
}

static int parse(const char *json, jsmntok_t *tokens)
{
    return parse_n(json, strlen(json), tokens);
}

/* The token after `at`'s whole subtree (jsmn's tokens are pre-order; an object's or array's size counts its children,
 * a string's or primitive's is zero). */
static int skip_token(const jsmntok_t *tokens, int count, int at)
{
    int children = tokens[at].size;

    at++;
    while (children > 0 && at < count) {
        children--;
        children += tokens[at].size;
        at++;
    }
    return at;
}

static int same(const char *json, const jsmntok_t *token, const char *name)
{
    return token->type == JSMN_STRING && (size_t)(token->end - token->start) == strlen(name) &&
           !strncmp(json + token->start, name, (size_t)(token->end - token->start));
}

/* The value token of key `name` in object token `object`, or -1. */
static int member(const char *json, const jsmntok_t *tokens, int count, int object, const char *name)
{
    int at = object + 1;

    for (int pair = 0; pair < tokens[object].size && at + 1 < count; pair++) {
        const int key = at, value = at + 1;
        if (same(json, &tokens[key], name)) return value;
        at = skip_token(tokens, count, value);
    }
    return -1;
}

static void copy_string(const char *json, const jsmntok_t *token, char *out, size_t cap)
{
    size_t length = cap ? cap - 1 : 0;

    if (token->type != JSMN_STRING && token->type != JSMN_PRIMITIVE) return;
    if (length > (size_t)(token->end - token->start)) length = (size_t)(token->end - token->start);
    memcpy(out, json + token->start, length);
    out[length] = 0;
}

static int copy_uint(const char *json, const jsmntok_t *token, uint32_t *out)
{
    uint64_t value = 0;
    int digits = 0;

    if (token->type != JSMN_PRIMITIVE) return 0;
    for (int at = token->start; at < token->end; at++) {
        if (json[at] < '0' || json[at] > '9' || ++digits > 10) return 0;
        value = value * 10 + (uint64_t)(json[at] - '0');
    }
    *out = (uint32_t)value;
    return digits > 0;
}

static int is_true(const char *json, const jsmntok_t *token)
{
    return token->type == JSMN_PRIMITIVE && token->end - token->start == 4 && !strncmp(json + token->start, "true", 4);
}

int pw_aurelia_parse_event(const char *line, PwAureliaEvent *out)
{
    jsmntok_t *tokens = shared_tokens;
    int event, url, error, hint, ok, status;

    if (!out) return 0;
    memset(out, 0, sizeof(*out));
    if (!line || parse(line, tokens) < 1 || tokens[0].type != JSMN_OBJECT) return 0;

    event = member(line, tokens, TOKENS, 0, "event");
    if (event >= 0 && tokens[event].type == JSMN_STRING) {
        if (same(line, &tokens[event], "qr_challenge")) {
            url = member(line, tokens, TOKENS, 0, "url");
            if (url < 0) return 0;
            out->kind = PW_AURELIA_EVENT_QR_CHALLENGE;
            copy_string(line, &tokens[url], out->url, sizeof(out->url));
            return 1;
        }
        if (same(line, &tokens[event], "qr_scanned")) {
            out->kind = PW_AURELIA_EVENT_QR_SCANNED;
            return 1;
        }
        if (same(line, &tokens[event], "result")) {
            ok = member(line, tokens, TOKENS, 0, "ok");
            status = member(line, tokens, TOKENS, 0, "status");
            out->kind = PW_AURELIA_EVENT_RESULT;
            // An install's result carries no "ok": {"event":"result","status":"installed"}. Treat the client's own
            // success words as success; anything else without ok=true is a failure (it was logged as one until
            // 2026-10-22, after the 2D game had installed).
            out->result_ok = (ok >= 0 && is_true(line, &tokens[ok])) ||
                (status >= 0 && tokens[status].type == JSMN_STRING &&
                 (same(line, &tokens[status], "installed") || same(line, &tokens[status], "success") ||
                  same(line, &tokens[status], "ok") || same(line, &tokens[status], "completed") ||
                  same(line, &tokens[status], "done")));
            if (status >= 0) copy_string(line, &tokens[status], out->status, sizeof(out->status));
            return 1;
        }
        // An install's progress: {"event":"progress","state":"downloading","percent":29.6,...}. This branch was below
        // the event-string block and so unreachable until 2026-10-22: every progress line was "not known".
        if (same(line, &tokens[event], "progress")) {
            const int percent = member(line, tokens, TOKENS, 0, "percent"), state = member(line, tokens, TOKENS, 0, "state");
            out->kind = PW_AURELIA_EVENT_PROGRESS;
            out->percent = -1;
            if (percent >= 0 && tokens[percent].type == JSMN_PRIMITIVE) {
                int value = 0, digits = 0;
                for (int at = tokens[percent].start; at < tokens[percent].end && line[at] >= '0' && line[at] <= '9'; at++, digits++)
                    value = value * 10 + (line[at] - '0');
                if (digits) out->percent = value;
            }
            if (state >= 0) copy_string(line, &tokens[state], out->state, sizeof(out->state));
            return 1;
        }
        return 0;
    }
    // The login's own result: {"account":"name","logged_in":true} (no event field).
    {
        const int logged = member(line, tokens, TOKENS, 0, "logged_in"), account = member(line, tokens, TOKENS, 0, "account");
        if (logged >= 0 && is_true(line, &tokens[logged])) {
            out->kind = PW_AURELIA_EVENT_SIGNED_IN;
            if (account >= 0) copy_string(line, &tokens[account], out->account, sizeof(out->account));
            return 1;
        }
    }
    error = member(line, tokens, TOKENS, 0, "error");
    if (error < 0 || tokens[error].type != JSMN_STRING) return 0;
    hint = member(line, tokens, TOKENS, 0, "hint");
    copy_string(line, &tokens[error], out->error, sizeof(out->error));
    if (hint >= 0) copy_string(line, &tokens[hint], out->hint, sizeof(out->hint));
    out->kind = strstr(out->error, "logged in") ? PW_AURELIA_EVENT_AUTH_REQUIRED : PW_AURELIA_EVENT_ERROR;
    return 1;
}

static const char *skip_space(const char *at)
{
    while (*at == ' ' || *at == '\t' || *at == '\r' || *at == '\n') at++;
    return at;
}

/* `at` is on an object's '{': the text after its matching '}', or NULL when the text ends first. Strings and their
 * escapes are skipped, so a brace in a name or a cover URL does not count. */
static const char *object_end(const char *at)
{
    int depth = 0, in_string = 0;

    for (; *at; at++) {
        if (in_string) {
            if (*at == '\\' && at[1]) at++;
            else if (*at == '"') in_string = 0;
        } else if (*at == '"') {
            in_string = 1;
        } else if (*at == '{' || *at == '[') {
            depth++;
        } else if ((*at == '}' || *at == ']') && --depth <= 0) {
            return depth == 0 ? at + 1 : NULL;
        }
    }
    return NULL;
}

/* One game object, parsed on its own. A library parsed as one document ran out of tokens from 96 games on (43 tokens
 * a game against TOKENS 4096) and the page stayed empty; one object at a time has no ceiling but the game's own size. */
static int read_game(const char *object, size_t length, PwAureliaGame *game)
{
    jsmntok_t *tokens = shared_tokens;
    int name, id, playtime, update;

    if (parse_n(object, length, tokens) < 1 || tokens[0].type != JSMN_OBJECT) return 0;
    name = member(object, tokens, TOKENS, 0, "name");
    id = member(object, tokens, TOKENS, 0, "app_id");
    playtime = member(object, tokens, TOKENS, 0, "playtime_forever_minutes");
    update = member(object, tokens, TOKENS, 0, "update_available");
    if (name < 0 || id < 0) return 0;
    memset(game, 0, sizeof(*game));
    if (!copy_uint(object, &tokens[id], &game->app_id)) return 0;
    copy_string(object, &tokens[name], game->name, sizeof(game->name));
    if (playtime >= 0) copy_uint(object, &tokens[playtime], &game->playtime_minutes);
    game->update_available = update >= 0 && is_true(object, &tokens[update]);
    return 1;
}

int pw_aurelia_parse_library(const char *json, PwAureliaGame *games, size_t cap)
{
    const char *at;
    int written = 0;

    if (!json) return -1;
    at = skip_space(json);
    if (*at++ != '[') return -1;
    for (;;) {
        const char *end;

        at = skip_space(at);
        if (*at == ',') at = skip_space(at + 1);
        if (*at == ']') return written;
        if (*at != '{') return -1;
        end = object_end(at);
        if (!end) return -1;   /* cut inside an object, or inside the array */
        if ((size_t)written < cap && read_game(at, (size_t)(end - at), &games[written])) written++;
        at = end;
    }
}

int pw_aurelia_parse_libraries(const char *json)
{
    jsmntok_t *tokens = shared_tokens;
    int libraries;

    if (!json || parse(json, tokens) < 1 || tokens[0].type != JSMN_OBJECT) return -1;
    libraries = member(json, tokens, TOKENS, 0, "libraries");
    if (libraries < 0 || tokens[libraries].type != JSMN_ARRAY) return -1;
    return tokens[libraries].size;
}
