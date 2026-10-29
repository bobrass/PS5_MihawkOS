/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The payload checklist (pw_payload_check.h). */
#include "pw_payload_check.h"
#include "pw_memory_helper.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const known_names[PW_PAYLOAD_KNOWN] = {
    "elfldr", "kstuff", "shadowmountplus", "websrv", "nanodns", "klogsrv", "ftpsrv", "pldmgr"
};
static const char *const labels[PW_PAYLOAD_KNOWN] = {
    "elfldr", "Kstuff Lite 1.11 Beta or later", "ShadowMountPlus 1.7beta10 or later", "websrv", "NanoDNS",
    "klog", "ftpsrv", "pldmgr"
};
/* MihawkOS's own: the Lapy daemon, the helpers' loader and helpers (in Sony's carrier, eboot.bin), the tiles' worker,
 * the test harness's control payload, and the PC tools' read-only samplers (tools/console-*.py). */
static const char *const ours[] = { "lapy-root-daemon", "proton-payload-child", "child", "worker", "ps5vkctl", "eboot.bin",
                                    "console-pagetables", "console-memory", "console-processes" };

/* name without a trailing ".elf" equals want */
static int named(const char *name, const char *want)
{
    const size_t n = strlen(want);
    return !strncmp(name, want, n) && (!name[n] || !strcmp(name + n, ".elf"));
}

const char *pw_payload_label(int which)
{
    return which >= 0 && which < PW_PAYLOAD_KNOWN ? labels[which] : "";
}

int pw_shadowmount_new_enough(const char *version)
{
    char *end;
    if (!version) return 0;
    if (*version == 'v' || *version == 'V') version++;
    if (*version < '0' || *version > '9') return 0;
    const unsigned long major = strtoul(version, &end, 10);
    if (*end != '.' || end[1] < '0' || end[1] > '9') return 0;
    const unsigned long minor = strtoul(end + 1, &end, 10);
    if (major != 1) return major > 1;
    if (minor != 7) return minor > 7;
    if (strncmp(end, "beta", 4)) return 1;   /* 1.7 itself, or 1.7.x: a release */
    if (end[4] < '0' || end[4] > '9') return 0;
    return strtoul(end + 4, NULL, 10) >= 10;
}

void pw_payload_check(const PwPayloadProcess *list, unsigned count, int self, const PwPayloadFacts *facts,
                      PwPayloadReport *out)
{
    memset(out, 0, sizeof(*out));
    int syscore = 0, websrv = 0;
    for (unsigned i = 0; i < count; ++i) {
        if (!strncmp(list[i].name, "SceSysCore", 10)) syscore = list[i].pid;
        if (named(list[i].name, "websrv")) websrv = list[i].pid;
    }
    /* A tester's title without root listed itself alone ("processes=1") and called every payload missing (2026-10-29) */
    out->listed = facts->listed && syscore > 0;
    if (!out->listed) {
        out->running[PW_PAYLOAD_SHADOWMOUNT] = facts->shadowmount_version[0] != 0;
        out->running[PW_PAYLOAD_WEBSRV] = facts->websrv_version[0] != 0;
    }
    for (unsigned i = 0; out->listed && i < count; ++i) {
        const PwPayloadProcess *p = &list[i];
        if (p->pid == self || p->ppid == self || p->pid == syscore || !pw_memory_is_payload(p->ppid, syscore, p->name))
            continue;
        int mine = 0;
        for (size_t k = 0; k < sizeof(ours) / sizeof(ours[0]); ++k) mine |= named(p->name, ours[k]);
        /* The Lapy daemon, which the title starts through websrv as lapy-root-daemon.elf, is listed as "payload"
         * (payload-quick-2026-10-29-02, its log's start and donor pids). shortcut: any websrv child named "payload"
         * passes; websrv names an upload after its file, so a user's would need that very name. */
        mine |= websrv > 0 && p->ppid == websrv && named(p->name, "payload");
        if (mine) continue;
        out->payload_resident += p->resident;
        int which = -1;
        for (int k = 0; k < PW_PAYLOAD_KNOWN && which < 0; ++k)
            if (named(p->name, known_names[k])) which = k;
        if (which >= 0) {
            out->running[which]++;
            out->resident[which] += p->resident;
            continue;
        }
        if (out->others < PW_PAYLOAD_OTHERS) {
            snprintf(out->other_names[out->others], sizeof(out->other_names[0]), "%s", p->name);
            out->other_each[out->others] = p->resident;
        }
        out->others++;
        out->other_resident += p->resident;
    }
    out->ok[PW_PAYLOAD_ELFLDR] = out->running[PW_PAYLOAD_ELFLDR] > 0;
    /* kstuff lives in the kernel once loaded: its kekcalls are the proof, its process only a sign */
    out->ok[PW_PAYLOAD_KSTUFF] = facts->kstuff == PW_KSTUFF_OK;
    out->ok[PW_PAYLOAD_SHADOWMOUNT] = out->running[PW_PAYLOAD_SHADOWMOUNT] > 0 &&
                                      pw_shadowmount_new_enough(facts->shadowmount_version);
    out->ok[PW_PAYLOAD_WEBSRV] = out->running[PW_PAYLOAD_WEBSRV] > 0;
    out->ok[PW_PAYLOAD_NANODNS] = out->running[PW_PAYLOAD_NANODNS] > 0;
    out->pass = out->listed && !out->others;
    for (int k = 0; k < PW_PAYLOAD_REQUIRED; ++k) out->pass &= out->ok[k];
}

