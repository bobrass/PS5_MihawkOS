/* SPDX-License-Identifier: GPL-3.0-or-later
 * A stand-in for a session's program, so a test can see the session screens the title draws from that program's
 * output (native/proton_loading.cpp, the Aurelia and Steam-runtime overlays) without a signed-in account:
 *
 *   <name>.exe LINES-FILE SECONDS   each line of LINES-FILE to stdout, a second apart, then SECONDS of waiting
 *
 * The title chooses the session by the program's path: one with "aurelia_windows" in it is read as the Aurelia client's
 * --json lines (src/pw_aurelia.h: a list, progress), one with "steamcmd" in it opens the Steam runtime's setup page.
 * The Aero theme's first console run needed a frame of each (2026-10-26). Ends by itself. */
#include <windows.h>

void mainCRTStartup(void)
{
    char *line = GetCommandLineA(), *path, *seconds, *text;
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE), file;
    DWORD size, got, written, wait = 30;

    /* the program's own name, then the file, then the seconds */
    if (*line == '"') {
        for (line++; *line && *line != '"'; line++)
            ;
        if (*line) line++;
    }
    while (*line && *line != ' ') line++;
    while (*line == ' ') line++;
    path = line;
    while (*line && *line != ' ') line++;
    seconds = *line ? line + 1 : NULL;
    *line = 0;
    if (seconds) for (wait = 0; *seconds >= '0' && *seconds <= '9'; seconds++) wait = wait * 10 + (DWORD)(*seconds - '0');

    file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (file != INVALID_HANDLE_VALUE) {
        size = GetFileSize(file, NULL);
        text = HeapAlloc(GetProcessHeap(), 0, size + 1);
        if (text && ReadFile(file, text, size, &got, NULL)) {
            char *start = text, *end = text + got;
            while (start < end) {
                char *stop = start;
                while (stop < end && *stop != '\n') stop++;
                WriteFile(out, start, (DWORD)(stop - start + (stop < end)), &written, NULL);
                FlushFileBuffers(out);
                start = stop + 1;
                Sleep(1000);
            }
        }
        CloseHandle(file);
    }
    Sleep(wait * 1000);
    ExitProcess(0);
}
