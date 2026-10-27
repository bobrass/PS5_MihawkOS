/* SPDX-License-Identifier: GPL-3.0-or-later
 * The installer slice: one Tier 1 installer kind, end to end, in one launch, in a throwaway prefix
 * (tools/run-installer-slice.py makes it; the owner's prefixes are never used).
 *
 *   installer-slice-probe.exe KIND FIXTURES      KIND: nsis inno msi steam vcredist dxsetup
 *
 * FIXTURES is the folder with the installers (tools/build-installer-fixtures.sh, tools/fetch-installers.py) and
 * steam-app\ (tests/installers/). For each kind, in order, every step timed:
 *
 *   snapshot   what the install could touch: Program Files (both), the common Start menu and desktop, the top of
 *              system32 and syswow64, Windows\Installer, and the uninstall/fixture/runtime registry keys (both views)
 *   install    silently (/S, /VERYSILENT, /qn, /quiet); an installer still running after its budget counts as a hung
 *              prompt (an elevation or message box nobody can answer) and is ended
 *   files, registry, shortcuts, native   what it must have written; "native" is a redistributable's DLL that is no
 *              longer Wine's own (Wine's builtin and placeholder DLLs carry "Wine builtin DLL" / "Wine placeholder DLL")
 *   app        the installed program (or a program loading the redistributable) starts and passes (exit code 77)
 *   reinstall  over the top, or a repair; the files, registry and app checks again
 *   uninstall  then clean: the snapshot again, nothing left that was not there before
 *
 * The Steam kind has no Steam: the slice's own runner of installscript.vdf (Registry, Run Process with HasRunKey,
 * Run Process On Uninstall) stands in for SteamService.exe /installscript, which the console replaces with hostname.exe.
 * DirectX's redistributable has no uninstaller, so its uninstall and clean checks are reported as not applicable.
 *
 * Lines: "PS5_PROTON_INSTALLER_CHECK KIND STEP PASS|FAIL|N/A MS ms: detail"; the verdict line at the end is
 * "PS5_PROTON_INSTALLER PASS|FAIL: ...". The same program on the PC's Wine is the oracle. Ends by itself. */
#include <windows.h>

#define ARRAY_SIZE(a) ((int)(sizeof(a) / sizeof((a)[0])))

enum { MAX_ENTRIES = 12000, POOL = 6 * 1024 * 1024 / 2 };

struct reg { int view; const WCHAR *key, *value; };   /* view 32 or 64; value NULL: the key itself */
struct kind {
    const char *name;
    const WCHAR *install[2], *reinstall[2], *uninstall[2];
    DWORD budget_ms;
    const WCHAR *files[6];
    struct reg regs[4];
    const WCHAR *shortcuts[2];
    const WCHAR *native[8];
    const WCHAR *apps[2];
    const WCHAR *gone;        /* after the uninstall, this is waited for to disappear (an uninstaller copies itself) */
};

#define SM L"%ProgramData%\\Microsoft\\Windows\\Start Menu\\Programs\\"
#define DESK L"%PUBLIC%\\Desktop\\"
#define UNINST L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\"
#define NSISDIR L"%ProgramFiles(x86)%\\PwFixture NSIS"
#define INNODIR L"%ProgramFiles%\\PwFixture Inno"
#define MSIDIR L"%ProgramFiles%\\PwFixture MSI"
#define STEAMDIR L"%ProgramFiles(x86)%\\Steam\\steamapps\\common\\PwFixture"
#define VCKEY L"SOFTWARE\\Microsoft\\VisualStudio\\14.0\\VC\\Runtimes\\"
#define APP L"\"%FIX%\\steam-app\\pwapp"