static void add(char *out, size_t size, size_t *used, const char *text)
{
    const int n = snprintf(out + *used, *used < size ? size - *used : 0, "%s%s", *used ? " " : "", text);
    if (n > 0) *used += (size_t)n;
}

size_t pw_payload_advice(const PwPayloadReport *r, const PwPayloadFacts *facts, char *out, size_t size)
{
    char line[400];
    size_t used = 0;
    if (size) out[0] = 0;
    if (!r->listed) {
        if (facts->data_denied)
            add(out, size, &used, "MihawkOS could not get access to /data, so it cannot check your payloads. Check that "
                "/data/homebrew/dev.conf holds the two lines DEV_SERVER=127.0.0.1 and DEV_PORT=8080, then restart MihawkOS.");
        else
            add(out, size, &used, "MihawkOS could not see which payloads are running. Restart the PS5, load the payloads "
                "listed here, then open MihawkOS again.");
        return used < size ? used : size - 1;
    }
    int missing = 0, restart = r->others > 0;
    for (int k = 0; k < PW_PAYLOAD_REQUIRED; ++k) missing += !r->ok[k] && !r->running[k];
    if (missing) {
        int n = snprintf(line, sizeof(line), "Load");
        for (int k = 0, said = 0; k < PW_PAYLOAD_REQUIRED; ++k)
            if (!r->ok[k] && !r->running[k] && !(k == PW_PAYLOAD_KSTUFF && facts->kstuff == PW_KSTUFF_OLD))
                n += snprintf(line + n, sizeof(line) - (size_t)n, "%s %s", said++ ? "," : "", labels[k]);
        if (n > 4) {
            snprintf(line + n, sizeof(line) - (size_t)n, ", then press CROSS to check again.");
            add(out, size, &used, line);
        }
    }
    if (facts->kstuff == PW_KSTUFF_OLD) {
        add(out, size, &used, "Your Kstuff is older than Kstuff Lite 1.11 Beta.");
        restart = 1;
    } else if (facts->kstuff == PW_KSTUFF_ABSENT && r->running[PW_PAYLOAD_KSTUFF]) {
        add(out, size, &used, "Kstuff is running but does not answer.");
        restart = 1;
    }
    if (r->running[PW_PAYLOAD_SHADOWMOUNT] && !r->ok[PW_PAYLOAD_SHADOWMOUNT]) {
        if (facts->shadowmount_version[0])
            snprintf(line, sizeof(line), "Your ShadowMountPlus is %s, too old: MihawkOS needs 1.7beta10 or later.",
                     facts->shadowmount_version);
        else
            snprintf(line, sizeof(line), "ShadowMountPlus is running but does not say its version: turn its web API on "
                     "(api_enabled=1), or use 1.7beta10 or later.");
        add(out, size, &used, line);
        restart = 1;
    }
    if (r->others) {
        int n = snprintf(line, sizeof(line), "%u other payload%s %s running, using %llu MiB that games need:", r->others,
                         r->others == 1 ? "" : "s", r->others == 1 ? "is" : "are",
                         (unsigned long long)((r->other_resident + (1u << 19)) >> 20));
        for (unsigned k = 0; k < r->others && k < PW_PAYLOAD_OTHERS; ++k)
            n += snprintf(line + n, sizeof(line) - (size_t)n, "%s %s", k ? "," : "", r->other_names[k]);
        snprintf(line + n, sizeof(line) - (size_t)n, "%s.", r->others > PW_PAYLOAD_OTHERS ? " and more" : "");
        add(out, size, &used, line);
    }
    if (restart)
        add(out, size, &used, "Restart the PS5, then load only the payloads listed here: a restart is the sure way to "
            "close or replace a payload.");
    return used < size ? used : size - 1;
}
