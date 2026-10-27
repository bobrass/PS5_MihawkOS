/* SPDX-License-Identifier: GPL-3.0-or-later
 * The persistent test session: one title launch and one Wine session that runs Wine's conformance test files from a
 * queue, so a file costs its own run and not a title start and a Wine session start and stop each (20-30 s for every
 * two files in the per-launch mode). tools/run-wine-session.py launches it and feeds it over FTP.
 *
 *   wt-session.exe JOBS IDLE_SECONDS DIR       DIR: the session's folder (a new one each session), C:\wt by default
 *
 * DIR\queue.txt is replaced whole by the host (written beside it, then renamed): one entry a line,
 *
 *   SEQ <tab> ID <tab> p|s|u <tab> TIMEOUT_S <tab> COMMAND      a test file; p: may run beside others, s: alone,
 *                                                             u: before everything queued (a probe for a finding)
 *   SEQ <tab> STOP                                             end once everything before it has ended
 *
 * A COMMAND may start with {NAME=VALUE;NAME=VALUE}: those variables are set for that file's process only (a traced
 * rerun: {WINEDEBUG=+seh,+bcrypt}C:\bcrypt_test.exe bcrypt,).
 *
 * Entries are taken in SEQ order, each SEQ once. A file runs as its own process (as the PC's oracle runs it), from C:\,
 * with standard output and error into DIR\out\ID.txt, and WINETEST_FILE_TIMEOUT set to its timeout so the test's
 * own watchdog ends it the normal way first; one still running TIMEOUT_S + 30 s after its start is ended. Up to JOBS
 * files run at once; an "s" entry waits until nothing else runs and runs alone; and an entry waits while one with the
 * same ID after its first '-' runs (the same test file built for the other architecture: both use the same pipe,
 * mutex and file names; ntdll's pipe test failed 146 times when they overlapped). Each ended file appends
 *
 *   SEQ <tab> ID <tab> EXIT_CODE <tab> MILLISECONDS <tab> exited|killed|unstarted
 *
 * to DIR\results.txt. A test that crashes is one result; the session goes on. It ends after STOP, or when nothing
 * has been queued or running for IDLE_SECONDS. */
#include <windows.h>

enum { MAX_ENTRIES = 20000, MAX_JOBS = 8, QUEUE_MAX = 4 << 20 };

struct entry { unsigned seq; char id[128]; char serial, urgent, started; unsigned timeout; char command[512]; };
struct slot { HANDLE process; int entry; ULONGLONG started; };

static struct entry entries[MAX_ENTRIES];
static int entry_count, next_entry;
static unsigned last_seq;
static BOOL stop_seen;
static struct slot slots[MAX_JOBS];
static char queue_text[QUEUE_MAX], base[260] = "C:\\wt";

/* base + name */
static const char *in_base(char *out, const char *name)
{
    lstrcpyA(out, base);
    lstrcatA(out, name);
    return out;
}

/* what follows the first '-' of an ID: the test file, whatever its architecture */
static const char *group_of(const char *id)
{
    const char *dash = id;
    while (*dash && *dash != '-') dash++;
    return *dash ? dash + 1 : id;
}

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

static unsigned parse_number(const char *at, const char **end)
{
    unsigned value = 0;
    while (*at >= '0' && *at <= '9') value = value * 10 + (unsigned)(*at++ - '0');
    *end = at;
    return value;
}

/* copy up to a tab or the line's end; returns the position after the tab, or NULL at the line's end */
static const char *field(const char *at, const char *line_end, char *out, int size)
{
    int n = 0;
    while (at < line_end && *at != '\t') { if (n < size - 1) out[n++] = *at; at++; }
    out[n] = 0;
    return at < line_end ? at + 1 : NULL;
}

/* Read the queue; add the complete lines whose SEQ is new. */
static void read_queue(void)
{
    char path[300];
    HANDLE file = CreateFileA(in_base(path, "\\queue.txt"), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                              OPEN_EXISTING, 0, NULL);
    DWORD done = 0;
    const char *at, *end;

    if (file == INVALID_HANDLE_VALUE) return;
    ReadFile(file, queue_text, QUEUE_MAX - 1, &done, NULL);
    CloseHandle(file);
    queue_text[done] = 0;
    at = queue_text;
    end = queue_text + done;
    while (at < end) {
        const char *line_end = at, *next;
        char word[16];
        unsigned seq;
        while (line_end < end && *line_end != '\n') line_end++;
        if (line_end >= end) break;   /* not ended yet: the next read has it */
        seq = parse_number(at, &next);
        if (next < line_end && *next == '\t' && seq > last_seq) {
            next++;
            if (line_end - next >= 4 && next[0] == 'S' && next[1] == 'T' && next[2] == 'O' && next[3] == 'P') stop_seen = TRUE;
            else if (entry_count < MAX_ENTRIES) {
                struct entry *e = &entries[entry_count];
                const char *p = field(next, line_end, e->id, sizeof(e->id));
                if (p) { p = field(p, line_end, word, sizeof(word)); e->serial = word[0] == 's'; e->urgent = word[0] == 'u'; e->started = 0; }
                if (p) { const char *q; e->timeout = parse_number(p, &q); p = q < line_end && *q == '\t' ? q + 1 : NULL; }
                if (p) {
                    field(p, line_end, e->command, sizeof(e->command));
                    e->seq = seq;
                    entry_count++;
                }
            }
            last_seq = seq;
        }
        at = line_end + 1;
    }
}