static const struct kind kinds[] = {
    { "nsis", { L"\"%FIX%\\fixture-nsis.exe\" /S" }, { L"\"%FIX%\\fixture-nsis.exe\" /S" }, { L"\"" NSISDIR L"\\uninstall.exe\" /S" }, 120000,
      { NSISDIR L"\\pwapp32.exe", NSISDIR L"\\pwapp64.exe", NSISDIR L"\\pwapp.dat", NSISDIR L"\\uninstall.exe" },
      { { 32, L"Software\\PwFixture\\NSIS", L"InstallDir" }, { 32, UNINST L"PwFixtureNSIS", L"DisplayName" } },
      { SM L"PwFixture NSIS.lnk", DESK L"PwFixture NSIS.lnk" }, { NULL },
      { L"\"" NSISDIR L"\\pwapp32.exe\"", L"\"" NSISDIR L"\\pwapp64.exe\"" }, NSISDIR },
    { "inno", { L"\"%FIX%\\fixture-inno.exe\" /VERYSILENT /SUPPRESSMSGBOXES /NORESTART /SP-" },
      { L"\"%FIX%\\fixture-inno.exe\" /VERYSILENT /SUPPRESSMSGBOXES /NORESTART /SP-" },
      { L"\"" INNODIR L"\\unins000.exe\" /VERYSILENT /SUPPRESSMSGBOXES /NORESTART" }, 120000,
      { INNODIR L"\\pwapp32.exe", INNODIR L"\\pwapp64.exe", INNODIR L"\\pwapp.dat", INNODIR L"\\unins000.exe" },
      { { 64, L"Software\\PwFixture\\Inno", L"InstallDir" }, { 64, UNINST L"PwFixtureInno_is1", L"DisplayName" } },
      { SM L"PwFixture Inno.lnk", DESK L"PwFixture Inno.lnk" }, { NULL },
      { L"\"" INNODIR L"\\pwapp32.exe\"", L"\"" INNODIR L"\\pwapp64.exe\"" }, INNODIR },
    { "msi", { L"msiexec /i \"%FIX%\\fixture.msi\" /qn /norestart" }, { L"msiexec /fa \"%FIX%\\fixture.msi\" /qn /norestart" },
      { L"msiexec /x \"%FIX%\\fixture.msi\" /qn /norestart" }, 120000,
      { MSIDIR L"\\pwapp32.exe", MSIDIR L"\\pwapp64.exe", MSIDIR L"\\pwapp.dat" },
      { { 64, L"Software\\PwFixture\\MSI", L"InstallDir" } },
      { SM L"PwFixture MSI.lnk", DESK L"PwFixture MSI.lnk" }, { NULL },
      { L"\"" MSIDIR L"\\pwapp32.exe\"", L"\"" MSIDIR L"\\pwapp64.exe\"" }, MSIDIR },
    { "steam", { L"steam" }, { L"steam" }, { L"steam" }, 120000,
      { STEAMDIR L"\\pwapp32.exe", STEAMDIR L"\\pwapp64.exe", STEAMDIR L"\\pwapp.dat", STEAMDIR L"\\redist.done" },
      { { 32, L"Software\\PwFixture\\Steam", L"InstallDir" }, { 32, L"Software\\PwFixture\\Steam", L"Installed" },
        { 32, L"Software\\PwFixture\\SteamRedist", L"PwFixture redistributable" } },
      { NULL }, { NULL }, { L"\"" STEAMDIR L"\\pwapp32.exe\"", L"\"" STEAMDIR L"\\pwapp64.exe\"" }, STEAMDIR },
    { "vcredist", { L"\"%FIX%\\vc_redist.x64.exe\" /install /quiet /norestart", L"\"%FIX%\\vc_redist.x86.exe\" /install /quiet /norestart" },
      { L"\"%FIX%\\vc_redist.x64.exe\" /repair /quiet /norestart", L"\"%FIX%\\vc_redist.x86.exe\" /repair /quiet /norestart" },
      { L"\"%FIX%\\vc_redist.x64.exe\" /uninstall /quiet /norestart", L"\"%FIX%\\vc_redist.x86.exe\" /uninstall /quiet /norestart" }, 300000,
      { NULL }, { { 64, VCKEY L"x64", L"Installed" }, { 32, VCKEY L"x86", L"Installed" } }, { NULL },
      { L"%SystemRoot%\\system32\\msvcp140.dll", L"%SystemRoot%\\system32\\vcruntime140.dll", L"%SystemRoot%\\system32\\vcruntime140_1.dll",
        L"%SystemRoot%\\syswow64\\msvcp140.dll", L"%SystemRoot%\\syswow64\\vcruntime140.dll" },
      { APP L"32.exe\" /load msvcp140.dll _Query_perf_frequency", APP L"64.exe\" /load msvcp140.dll _Query_perf_frequency" }, NULL },
    { "dxsetup", { L"\"%FIX%\\directx_Jun2010_redist.exe\" /Q /T:\"%TEMP%\\pwdx\"", L"\"%TEMP%\\pwdx\\DXSETUP.exe\" /silent" },
      { L"\"%TEMP%\\pwdx\\DXSETUP.exe\" /silent" }, { NULL }, 600000,
      { NULL }, { { 0 } }, { NULL },
      { L"%SystemRoot%\\system32\\d3dx9_43.dll", L"%SystemRoot%\\system32\\d3dx11_43.dll", L"%SystemRoot%\\system32\\d3dcompiler_43.dll",
        L"%SystemRoot%\\syswow64\\d3dx9_43.dll", L"%SystemRoot%\\syswow64\\d3dx11_43.dll", L"%SystemRoot%\\syswow64\\d3dcompiler_43.dll" },
      { APP L"32.exe\" /load d3dx9_43.dll D3DXCheckVersion", APP L"64.exe\" /load d3dx9_43.dll D3DXCheckVersion" }, NULL },
};

