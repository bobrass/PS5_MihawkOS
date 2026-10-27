/* SPDX-License-Identifier: GPL-3.0-or-later
 * Platform stress that Wine's own tests do not reach, in a 32-bit (WoW64) and a 64-bit build of the same file, run in a
 * helper as games are (PROTON_LAYOUT=launcher). The console lets a title hold about 256 files; the platform layer parks
 * idle ones and reopens them on use (wine/ps5/pw_wine_fdv.h), so a game that holds thousands of files only works if a
 * parked file keeps its contents, position, renames and deletes. And a Windows path is case-insensitive at every level,
 * which the console's file system is not. The same program on the PC's Wine is the oracle: it passes there.
 *
 *   1. FILES files created under %TEMP%\HpProbe, each written with its own number, all held open together, plus
 *      EVENTS events: thousands of server handles.
 *   2. Every file read back through its handle from offset 0 (a parked file reopened), then its position checked.
 *   3. Every 4th file renamed while open (MoveFileEx on its path), then written and read through the old handle; every
 *      other 7th marked delete-on-close (not a renamed one: Wine's server deletes the name the file was opened by). After the close, renamed files exist under the new name and deleted ones are gone.
 *   4. A directory chain DEPTH deep with mixed-case names, past MAX_PATH (about 340 characters), made, opened, listed
 *      and deleted in the other case through a \\?\ path. 20 levels: the console refuses a path of more than 33 Unix
 *      components (a 22nd level here, 2026-10-25), measured by tests/path-limit-probe.c, not by this check.
 * PASS when every check holds; ends by itself. */
#include <windows.h>

enum { FILES = 3000, EVENTS = 6000, DEPTH = 20 };

static int failures;

static void say(const char *text)
{
    DWORD written;
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), text, lstrlenA(text), &written, NULL);
    OutputDebugStringA(text);
}

static void decimal(char *out, unsigned long value)
{
    char digits[12];
    int i = 0, j;
    do { digits[i++] = (char)('0' + value % 10); value /= 10; } while (value);
    for (j = 0; j < i; j++) out[j] = digits[i - 1 - j];
    out[i] = 0;
}

static void result(const char *what, unsigned long value)
{
    char text[300], number[12];
    decimal(number, value);
    lstrcpyA(text, "PS5_PROTON_HANDLES_PATHS ");
    lstrcatA(text, what);
    lstrcatA(text, ": ");
    lstrcatA(text, number);
    lstrcatA(text, "\n");
    say(text);
}

static void check(int ok, const char *what, unsigned long detail)
{
    if (ok) return;
    failures++;
    if (failures <= 20) result(what, detail);
}

/* dir\fNNNN.dat, or with a prefix letter for the renamed name */
static void file_name(WCHAR *out, const WCHAR *dir, const WCHAR *prefix, int n)
{
    WCHAR digits[8];
    int i;
    lstrcpyW(out, dir);
    lstrcatW(out, prefix);
    for (i = 0; i < 4; i++) { digits[3 - i] = (WCHAR)('0' + n % 10); n /= 10; }
    digits[4] = 0;
    lstrcatW(out, digits);
    lstrcatW(out, L".dat");
}

/* The other case of every letter in a name */
static void flip_case(WCHAR *path)
{
    for (; *path; path++) {
        if (*path >= 'a' && *path <= 'z') *path = (WCHAR)(*path - 32);
        else if (*path >= 'A' && *path <= 'Z') *path = (WCHAR)(*path + 32);
    }
}

