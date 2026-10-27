/* SPDX-License-Identifier: GPL-3.0-or-later
 * MihawkOS - what a game forwarder tile (forwarder/tile.c) and its launch payload (forwarder/launch.c) share. The
 * design is PS5_RetroArch's (the same owner's forwarder/, GPL-3.0-or-later): a tile cannot start another title, so it
 * writes its own title id over FORWARDER_SLOT_MARKER in the payload's bytes and sends them to the console's ELF
 * loader; the payload reads that tile's forward.txt and starts MihawkOS with it. No file is written for the hand-over,
 * so two tiles cannot race.
 *
 * Tiles are PPSA99300 to PPSA99399: clear of MihawkOS (PPSA99170), of its memory probe (PPSA99171, the tester probe
 * title) and of PS5_RetroArch's tiles, whose range begins at MihawkOS's own id and so holds both (the owner asked to
 * keep the two apart, 2026-10-26). */
#ifndef MIHAWK_FORWARDER_H
#define MIHAWK_FORWARDER_H

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define FORWARDER_SLOT_MARKER "MIHAWK-FORWARDER-TILE-ID-SLOT"
#define FORWARDER_SLOT_SIZE 32
#define FORWARDER_ID_SIZE 10   /* four capitals, five digits and the NUL */
#define FORWARDER_MAX_ARGUMENTS 8
#define FORWARDER_MAX_ARGUMENT 512

static inline int forwarder_valid_id(const char *id, size_t length)
{
    if (!id || length != 9) return 0;
    for (size_t i = 0; i < 9; i++)
        if (i < 4 ? !(id[i] >= 'A' && id[i] <= 'Z') : !(id[i] >= '0' && id[i] <= '9')) return 0;
    return 1;
}

/* ShadowMountPlus's answers, as forwarder/worker.c reads them (its API: a JSON object whose "status" is 0 on success):
 * the status, or -1 when the answer has none. */
static inline int forwarder_reply_status(const char *reply)
{
    const char *at = reply ? strstr(reply, "\"status\"") : NULL;
    if (!at) return -1;
    at += 8;
    while (*at == ' ' || *at == '\t' || *at == '\n' || *at == '\r' || *at == ':') at++;
    int sign = 1, value = 0, digits = 0;
    if (*at == '-') sign = -1, at++;
    for (; *at >= '0' && *at <= '9' && digits < 9; at++, digits++) value = value * 10 + (*at - '0');
    return digits ? sign * value : -1;
}
/* 1 when its game list holds this title id as installed ("installed": true in the same object, before or after the
 * id), else 0. */
static inline int forwarder_reply_installed(const char *reply, const char *id)
{
    char quoted[FORWARDER_ID_SIZE + 2];
    snprintf(quoted, sizeof quoted, "\"%s\"", id);
    for (const char *at = reply ? strstr(reply, quoted) : NULL; at; at = strstr(at + 1, quoted)) {
        const char *open = at, *close = strchr(at, '}');
        while (open > reply && *open != '{') open--;
        if (!close || *open != '{') continue;
        for (const char *field = strstr(open, "\"installed\""); field && field < close; field = strstr(field + 1, "\"installed\"")) {
            const char *value = field + 11;
            while (*value == ' ' || *value == ':') value++;
            if (!strncmp(value, "true", 4)) return 1;
        }
    }
    return 0;
}

int forwarder_title_id(const char *param, char id[FORWARDER_ID_SIZE]);
int forwarder_stamp(char *payload, long size, const char *id);

#endif