static const struct kind *kind;
static int checks, failures;

/* ---- output, without a C runtime ---- */

static void say(const char *text)
{
    DWORD written;
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), text, lstrlenA(text), &written, NULL);
    OutputDebugStringA(text);
}

static void cat_number(char *out, ULONGLONG value)
{
    char digits[24];
    int i = 0, n = lstrlenA(out);
    do { digits[i++] = (char)('0' + value % 10); value /= 10; } while (value);
    while (i) out[n++] = digits[--i];
    out[n] = 0;
}

static void cat_wide(char *out, const WCHAR *text)
{
    int n = lstrlenA(out), i;
    for (i = 0; text && text[i] && n < 900; i++) out[n++] = text[i] < 128 ? (char)text[i] : '?';
    out[n] = 0;
}

/* verdict: 1 pass, 0 fail, -1 not applicable */
static void report(const char *step, int verdict, ULONGLONG ms, const char *detail, const WCHAR *what)
{
    char text[1024];
    lstrcpyA(text, "PS5_PROTON_INSTALLER_CHECK ");
    lstrcatA(text, kind->name);
    lstrcatA(text, " ");
    lstrcatA(text, step);
    lstrcatA(text, verdict > 0 ? " PASS " : verdict < 0 ? " N/A " : " FAIL ");
    cat_number(text, ms);
    lstrcatA(text, " ms: ");
    lstrcatA(text, detail);
    if (what) { lstrcatA(text, " "); cat_wide(text, what); }
    lstrcatA(text, "\n");
    say(text);
    if (verdict >= 0) checks++;
    if (!verdict) failures++;
}

/* ---- the prefix ---- */

static void expand(const WCHAR *in, WCHAR *out, DWORD size)
{
    if (!ExpandEnvironmentStringsW(in, out, size)) lstrcpynW(out, in, size);
}

