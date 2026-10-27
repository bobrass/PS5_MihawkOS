/* SPDX-License-Identifier: GPL-3.0-or-later
 * Real Start/taskbar/Explorer integration with disposable files. Saves and
 * restores the user's pins. Host and console run the same PE32/PE64 program. */
#define COBJMACROS
#include <windows.h>
#include <ole2.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <uxtheme.h>
#include "appearance.h"
#include "pw_window_snapshot.h"
#include "pw_explorer_layout.h"
#include <dwmapi.h>
#include <stdlib.h>
#include <string.h>
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
int _fltused; /* no C runtime: the command bar's layout is floating point */
static unsigned checks, failures;
static HANDLE log_file = INVALID_HANDLE_VALUE;
static const WCHAR pin_key[] = L"Software\\Mihawk\\Shell";
static void line(const char *text)
{
    DWORD written;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), text, lstrlenA(text), &written, NULL);
    if (log_file != INVALID_HANDLE_VALUE) WriteFile(log_file, text, lstrlenA(text), &written, NULL);
    OutputDebugStringA(text);
}
static void check(BOOL okay, const char *name)
{
    char text[256]; ++checks; failures += !okay;
    wsprintfA(text, "shell-probe %s: %s error=%lu\n", okay ? "PASS" : "FAIL", name, GetLastError()); line(text);
}
static void pump(DWORD ms)
{
    DWORD start = GetTickCount(); MSG msg;
    while (GetTickCount() - start < ms)
    {
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        MsgWaitForMultipleObjects(0, NULL, FALSE, 10, QS_ALLINPUT);
    }
}
static unsigned captures;
static void capture_requirements(HWND target, const char *phase)
{
    RECT visible; POINT pointer = {0, 0}; char note[200];
    /* File Explorer (Wine patch 1150) is the prototype's window: tools/run-shell.py composes the prototype's sprites over
     * its surfaces (tools/pw_explorer_expected.py) and compares, as desktop-session does. Start, the desktop and the
     * preview are judged by their material there. */
    if (strncmp(phase, "explorer", 8)) return;
    if (FAILED(DwmGetWindowAttribute(target, DWMWA_EXTENDED_FRAME_BOUNDS, &visible, sizeof(visible)))) GetWindowRect(target, &visible);
    GetCursorPos(&pointer);
    wsprintfA(note, "shell-explorer number=%u frame=%ld,%ld,%ld,%ld active=%d pointer=%ld,%ld\n", captures, visible.left,
              visible.top, visible.right, visible.bottom, GetForegroundWindow() == target, pointer.x, pointer.y); line(note);
}
/* judged: TRUE when the host computes the expected pixel (DWM materials). The
 * pointer stays put for previews, which close when it leaves their button. */
static BOOL capture_shell_ex(HWND target, const char *phase, COLORREF expected, BOOL judged, BOOL keep_pointer)
{
    char path[MAX_PATH], request[MAX_PATH], note[256]; WCHAR host[MAX_PATH]; RECT rect; POINT point = {0,0};
    DWORD written, read; HANDLE file; BOOL okay = FALSE; unsigned char pixel[3];
    ++captures; capture_requirements(target, phase); ClientToScreen(target, &point); GetClientRect(target, &rect);
    int x = point.x + 10, y = point.y + 10;
    if (!strncmp(phase, "start", 5))
    {
        RECT window; GetWindowRect(target, &window);
        wsprintfA(note, "shell-glass number=%u window=%ld,%ld,%ld,%ld\n", captures, window.left, window.top, window.right, window.bottom); line(note);
    }
    if (!keep_pointer) SetCursorPos(1800, 100);
    pump(150);
    if (GetEnvironmentVariableW(L"MIHAWK_SHELL_CAPTURE_DIR", host, MAX_PATH))
    {
        /* Host-only GDI screen oracle, never presented as console evidence. */
        BITMAPINFO info = {0}; BITMAPFILEHEADER header = {0}; void *bits; HDC screen = GetDC(NULL), dc = CreateCompatibleDC(screen);
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); info.bmiHeader.biWidth = 1920; info.bmiHeader.biHeight = -1080;
        info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
        HBITMAP bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, NULL, 0); HGDIOBJ old = bitmap ? SelectObject(dc, bitmap) : NULL;
        if (bitmap && BitBlt(dc, 0, 0, 1920, 1080, screen, 0, 0, SRCCOPY))
        {
            WCHAR destination[MAX_PATH]; wsprintfW(destination, L"%s\\shell-%u-%u.bmp", host, (unsigned)sizeof(void *) * 8, captures);
            file = CreateFileW(destination, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, 0, NULL);
            header.bfType = 0x4d42; header.bfOffBits = sizeof(header) + sizeof(info.bmiHeader); header.bfSize = header.bfOffBits + 1920 * 1080 * 4;
            if (file != INVALID_HANDLE_VALUE)
            {
                okay = WriteFile(file, &header, sizeof(header), &written, NULL) && WriteFile(file, &info.bmiHeader, sizeof(info.bmiHeader), &written, NULL) && WriteFile(file, bits, 1920 * 1080 * 4, &written, NULL);
                CloseHandle(file);
                const unsigned char *sample = (const unsigned char *)bits + (y * 1920 + x) * 4;
                okay = okay && (judged || (sample[2] == GetRValue(expected) && sample[1] == GetGValue(expected) && sample[0] == GetBValue(expected)));
            }
        }
        if (old) SelectObject(dc, old); if (bitmap) DeleteObject(bitmap); DeleteDC(dc); ReleaseDC(NULL, screen);
    }
    else if (GetEnvironmentVariableW(L"MIHAWK_SHELL_CAPTURE", host, MAX_PATH))
    {
        wsprintfA(path, "Z:\\data\\ps5proton\\logs\\game-frame-%02u.ppm", captures);
        lstrcpyA(request, "Z:\\data\\ps5proton\\logs\\game-frame.ppm.broker-request"); DeleteFileA(path);
        file = CreateFileA(request, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, 0, NULL);
        if (file == INVALID_HANDLE_VALUE) return FALSE; CloseHandle(file);
        DWORD start = GetTickCount();
        while (GetTickCount() - start < 10000)
        {
            char header[18];
            file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
            if (file != INVALID_HANDLE_VALUE)
            {
                if (ReadFile(file, header, 17, &read, NULL) && read == 17 && !memcmp(header, "P6\n1920 1080\n255\n", 17) && GetFileSize(file, NULL) == 17 + 1920 * 1080 * 3)
                {
                    SetFilePointer(file, 17 + (y * 1920 + x) * 3, NULL, FILE_BEGIN);
                    okay = ReadFile(file, pixel, 3, &read, NULL) && read == 3 &&
                           (judged || (pixel[0] == GetRValue(expected) && pixel[1] == GetGValue(expected) && pixel[2] == GetBValue(expected)));
                    CloseHandle(file); break;
                }
                CloseHandle(file);
            }
            pump(20);
        }
    }
    else return TRUE; /* Semantic-only diagnostic requested explicitly by its caller. */
    wsprintfA(note, "shell-capture number=%u phase=%s point=%d,%d expected=%u,%u,%u result=%s\n", captures, phase, x, y, GetRValue(expected), GetGValue(expected), GetBValue(expected), okay ? (judged ? "HOST" : "PASS") : "FAIL"); line(note);
    return okay;
}
typedef HRESULT (WINAPI *open_theme_fn)(const WCHAR *, const WCHAR *, const WCHAR *, HANDLE *, DWORD);
typedef HRESULT (WINAPI *apply_theme_fn)(HANDLE, char *, HWND);
typedef HRESULT (WINAPI *close_theme_fn)(HANDLE);
static HRESULT apply_theme(const WCHAR *path, const WCHAR *colour, const WCHAR *size)
{
    HMODULE module = LoadLibraryW(L"uxtheme.dll"); HANDLE theme = NULL; HRESULT result = E_NOTIMPL;
    if (!module) return result;
    open_theme_fn open = (open_theme_fn)GetProcAddress(module, MAKEINTRESOURCEA(2));
    apply_theme_fn apply = (apply_theme_fn)GetProcAddress(module, MAKEINTRESOURCEA(4));
    close_theme_fn close = (close_theme_fn)GetProcAddress(module, MAKEINTRESOURCEA(3));
    if (open && apply && close)
    {
        if (!path) result = apply(NULL, NULL, NULL);
        else if (SUCCEEDED(result = open(path, colour, size, &theme, 0))) { result = apply(theme, NULL, NULL); close(theme); }
    }
    FreeLibrary(module); pump(150); return result;
}

