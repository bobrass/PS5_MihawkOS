/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The payload checklist (src/pw_payload_check.h) on the base PS5's process table of 2026-10-29 (the official set,
 * evidence/payloads-base-2026-10-29-01.json), and on what users load besides. */
#include "../src/pw_payload_check.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define MIB(x) ((uint64_t)((x) * 1048576.0))
static PwPayloadProcess base[] = {
    { 53, 1, "SceSysCore.elf", 0 },     { 57, 53, "SceShellCore", 0 },   { 69, 53, "SceRedisServer", 0 },
    { 75, 53, "fs_cleaner.elf", MIB(2.7) },   /* SceSysCore's: not a payload */
    { 1, 0, "mini-syscore.elf", 0 },     { 0, 0, "kernel", 0 },
    { 87, 69, "elfldr.elf", MIB(22.8) }, { 93, 87, "pldmgr.elf", MIB(12.3) }, { 97, 87, "nanodns.elf", MIB(4.1) },
    { 100, 87, "kstuff.elf", MIB(7.7) }, { 101, 87, "shadowmountplus.elf", MIB(25.2) },
    { 102, 87, "websrv.elf", MIB(8.2) }, { 103, 87, "klogsrv.elf", MIB(3.9) }, { 104, 87, "ftpsrv.elf", MIB(4.1) },
    /* MihawkOS's own: the title, its daemon through websrv, a helper in its carrier */
    { 120, 53, "eboot.bin", MIB(300) }, { 121, 102, "payload", MIB(3.9) }, { 122, 102, "eboot.bin", MIB(90) },
    { 123, 102, "console-pagetables", MIB(3) },   /* the PC's page-table sampler, websrv's child */
};
enum { BASE = sizeof(base) / sizeof(base[0]), SELF = 120 };

static PwPayloadFacts good(void)
{
    PwPayloadFacts f = { 1, PW_KSTUFF_OK, "1.7beta10", "v0.34" };
    return f;
}

static void advise(const PwPayloadReport *r, const PwPayloadFacts *f, char *out)
{
    pw_payload_advice(r, f, out, 1024);
    printf("  advice: %s\n", out[0] ? out : "(none)");
}