/* A command, waited for; FALSE and *hung when it was still running after budget_ms (then it is ended). */
static BOOL run(const WCHAR *command, DWORD budget_ms, DWORD *code, BOOL *hung)
{
    static WCHAR line[2048];
    STARTUPINFOW startup = { 0 };
    PROCESS_INFORMATION process;
    startup.cb = sizeof(startup);
    *hung = FALSE;
    *code = (DWORD)-1;
    expand(command, line, ARRAY_SIZE(line));
    if (!CreateProcessW(NULL, line, NULL, NULL, FALSE, 0, NULL, NULL, &startup, &process)) return FALSE;
    if (WaitForSingleObject(process.hProcess, budget_ms) == WAIT_TIMEOUT) {
        *hung = TRUE;
        TerminateProcess(process.hProcess, 1);
        WaitForSingleObject(process.hProcess, 10000);
    }
    GetExitCodeProcess(process.hProcess, code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return !*hung;
}

static BOOL exists(const WCHAR *path)
{
    WCHAR full[MAX_PATH * 2];
    expand(path, full, ARRAY_SIZE(full));
    return GetFileAttributesW(full) != INVALID_FILE_ATTRIBUTES;
}

static HKEY open_key(int view, const WCHAR *key, REGSAM access)
{
    HKEY handle;
    REGSAM flags = access | (view == 32 ? KEY_WOW64_32KEY : KEY_WOW64_64KEY);
    return RegOpenKeyExW(HKEY_LOCAL_MACHINE, key, 0, flags, &handle) ? NULL : handle;
}

static BOOL reg_present(const struct reg *r)
{
    HKEY handle = open_key(r->view, r->key, KEY_QUERY_VALUE);
    BOOL found;
    if (!handle) return FALSE;
    found = !r->value || !RegQueryValueExW(handle, r->value, NULL, NULL, NULL, NULL);
    RegCloseKey(handle);
    return found;
}

/* A DLL that is there and no longer Wine's own. */
static BOOL native_dll(const WCHAR *path)
{
    static const char *markers[] = { "Wine builtin DLL", "Wine placeholder DLL" };
    WCHAR full[MAX_PATH * 2];
    char head[1024];
    DWORD done = 0;
    HANDLE file;
    int i, m;
    expand(path, full, ARRAY_SIZE(full));
    file = CreateFileW(full, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (file == INVALID_HANDLE_VALUE) return FALSE;
    ReadFile(file, head, sizeof(head), &done, NULL);
    CloseHandle(file);
    if (done < 64 || head[0] != 'M' || head[1] != 'Z') return FALSE;
    for (m = 0; m < 2; m++) {
        int length = lstrlenA(markers[m]);
        for (i = 0; i + length <= (int)done; i++) {
            int k = 0;
            while (k < length && head[i + k] == markers[m][k]) k++;
            if (k == length) return FALSE;
        }
    }
    return TRUE;
}

/* ---- the snapshot: one string per file (with its size) or registry key ---- */

static WCHAR pool[POOL];
static struct snapshot { const WCHAR *entries[MAX_ENTRIES]; int count; } before, after;
static int pool_used;

static void add(struct snapshot *s, const WCHAR *text)
{
    int length = lstrlenW(text) + 1;
    if (s->count >= MAX_ENTRIES || pool_used + length > POOL) return;
    lstrcpyW(pool + pool_used, text);
    s->entries[s->count++] = pool + pool_used;
    pool_used += length;
}

static void walk(struct snapshot *s, const WCHAR *dir, BOOL recurse)
{
    WCHAR pattern[MAX_PATH * 2], entry[MAX_PATH * 2];
    WIN32_FIND_DATAW found;
    HANDLE find;
    lstrcpyW(pattern, dir);
    lstrcatW(pattern, L"\\*");
    if ((find = FindFirstFileW(pattern, &found)) == INVALID_HANDLE_VALUE) return;
    do {
        char size[24] = "";
        int i;
        if (!lstrcmpW(found.cFileName, L".") || !lstrcmpW(found.cFileName, L"..")) continue;
        lstrcpyW(entry, dir);
        lstrcatW(entry, L"\\");
        lstrcatW(entry, found.cFileName);
        if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            lstrcatW(entry, L"\\");
            add(s, entry);
            entry[lstrlenW(entry) - 1] = 0;
            if (recurse) walk(s, entry, TRUE);
        } else {
            WCHAR *at = entry + lstrlenW(entry);
            cat_number(size, ((ULONGLONG)found.nFileSizeHigh << 32) | found.nFileSizeLow);
            *at++ = '|';
            for (i = 0; size[i]; i++) *at++ = size[i];
            *at = 0;
            add(s, entry);
        }
    } while (FindNextFileW(find, &found));
    FindClose(find);
}

static void walk_keys(struct snapshot *s, int view, const WCHAR *key)
{
    WCHAR name[256], entry[600];
    HKEY handle = open_key(view, key, KEY_ENUMERATE_SUB_KEYS | KEY_QUERY_VALUE);
    DWORD i, length;
    if (!handle) return;
    lstrcpyW(entry, view == 32 ? L"HKLM32\\" : L"HKLM64\\");
    lstrcatW(entry, key);
    add(s, entry);
    for (i = 0; length = ARRAY_SIZE(name), !RegEnumKeyExW(handle, i, name, &length, NULL, NULL, NULL, NULL); i++) {
        WCHAR child[600];
        lstrcpyW(child, key);
        lstrcatW(child, L"\\");
        lstrcatW(child, name);
        walk_keys(s, view, child);
    }
    RegCloseKey(handle);
}

static void take(struct snapshot *s)
{
    static const struct { const WCHAR *dir; BOOL recurse; } dirs[] = {
        { L"%ProgramFiles%", TRUE }, { L"%ProgramFiles(x86)%", TRUE }, { L"%ProgramData%\\Microsoft\\Windows\\Start Menu", TRUE },
        { L"%PUBLIC%\\Desktop", TRUE }, { L"%SystemRoot%\\system32", FALSE }, { L"%SystemRoot%\\syswow64", FALSE },
        { L"%SystemRoot%\\Installer", TRUE },
    };
    static const WCHAR *keys[] = { UNINST, L"Software\\PwFixture", L"SOFTWARE\\Microsoft\\VisualStudio\\14.0\\VC\\Runtimes" };
    WCHAR full[MAX_PATH * 2];
    int i, view;
    s->count = 0;
    for (i = 0; i < ARRAY_SIZE(dirs); i++) { expand(dirs[i].dir, full, ARRAY_SIZE(full)); walk(s, full, dirs[i].recurse); }
    for (view = 32; view <= 64; view += 32)
        for (i = 0; i < ARRAY_SIZE(keys); i++) walk_keys(s, view, keys[i]);
}

static BOOL listed(const struct snapshot *s, const WCHAR *entry)
{
    int i;
    for (i = 0; i < s->count; i++) if (!lstrcmpiW(s->entries[i], entry)) return TRUE;
    return FALSE;
}

/* What the uninstall left: entries added or changed since the first snapshot, and ones it removed. */
static void clean_check(ULONGLONG started)
{
    int i, added = 0, removed = 0;
    take(&after);
    for (i = 0; i < after.count; i++)
        if (!listed(&before, after.entries[i])) { if (added++ < 8) report("clean-leftover", -1, 0, "left:", after.entries[i]); }
    for (i = 0; i < before.count; i++)
        if (!listed(&after, before.entries[i])) { if (removed++ < 8) report("clean-removed", -1, 0, "gone or changed:", before.entries[i]); }
    {
        char detail[96] = "added or changed ";
        cat_number(detail, (ULONGLONG)added);
        lstrcatA(detail, ", removed ");
        cat_number(detail, (ULONGLONG)removed);
        lstrcatA(detail, " (of the snapshot's entries)");
        report("clean", !added && !removed, GetTickCount64() - started, detail, NULL);
    }
}

/* ---- the slice's own installscript.vdf runner, for the Steam kind ---- */

static char vdf[16384];
static int vdf_at, vdf_length;

/* The next token: a quoted string (with \\ unescaped), "{" or "}"; 0 at the end. */
static int token(char *out, int size)
{
    int n = 0;
    while (vdf_at < vdf_length && (vdf[vdf_at] == ' ' || vdf[vdf_at] == '\t' || vdf[vdf_at] == '\r' || vdf[vdf_at] == '\n')) vdf_at++;
    if (vdf_at >= vdf_length) return 0;
    if (vdf[vdf_at] == '{' || vdf[vdf_at] == '}') { out[0] = vdf[vdf_at++]; out[1] = 0; return out[0]; }
    if (vdf[vdf_at] != '"') { vdf_at = vdf_length; return 0; }
    vdf_at++;
    while (vdf_at < vdf_length && vdf[vdf_at] != '"') {
        char c = vdf[vdf_at++];
        if (c == '\\' && vdf_at < vdf_length) c = vdf[vdf_at++];
        if (n < size - 1) out[n++] = c;
    }
    vdf_at++;
    out[n] = 0;
    return '"';
}

static void skip_block(void)
{
    char text[512];
    int depth = 1, t;
    while (depth && (t = token(text, sizeof(text)))) depth += t == '{' ? 1 : t == '}' ? -1 : 0;
}

/* %INSTALLDIR% replaced; a HKEY_LOCAL_MACHINE\ prefix removed (only HKLM is used by the fixture) */
static void steam_text(const char *in, WCHAR *out, int size)
{
    static WCHAR wide[1024], dir[MAX_PATH * 2];
    const WCHAR *at;
    int n = 0, i;
    MultiByteToWideChar(CP_UTF8, 0, in, -1, wide, ARRAY_SIZE(wide));
    expand(STEAMDIR, dir, ARRAY_SIZE(dir));
    at = wide;
    if (!_wcsnicmp(at, L"HKEY_LOCAL_MACHINE\\", 19)) at += 19;
    while (*at && n < size - 1) {
        if (!_wcsnicmp(at, L"%INSTALLDIR%", 12)) { for (i = 0; dir[i] && n < size - 1; i++) out[n++] = dir[i]; at += 12; }
        else out[n++] = *at++;
    }
    out[n] = 0;
}

static int steam_failures;

static void steam_registry(BOOL remove)
{
    char key[512], type[64], name[256], value[512];
    while (token(key, sizeof(key)) == '"') {
        WCHAR wkey[512];
        HKEY handle = NULL;
        steam_text(key, wkey, ARRAY_SIZE(wkey));
        token(value, sizeof(value));   /* { */
        if (remove) { if (RegDeleteKeyExW(HKEY_LOCAL_MACHINE, wkey, KEY_WOW64_32KEY, 0)) steam_failures++; }
        else if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, wkey, 0, NULL, 0, KEY_SET_VALUE | KEY_WOW64_32KEY, NULL, &handle, NULL)) steam_failures++;
        while (token(type, sizeof(type)) == '"') {
            token(value, sizeof(value));   /* { */
            while (token(name, sizeof(name)) == '"') {
                WCHAR wname[256], wvalue[512];
                token(value, sizeof(value));
                steam_text(name, wname, ARRAY_SIZE(wname));
                steam_text(value, wvalue, ARRAY_SIZE(wvalue));
                if (!handle) continue;
                if (!lstrcmpiA(type, "dword")) {
                    DWORD number = 0;
                    int i;
                    for (i = 0; value[i] >= '0' && value[i] <= '9'; i++) number = number * 10 + (DWORD)(value[i] - '0');
                    if (RegSetValueExW(handle, wname, 0, REG_DWORD, (BYTE *)&number, sizeof(number))) steam_failures++;
                } else if (RegSetValueExW(handle, wname, 0, REG_SZ, (BYTE *)wvalue, (lstrlenW(wvalue) + 1) * 2)) steam_failures++;
            }
        }
        if (handle) RegCloseKey(handle);
    }
}