static HWND wait_window(const WCHAR *cls, DWORD ms)
{
    DWORD start = GetTickCount(); HWND window;
    do { window = FindWindowW(cls, NULL); if (window && IsWindowVisible(window)) return window; pump(20); } while (GetTickCount() - start < ms);
    return NULL;
}
static BOOL file_exists(const WCHAR *path) { return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES; }
/* A run cut short leaves its fixed-name fixtures behind, and the next run's exclusive create then fails (error 183 for
 * "Mihawk shell qualification" in gate-ntsync-2026-10-17-01, shell32 and shell64): remove only that fixture, a file or
 * a folder of files, and say so. */
static void remove_leftover(const WCHAR *path, const char *what)
{
    DWORD attributes = GetFileAttributesW(path);
    if (attributes == INVALID_FILE_ATTRIBUTES) return;
    if (attributes & FILE_ATTRIBUTE_DIRECTORY)
    {
        WCHAR mask[MAX_PATH], entry[MAX_PATH]; WIN32_FIND_DATAW found; HANDLE search;
        PathCombineW(mask, path, L"*");
        if ((search = FindFirstFileW(mask, &found)) != INVALID_HANDLE_VALUE)
        {
            do if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) { PathCombineW(entry, path, found.cFileName); DeleteFileW(entry); }
            while (FindNextFileW(search, &found));
            FindClose(search);
        }
        RemoveDirectoryW(path);
    }
    else DeleteFileW(path);
    char note[160]; wsprintfA(note, "shell-probe removed leftover from an earlier run: %s (%s)\n", what, file_exists(path) ? "still there" : "gone");
    line(note);
}
static BOOL write_file(const WCHAR *path)
{
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL); DWORD n;
    if (file == INVALID_HANDLE_VALUE) return FALSE;
    BOOL result = WriteFile(file, "Mihawk shell probe\n", 19, &n, NULL) && n == 19; CloseHandle(file); return result;
}
/* A key as a user presses it, with a modifier (VK_CONTROL, VK_MENU: File Explorer's Alt+arrows) or none. */
static BOOL key(WORD code, WORD modifier)
{
    INPUT events[4] = {{0}}; unsigned count = modifier ? 4 : 2;
    for (unsigned i = 0; i < count; ++i) events[i].type = INPUT_KEYBOARD;
    if (modifier) { events[0].ki.wVk = events[3].ki.wVk = modifier; events[1].ki.wVk = events[2].ki.wVk = code; events[2].ki.dwFlags = events[3].ki.dwFlags = KEYEVENTF_KEYUP; }
    else { events[0].ki.wVk = events[1].ki.wVk = code; events[1].ki.dwFlags = KEYEVENTF_KEYUP; }
    return SendInput(count, events, sizeof(INPUT)) == count;
}
static BOOL CALLBACK find_start(HWND child, LPARAM param)
{
    WCHAR title[64]; GetWindowTextW(child, title, ARRAY_SIZE(title));
    if (!lstrcmpW(title, L"Start")) *(HWND *)param = child;
    return TRUE;
}
static HWND open_start(HWND button)
{
    PostMessageW(button, BM_CLICK, 0, 0); return wait_window(L"MihawkStart", 5000);
}
static void close_start(HWND window)
{
    if (!window) return; PostMessageW(window, WM_CLOSE, 0, 0);
    for (unsigned i = 0; i < 100 && IsWindow(window); ++i) pump(10);
}
static BOOL pins_contain(const WCHAR *path)
{
    static WCHAR values[18 * MAX_PATH + 2]; WCHAR *p; DWORD size = sizeof(values), type;
    if (RegGetValueW(HKEY_CURRENT_USER, pin_key, L"StartPins", RRF_RT_REG_MULTI_SZ, &type, values, &size)) return FALSE;
    values[ARRAY_SIZE(values) - 1] = 0;
    for (p = values; *p; p += lstrlenW(p) + 1) if (!lstrcmpiW(p, path)) return TRUE;
    return FALSE;
}
static BOOL click_point(HWND window, int x, int y)
{
    POINT point = {x,y}; INPUT events[3] = {{0}};
    ClientToScreen(window, &point);
    for (unsigned i = 0; i < 3; ++i) events[i].type = INPUT_MOUSE;
    events[0].mi.dx = point.x * 65535 / 1919; events[0].mi.dy = point.y * 65535 / 1079;
    events[0].mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
    events[1].mi.dwFlags = MOUSEEVENTF_LEFTDOWN; events[2].mi.dwFlags = MOUSEEVENTF_LEFTUP;
    return SendInput(3, events, sizeof(INPUT)) == 3;
}
static HWND find_listview;
static BOOL CALLBACK find_list(HWND window, LPARAM unused)
{
    WCHAR cls[64]; (void)unused; GetClassNameW(window, cls, ARRAY_SIZE(cls));
    if (!lstrcmpiW(cls, WC_LISTVIEWW) && IsWindowVisible(window)) find_listview = window;
    return TRUE;
}
/* File Explorer's title names its location: "<name> - File Explorer" (Wine patch 1150). Waits for it up to 3 s. */
static BOOL titled(HWND window, const WCHAR *name)
{
    WCHAR expected[MAX_PATH + 32], actual[MAX_PATH + 32]; DWORD started = GetTickCount();
    wsprintfW(expected, L"%s - File Explorer", name);
    do { GetWindowTextW(window, actual, ARRAY_SIZE(actual)); if (!lstrcmpW(actual, expected)) return TRUE; pump(10); }
    while (GetTickCount() - started < 3000);
    return FALSE;
}
static BOOL explorer_key(HWND window, WORD code, WORD modifier)
{
    SetForegroundWindow(window);
    for (unsigned i = 0; i < 100 && GetForegroundWindow() != window; ++i) pump(10);
    BOOL queued = key(code, modifier); pump(300); return queued;
}
static void address(HWND window, const WCHAR *path)
{
    HWND edit = GetDlgItem(window, 4300); /* the address box's edit, hidden until Ctrl+L or Alt+D */
    /* Native Ctrl+L and Return enter through the edit's real input path. */
    check(explorer_key(window, 'L', VK_CONTROL), "Ctrl+L input queued");
    GUITHREADINFO state = {sizeof(state)};
    for (unsigned i = 0; i < 100; ++i)
    {
        if (GetGUIThreadInfo(GetWindowThreadProcessId(window, NULL), &state) && state.hwndFocus == edit) break;
        pump(10);
    }
    BOOL focused = edit && IsWindowVisible(edit) && GetGUIThreadInfo(GetWindowThreadProcessId(window, NULL), &state) && state.hwndFocus == edit;
    if (!focused)
    {
        char note[160]; wsprintfA(note, "shell-focus owner=%lx foreground=%lx focus=%lx edit=%lx\n",
            (unsigned long)(ULONG_PTR)window, (unsigned long)(ULONG_PTR)GetForegroundWindow(),
            (unsigned long)(ULONG_PTR)state.hwndFocus, (unsigned long)(ULONG_PTR)edit); line(note);
    }
    check(focused, "Ctrl+L shows and focuses the address box's edit");
    SendMessageW(edit, WM_SETTEXT, 0, (LPARAM)path);
    WCHAR actual[MAX_PATH]; SendMessageW(edit, WM_GETTEXT, MAX_PATH, (LPARAM)actual);
    check(!lstrcmpW(actual, path), "address edit contains requested path");
    check(key(VK_RETURN, 0), "address Return input queued");
    titled(window, PathFindFileNameW(path));
}
/* A command bar button's middle, where the prototype places it (src/pw_explorer_layout.h), clicked as a user does. */
static BOOL command_click(HWND window, int command)
{
    PwExplorerPlace place; RECT client; GetClientRect(window, &client);
    pw_explorer_place(&place, client.right, client.bottom, 1, 0);
    BOOL queued = click_point(window, (int)(place.command[command].x + place.command[command].w / 2),
                              (int)(place.command[command].y + place.command[command].h / 2));
    pump(300); return queued;
}
static BOOL visible_child(HWND window, const WCHAR *cls)
{
    HWND child = FindWindowExW(window, NULL, cls, NULL); return child && IsWindowVisible(child);
}
/* The marked snapshot rendezvous (PROTON_WINDOW_SNAPSHOT_PROBE): server state. */
static BOOL rendezvous_write(const char *path, const void *data, DWORD bytes)
{
    char tmp[MAX_PATH]; DWORD written; HANDLE f; BOOL okay;
    wsprintfA(tmp, "%s.tmp", path);
    if ((f = CreateFileA(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL)) == INVALID_HANDLE_VALUE) return FALSE;
    okay = WriteFile(f, data, bytes, &written, NULL) && written == bytes; okay = CloseHandle(f) && okay;
    return okay && MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING);
}
static BOOL rendezvous_snapshot(PwWindowSnapshot *out)
{
    static PwWindowProbeReply reply; static DWORD nonce; DWORD start = GetTickCount(), read; HANDLE f;
    ++nonce; pump(30);
    if (!rendezvous_write("C:\\.pw-window-request", &nonce, sizeof(nonce))) return FALSE;
    do
    {
        if ((f = CreateFileA("C:\\.pw-window-reply", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL)) != INVALID_HANDLE_VALUE)
        {
            BOOL okay = ReadFile(f, &reply, sizeof(reply), &read, NULL) && read == sizeof(reply); CloseHandle(f);
            if (okay && reply.nonce == nonce)
            {
                if (reply.status || reply.snapshot.version != PW_WINDOW_SNAPSHOT_VERSION || reply.snapshot.size != sizeof(*out)) return FALSE;
                *out = reply.snapshot; return TRUE;
            }
        }
        pump(10);
    } while (GetTickCount() - start < 5000);
    return FALSE;
}
static const PwWindowRow *snapshot_row(const PwWindowSnapshot *s, HWND window)
{
    for (unsigned i = 0; i < s->count; ++i) if (s->windows[i].hwnd == (uint32_t)(ULONG_PTR)window) return &s->windows[i];
    return NULL;
}
static LRESULT CALLBACK preview_target_proc(HWND hwnd, UINT msg, WPARAM w, LPARAM l)
{
    if (msg == WM_PAINT)
    {
        PAINTSTRUCT paint; HDC dc = BeginPaint(hwnd, &paint); HBRUSH brush = CreateSolidBrush(RGB(200, 60, 140));
        FillRect(dc, &paint.rcPaint, brush); DeleteObject(brush); EndPaint(hwnd, &paint); return 0;
    }
    return DefWindowProcW(hwnd, msg, w, l);
}
static HWND preview_button_for;
static BOOL CALLBACK find_preview_button(HWND child, LPARAM target)
{
    if (GetWindowLongPtrW(child, GWLP_ID) == target && IsWindowVisible(child)) preview_button_for = child;
    return TRUE;
}
static BOOL pointer_to(int x, int y)
{
    INPUT move = {0}; move.type = INPUT_MOUSE;
    move.mi.dx = x * 65535 / 1919; move.mi.dy = y * 65535 / 1079; move.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
    return SendInput(1, &move, sizeof(move)) == 1;
}
/* Hovering an application button shows a rounded acrylic preview with the
 * window's live DWM thumbnail; clicking it activates the window. */