int main(void)
{
    PwPayloadReport r;
    PwPayloadFacts f = good();
    char advice[1024];

    assert(pw_shadowmount_new_enough("1.7beta10") && pw_shadowmount_new_enough("1.7beta11"));
    assert(pw_shadowmount_new_enough("v1.7beta12-3-g8489dbe") && pw_shadowmount_new_enough("1.7"));
    assert(pw_shadowmount_new_enough("1.8beta1") && pw_shadowmount_new_enough("2.0"));
    assert(!pw_shadowmount_new_enough("1.7beta9") && !pw_shadowmount_new_enough("1.7beta6-fix2"));
    assert(!pw_shadowmount_new_enough("1.7beta4") && !pw_shadowmount_new_enough("1.6"));
    assert(!pw_shadowmount_new_enough("") && !pw_shadowmount_new_enough(NULL) && !pw_shadowmount_new_enough("unknown"));
    assert(!pw_shadowmount_new_enough("1.7beta"));

    /* The owner's base: passes, the optional three counted, ours and Sony's left out. */
    pw_payload_check(base, BASE, SELF, &f, &r);
    assert(r.pass && !r.others);
    for (int k = 0; k < PW_PAYLOAD_KNOWN; ++k) assert(r.running[k] == 1);
    assert(r.payload_resident == MIB(22.8) + MIB(12.3) + MIB(4.1) + MIB(7.7) + MIB(25.2) + MIB(8.2) + MIB(3.9) + MIB(4.1));
    advise(&r, &f, advice);
    assert(!advice[0]);

    /* Without the optional ones it still passes. */
    PwPayloadProcess bare[BASE];
    unsigned n = 0;
    for (unsigned i = 0; i < BASE; ++i)
        if (strcmp(base[i].name, "klogsrv.elf") && strcmp(base[i].name, "ftpsrv.elf") && strcmp(base[i].name, "pldmgr.elf"))
            bare[n++] = base[i];
    pw_payload_check(bare, n, SELF, &f, &r);
    assert(r.pass && !r.running[PW_PAYLOAD_KLOG]);

    /* An extra payload refuses entry and is named, with its memory. */
    PwPayloadProcess extra[BASE + 2];
    memcpy(extra, base, sizeof(base));
    extra[BASE] = (PwPayloadProcess){ 130, 87, "etaHEN.elf", MIB(40) };
    extra[BASE + 1] = (PwPayloadProcess){ 131, 87, "payload", MIB(3) };   /* sent to elfldr itself: not ours */
    pw_payload_check(extra, BASE + 2, SELF, &f, &r);
    assert(!r.pass && r.others == 2 && r.other_resident == MIB(40) + MIB(3));
    assert(!strcmp(r.other_names[0], "etaHEN.elf") && !strcmp(r.other_names[1], "payload"));
    assert(r.other_each[0] == MIB(40) && r.other_each[1] == MIB(3));
    advise(&r, &f, advice);
    assert(strstr(advice, "2 other payloads are running, using 43 MiB") && strstr(advice, "etaHEN.elf, payload."));
    assert(strstr(advice, "Restart the PS5"));

    /* Missing ones are named to load. */
    PwPayloadProcess few[] = { { 53, 1, "SceSysCore.elf", 0 }, { 69, 53, "SceRedisServer", 0 },
                               { 87, 69, "elfldr.elf", 0 }, { 102, 87, "websrv.elf", 0 } };
    f.kstuff = PW_KSTUFF_ABSENT;
    f.shadowmount_version[0] = 0;
    pw_payload_check(few, 4, SELF, &f, &r);
    assert(!r.pass && r.ok[PW_PAYLOAD_ELFLDR] && r.ok[PW_PAYLOAD_WEBSRV] && !r.ok[PW_PAYLOAD_KSTUFF]);
    advise(&r, &f, advice);
    assert(strstr(advice, "Load Kstuff Lite 1.11 Beta or later, ShadowMountPlus 1.7beta10 or later, NanoDNS, then press CROSS"));
    assert(!strstr(advice, "Restart"));

    /* Too old: restart, as nothing can be unloaded. */
    f = good();
    snprintf(f.shadowmount_version, sizeof(f.shadowmount_version), "1.7beta4");
    f.kstuff = PW_KSTUFF_OLD;
    pw_payload_check(base, BASE, SELF, &f, &r);
    assert(!r.pass && !r.ok[PW_PAYLOAD_SHADOWMOUNT] && !r.ok[PW_PAYLOAD_KSTUFF]);
    advise(&r, &f, advice);
    assert(strstr(advice, "older than Kstuff Lite 1.11") && strstr(advice, "is 1.7beta4, too old") && strstr(advice, "Restart the PS5"));
    assert(!strstr(advice, "Load"));

    /* The Pro's of 2026-10-29 (alpha5-pro gate): the owner's PR build, too old by its version */
    f = good();
    snprintf(f.shadowmount_version, sizeof(f.shadowmount_version), "1.7beta4-hook-lifetime");
    pw_payload_check(base, BASE, SELF, &f, &r);
    assert(!r.pass && r.running[PW_PAYLOAD_SHADOWMOUNT] && !r.ok[PW_PAYLOAD_SHADOWMOUNT]);
    advise(&r, &f, advice);
    assert(strstr(advice, "is 1.7beta4-hook-lifetime, too old"));

    /* ShadowMountPlus without its API: no version, not new enough. */
    f = good();
    f.shadowmount_version[0] = 0;
    pw_payload_check(base, BASE, SELF, &f, &r);
    assert(!r.pass);
    advise(&r, &f, advice);
    assert(strstr(advice, "does not say its version"));

    /* kstuff's process there but no kekcall answers. */
    f = good();
    f.kstuff = PW_KSTUFF_ABSENT;
    pw_payload_check(base, BASE, SELF, &f, &r);
    advise(&r, &f, advice);
    assert(!r.pass && strstr(advice, "does not answer"));

    /* Nothing listed fails, never passes blind. */
    f = good();
    f.listed = 0;
    pw_payload_check(NULL, 0, SELF, &f, &r);
    assert(!r.pass);
    advise(&r, &f, advice);
    assert(strstr(advice, "could not see"));

    puts("test_pw_payload_check: ok");
    return 0;
}