/* Every entry: its "process N"/"command N" pairs, skipped when its HasRunKey holds the entry's name. mode 0 runs an
 * install's entries (and marks HasRunKey), 1 an uninstall's, 2 only forgets an install's HasRunKey marks. */
static void steam_run(int mode, int *ran, int *skipped)
{
    char entry[256], name[256], value[512];
    while (token(entry, sizeof(entry)) == '"') {
        static char processes[4][512], commands[4][512];
        WCHAR has_run[512] = L"", wentry[256];
        int i, count = 0;
        for (i = 0; i < 4; i++) processes[i][0] = commands[i][0] = 0;
        token(value, sizeof(value));   /* { */
        while (token(name, sizeof(name)) == '"') {
            token(value, sizeof(value));
            if (!lstrcmpiA(name, "HasRunKey")) steam_text(value, has_run, ARRAY_SIZE(has_run));
            else if (!_strnicmp(name, "process ", 8) && name[8] >= '1' && name[8] <= '4') { lstrcpyA(processes[name[8] - '1'], value); if (name[8] - '0' > count) count = name[8] - '0'; }
            else if (!_strnicmp(name, "command ", 8) && name[8] >= '1' && name[8] <= '4') lstrcpyA(commands[name[8] - '1'], value);
        }
        steam_text(entry, wentry, ARRAY_SIZE(wentry));
        if (!mode && has_run[0]) {
            struct reg mark = { 32, has_run, wentry };
            if (reg_present(&mark)) { (*skipped)++; continue; }
        }
        for (i = 0; i < count && mode < 2; i++) {
            WCHAR process[512], command[512], line[1100];
            DWORD code;
            BOOL hung;
            steam_text(processes[i], process, ARRAY_SIZE(process));
            steam_text(commands[i], command, ARRAY_SIZE(command));
            lstrcpyW(line, L"\"");
            lstrcatW(line, process);
            lstrcatW(line, L"\" ");
            lstrcatW(line, command);
            if (!run(line, 60000, &code, &hung) || code) steam_failures++;
            (*ran)++;
        }
        if (has_run[0]) {
            HKEY handle;
            if (mode) {
                if ((handle = open_key(32, has_run, KEY_SET_VALUE))) { RegDeleteValueW(handle, wentry); RegCloseKey(handle); }
                RegDeleteKeyExW(HKEY_LOCAL_MACHINE, has_run, KEY_WOW64_32KEY, 0);
            } else if (!RegCreateKeyExW(HKEY_LOCAL_MACHINE, has_run, 0, NULL, 0, KEY_SET_VALUE | KEY_WOW64_32KEY, NULL, &handle, NULL)) {
                DWORD one = 1;
                RegSetValueExW(handle, wentry, 0, REG_DWORD, (BYTE *)&one, sizeof(one));
                RegCloseKey(handle);
            }
        }
    }
}