static void files_and_handles(const WCHAR *dir)
{
    static HANDLE files[FILES], events[EVENTS];
    static WCHAR path[MAX_PATH], renamed[MAX_PATH];
    int n, opened = 0, made = 0, read_ok = 0, renamed_ok = 0, after_ok = 0;
    DWORD done;

    for (n = 0; n < FILES; n++) {
        DWORD value = 0x5a000000u | (DWORD)n;
        file_name(path, dir, L"f", n);
        files[n] = CreateFileW(path, GENERIC_READ | GENERIC_WRITE | DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                               CREATE_ALWAYS, 0, NULL);
        if (files[n] == INVALID_HANDLE_VALUE) { check(0, "create failed at file (last error follows)", (unsigned long)n);
                                                 result("  last error", GetLastError()); break; }
        opened++;
        check(WriteFile(files[n], &value, sizeof(value), &done, NULL) && done == sizeof(value), "write failed at file", (unsigned long)n);
    }
    result("files created and held open", (unsigned long)opened);
    for (n = 0; n < EVENTS; n++) {
        events[n] = CreateEventW(NULL, TRUE, n & 1, NULL);
        if (!events[n]) break;
        made++;
    }
    result("events held besides", (unsigned long)made);
    check(opened == FILES && made == EVENTS, "fewer handles than asked", (unsigned long)(opened + made));
    for (n = 0; n < made; n++)
        check(WaitForSingleObject(events[n], 0) == (n & 1 ? WAIT_OBJECT_0 : WAIT_TIMEOUT), "event state wrong at", (unsigned long)n);

    /* Read back from the start: a parked descriptor is reopened, and its position must be the handle's. */
    for (n = 0; n < opened; n++) {
        DWORD value = 0, position;
        LONG high = 0;
        if (SetFilePointer(files[n], 0, NULL, FILE_BEGIN) || !ReadFile(files[n], &value, sizeof(value), &done, NULL) || done != sizeof(value) ||
            value != (0x5a000000u | (DWORD)n)) { check(0, "read-back wrong at file", (unsigned long)n); continue; }
        position = SetFilePointer(files[n], 0, &high, FILE_CURRENT);
        check(position == sizeof(value) && !high, "position after read wrong at file", (unsigned long)n);
        read_ok++;
    }
    result("files read back through their handles", (unsigned long)read_ok);

    /* Rename every 4th while open, then use the old handle; mark every 7th delete-on-close. */
    for (n = 0; n < opened; n++) {
        if (n % 4 == 0) {
            DWORD value = 0xa5000000u | (DWORD)n, back = 0;
            file_name(path, dir, L"f", n);
            file_name(renamed, dir, L"R", n);
            if (!MoveFileExW(path, renamed, 0)) { check(0, "rename while open failed at file", (unsigned long)n); continue; }
            if (SetFilePointer(files[n], 0, NULL, FILE_BEGIN) || !WriteFile(files[n], &value, sizeof(value), &done, NULL) ||
                SetFilePointer(files[n], 0, NULL, FILE_BEGIN) || !ReadFile(files[n], &back, sizeof(back), &done, NULL) || back != value)
                { check(0, "old handle after rename wrong at file", (unsigned long)n); continue; }
            renamed_ok++;
        }
        if (n % 7 == 0 && n % 4) {
            FILE_DISPOSITION_INFO gone = { TRUE };
            check(SetFileInformationByHandle(files[n], FileDispositionInfo, &gone, sizeof(gone)), "delete-on-close refused at file", (unsigned long)n);
        }
    }
    result("files renamed while open and used through the old handle", (unsigned long)renamed_ok);
    for (n = 0; n < made; n++) CloseHandle(events[n]);
    for (n = 0; n < opened; n++) CloseHandle(files[n]);

    for (n = 0; n < opened; n++) {
        DWORD value = 0, expected = n % 4 == 0 ? 0xa5000000u | (DWORD)n : 0x5a000000u | (DWORD)n;
        HANDLE file;
        file_name(path, dir, n % 4 == 0 ? L"R" : L"f", n);
        file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
        if (n % 7 == 0 && n % 4) { check(file == INVALID_HANDLE_VALUE, "deleted file still exists at", (unsigned long)n); if (file != INVALID_HANDLE_VALUE) CloseHandle(file); else after_ok++; continue; }
        if (file == INVALID_HANDLE_VALUE) { check(0, "file missing after close at", (unsigned long)n); continue; }
        if (ReadFile(file, &value, sizeof(value), &done, NULL) && value == expected) after_ok++;
        else check(0, "contents after close wrong at file", (unsigned long)n);
        CloseHandle(file);
        DeleteFileW(path);
        if (n % 4 == 0) { file_name(path, dir, L"f", n); check(GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES, "old name still exists at", (unsigned long)n); }
    }
    result("files as expected after close (renamed, kept or deleted)", (unsigned long)after_ok);
}

