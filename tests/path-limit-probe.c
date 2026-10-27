/* SPDX-License-Identifier: GPL-3.0-or-later
 * The longest path the console lets a program make, open and remove. tests/handles-paths-probe.c made 21 directory levels
 * and no more on the console (2026-10-25, both builds), where the PC's Wine made 40: the 22nd was a Windows path of 217
 * characters, the prefix's Unix path of it 253. Here: directories of 10-character names under C:\pwlen, then the last one
 * shortened a character at a time, so the longest Windows path that works is found to the character; at each length a
 * file is made in it too. Prints the lengths and the error of the first that fails; ends by itself (PASS: measured). */
#include <windows.h>

static void say(const char *text)
{
    DWORD written;
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), text, lstrlenA(text), &written, NULL);
    OutputDebugStringA(text);
}

static void result(const char *what, unsigned long value)
{
    char text[200], digits[12], number[12];
    int i = 0, j;
    do { digits[i++] = (char)('0' + value % 10); value /= 10; } while (value);
    for (j = 0; j < i; j++) number[j] = digits[i - 1 - j];
    number[i] = 0;
    lstrcpyA(text, "PS5_PROTON_PATH_LIMIT ");
    lstrcatA(text, what);
    lstrcatA(text, ": ");
    lstrcatA(text, number);
    lstrcatA(text, "\n");
    say(text);
}

/* path + "\\" + a name of n characters 'a'.. */
static void append(WCHAR *path, int n)
{
    int at = lstrlenW(path), i;
    path[at++] = '\\';
    for (i = 0; i < n; i++) path[at++] = (WCHAR)('a' + i);
    path[at] = 0;
}

void mainCRTStartup(void)
{
    static WCHAR path[2048], made[64][2048], file[2048];
    int depth = 0, longest_dir = 0, longest_file = 0, n;
    DWORD dir_error = 0, file_error = 0;
    HANDLE handle;

    lstrcpyW(path, L"\\\\?\\C:\\pwlen");
    CreateDirectoryW(path, NULL);
    /* whole 10-character levels while they can be made */
    for (;;) {
        WCHAR next[2048];
        lstrcpyW(next, path);
        append(next, 10);
        if (lstrlenW(next) > 1000 || !CreateDirectoryW(next, NULL)) { dir_error = GetLastError(); break; }
        lstrcpyW(path, next);
        lstrcpyW(made[depth++], path);
        longest_dir = lstrlenW(path) - 4;
    }
    /* then one more level, a character at a time */
    for (n = 1; n < 10; n++) {
        WCHAR next[2048];
        lstrcpyW(next, path);
        append(next, n);
        if (!CreateDirectoryW(next, NULL)) { dir_error = GetLastError(); break; }
        longest_dir = lstrlenW(next) - 4;
        RemoveDirectoryW(next);
    }
    /* the longest file: in the deepest directory, names of 1.. characters */
    for (n = 1; n < 40; n++) {
        lstrcpyW(file, path);
        append(file, n > 26 ? 26 : n);
        if (n > 26) { int at = lstrlenW(file), k; for (k = 26; k < n; k++) file[at++] = 'z'; file[at] = 0; }
        handle = CreateFileW(file, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
        if (handle == INVALID_HANDLE_VALUE) { file_error = GetLastError(); break; }
        CloseHandle(handle);
        longest_file = lstrlenW(file) - 4;
        DeleteFileW(file);
    }
    result("whole 10-character levels made", (unsigned long)depth);
    result("longest directory made, Windows characters (C:\\...)", (unsigned long)longest_dir);
    result("then refused with error", dir_error);
    result("longest file made, Windows characters", (unsigned long)longest_file);
    result("then refused with error", file_error);
    while (depth > 0) RemoveDirectoryW(made[--depth]);
    RemoveDirectoryW(L"\\\\?\\C:\\pwlen");
    say("PS5_PROTON_PATH_LIMIT PASS: measured\n");
    ExitProcess(0);
}