/* What Steam does for an install (the download copied into steamapps\common, then the script) or an uninstall
 * (the uninstall entries, the script's registry keys removed, the folder deleted). Returns the failures. */
static int steam(int phase /* 0 install, 1 reinstall, 2 uninstall */, int *ran, int *skipped)
{
    static const WCHAR *names[] = { L"pwapp32.exe", L"pwapp64.exe", L"pwapp.dat", L"installscript.vdf" };
    WCHAR dir[MAX_PATH * 2], from[MAX_PATH * 2], to[MAX_PATH * 2], script[MAX_PATH * 2];
    char section[128];
    DWORD done = 0;
    HANDLE file;
    unsigned i;
    steam_failures = 0;
    expand(STEAMDIR, dir, ARRAY_SIZE(dir));
    if (phase == 0) {
        WCHAR *at;
        lstrcpyW(to, dir);
        for (at = to + 3; *at; at++) if (*at == '\\') { *at = 0; CreateDirectoryW(to, NULL); *at = '\\'; }
        CreateDirectoryW(to, NULL);
        for (i = 0; i < ARRAY_SIZE(names); i++) {
            expand(L"%FIX%\\steam-app\\", from, ARRAY_SIZE(from));
            lstrcatW(from, names[i]);
            lstrcpyW(to, dir); lstrcatW(to, L"\\"); lstrcatW(to, names[i]);
            if (!CopyFileW(from, to, FALSE)) steam_failures++;
        }
    }
    lstrcpyW(script, dir);
    lstrcatW(script, L"\\installscript.vdf");
    file = CreateFileW(script, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (file == INVALID_HANDLE_VALUE) return steam_failures + 1;
    ReadFile(file, vdf, sizeof(vdf) - 1, &done, NULL);
    CloseHandle(file);
    vdf_length = (int)done;
    vdf_at = 0;
    if (token(section, sizeof(section)) != '"' || token(section, sizeof(section)) != '{') return steam_failures + 1;
    while (token(section, sizeof(section)) == '"') {
        token(section + 64, 4);   /* { */
        if (!lstrcmpiA(section, "Registry")) steam_registry(phase == 2);
        else if (!lstrcmpiA(section, "Run Process")) steam_run(phase < 2 ? 0 : 2, ran, skipped);
        else if (!lstrcmpiA(section, "Run Process On Uninstall") && phase == 2) steam_run(1, ran, skipped);
        else skip_block();
    }
    if (phase == 2) {
        RegDeleteKeyExW(HKEY_LOCAL_MACHINE, L"Software\\PwFixture", KEY_WOW64_32KEY, 0);   /* empty once its children are gone */
        for (i = 0; i < ARRAY_SIZE(names); i++) { lstrcpyW(to, dir); lstrcatW(to, L"\\"); lstrcatW(to, names[i]); DeleteFileW(to); }
        if (!RemoveDirectoryW(dir)) steam_failures++;
        /* The folders Steam itself owns stay with Steam; here they were made by this runner, so they go too. */
        for (i = 0; i < 3; i++) { WCHAR *cut = dir + lstrlenW(dir); while (cut > dir && *cut != '\\') cut--; *cut = 0; RemoveDirectoryW(dir); }
    }
    return steam_failures;
}

/* ---- the steps ---- */

static void step_commands(const char *step, const WCHAR *const *commands)
{
    ULONGLONG started = GetTickCount64();
    int i, ok = 1;
    char detail[200] = "exit codes";
    if (!lstrcmpW(commands[0], L"steam")) {
        int ran = 0, skipped = 0, failed = steam(!lstrcmpA(step, "install") ? 0 : !lstrcmpA(step, "reinstall") ? 1 : 2, &ran, &skipped);
        lstrcpyA(detail, "installscript runner: processes run ");
        cat_number(detail, (ULONGLONG)ran);
        lstrcatA(detail, ", skipped by HasRunKey ");
        cat_number(detail, (ULONGLONG)skipped);
        lstrcatA(detail, ", failures ");
        cat_number(detail, (ULONGLONG)failed);
        /* the reinstall must skip the redistributable that already ran */
        report(step, !failed && (lstrcmpA(step, "reinstall") || (skipped == 1 && !ran)), GetTickCount64() - started, detail, NULL);
        return;
    }
    for (i = 0; i < 2 && commands[i]; i++) {
        DWORD code;
        BOOL hung;
        BOOL started_ok = run(commands[i], kind->budget_ms, &code, &hung);
        lstrcatA(detail, " ");
        if (hung) { lstrcatA(detail, "HUNG (a prompt nobody answers?)"); ok = 0; break; }
        if (!started_ok) { lstrcatA(detail, "not started"); ok = 0; break; }
        cat_number(detail, code);
        /* 3010/1641: a restart is wanted; 1638: a newer one of the same redistributable is there */
        if (code && code != 3010 && code != 1641 && code != 1638) ok = 0;
    }
    report(step, ok, GetTickCount64() - started, detail, NULL);
}

static void step_contents(const char *step)
{
    ULONGLONG started = GetTickCount64();
    int i;
    char name[48];
    for (i = 0; i < 6 && kind->files[i]; i++) {
        lstrcpyA(name, step); lstrcatA(name, "-file");
        report(name, exists(kind->files[i]), GetTickCount64() - started, "present:", kind->files[i]);
    }
    for (i = 0; i < 4 && kind->regs[i].key; i++) {
        lstrcpyA(name, step); lstrcatA(name, "-registry");
        report(name, reg_present(&kind->regs[i]), GetTickCount64() - started, kind->regs[i].view == 32 ? "HKLM (32-bit view)" : "HKLM (64-bit view)",
               kind->regs[i].key);
    }
    for (i = 0; i < 2 && kind->shortcuts[i]; i++) {
        lstrcpyA(name, step); lstrcatA(name, "-shortcut");
        report(name, exists(kind->shortcuts[i]), GetTickCount64() - started, "present:", kind->shortcuts[i]);
    }
    for (i = 0; i < 8 && kind->native[i]; i++) {
        lstrcpyA(name, step); lstrcatA(name, "-native");
        report(name, native_dll(kind->native[i]), GetTickCount64() - started, "not Wine's own:", kind->native[i]);
    }
    for (i = 0; i < 2 && kind->apps[i]; i++) {
        ULONGLONG app_started = GetTickCount64();
        DWORD code;
        BOOL hung;
        char detail[64] = "exit code ";
        run(kind->apps[i], 30000, &code, &hung);
        cat_number(detail, code);
        lstrcpyA(name, step); lstrcatA(name, "-app");
        report(name, !hung && code == 77, GetTickCount64() - app_started, detail, kind->apps[i]);
    }
}

void mainCRTStartup(void)
{
    static WCHAR arguments[3][MAX_PATH * 2];
    WCHAR *line = GetCommandLineW();
    ULONGLONG started;
    unsigned i;
    int n;

    /* the program's name, then KIND and FIXTURES (the folder may be quoted) */
    for (n = 0; n < 3; n++) {
        int k = 0;
        while (*line == ' ') line++;
        if (*line == '"') { line++; while (*line && *line != '"' && k < MAX_PATH * 2 - 1) arguments[n][k++] = *line++; if (*line) line++; }
        else while (*line && *line != ' ' && k < MAX_PATH * 2 - 1) arguments[n][k++] = *line++;
        arguments[n][k] = 0;
    }
    for (i = 0; i < ARRAY_SIZE(kinds); i++) {
        char name[16];
        int k;
        for (k = 0; arguments[1][k] && k < 15; k++) name[k] = (char)arguments[1][k];
        name[k] = 0;
        if (!lstrcmpiA(name, kinds[i].name)) kind = &kinds[i];
    }
    if (!kind || !arguments[2][0]) {
        say("PS5_PROTON_INSTALLER FAIL: usage: installer-slice-probe.exe nsis|inno|msi|steam|vcredist|dxsetup FIXTURES\n");
        ExitProcess(2);
    }
    SetEnvironmentVariableW(L"FIX", arguments[2]);

    started = GetTickCount64();
    take(&before);
    {
        char detail[64] = "entries ";
        cat_number(detail, (ULONGLONG)before.count);
        report("snapshot", -1, GetTickCount64() - started, detail, NULL);
    }
    step_commands("install", kind->install);
    step_contents("install");
    step_commands("reinstall", kind->reinstall);
    step_contents("reinstall");
    if (kind->uninstall[0]) {
        step_commands("uninstall", kind->uninstall);
        if (kind->gone) {
            ULONGLONG waited = GetTickCount64();
            while (exists(kind->gone) && GetTickCount64() - waited < 60000) Sleep(250);
            report("uninstall-removed", !exists(kind->gone), GetTickCount64() - waited, "folder gone:", kind->gone);
        }
        clean_check(GetTickCount64());
    } else {
        report("uninstall", -1, 0, "the DirectX redistributable has no uninstaller", NULL);
        report("clean", -1, 0, "not applicable without an uninstaller", NULL);
    }
    {
        char text[200] = "PS5_PROTON_INSTALLER ";
        lstrcatA(text, failures ? "FAIL: " : "PASS: ");
        lstrcatA(text, kind->name);
        lstrcatA(text, ", ");
        cat_number(text, (ULONGLONG)checks);
        lstrcatA(text, " checks, ");
        cat_number(text, (ULONGLONG)failures);
        lstrcatA(text, " failed, ");
        cat_number(text, GetTickCount64() - started);
        lstrcatA(text, " ms in all\n");
        say(text);
    }
    ExitProcess(failures ? 1 : 0);
}
