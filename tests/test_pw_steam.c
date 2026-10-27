/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The Steam option's portable part (src/pw_steam.h): the installer sanity check, the remembered sign-in, the process
 * rules and the catalog entries. */
#include "../src/pw_steam.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A PE header the way a signed 32-bit installer has it: "MZ", the PE header at 0x80, a PE32 optional header (96 bytes
 * before its 16 data directories), the certificate table (directory 4) set or not. */
static void installer_head(uint8_t *head, size_t n, int magic, int signed_file)
{
    memset(head, 0, n);
    head[0] = 'M'; head[1] = 'Z';
    head[0x3c] = 0x80;
    memcpy(head + 0x80, "PE\0\0", 4);
    head[0x80 + 4 + 20] = (uint8_t)(magic & 0xff);
    head[0x80 + 4 + 21] = (uint8_t)(magic >> 8);
    const size_t directories = magic == 0x20b ? 112 : 96;
    uint8_t *certificate = head + 0x80 + 4 + 20 + directories + 4 * 8;
    if (signed_file) { certificate[0] = 0x00; certificate[1] = 0x40; certificate[4] = 0x20; certificate[5] = 0x0b; }
}

static void installer_cases(void)
{
    uint8_t head[1024];
    installer_head(head, sizeof(head), 0x10b, 1);
    assert(pw_steam_installer_check(head, sizeof(head), 2u << 20) == PW_STEAM_INSTALLER_OK);
    installer_head(head, sizeof(head), 0x20b, 1);
    assert(pw_steam_installer_check(head, sizeof(head), 2u << 20) == PW_STEAM_INSTALLER_OK);   /* PE32+ too */
    assert(pw_steam_installer_check(head, sizeof(head), 100) == PW_STEAM_INSTALLER_SIZE);
    assert(pw_steam_installer_check(head, sizeof(head), PW_STEAM_INSTALLER_MAX + 1) == PW_STEAM_INSTALLER_SIZE);
    assert(pw_steam_installer_check(head, sizeof(head), PW_STEAM_INSTALLER_MIN) == PW_STEAM_INSTALLER_OK);
    installer_head(head, sizeof(head), 0x10b, 0);
    assert(pw_steam_installer_check(head, sizeof(head), 2u << 20) == PW_STEAM_INSTALLER_NOT_SIGNED);
    installer_head(head, sizeof(head), 0x10b, 1);
    head[0] = 'P';   /* an HTML error page starts with "<" and a ZIP with "PK": not MZ */
    assert(pw_steam_installer_check(head, sizeof(head), 2u << 20) == PW_STEAM_INSTALLER_NOT_PE);
    installer_head(head, sizeof(head), 0x10b, 1);
    head[0x3c] = 0xff; head[0x3d] = 0xff;   /* a PE offset far outside what was read */
    assert(pw_steam_installer_check(head, sizeof(head), 2u << 20) == PW_STEAM_INSTALLER_NOT_PE);
    installer_head(head, sizeof(head), 0x10b, 1);
    head[0x80] = 'Q';
    assert(pw_steam_installer_check(head, sizeof(head), 2u << 20) == PW_STEAM_INSTALLER_NOT_PE);
    installer_head(head, sizeof(head), 0x107, 1);   /* a ROM image, not a program */
    assert(pw_steam_installer_check(head, sizeof(head), 2u << 20) == PW_STEAM_INSTALLER_NOT_PE);
    installer_head(head, sizeof(head), 0x10b, 1);
    assert(pw_steam_installer_check(head, 100, 2u << 20) == PW_STEAM_INSTALLER_NOT_PE);   /* too few bytes read */
    assert(pw_steam_installer_check(NULL, 0, 2u << 20) == PW_STEAM_INSTALLER_NOT_PE);
    /* Every cut length of a good header is refused or accepted, and never reads past what it was given. */
    installer_head(head, sizeof(head), 0x10b, 1);
    for (size_t n = 0; n <= sizeof(head); n++) (void)pw_steam_installer_check(head, n, 2u << 20);
}

static const char kUsers[] =
    "\"users\"\n{\n"
    "\t\"76561190000000001\"\n\t{\n\t\t\"AccountName\"\t\t\"one\"\n\t\t\"PersonaName\"\t\t\"Older Name\"\n"
    "\t\t\"RememberPassword\"\t\t\"1\"\n\t\t\"MostRecent\"\t\t\"0\"\n\t\t\"AllowAutoLogin\"\t\t\"1\"\n\t}\n"
    "\t\"76561190000000002\"\n\t{\n\t\t\"AccountName\"\t\t\"two\"\n\t\t\"PersonaName\"\t\t\"Recent \\\"Name\\\"\"\n"
    "\t\t\"RememberPassword\"\t\t\"1\"\n\t\t\"MostRecent\"\t\t\"1\"\n\t\t\"AllowAutoLogin\"\t\t\"1\"\n\t}\n}\n";