static void preview_checks(HWND tray)
{
    WNDCLASSW type = {0}; HWND target, preview = NULL; RECT button_rect, preview_rect; char note[256]; static PwWindowSnapshot snap;
    const PwWindowRow *row; const PwWindowThumbnail *thumb = NULL; DWORD start;
    type.lpfnWndProc = preview_target_proc; type.hInstance = GetModuleHandleW(NULL); type.lpszClassName = L"MihawkPreviewTarget";
    type.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    RegisterClassW(&type);
    target = CreateWindowExW(WS_EX_APPWINDOW, type.lpszClassName, L"Mihawk preview target", WS_POPUP | WS_VISIBLE,
                             100, 120, 400, 300, NULL, NULL, type.hInstance, NULL);
    check(target != NULL, "preview target window"); if (!target) return;
    UpdateWindow(target);
    preview_button_for = NULL; start = GetTickCount();
    while (!preview_button_for && GetTickCount() - start < 4000) { pump(50); EnumChildWindows(tray, find_preview_button, (LPARAM)target); }
    check(preview_button_for != NULL, "taskbar button for the preview target");
    if (!preview_button_for) { DestroyWindow(target); return; }
    GetWindowRect(preview_button_for, &button_rect);
    pointer_to((button_rect.left + button_rect.right) / 2, (button_rect.top + button_rect.bottom) / 2);
    start = GetTickCount();
    while (GetTickCount() - start < 4000)
    {
        pump(50); preview = FindWindowW(L"MihawkTaskbarPreview", NULL);
        if (preview && IsWindowVisible(preview)) break;
    }
    if (!preview || !IsWindowVisible(preview))
    {
        /* Say why (gate-ntsync-2026-10-17-03 shell64 had no line): where the button is now against where the pointer was
         * sent, what is under the pointer, and whether the taskbar marked the button hot (patch 0994: a first move marks
         * it and starts the preview's 400 ms timer; a lost mark, or a button moved by the taskbar's layout, shows none). */
        RECT now = {0}; POINT cursor = {0, 0}; HWND under; WCHAR cls[64] = L""; char note[320];
        GetWindowRect(preview_button_for, &now); GetCursorPos(&cursor); under = WindowFromPoint(cursor);
        if (under) GetClassNameW(under, cls, ARRAY_SIZE(cls));
        wsprintfA(note, "shell-probe preview missing: button was=%ld,%ld,%ld,%ld now=%ld,%ld,%ld,%ld cursor=%ld,%ld "
                  "under=%s class=%ls id=%ld hot=%d preview=%s\n", button_rect.left, button_rect.top, button_rect.right, button_rect.bottom,
                  now.left, now.top, now.right, now.bottom, cursor.x, cursor.y, under == preview_button_for ? "button" : "other", cls,
                  under ? (long)GetWindowLongPtrW(under, GWLP_ID) : 0L, GetPropW(preview_button_for, L"MihawkTaskbarHot") != NULL,
                  preview ? "hidden" : "absent");
        line(note);
    }
    check(preview && IsWindowVisible(preview), "hover shows the live window preview");
    if (preview && IsWindowVisible(preview))
    {
        WCHAR title[64]; GetWindowTextW(preview, title, ARRAY_SIZE(title));
        check(!lstrcmpW(title, L"Mihawk preview target"), "preview names its window");
        GetWindowRect(preview, &preview_rect);
        check(preview_rect.bottom <= button_rect.top && preview_rect.left < button_rect.right && preview_rect.right > button_rect.left,
              "preview sits above its button");
        if (rendezvous_snapshot(&snap))
        {
            row = snapshot_row(&snap, preview);
            check(row && row->radius == 8 && (row->flags & PW_WINDOW_GLASS) && row->backdrop == PW_BACKDROP_ACRYLIC &&
                  (row->flags & PW_WINDOW_BORDER), "preview is a rounded bordered acrylic surface");
            for (unsigned i = 0; i < snap.thumbnail_count; ++i)
                if (snap.thumbnails[i].destination == (uint32_t)(ULONG_PTR)preview && snap.thumbnails[i].source == (uint32_t)(ULONG_PTR)target)
                    thumb = &snap.thumbnails[i];
            check(thumb && thumb->opacity == 255 && thumb->right - thumb->left > 100 && thumb->bottom - thumb->top > 60,
                  "live thumbnail of the hovered window");
            const PwWindowRow *bar = snapshot_row(&snap, tray);
            check(bar && (bar->flags & PW_WINDOW_GLASS) && bar->backdrop == PW_BACKDROP_TASKBAR && !bar->radius && !bar->shadow,
                  "taskbar is a square surface of the prototype's taskbar acrylic");
        }
        else check(FALSE, "snapshot rendezvous for previews");
        if (thumb)
        {
            wsprintfA(note, "shell-preview number=%u thumb=%ld,%ld,%ld,%ld expected=200,60,140\n", captures + 1,
                      (long)thumb->left, (long)thumb->top, (long)thumb->right, (long)thumb->bottom); line(note);
        }
        check(capture_shell_ex(preview, "preview", RGB(200, 60, 140), TRUE, TRUE), "actual preview capture");
        /* A taskbar click on the foreground window minimizes it; make the target
         * a background window first, with a tool window that has no button. */
        HWND decoy = CreateWindowExW(WS_EX_TOOLWINDOW, type.lpszClassName, L"", WS_POPUP | WS_VISIBLE, 1500, 700, 60, 40,
                                     NULL, NULL, type.hInstance, NULL);
        if (decoy) SetForegroundWindow(decoy);
        for (unsigned i = 0; i < 100 && GetForegroundWindow() == target; ++i) pump(10);
        check(GetForegroundWindow() != target, "preview target is a background window");
        click_point(preview, (preview_rect.right - preview_rect.left) / 2, (preview_rect.bottom - preview_rect.top) / 2);
        start = GetTickCount();
        while (GetTickCount() - start < 3000 && (GetForegroundWindow() != target || IsWindowVisible(preview))) pump(20);
        check(GetForegroundWindow() == target && !IsWindowVisible(preview), "clicking the preview activates its window");
        if (decoy) DestroyWindow(decoy);
    }
    pointer_to(1800, 100); pump(500);
    check(!preview || !IsWindowVisible(preview), "preview hides when the pointer leaves");
    DestroyWindow(target); pump(200);
    if (rendezvous_snapshot(&snap))
    {
        BOOL leftover = FALSE;
        for (unsigned i = 0; i < snap.thumbnail_count; ++i) leftover |= snap.thumbnails[i].source == (uint32_t)(ULONG_PTR)target;
        check(!leftover, "no thumbnail outlives its preview");
    }
}