/* The child's process id at the end of its output, as Wine's debug records name it (%04x): a probe that writes its
 * verdict only through buffered stdio or OutputDebugString is found in the helpers' logs by it (run-battery --session). */
static void note_process(const struct entry *e, HANDLE process)
{
    static const char digits[] = "0123456789abcdef";
    char path[300], line[64] = "wt-session: process ";
    DWORD pid = GetProcessId(process), written, at = lstrlenA(line);
    HANDLE file;
    int shift;

    for (shift = 28; shift > 12 && !((pid >> shift) & 15); shift -= 4) {}
    for (; shift >= 0; shift -= 4) line[at++] = digits[(pid >> shift) & 15];
    line[at++] = '\n';
    line[at] = 0;
    in_base(path, "\\out\\");
    lstrcatA(path, e->id);
    lstrcatA(path, ".txt");
    file = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (file == INVALID_HANDLE_VALUE) return;
    WriteFile(file, line, at, &written, NULL);
    CloseHandle(file);
}

static void result(const struct entry *e, DWORD code, ULONGLONG ms, const char *how)
{
    char line[300], path[300];
    DWORD written;
    HANDLE file = CreateFileA(in_base(path, "\\results.txt"), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, 0, NULL);
    line[0] = 0;
    cat_number(line, e->seq);
    lstrcatA(line, "\t");
    lstrcatA(line, e->id);
    lstrcatA(line, "\t");
    cat_number(line, code);
    lstrcatA(line, "\t");
    cat_number(line, ms);
    lstrcatA(line, "\t");
    lstrcatA(line, how);
    lstrcatA(line, "\n");
    if (file != INVALID_HANDLE_VALUE) {
        WriteFile(file, line, lstrlenA(line), &written, NULL);
        FlushFileBuffers(file);
        CloseHandle(file);
    }
    say("wt-session: ");
    say(line);
}

/* whether the same test file of the other architecture runs now */
static BOOL twin_running(int index, unsigned jobs)
{
    unsigned i;
    for (i = 0; i < jobs; i++)
        if (slots[i].process && !lstrcmpA(group_of(entries[slots[i].entry].id), group_of(entries[index].id))) return TRUE;
    return FALSE;
}

static BOOL start(int index, struct slot *slot)
{
    const struct entry *e = &entries[index];
    SECURITY_ATTRIBUTES inherit = { sizeof(inherit), NULL, TRUE };
    STARTUPINFOA startup;
    PROCESS_INFORMATION process;
    char path[400], timeout[16] = "", command[512], names[256];
    HANDLE out;
    BOOL ok;

    in_base(path, "\\out\\");
    lstrcatA(path, e->id);
    lstrcatA(path, ".txt");
    out = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, CREATE_ALWAYS, 0, NULL);
    if (out == INVALID_HANDLE_VALUE) { result(e, GetLastError(), 0, "unstarted"); return FALSE; }
    memset(&startup, 0, sizeof(startup));
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = out;
    startup.hStdError = out;
    cat_number(timeout, e->timeout);
    SetEnvironmentVariableA("WINETEST_FILE_TIMEOUT", timeout);   /* inherited by this child only: set just before it */
    lstrcpyA(command, e->command);
    names[0] = 0;
    if (command[0] == '{') {   /* {NAME=VALUE;...}: set for this child, unset once it has started */
        char *at = command + 1, *end = command;
        while (*end && *end != '}') end++;
        if (*end) {
            *end = 0;
            while (*at) {
                char *semi = at, *eq = at;
                while (*semi && *semi != ';') semi++;
                if (*semi) *semi++ = 0;
                while (*eq && *eq != '=') eq++;
                if (*eq == '=' && lstrlenA(names) + (eq - at) + 2 < (int)sizeof(names)) {
                    *eq = 0;
                    SetEnvironmentVariableA(at, eq + 1);
                    /* a file's own temporary folder: files running together wrote one Temp\test.avi (quartz, error 32) */
                    if (!lstrcmpiA(at, "TEMP") || !lstrcmpiA(at, "TMP")) CreateDirectoryA(eq + 1, NULL);
                    lstrcatA(names, at);
                    lstrcatA(names, ";");
                }
                at = semi;
            }
            lstrcpyA(command, end + 1);
        }
    }
    ok = CreateProcessA(NULL, command, NULL, NULL, TRUE, 0, NULL, "C:\\", &startup, &process);
    {
        char *name = names, *semi;
        while (*name) {
            semi = name;
            while (*semi != ';') semi++;
            *semi = 0;
            SetEnvironmentVariableA(name, NULL);
            name = semi + 1;
        }
    }
    CloseHandle(out);
    if (!ok) { result(e, GetLastError(), 0, "unstarted"); return FALSE; }
    CloseHandle(process.hThread);
    slot->process = process.hProcess;
    slot->entry = index;
    slot->started = GetTickCount64();
    return TRUE;
}