static void login_cases(void)
{
    char persona[64];
    /* The most recent remembered user wins, and its escaped quotes come back unescaped. */
    assert(pw_steam_remembered_user(kUsers, sizeof(kUsers) - 1, persona, sizeof(persona)) == 1);
    assert(!strcmp(persona, "Recent \"Name\""));
    /* Without the persona buffer it still answers. */
    assert(pw_steam_remembered_user(kUsers, sizeof(kUsers) - 1, NULL, 0) == 1);
    /* A user whose password is not remembered, or whose auto-login is off, is not a sign-in. */
    const char forgot[] = "\"users\"{\"7656\"{\"PersonaName\"\"X\"\"RememberPassword\"\"0\"\"AllowAutoLogin\"\"1\"}}";
    assert(!pw_steam_remembered_user(forgot, sizeof(forgot) - 1, persona, sizeof(persona)) && !persona[0]);
    const char noauto[] = "\"users\"\n{\n\"7656\"\n{\n\"PersonaName\"\"X\"\n\"RememberPassword\"\"1\"\n\"AllowAutoLogin\"\"0\"\n}\n}";
    assert(!pw_steam_remembered_user(noauto, sizeof(noauto) - 1, persona, sizeof(persona)));
    /* RememberPassword with no AllowAutoLogin at all (older clients) counts. */
    const char old[] = "\"users\"{\"7656\"{\"PersonaName\"\"Old\"\"RememberPassword\"\"1\"}}";
    assert(pw_steam_remembered_user(old, sizeof(old) - 1, persona, sizeof(persona)) && !strcmp(persona, "Old"));
    /* A comment, a nested block, and a name longer than the buffer (cut, terminated). */
    const char busy[] = "// header\n\"users\"{\"7656\"{\"Extra\"{\"a\"\"b\"}\"PersonaName\"\"A very long display name indeed\""
                        "\"RememberPassword\"\"1\"}}";
    char small[8];
    assert(pw_steam_remembered_user(busy, sizeof(busy) - 1, small, sizeof(small)) == 1 && !strcmp(small, "A very "));
    /* Empty, unbalanced and cut text never answers yes and never overruns (every cut of the good text, under ASan). */
    assert(!pw_steam_remembered_user("", 0, persona, sizeof(persona)));
    assert(!pw_steam_remembered_user(NULL, 0, persona, sizeof(persona)));
    assert(!pw_steam_remembered_user("}}}{{{\"", 7, persona, sizeof(persona)));
    for (size_t n = 0; n < sizeof(kUsers); n++) {
        char *copy = malloc(n ? n : 1);   /* a heap copy of exactly n bytes: any read past it is caught */
        memcpy(copy, kUsers, n);
        (void)pw_steam_remembered_user(copy, n, persona, sizeof(persona));
        free(copy);
    }
}

static void step_cases(void)
{
    /* installed, signed_in, downloading, downloaded, failed */
    assert(pw_steam_step(0, 0, 0, 0, 0) == PW_STEAM_STEP_NOT_INSTALLED);
    assert(pw_steam_step(0, 0, 1, 0, 0) == PW_STEAM_STEP_DOWNLOADING);
    assert(pw_steam_step(0, 0, 0, 1, 0) == PW_STEAM_STEP_INSTALLING);
    assert(pw_steam_step(0, 0, 0, 0, 1) == PW_STEAM_STEP_FAILED);
    assert(pw_steam_step(1, 0, 0, 0, 0) == PW_STEAM_STEP_SIGN_IN_NEEDED);
    assert(pw_steam_step(1, 1, 0, 0, 0) == PW_STEAM_STEP_SIGNED_IN);
    /* the files decide: a client that is installed is never "downloading" or "failed", whatever the thread says */
    assert(pw_steam_step(1, 0, 1, 1, 1) == PW_STEAM_STEP_SIGN_IN_NEEDED);
    assert(pw_steam_step(1, 1, 1, 0, 1) == PW_STEAM_STEP_SIGNED_IN);
    /* a remembered sign-in without the program is not a sign-in: the client must be installed first */
    assert(pw_steam_step(0, 1, 0, 0, 0) == PW_STEAM_STEP_NOT_INSTALLED);
    /* every step has words, a badge only where there is something to say, and an action only where CROSS does something */
    for (int step = PW_STEAM_STEP_NOT_INSTALLED; step <= PW_STEAM_STEP_SIGNED_IN; step++) {
        assert(strlen(pw_steam_step_subtitle((PwSteamStep)step)) >= 8);
        assert(pw_steam_step_badge((PwSteamStep)step) != NULL && pw_steam_step_action((PwSteamStep)step) != NULL);
    }
    assert(!strcmp(pw_steam_step_badge(PW_STEAM_STEP_NOT_INSTALLED), ""));
    assert(!strcmp(pw_steam_step_action(PW_STEAM_STEP_NOT_INSTALLED), "Download and install"));
    assert(!strcmp(pw_steam_step_action(PW_STEAM_STEP_SIGN_IN_NEEDED), "Sign in"));
    assert(!strcmp(pw_steam_step_action(PW_STEAM_STEP_SIGNED_IN), "") && !strcmp(pw_steam_step_action(PW_STEAM_STEP_DOWNLOADING), ""));
    assert(!strcmp(pw_steam_step_badge(PW_STEAM_STEP_SIGNED_IN), "SIGNED IN"));
}

