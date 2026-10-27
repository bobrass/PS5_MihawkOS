/* SPDX-License-Identifier: GPL-3.0-or-later
 * The program every fixture installer installs (tests/installers/), built 32- and 64-bit. tests/installer-slice-probe.c
 * starts it after an install and reads its exit code: 77 is a pass.
 *
 *   pwapp.exe                  passes when pwapp.dat, installed beside it, holds "PwFixture"
 *   pwapp.exe /load DLL EXPORT passes when DLL loads and exports EXPORT (a redistributable's runtime)
 *   pwapp.exe /redist          what a Steam install script runs: writes redist.done beside itself
 *   pwapp.exe /unredist        and its uninstall step: removes it */
#include <windows.h>

static void say(const char *text)
{
    DWORD written;
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), text, lstrlenA(text), &written, NULL);
}

/* The directory this program is in, with a trailing backslash, then NAME. */
static void beside(WCHAR *path, const WCHAR *name)
{
    WCHAR *slash;
    GetModuleFileNameW(NULL, path, MAX_PATH);
    slash = path + lstrlenW(path);
    while (slash > path && slash[-1] != '\\') slash--;
    lstrcpyW(slash, name);
}

/* The Nth space-separated word of the command line after the program's name (N from 1), or NULL. */
static WCHAR *word(int n, WCHAR *out)
{
    WCHAR *at = GetCommandLineW();
    int i, k;
    if (*at == '"') { at++; while (*at && *at != '"') at++; if (*at) at++; }
    else while (*at && *at != ' ') at++;
    for (i = 1; ; i++) {
        while (*at == ' ') at++;
        if (!*at) return NULL;
        for (k = 0; *at && *at != ' ' && k < MAX_PATH - 1; k++) out[k] = *at++;
        out[k] = 0;
        if (i == n) return out;
    }
}

void mainCRTStartup(void)
{
    static WCHAR path[MAX_PATH], first[MAX_PATH], second[MAX_PATH], third[MAX_PATH];
    const char *bits = sizeof(void *) == 8 ? "64" : "32";
    char tag[64];
    DWORD done;
    HANDLE file;

    lstrcpyA(tag, "PS5_PROTON_INSTALLED_APP ");
    lstrcatA(tag, bits);
    if (word(1, first) && !lstrcmpiW(first, L"/load")) {
        char export_name[128];
        HMODULE module;
        int i;
        if (!word(2, second) || !word(3, third)) { say("usage: /load DLL EXPORT\n"); ExitProcess(2); }
        for (i = 0; third[i] && i < 127; i++) export_name[i] = (char)third[i];
        export_name[i] = 0;
        module = LoadLibraryW(second);
        if (!module || !GetProcAddress(module, export_name)) {
            say(tag); say(" FAIL: the library did not load or lacks the export\n");
            ExitProcess(1);
        }
        say(tag); say(" PASS: library loaded\n");
        ExitProcess(77);
    }
    if (first[0] && (!lstrcmpiW(first, L"/redist") || !lstrcmpiW(first, L"/unredist"))) {
        beside(path, L"redist.done");
        if (!lstrcmpiW(first, L"/unredist")) ExitProcess(DeleteFileW(path) ? 0 : 1);
        file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
        if (file == INVALID_HANDLE_VALUE) ExitProcess(1);
        WriteFile(file, "redist", 6, &done, NULL);
        CloseHandle(file);
        ExitProcess(0);
    }
    beside(path, L"pwapp.dat");
    file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (file != INVALID_HANDLE_VALUE) {
        char text[16] = {0};
        ReadFile(file, text, 9, &done, NULL);
        CloseHandle(file);
        if (done == 9 && !lstrcmpA(text, "PwFixture")) { say(tag); say(" PASS: started beside its data\n"); ExitProcess(77); }
    }
    say(tag); say(" FAIL: pwapp.dat missing or wrong\n");
    ExitProcess(1);
}