void mainCRTStartup(void)
{
    const char *cmdline = GetCommandLineA(), *p;
    unsigned jobs = 4, idle = 600;
    ULONGLONG last_activity = GetTickCount64(), last_read = 0;
    int running = 0, i;
    BOOL serial_running = FALSE;

    /* the program's name, then JOBS and IDLE_SECONDS */
    if (*cmdline == '"') { cmdline++; while (*cmdline && *cmdline != '"') cmdline++; if (*cmdline) cmdline++; }
    else while (*cmdline && *cmdline != ' ') cmdline++;
    while (*cmdline == ' ') cmdline++;
    if (*cmdline >= '0' && *cmdline <= '9') { jobs = parse_number(cmdline, &p); cmdline = p; }
    while (*cmdline == ' ') cmdline++;
    if (*cmdline >= '0' && *cmdline <= '9') { idle = parse_number(cmdline, &p); cmdline = p; }
    while (*cmdline == ' ') cmdline++;
    if (*cmdline) { lstrcpynA(base, cmdline, sizeof(base)); for (i = lstrlenA(base); i > 0 && base[i - 1] == ' '; i--) base[i - 1] = 0; }
    if (jobs < 1) jobs = 1;
    if (jobs > MAX_JOBS) jobs = MAX_JOBS;
    {
        char path[300];
        CreateDirectoryA("C:\\wt", NULL);
        CreateDirectoryA(base, NULL);
        CreateDirectoryA(in_base(path, "\\out"), NULL);
    }
    say("PS5_PROTON_WT_SESSION started\n");

    for (;;) {
        ULONGLONG now = GetTickCount64();
        /* the ended and the overdue */
        for (i = 0; i < (int)jobs; i++) {
            struct slot *s = &slots[i];
            const struct entry *e;
            DWORD code = 0;
            if (!s->process) continue;
            e = &entries[s->entry];
            if (WaitForSingleObject(s->process, 0) == WAIT_OBJECT_0) {
                GetExitCodeProcess(s->process, &code);
                note_process(e, s->process);
                result(e, code, now - s->started, "exited");
            } else if (now - s->started > ((ULONGLONG)e->timeout + 30) * 1000) {
                TerminateProcess(s->process, 124);
                WaitForSingleObject(s->process, 10000);
                note_process(e, s->process);
                result(e, 124, now - s->started, "killed");
            } else continue;
            CloseHandle(s->process);
            s->process = NULL;
            if (e->serial) serial_running = FALSE;
            running--;
            last_activity = now;
        }
        if (now - last_read >= 500 || next_entry >= entry_count) { read_queue(); last_read = now; }
        while (next_entry < entry_count && entries[next_entry].started) next_entry++;
        /* start what may start: an urgent entry first, then the queue in order; an entry whose other architecture
         * still runs is passed over for the ones after it (ws2_32 sock's two builds would otherwise hold every file
         * behind them for up to two of its 900 s budgets), but nothing passes a serial entry, which waits to run alone */
        for (;;) {
            int pick = -1, j;
            const struct entry *e;
            while (next_entry < entry_count && entries[next_entry].started) next_entry++;
            if (serial_running || running >= (int)jobs || next_entry >= entry_count) break;
            for (i = 0; i < (int)jobs; i++) if (!slots[i].process) break;
            if (i >= (int)jobs) break;
            for (j = next_entry; j < entry_count && pick < 0; j++)
                if (entries[j].urgent && !entries[j].started && !twin_running(j, jobs)) pick = j;
            for (j = next_entry; j < entry_count && pick < 0; j++) {
                if (entries[j].started) continue;
                if (entries[j].serial) { if (!running) pick = j; break; }   /* waits for the others to end */
                if (!twin_running(j, jobs)) pick = j;
            }
            if (pick < 0) break;
            e = &entries[pick];
            entries[pick].started = 1;
            if (start(pick, &slots[i])) {
                running++;
                if (e->serial) serial_running = TRUE;
            }
            last_activity = now;
        }
        if (!running && next_entry >= entry_count) {
            if (stop_seen) break;
            if (now - last_activity > (ULONGLONG)idle * 1000) { say("PS5_PROTON_WT_SESSION idle: ending\n"); break; }
        }
        Sleep(running ? 100 : 400);
    }
    say("PS5_PROTON_WT_SESSION PASS: stopped\n");
    ExitProcess(0);
}