static void rules_cases(void)
{
    char rules[1024];
    const size_t length = pw_steam_rules(rules, sizeof(rules), 1);
    assert(length && length == strlen(rules) && length < 700);
    char tiny[16];
    assert(pw_steam_rules(tiny, sizeof(tiny), 1) == 0 && !tiny[0]);   /* too small: nothing partial */
    assert(pw_steam_rule_count() == 10);
    unsigned fails = 0;
    for (unsigned i = 0; i < pw_steam_rule_count(); i++) {
        const char *name, *action, *value;
        assert(pw_steam_rule(i, &name, &action, &value) && *name);
        assert(!strcmp(action, "fail") || !strcmp(action, "run") || !strcmp(action, "add"));
        assert(!strchr(name, ';') && !strchr(name, '|') && !strchr(value, ';') && !strchr(value, '|'));   /* the rule syntax */
        if (!strcmp(action, "fail")) { fails++; assert(!*value); }
        if (!strcmp(action, "add")) assert(value[0] == ' ');
    }
    assert(fails == 8 && !pw_steam_rule(pw_steam_rule_count(), NULL, NULL, NULL));
    /* The two programs the client cannot run without are not refused. */
    assert(!strstr(rules, "|fail|;steam.exe") && !strstr(rules, "steamservice.exe") && !strstr(rules, "|steam.exe|"));
    assert(strstr(rules, "steamwebhelper.exe|add| --single-process"));
    /* Where memory allows, the browser keeps its own processes: the same rules without that one, still well formed. */
    char separate[1024];
    const size_t separate_length = pw_steam_rules(separate, sizeof(separate), 0);
    assert(separate_length && !strstr(separate, "steamwebhelper") && separate[0] != ';' && !strstr(separate, ";;"));
    assert(strstr(separate, "gameoverlayui64.exe|fail|") && strstr(separate, "bin\\SteamService.exe|run|"));
    char client[2048];
    assert(pw_steam_client_entry(client, sizeof(client), 1, NULL, 0) && !strstr(client, "single-process"));
    assert(pw_steam_client_entry(client, sizeof(client), 1, NULL, 1) && strstr(client, "single-process"));
}