static void deep_paths(const WCHAR *dir)
{
    static WCHAR path[2048], other[2048], longer[2048];
    static const WCHAR *names[] = { L"ContentArchive", L"DATA_streaming", L"textures_hires", L"MixedCaseFolder", L"sUbDiReCtOrY",
                                    L"LevelsAndMaps" };
    WIN32_FIND_DATAW found;
    HANDLE file, find;
    int level, made = 0, removed = 0;
    DWORD done, value = 0x1234abcd, back = 0;

    lstrcpyW(path, dir);
    lstrcatW(path, L"Deep");
    check(CreateDirectoryW(path, NULL), "top directory not made", GetLastError());
    for (level = 0; level < DEPTH; level++) {
        lstrcatW(path, L"\\");
        lstrcatW(path, names[level % 6]);
        lstrcpyW(longer, L"\\\\?\\");
        lstrcatW(longer, path);
        if (CreateDirectoryW(longer, NULL)) made++;
        else check(0, "directory not made at level", (unsigned long)level);
    }
    result("directory levels made (mixed case)", (unsigned long)made);
    result("path length in characters", (unsigned long)lstrlenW(path));
    check(lstrlenW(path) > MAX_PATH, "path not past MAX_PATH", (unsigned long)lstrlenW(path));

    lstrcatW(path, L"\\Save.Bin");
    lstrcpyW(longer, L"\\\\?\\");
    lstrcatW(longer, path);
    file = CreateFileW(longer, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    check(file != INVALID_HANDLE_VALUE && WriteFile(file, &value, sizeof(value), &done, NULL), "deep file not written", GetLastError());
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);

    /* Every component in the other case (the drive and %TEMP% too). */
    lstrcpyW(other, longer + 4);
    flip_case(other);
    lstrcpyW(longer, L"\\\\?\\");
    lstrcatW(longer, other);
    file = CreateFileW(longer, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    check(file != INVALID_HANDLE_VALUE, "deep file not found in the other case", GetLastError());
    if (file != INVALID_HANDLE_VALUE) {
        check(ReadFile(file, &back, sizeof(back), &done, NULL) && back == value, "deep file read wrong in the other case", back);
        CloseHandle(file);
    }
    check(GetFileAttributesW(longer) != INVALID_FILE_ATTRIBUTES, "attributes not found in the other case", GetLastError());

    /* A wildcard listing in the other case returns the name as it was made. */
    lstrcpyW(other, longer);
    lstrcpyW(other + lstrlenW(other) - lstrlenW(L"sAVE.bIN"), L"S*.BIN");
    find = FindFirstFileW(other, &found);
    check(find != INVALID_HANDLE_VALUE && !lstrcmpW(found.cFileName, L"Save.Bin"), "listing in the other case wrong", GetLastError());
    if (find != INVALID_HANDLE_VALUE) FindClose(find);

    /* Remove it all in the other case: the file, then every directory from the deepest. */
    check(DeleteFileW(longer), "deep file not deleted in the other case", GetLastError());
    for (level = DEPTH; level > 0; level--) {
        WCHAR *cut = longer + lstrlenW(longer);
        while (cut > longer && *cut != '\\') cut--;
        *cut = 0;
        if (RemoveDirectoryW(longer)) removed++;
        else check(0, "directory not removed in the other case at level", (unsigned long)level);
    }
    lstrcpyW(path, dir);
    lstrcatW(path, L"Deep");
    check(RemoveDirectoryW(path), "top directory not removed", GetLastError());
    result("directory levels removed in the other case", (unsigned long)removed);
}

void mainCRTStartup(void)
{
    static WCHAR dir[MAX_PATH];
    DWORD length = GetTempPathW(MAX_PATH - 20, dir);

    if (!length) { say("PS5_PROTON_HANDLES_PATHS FAIL: no temporary directory\n"); ExitProcess(1); }
    lstrcatW(dir, L"HpProbe");
    CreateDirectoryW(dir, NULL);
    lstrcatW(dir, L"\\");
    files_and_handles(dir);
    deep_paths(dir);
    dir[lstrlenW(dir) - 1] = 0;
    RemoveDirectoryW(dir);

    if (failures) {
        result("failures", (unsigned long)failures);
        say("PS5_PROTON_HANDLES_PATHS FAIL\n");
        ExitProcess(1);
    }
    say("PS5_PROTON_HANDLES_PATHS PASS: 3000 files and 6000 events held together; every file read back, renamed or deleted "
        "while open as asked; a 20-level path past MAX_PATH made, opened, listed and removed in the other case\n");
    ExitProcess(0);
}
