/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PW_PAYLOAD_CHECK_H
#define PW_PAYLOAD_CHECK_H
/*
 * The payload checklist: MihawkOS runs only with the official payload set, and nothing more (the owner, 2026-10-29: most
 * users' problems came from other payloads, and every payload holds memory games need). Every start takes it
 * (ps5/src/proton_payloads.cpp gathers the facts; the launcher shows it and refuses entry until it passes; a game start
 * that fails it returns to the launcher).
 *
 *   required  elfldr, Kstuff Lite 1.11 Beta or later, ShadowMountPlus 1.7beta10 or later, websrv, NanoDNS
 *   optional  klogsrv, ftpsrv, pldmgr (the owner, 2026-10-29: pldmgr is allowed)
 *   ours      what MihawkOS starts itself: the Lapy daemon (listed as "payload", websrv's child), its helpers, the
 *             tiles' worker, the harness's ps5vkctl
 * Any other payload fails it.
 *
 * How each is known, measured on the base PS5 (CFI-1215A, 13.60) with the official set, 2026-10-29
 * (evidence/payloads-base-2026-10-29-01.json): every payload is elfldr's child, by its file name (elfldr.elf, whose
 * parent is SceRedisServer; kstuff.elf, shadowmountplus.elf, websrv.elf, nanodns.elf, klogsrv.elf, ftpsrv.elf,
 * pldmgr.elf). Resident: shadowmountplus 25.2 MiB, elfldr 22.8, pldmgr 12.3, websrv 8.2, kstuff 7.7, nanodns 4.1,
 * ftpsrv 4.1, klogsrv 3.9. Versions:
 *   ShadowMountPlus  POST {} to 127.0.0.1:10101/api/v1/version answers "shadowmount_version":"1.7beta10". 1.7beta6 and
 *                    later keep ShellCore's hook pages resident (upstream 35fff9fc, the fix of the owner's PR 139); its
 *                    log said "hook pages retained: cave=1 sites=3" on the base, so launch-after-exit is fixed there.
 *   websrv           GET 127.0.0.1:8080/version answers {"tag": "v0.34", ...}; no minimum, shown only.
 *   Kstuff           by its process only. kstuff-lite has no version call; v1.11 is the first to answer kekcall 7, but
 *                    the title cannot ask: a raw system call from a title ends it (SYSTEM_ILLEGAL_FUNCTION_CALL,
 *                    payload-quick-2026-10-29-01). Its minimum version is shown, not checked; a helper could ask.
 * A payload is pw_memory_is_payload's (src/pw_memory_helper.h): not started by SceSysCore and not named "Sce...".
 */
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One process, as kern.proc lists it. */
typedef struct {
    int pid, ppid;
    char name[20];      /* ki_comm */
    uint64_t resident;  /* bytes */
} PwPayloadProcess;

/* The payloads the checklist knows: the required ones first. */
enum {
    PW_PAYLOAD_ELFLDR, PW_PAYLOAD_KSTUFF, PW_PAYLOAD_SHADOWMOUNT, PW_PAYLOAD_WEBSRV, PW_PAYLOAD_NANODNS,
    PW_PAYLOAD_REQUIRED,
    PW_PAYLOAD_KLOG = PW_PAYLOAD_REQUIRED, PW_PAYLOAD_FTP, PW_PAYLOAD_PLDMGR,
    PW_PAYLOAD_KNOWN
};

/* What kstuff's kekcalls said. */
enum { PW_KSTUFF_ABSENT, PW_KSTUFF_OLD, PW_KSTUFF_OK };

/* What the title found besides the processes. A version is "" when nothing answered. */
typedef struct {
    int listed;                    /* 1 when kern.proc answered; 0 fails the check (nothing can be seen) */
    int kstuff;                    /* PW_KSTUFF_...: OK when its process runs (the title cannot tell OLD) */
    char shadowmount_version[32];
    char websrv_version[32];
} PwPayloadFacts;

#define PW_PAYLOAD_OTHERS 8
typedef struct {
    int running[PW_PAYLOAD_KNOWN];
    uint64_t resident[PW_PAYLOAD_KNOWN];
    int ok[PW_PAYLOAD_REQUIRED];             /* running, and new enough where a version is required */
    unsigned others;                          /* other payloads (all of them counted; the first PW_PAYLOAD_OTHERS named) */
    char other_names[PW_PAYLOAD_OTHERS][20];
    uint64_t other_each[PW_PAYLOAD_OTHERS];   /* bytes, each named one's */
    uint64_t other_resident;                  /* bytes, all of them */
    uint64_t payload_resident;                /* bytes: every payload's, known or not, ours left out */
    int listed;
    int pass;
} PwPayloadReport;

/* The report for these processes (self: the title's pid; its children are its own). */
void pw_payload_check(const PwPayloadProcess *list, unsigned count, int self, const PwPayloadFacts *facts,
                      PwPayloadReport *out);
/* The payload's name for the screen, with its minimum version where one is required. */
const char *pw_payload_label(int which);
/* 1 when version (as ShadowMountPlus says it: "1.7beta10", "1.7", "v1.8beta2-3-gabc") is 1.7beta10 or later. A release
 * is later than its betas. 0 for anything else, unreadable included. */
int pw_shadowmount_new_enough(const char *version);
/* What the user must do, in plain words, one sentence per problem; "" when the check passed. Returns the length. */
size_t pw_payload_advice(const PwPayloadReport *report, const PwPayloadFacts *facts, char *out, size_t size);

#ifdef __cplusplus
}
#endif
#endif