static void entry_cases(void)
{
    char text[2048];
    /* A field with a quote, a backslash, a newline and a control character is escaped; nothing else is. */
    const size_t n = pw_steam_entry_json(text, sizeof(text), "N\"a\\me\n", "d", "C:\\a.exe", "C:\\", "", 1, NULL, NULL);
    assert(n && n == strlen(text));
    assert(strstr(text, "\"name\": \"N\\\"a\\\\me\\n\",") && strstr(text, "\"desktop\": true,") && strstr(text, "\"env\": []\n}"));
    const char *const environment[] = { "A=B", "C=\"D\"", NULL };
    assert(pw_steam_entry_json(text, sizeof(text), "n", "d", "e", "f", "g", 0, environment, NULL));
    assert(strstr(text, "\"env\": [\n    \"A=B\",\n    \"C=\\\"D\\\"\"\n  ]\n}"));
    /* Too small: empty, never half an entry. */
    for (size_t cap = 1; cap < n; cap += 7) {
        char small[512];
        assert(cap <= sizeof(small));
        assert(pw_steam_entry_json(small, cap, "N\"a\\me\n", "d", "C:\\a.exe", "C:\\", "", 1, NULL, NULL) == 0 && !small[0]);
    }
    assert(!pw_steam_entry_json(text, sizeof(text), NULL, "d", "e", "f", "g", 0, NULL, NULL));
    /* The installer's entry runs the downloaded file silently, off the desktop. */
    assert(pw_steam_installer_entry(text, sizeof(text), NULL));
    assert(strstr(text, "\"executable\": \"Z:\\\\data\\\\ps5proton\\\\downloads\\\\SteamSetup.exe\"") &&
           strstr(text, "\"arguments\": \"/S\"") && strstr(text, "\"prefix\": \"mihawk\"") && strstr(text, "\"desktop\": false") &&
           strstr(text, "PROTON_STEAM_GUIDE=install"));
    /* The client's entry: its own window (no arguments), the rules and the sign-in watch in its environment. */
    assert(pw_steam_client_entry(text, sizeof(text), 0, NULL, 1));
    assert(strstr(text, "\"executable\": \"C:\\\\Program Files (x86)\\\\Steam\\\\steam.exe\"") &&
           strstr(text, "\"arguments\": \"\"") && strstr(text, "WINE_PS5_PROCESS_RULES=steamsysinfo.exe|fail|;") &&
           strstr(text, "bin\\\\SteamService.exe|run|C:\\\\windows\\\\system32\\\\hostname.exe") &&
           strstr(text, "PROTON_STEAM_SIGNIN_WATCH=1") && strstr(text, "PROTON_SESSION_FOLLOWS=processes") && strstr(text, "WINE_PS5_WINDOW_SURFACES=1") &&
           strstr(text, "PROTON_STEAM_GUIDE=signin") && strstr(text, "\"desktop\": false"));
    /* Signed in: Big Picture, the rules and the window route, and no sign-in watch (the session ends when the user leaves Steam). */
    assert(pw_steam_client_entry(text, sizeof(text), 1, NULL, 1));
    assert(strstr(text, "\"arguments\": \"-bigpicture -noverifyfiles\"") && strstr(text, "WINE_PS5_PROCESS_RULES=") &&
           strstr(text, "PROTON_SESSION_FOLLOWS=processes") && strstr(text, "WINE_PS5_WINDOW_SURFACES=1") &&
           !strstr(text, "PROTON_STEAM_SIGNIN_WATCH") && !strstr(text, "PROTON_STEAM_GUIDE") && strstr(text, "\"desktop\": false"));
    assert(pw_steam_client_entry(text, sizeof(text), 0, NULL, 1) && !strstr(text, "-bigpicture"));
    /* A test's own prefix: a plain word only, and the entries carry it */
    char word[40];
    assert(pw_steam_prefix_word("steamguide-fresh\n", 17, word, sizeof(word)) && !strcmp(word, "steamguide-fresh"));
    assert(!pw_steam_prefix_word("../mihawk", 9, word, sizeof(word)) && !pw_steam_prefix_word("Mihawk", 6, word, sizeof(word)));
    assert(!pw_steam_prefix_word("", 0, word, sizeof(word)) && !pw_steam_prefix_word("a b", 3, word, sizeof(word)));
    assert(!pw_steam_prefix_word("abcdefghijklmnopqrstuvwxyz0123456", 33, word, sizeof(word)));
    assert(!pw_steam_prefix_word(NULL, 4, word, sizeof(word)) && !pw_steam_prefix_word("abc", 3, word, 3));
    assert(pw_steam_installer_entry(text, sizeof(text), "steamguide-fresh") && strstr(text, "\"prefix\": \"steamguide-fresh\""));
    assert(pw_steam_client_entry(text, sizeof(text), 0, "steamguide-fresh", 1) && strstr(text, "\"prefix\": \"steamguide-fresh\""));
    assert(pw_steam_client_entry(text, sizeof(text), 0, NULL, 1) && strstr(text, "\"prefix\": \"mihawk\""));
    /* The whole thing is balanced. */
    int braces = 0, brackets = 0, quotes = 0;
    for (const char *p = text; *p; p++) {
        if (*p == '\\' && p[1]) { p++; continue; }
        if (*p == '"') quotes ^= 1;
        else if (!quotes && *p == '{') braces++;
        else if (!quotes && *p == '}') braces--;
        else if (!quotes && *p == '[') brackets++;
        else if (!quotes && *p == ']') brackets--;
    }
    assert(!braces && !brackets && !quotes);
}

int main(void)
{
    installer_cases();
    login_cases();
    step_cases();
    rules_cases();
    entry_cases();
    puts("steam: installer check, remembered sign-in, process rules, catalog entries");
    return 0;
}
