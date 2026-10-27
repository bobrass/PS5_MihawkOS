/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The Steam guide's shared rules (src/pw_steam_guide.h): the failures' words, the record, where the launcher resumes,
 * and the sign-in session's screens over a first start shaped like the one measured on the Pro. */
#include "../src/pw_steam_guide.h"
#include "../src/pw_https.h"
#include "../src/pw_setup.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static PwSteamSessionInput second(unsigned seconds, unsigned long long bytes, unsigned processes)
{
    PwSteamSessionInput input = {0};
    input.seconds = seconds;
    input.package_bytes = bytes;
    input.processes = processes;
    return input;
}

int main(void)
{
    /* Downloads: no network, a wrong clock, everything else. */
    assert(pw_steam_guide_fetch_failure(PW_HTTPS_OK) == PW_STEAM_FAIL_NONE);
    assert(pw_steam_guide_fetch_failure(PW_HTTPS_DNS) == PW_STEAM_FAIL_OFFLINE);
    assert(pw_steam_guide_fetch_failure(PW_HTTPS_CONNECT) == PW_STEAM_FAIL_OFFLINE);
    assert(pw_steam_guide_fetch_failure(PW_HTTPS_CLOCK) == PW_STEAM_FAIL_CLOCK);
    assert(pw_steam_guide_fetch_failure(PW_HTTPS_STATUS) == PW_STEAM_FAIL_DOWNLOAD);
    assert(pw_steam_guide_fetch_failure(PW_HTTPS_TIMEOUT) == PW_STEAM_FAIL_DOWNLOAD);
    /* Every failure has its own words, in every language (test_pw_setup checks the words themselves). */
    for (int a = PW_STEAM_FAIL_OFFLINE; a <= PW_STEAM_FAIL_ENDED; ++a)
        for (int b = a + 1; b <= PW_STEAM_FAIL_ENDED; ++b)
            assert(pw_steam_guide_failure_text((PwSteamFailure)a) != pw_steam_guide_failure_text((PwSteamFailure)b));
    assert(pw_steam_guide_failure_text(PW_STEAM_FAIL_GUARD) == PW_TEXT_STEAM_ERR_GUARD);

    /* The download's bar: proportional to the measured installer, never full before the check. */
    assert(pw_steam_guide_download_fraction(0) == 0.0f);
    assert(pw_steam_guide_download_fraction(PW_STEAM_INSTALLER_BYTES / 2) > 0.49f &&
           pw_steam_guide_download_fraction(PW_STEAM_INSTALLER_BYTES / 2) < 0.51f);
    assert(pw_steam_guide_download_fraction(PW_STEAM_INSTALLER_BYTES * 3) == 0.97f);

    /* The record: written, read back, and nothing when it is missing or holds words it does not know. */
    char folder[] = "/tmp/proton-steam-guide-XXXXXX", path[256];
    assert(mkdtemp(folder));
    snprintf(path, sizeof(path), "%s/steam-guide.txt", folder);
    PwSteamGuideRecord record = {PW_STEAM_STAGE_SIGNIN, PW_STEAM_RESULT_GUARD}, read = {0};
    assert(pw_steam_guide_record_load(&read, path) == ENOENT && read.stage == PW_STEAM_STAGE_NONE);
    assert(pw_steam_guide_record_save(&record, path) == 0);
    assert(pw_steam_guide_record_load(&read, path) == 0 && read.stage == PW_STEAM_STAGE_SIGNIN &&
           read.result == PW_STEAM_RESULT_GUARD);
    for (int r = 0; r < PW_STEAM_RESULTS; ++r) {
        record.result = (PwSteamResult)r;
        assert(pw_steam_guide_record_save(&record, path) == 0 && pw_steam_guide_record_load(&read, path) == 0);
        assert(read.result == (PwSteamResult)r);
    }
    FILE *file = fopen(path, "w");
    fputs("stage=elsewhere\r\nresult=maybe\r\n", file);
    fclose(file);
    assert(pw_steam_guide_record_load(&read, path) == 0 && read.stage == PW_STEAM_STAGE_NONE && !read.result);
    unlink(path);
    rmdir(folder);

    /* Where the launcher goes on. */
    PwSteamFailure failure;
    PwSteamGuideRecord none = {0}, install = {PW_STEAM_STAGE_INSTALL, 0}, signin = {PW_STEAM_STAGE_SIGNIN, 0};
    assert(pw_steam_guide_resume(&none, 1, 0, &failure) == PW_STEAM_RESUME_NOTHING);
    assert(pw_steam_guide_resume(NULL, 1, 0, &failure) == PW_STEAM_RESUME_NOTHING);
    assert(pw_steam_guide_resume(&install, 1, 0, &failure) == PW_STEAM_RESUME_SIGNIN);
    assert(pw_steam_guide_resume(&install, 0, 0, &failure) == PW_STEAM_RESUME_FAILED && failure == PW_STEAM_FAIL_INSTALL);
    assert(pw_steam_guide_resume(&install, 1, 1, &failure) == PW_STEAM_RESUME_DONE);
    assert(pw_steam_guide_resume(&signin, 1, 1, &failure) == PW_STEAM_RESUME_DONE);
    assert(pw_steam_guide_resume(&signin, 1, 0, &failure) == PW_STEAM_RESUME_FAILED && failure == PW_STEAM_FAIL_ENDED);
    const struct { PwSteamResult result; PwSteamResume resume; PwSteamFailure failure; } ends[] = {
        {PW_STEAM_RESULT_RETRY, PW_STEAM_RESUME_SIGNIN, PW_STEAM_FAIL_NONE},
        {PW_STEAM_RESULT_CANCELLED, PW_STEAM_RESUME_FAILED, PW_STEAM_FAIL_CANCELLED},
        {PW_STEAM_RESULT_TIMEOUT, PW_STEAM_RESUME_FAILED, PW_STEAM_FAIL_TIMEOUT},
        {PW_STEAM_RESULT_STALLED, PW_STEAM_RESUME_FAILED, PW_STEAM_FAIL_STALLED},
        {PW_STEAM_RESULT_GUARD, PW_STEAM_RESUME_FAILED, PW_STEAM_FAIL_GUARD},
        {PW_STEAM_RESULT_SIGNED_IN, PW_STEAM_RESUME_FAILED, PW_STEAM_FAIL_ENDED},   /* said so, but nothing on disk */
    };
    for (size_t i = 0; i < sizeof(ends) / sizeof(ends[0]); ++i) {
        signin.result = ends[i].result;
        assert(pw_steam_guide_resume(&signin, 1, 0, &failure) == ends[i].resume && failure == ends[i].failure);
    }

    /* The install session: one screen, and it never ends the session itself. */
    PwSteamSession session;
    pw_steam_session_begin(&session, 0);
    PwSteamSessionInput input = second(400, 0, 0);
    input.cross = 1;
    assert(pw_steam_session_tick(&session, &input) == -1 && session.screen == PW_STEAM_SCREEN_INSTALLING);

    /* A first start like the Pro's: the updates move the package folder and restart the client; the browser starts at
     * 310 s; the sign-in page; a sign-in saved at 600 s. */
    pw_steam_session_begin(&session, 1);
    unsigned long long bytes = 0;
    unsigned processes = 1;
    for (unsigned s = 1; s < 310; ++s) {
        if (s < 140 || (s > 160 && s < 290)) bytes += 1800000;   /* two downloads, a quiet unpacking between */
        if (s == 150 || s == 300) ++processes;                    /* the client restarts itself after each */
        input = second(s, bytes, processes);
        assert(pw_steam_session_tick(&session, &input) == -1 && session.screen == PW_STEAM_SCREEN_UPDATING);
    }
    input = second(310, bytes, processes + 1);
    input.browser = 1;
    assert(pw_steam_session_tick(&session, &input) == -1 && session.screen == PW_STEAM_SCREEN_SCAN);
    input = second(310 + PW_STEAM_SCAN_SECONDS - 1, bytes, processes + 1);
    input.browser = 1;
    assert(pw_steam_session_tick(&session, &input) == -1 && session.screen == PW_STEAM_SCREEN_SCAN);
    input.seconds = 310 + PW_STEAM_SCAN_SECONDS;
    assert(pw_steam_session_tick(&session, &input) == -1 && session.screen == PW_STEAM_SCREEN_NONE);
    input.seconds = 599;   /* the user is at the QR code: nothing over it, however long */
    assert(pw_steam_session_tick(&session, &input) == -1 && session.screen == PW_STEAM_SCREEN_NONE);
    input.seconds = 600;
    input.signed_in = 1;   /* the sign-in ends the session at once: no countdown over the running client */
    assert(pw_steam_session_tick(&session, &input) == PW_STEAM_RESULT_SIGNED_IN && session.screen == PW_STEAM_SCREEN_SIGNED_IN);
    pw_steam_session_begin(&session, 1);
    input = second(5, 1, 1);
    input.signed_in = 1;   /* before the browser too */
    assert(pw_steam_session_tick(&session, &input) == PW_STEAM_RESULT_SIGNED_IN);

    /* CROSS on the phone screen goes to the client's page at once. */
    pw_steam_session_begin(&session, 1);
    input = second(100, 5, 2);
    input.browser = 1;
    assert(pw_steam_session_tick(&session, &input) == -1 && session.screen == PW_STEAM_SCREEN_SCAN);
    input.seconds = 101;
    input.cross = 1;
    assert(pw_steam_session_tick(&session, &input) == -1 && session.screen == PW_STEAM_SCREEN_NONE);

    /* Nothing moving: "longer than usual" at 180 s, CROSS starts again; at 600 s the session ends by itself; movement
     * puts it back to updating. */
    pw_steam_session_begin(&session, 1);
    input = second(10, 1000, 1);
    assert(pw_steam_session_tick(&session, &input) == -1);
    input.seconds = 10 + PW_STEAM_SLOW_SECONDS - 1;
    assert(pw_steam_session_tick(&session, &input) == -1 && session.screen == PW_STEAM_SCREEN_UPDATING);
    input.seconds = 10 + PW_STEAM_SLOW_SECONDS;
    assert(pw_steam_session_tick(&session, &input) == -1 && session.screen == PW_STEAM_SCREEN_SLOW);
    input.package_bytes = 2000;
    input.seconds += 1;
    assert(pw_steam_session_tick(&session, &input) == -1 && session.screen == PW_STEAM_SCREEN_UPDATING);
    input.seconds += PW_STEAM_SLOW_SECONDS;
    input.cross = 1;
    assert(pw_steam_session_tick(&session, &input) == PW_STEAM_RESULT_RETRY);
    pw_steam_session_begin(&session, 1);
    input = second(1, 1, 1);
    assert(pw_steam_session_tick(&session, &input) == -1);
    input.seconds = 1 + PW_STEAM_STALL_SECONDS;
    assert(pw_steam_session_tick(&session, &input) == PW_STEAM_RESULT_STALLED);

    /* A Steam Guard request, then no sign-in: the launcher explains the code; a sign-in after one is still a sign-in. */
    pw_steam_session_begin(&session, 1);
    input = second(320, 1, 3);
    input.browser = 1;
    input.guard = 1;
    assert(pw_steam_session_tick(&session, &input) == -1);
    assert(pw_steam_session_ended(&session, PW_STEAM_RESULT_CANCELLED) == PW_STEAM_RESULT_GUARD);
    assert(pw_steam_session_ended(&session, PW_STEAM_RESULT_TIMEOUT) == PW_STEAM_RESULT_GUARD);
    assert(pw_steam_session_ended(&session, PW_STEAM_RESULT_NONE) == PW_STEAM_RESULT_GUARD);
    input.seconds = 400;
    input.signed_in = 1;
    input.guard = 0;
    assert(pw_steam_session_tick(&session, &input) == PW_STEAM_RESULT_SIGNED_IN);
    assert(pw_steam_session_ended(&session, PW_STEAM_RESULT_NONE) == PW_STEAM_RESULT_NONE);
    pw_steam_session_begin(&session, 1);
    assert(pw_steam_session_ended(&session, PW_STEAM_RESULT_CANCELLED) == PW_STEAM_RESULT_CANCELLED);

    puts("steam guide: failures, record, resume, install and sign-in sessions (updates, phone, sign-in, slow, stall, "
         "Steam Guard)");
    return 0;
}