void mainCRTStartup(void)
{
    static WCHAR temp[MAX_PATH], marker[MAX_PATH], executable[MAX_PATH], log_path[MAX_PATH];
    GetTempPathW(MAX_PATH, temp); PathCombineW(marker, temp, L"mihawk-shell-launch.marker");
    GetModuleFileNameW(NULL, executable, MAX_PATH);
    if (!lstrcmpiW(PathFindFileNameW(executable), L"Mihawk Shell Launch Probe.exe")) { ExitProcess(write_file(marker) ? 0 : 1); }
    if (GetEnvironmentVariableW(L"MIHAWK_SHELL_PROBE_LOG", log_path, MAX_PATH)) log_file = CreateFileW(log_path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, 0, NULL);
    static WCHAR prior_theme[MAX_PATH], prior_colour[64], prior_size[64], managed_theme[MAX_PATH];
    HRESULT prior_result = GetCurrentThemeName(prior_theme, MAX_PATH, prior_colour, 64, prior_size, 64);
    GetWindowsDirectoryW(managed_theme, MAX_PATH); PathAppendW(managed_theme, L"resources\\themes\\light\\light.msstyles");
    check(SUCCEEDED(apply_theme(managed_theme, L"Blue", L"NormalSize")), "apply managed light theme");
    static WCHAR programs[MAX_PATH], fixture[MAX_PATH], app[MAX_PATH]; BYTE *saved = NULL; DWORD saved_size = 0, saved_type = 0; HKEY registry;
    LONG saved_result = RegGetValueW(HKEY_CURRENT_USER, pin_key, L"StartPins", RRF_RT_ANY, &saved_type, NULL, &saved_size);
    if (saved_result == ERROR_SUCCESS)
    {
        saved = malloc(saved_size); check(saved != NULL, "save original pin buffer");
        if (!saved || RegGetValueW(HKEY_CURRENT_USER, pin_key, L"StartPins", RRF_RT_ANY, &saved_type, saved, &saved_size)) ExitProcess(2);
    }
    else if (saved_result != ERROR_FILE_NOT_FOUND && saved_result != ERROR_PATH_NOT_FOUND) { check(FALSE, "read original pin preferences"); ExitProcess(2); }
    check(SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_PROGRAMS | CSIDL_FLAG_CREATE, NULL, 0, programs)), "real application directory");
    PathCombineW(fixture, programs, L"Mihawk shell qualification");
    remove_leftover(fixture, "disposable application folder");
    remove_leftover(marker, "launch marker");
    check(CreateDirectoryW(fixture, NULL), "exclusive disposable application folder");
    if (failures) goto restore;
    PathCombineW(app, fixture, L"Mihawk Shell Launch Probe.exe");
    /* An earlier run's pin of this fixture is a leftover too: the pin button toggles, so a kept one would be unpinned
     * by this run's click, and the restore would keep it for the next (shell64 failed so in gate-b3-2026-10-25-01,
     * after Explorer saved its pins over shell32's restore). Start reads the pins when it opens, after this. */
    if (saved && saved_type == REG_MULTI_SZ)
    {
        WCHAR *from = (WCHAR *)saved, *to = (WCHAR *)saved, *end = (WCHAR *)(saved + saved_size);
        BOOL dropped = FALSE;
        while (from < end && *from)
        {
            size_t length = lstrlenW(from) + 1;
            if (!lstrcmpiW(from, app)) dropped = TRUE;
            else { memmove(to, from, length * sizeof(WCHAR)); to += length; }
            from += length;
        }
        *to++ = 0;
        if (to == (WCHAR *)saved + 1) *to++ = 0;
        saved_size = (DWORD)((BYTE *)to - saved);
        if (dropped) check(!RegSetKeyValueW(HKEY_CURRENT_USER, pin_key, L"StartPins", REG_MULTI_SZ, saved, saved_size),
                           "an earlier run's leftover pin removed");
    }
    check(CopyFileW(executable, app, TRUE), "install disposable executable entry");
    check(!file_exists(marker), "launch marker initially absent");
    HWND tray = wait_window(L"Shell_TrayWnd", 5000), button = NULL, panel;
    check(tray != NULL, "real shell taskbar"); if (!tray) goto cleanup;
    EnumChildWindows(tray, find_start, (LPARAM)&button); check(button != NULL, "accessible Start button"); if (!button) goto cleanup;
    /* Start is acrylic: the host judges its glass from the desktop beneath it. */
    check(capture_shell_ex(tray, "desktop-light", 0, TRUE, FALSE), "desktop beneath Start capture");
    panel = open_start(button); check(panel != NULL, "native Start panel opens"); if (!panel) goto cleanup;
    check(capture_shell_ex(panel, "start-light", 0, TRUE, FALSE), "actual light acrylic Start capture");
    check(SUCCEEDED(apply_theme(managed_theme, L"Dark", L"NormalSize")), "live dark theme change");
    check(capture_shell_ex(panel, "start-dark", 0, TRUE, FALSE), "actual dark acrylic Start capture");
    check(SUCCEEDED(apply_theme(managed_theme, L"Blue", L"NormalSize")), "restore light before interactions");
    RECT bounds; GetWindowRect(panel, &bounds); check(bounds.right - bounds.left == 600 && bounds.bottom - bounds.top == 566, "approved Start geometry at 96 DPI");
    SendMessageW(GetDlgItem(panel, 100), WM_SETTEXT, 0, (LPARAM)L"Mihawk Shell Launch Probe"); pump(50);
    HWND list = GetDlgItem(panel, 101);
    check(IsWindowVisible(list) && SendMessageW(list, LB_GETCOUNT, 0, 0) == 1, "local search finds actual installed entry");
    PostMessageW(GetDlgItem(panel, 103), BM_CLICK, 0, 0); pump(150);
    check(pins_contain(app), "pin persisted in user registry");
    close_start(panel); panel = open_start(button); check(panel != NULL, "Start reopens with fresh snapshot");
    BOOL visible_pin = FALSE;
    if (panel)
    {
        for (unsigned i = 0; i < 18; ++i)
        {
            WCHAR name[MAX_PATH]; HWND pin = GetDlgItem(panel, 200 + i); GetWindowTextW(pin, name, MAX_PATH);
            if (!lstrcmpW(name, L"Mihawk Shell Launch Probe") && IsWindowVisible(pin)) visible_pin = TRUE;
        }
        check(visible_pin, "saved pin restored on reopening");
        SendMessageW(GetDlgItem(panel, 100), WM_SETTEXT, 0, (LPARAM)L"Mihawk Shell Launch Probe"); pump(50);
        PostMessageW(panel, WM_COMMAND, MAKEWPARAM(101, LBN_DBLCLK), (LPARAM)GetDlgItem(panel, 101));
        for (unsigned i = 0; i < 300 && !file_exists(marker); ++i) pump(10);
        check(file_exists(marker), "Start launches real child executable");
        check(!IsWindowVisible(panel), "successful launch dismisses Start"); close_start(panel);
    }
    panel = open_start(button);
    if (panel)
    {
        SendMessageW(GetDlgItem(panel, 100), WM_SETTEXT, 0, (LPARAM)L"NoSuchMihawkApplication-987"); pump(30);
        check(SendMessageW(GetDlgItem(panel, 101), LB_GETCOUNT, 0, 0) == 0 && !IsWindowEnabled(GetDlgItem(panel, 103)), "empty search disables pin action");
        SetForegroundWindow(panel); check(key(VK_ESCAPE, 0), "Escape enters hardware input queue"); pump(100);
        check(!IsWindowVisible(panel), "Escape dismisses Start"); close_start(panel);
    }
    preview_checks(tray);
    static WCHAR browser[MAX_PATH], command[3 * MAX_PATH], files[MAX_PATH], original[MAX_PATH];
    WCHAR unique[MAX_PATH]; wsprintfW(unique, L"Mihawk shell browser qualification %lu-%lu", GetCurrentProcessId(), GetTickCount());
    PathCombineW(files, temp, unique); BOOL own_files = CreateDirectoryW(files, NULL); check(own_files, "exclusive browser fixture");
    if (!own_files) goto cleanup;
    static WCHAR destination[MAX_PATH], copied[MAX_PATH];
    wsprintfW(unique, L"Mihawk shell paste qualification %lu-%lu", GetCurrentProcessId(), GetTickCount());
    PathCombineW(destination, temp, unique);
    BOOL own_destination = CreateDirectoryW(destination, NULL);
    check(own_destination, "exclusive paste fixture");
    PathCombineW(copied, destination, L"sample.txt");
    PathCombineW(original, files, L"sample.txt"); check(write_file(original), "real browser file fixture");
    if (!GetEnvironmentVariableW(L"MIHAWK_SHELL_EXPLORER", browser, MAX_PATH)) { GetWindowsDirectoryW(browser, MAX_PATH); PathAppendW(browser, L"explorer.exe"); }
    wsprintfW(command, L"\"%s\"", browser); /* no folder: File Explorer opens on Home */
    STARTUPINFOW startup = {sizeof(startup)}; PROCESS_INFORMATION process = {0};
    BOOL launched = CreateProcessW(NULL, command, NULL, NULL, FALSE, 0, NULL, NULL, &startup, &process);
    check(launched, "real Explorer child starts");
    BOOL clipboard_empty = CountClipboardFormats() == 0;
    HRESULT ole = OleInitialize(NULL);
    /* Qualification owns a fresh desktop with an empty clipboard. OleGetClipboard
     * is a lazy object, not a saved snapshot: never overwrite existing content. */
    check(clipboard_empty && SUCCEEDED(ole), "empty qualification clipboard precondition");
    if (launched)
    {
        HWND window = wait_window(L"ExplorerWClass", 6000); check(window != NULL, "real Explorer window renders");
        if (window)
        {
            /* Where the prototype's File Explorer is (1100 x 700 at 410,166; the window rect adds the invisible frame), so
             * the captures compare with its pixels, as desktop-session places it. */
            RECT outer, visible; GetWindowRect(window, &outer);
            if (FAILED(DwmGetWindowAttribute(window, DWMWA_EXTENDED_FRAME_BOUNDS, &visible, sizeof(visible)))) visible = outer;
            SetWindowPos(window, HWND_TOP, 410 - (visible.left - outer.left), 166 - (visible.top - outer.top),
                         1100 + (outer.right - outer.left) - (visible.right - visible.left),
                         700 + (outer.bottom - outer.top) - (visible.bottom - visible.top), SWP_SHOWWINDOW);
            SetForegroundWindow(window); pump(300);
            check(titled(window, L"Home"), "File Explorer opens on Home");
            HICON large_icon = NULL, small_icon = NULL;
            UINT extracted = ExtractIconExW(browser, 0, &large_icon, &small_icon, 1);
            check(extracted > 0 && extracted != ~0u && large_icon && small_icon, "Explorer exports caption and taskbar application identity");
            if (extracted > 0 && extracted != ~0u)
            { if (large_icon) DestroyIcon(large_icon); if (small_icon) DestroyIcon(small_icon); }
            INITCOMMONCONTROLSEX controls = {sizeof(controls), ICC_LISTVIEW_CLASSES};
            InitCommonControlsEx(&controls);
            HWND custom_list = CreateWindowW(WC_LISTVIEWW, L"", WS_POPUP, 0, 0, 10, 10, NULL, NULL, GetModuleHandleW(NULL), NULL);
            const COLORREF custom_colour = RGB(13,37,71);
            if (custom_list) SendMessageW(custom_list, LVM_SETBKCOLOR, 0, custom_colour);
            check(custom_list != NULL, "explicit-colour common control created");
            check(capture_shell_ex(window, "explorer-light", 0, TRUE, FALSE), "actual light Explorer capture");
            check(SUCCEEDED(apply_theme(managed_theme, L"Dark", L"NormalSize")), "Explorer live dark change");
            check(custom_list && SendMessageW(custom_list, LVM_GETBKCOLOR, 0, 0) == custom_colour, "theme change preserves explicit application background");
            if (custom_list) DestroyWindow(custom_list);
            pump(300);
            check(capture_shell_ex(window, "explorer-dark", 0, TRUE, FALSE), "actual dark Explorer capture");
            check(SUCCEEDED(apply_theme(managed_theme, L"Blue", L"NormalSize")), "restore light Explorer interactions");
            check(GetDlgItem(window, 4300) && visible_child(window, L"PwExplorerNav") && visible_child(window, L"PwExplorerHome"),
                  "the prototype's address box, navigation pane and Home page");
            /* The history as the prototype keeps it: nothing behind the first Home, Back from the first folder is Home. */
            explorer_key(window, VK_LEFT, VK_MENU);
            check(titled(window, L"Home"), "initial history has no Back");
            address(window, files);
            find_listview = NULL; EnumChildWindows(window, find_list, 0);
            BOOL at_fixture = titled(window, PathFindFileNameW(files)) && find_listview && SendMessageW(find_listview, LVM_GETITEMCOUNT, 0, 0) == 1;
            check(at_fixture, "address navigation shows the fixture's actual file");
            check(!visible_child(window, L"PwExplorerHome"), "a folder replaces the Home page");
            explorer_key(window, VK_LEFT, VK_MENU);
            check(titled(window, L"Home"), "Back from the first folder returns to Home");
            explorer_key(window, VK_RIGHT, VK_MENU);
            check(titled(window, PathFindFileNameW(files)), "Forward from Home returns to the folder");
            /* The navigation pane's Documents row (its fifth: Home, Gallery, Desktop, Downloads, Documents). */
            PwExplorerPlace place; RECT client; GetClientRect(window, &client);
            pw_explorer_place(&place, client.right, client.bottom, 1, 0);
            click_point(window, (int)(place.nav_row[4].x + place.nav_row[4].w / 2), (int)(place.nav_row[4].y + place.nav_row[4].h / 2));
            check(titled(window, L"Documents"), "navigation pane opens Documents");
            explorer_key(window, VK_RIGHT, VK_MENU);
            check(titled(window, L"Documents"), "a navigation truncates Forward");
            explorer_key(window, VK_LEFT, VK_MENU);
            check(titled(window, PathFindFileNameW(files)), "Back restores the fixture");
            explorer_key(window, VK_RIGHT, VK_MENU);
            check(titled(window, L"Documents"), "Forward restores Documents");
            explorer_key(window, VK_RIGHT, VK_MENU);
            check(titled(window, L"Documents"), "Forward exhausts history");
            address(window, files); find_listview = NULL; EnumChildWindows(window, find_list, 0);
            at_fixture = titled(window, PathFindFileNameW(files)) && find_listview && SendMessageW(find_listview, LVM_GETITEMCOUNT, 0, 0) == 1;
            check(at_fixture, "address navigation returns to fixture");
            if (at_fixture && own_destination && (clipboard_empty && SUCCEEDED(ole)))
            {
                check(click_point(find_listview, 40, 30), "file click enters hardware queue"); pump(100);
                check(key('A', VK_CONTROL), "select-all keyboard input"); pump(300);
                check(SendMessageW(find_listview, LVM_GETSELECTEDCOUNT, 0, 0) == 1, "real file selected");
                check(command_click(window, PW_EXPLORER_CMD_COPY), "command bar Copy clicked");
                address(window, destination);
                BOOL at_destination = titled(window, PathFindFileNameW(destination));
                check(at_destination, "paste stays inside owned destination");
                if (at_destination) check(command_click(window, PW_EXPLORER_CMD_PASTE), "command bar Paste clicked");
                for (unsigned i = 0; i < 200 && !file_exists(copied); ++i) pump(10);
                check(file_exists(copied), "Copy and Paste commands create actual destination file");
                if (file_exists(copied))
                {
                    char data[20] = {0}; DWORD n; HANDLE file = CreateFileW(copied, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
                    check(file != INVALID_HANDLE_VALUE && ReadFile(file, data, 19, &n, NULL) && n == 19 && !memcmp(data, "Mihawk shell probe\n", 19), "copied file bytes preserved");
                    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
                }
            }
            else check(FALSE, "clipboard and selection ready for file operations");
            if (own_destination)
            {
                address(window, destination);
                /* New opens the prototype's menu, whose one item is Folder. */
                check(command_click(window, PW_EXPLORER_CMD_NEW), "command bar New clicked");
                key(VK_DOWN, 0); pump(50); key(VK_RETURN, 0); pump(250);
                WCHAR mask[MAX_PATH], folder[MAX_PATH]; WIN32_FIND_DATAW found; BOOL created = FALSE;
                PathCombineW(mask, destination, L"*");
                /* The command runs in Explorer's own thread: the folder showed up after the fixed 250 ms in 5 gate runs of 6 and
                 * not in the sixth (gate-own-2026-10-11-02, "New folder ... error=18"), so wait for it, up to 4 s. */
                for (DWORD waited = 0; !created && waited <= 4000; waited += 100)
                {
                    HANDLE search = FindFirstFileW(mask, &found);
                    if (search != INVALID_HANDLE_VALUE)
                    {
                        do { if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && lstrcmpW(found.cFileName, L".") && lstrcmpW(found.cFileName, L"..")) { PathCombineW(folder, destination, found.cFileName); created = TRUE; break; } } while (FindNextFileW(search, &found));
                        FindClose(search);
                    }
                    if (!created) pump(100);
                }
                check(created, "New Folder command creates real directory");
                key(VK_ESCAPE, 0); pump(30);
                if (created)
                {
                    /* Explorer still holds the new folder for a moment after the rename is cancelled: error 32 on the first try in
                     * gate-own-2026-10-11-03 (shell32), so retry for up to 4 s. */
                    BOOL removed = RemoveDirectoryW(folder);
                    for (DWORD waited = 0; !removed && GetLastError() == ERROR_SHARING_VIOLATION && waited < 4000; waited += 100)
                    { pump(100); removed = RemoveDirectoryW(folder); }
                    check(removed, "disposable new folder removed");
                }
            }
            explorer_key(window, VK_F5, 0);
            check(IsWindow(window) && titled(window, PathFindFileNameW(own_destination ? destination : files)), "F5 refresh keeps the folder");
            PostMessageW(window, WM_CLOSE, 0, 0);
        }
        DWORD wait = WaitForSingleObject(process.hProcess, 3000); check(wait == WAIT_OBJECT_0, "Explorer clean exit");
        if (wait != WAIT_OBJECT_0) { TerminateProcess(process.hProcess, 1); WaitForSingleObject(process.hProcess, 1000); }
        CloseHandle(process.hThread); CloseHandle(process.hProcess);
    }
    if ((clipboard_empty && SUCCEEDED(ole)))
    {
        HRESULT restored; DWORD started = GetTickCount();
        do { restored = OleSetClipboard(NULL); if (restored == CLIPBRD_E_CANT_OPEN) pump(10); }
        while (restored == CLIPBRD_E_CANT_OPEN && GetTickCount() - started < 2000);
        char note[128]; wsprintfA(note, "shell-probe clipboard empty=%u restore=%08lx\n", clipboard_empty, (unsigned long)restored); line(note);
        check(SUCCEEDED(restored), "original clipboard restored");
    }
    if (SUCCEEDED(ole)) OleUninitialize();
    if (own_destination)
    {
        BOOL cleaned; DWORD waited = 0;
        DeleteFileW(copied);
        while (!(cleaned = RemoveDirectoryW(destination)) && waited < 4000 &&
               (GetLastError() == ERROR_SHARING_VIOLATION || GetLastError() == ERROR_DIR_NOT_EMPTY)) { pump(100); waited += 100; }
        check(cleaned, "owned paste directory cleaned");
    }
    DeleteFileW(original); RemoveDirectoryW(files);
cleanup:
    DeleteFileW(app); DeleteFileW(marker); RemoveDirectoryW(fixture);
restore:
    if (RegCreateKeyExW(HKEY_CURRENT_USER, pin_key, 0, NULL, 0, KEY_SET_VALUE, NULL, &registry, NULL) == ERROR_SUCCESS)
    {
        LONG result = saved ? RegSetValueExW(registry, L"StartPins", 0, saved_type, saved, saved_size) : RegDeleteValueW(registry, L"StartPins");
        check(result == ERROR_SUCCESS || (!saved && result == ERROR_FILE_NOT_FOUND), "original pins restored"); RegCloseKey(registry);
    }
    else check(FALSE, "original pins restored");
    free(saved);
    check(SUCCEEDED(SUCCEEDED(prior_result) ? apply_theme(prior_theme, prior_colour, prior_size) : apply_theme(NULL, NULL, NULL)), "original theme restored");
    char verdict[160]; wsprintfA(verdict, "PS5_PROTON_SHELL %s bits=%u checks=%u failures=%u\n", failures ? "FAIL" : "PASS", (unsigned)sizeof(void *) * 8, checks, failures); line(verdict);
    if (log_file != INVALID_HANDLE_VALUE) CloseHandle(log_file);
    PostMessageW(GetShellWindow() ? GetShellWindow() : GetDesktopWindow(), WM_CLOSE, 0, 0);
    ExitProcess(failures ? 1 : 0);
}
