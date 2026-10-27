/* SPDX-License-Identifier: GPL-3.0-or-later
 * MihawkOS - the home screen tiles' worker: a payload the WebUI sends to the console's ELF loader when a tile is made
 * or removed (src/pw_webui.cpp). Nothing is installed or uninstalled while a game or app runs, and MihawkOS is one, so
 * the WebUI only marks the tile (install.pending, remove.pending) and this finishes it once MihawkOS has closed, as
 * PS5_RetroArch's tiles appear "once RetroArch closes" (its forwarders, 2026-10-18). A tile installed while MihawkOS ran
 * reached the home screen without its picture, though the system held it (the base PS5, 2026-10-26).
 *
 * Every 3 s: while a big app runs, nothing; then each tile marked install.pending is registered again with
 * ShadowMountPlus (127.0.0.1:10101; it can drop a source as it installs) and a scan installs it (asked again every
 * 30 s), the mark going once the game list shows it installed; each tile marked remove.pending is uninstalled and its
 * folder deleted. It ends when no tile waits, or after a day (the launcher sends it again at its next start). One runs
 * at a time: the one that holds 127.0.0.1:FORWARDER_WORKER_PORT. What it does goes to logs/tile-worker.log, which
 * names the app by its port: a binary holding its name reads as a copy of it (tools/publication-check.py).
 */
#include <arpa/inet.h>
#include <dirent.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "forwarder.h"

#ifndef FORWARDER_HOMEBREW
#define FORWARDER_HOMEBREW "/data/homebrew"
#endif
#define FORWARDER_WORKER_PORT 6768
#define FORWARDER_SMP_PORT 10101
#define FORWARDER_FIRST_TILE 99300
#define FORWARDER_LAST_TILE 99399
#define FORWARDER_WORKER_LOG "/data/ps5proton/logs/tile-worker.log"
#define FORWARDER_ROUNDS (24 * 60 * 20)   /* a day of 3 s rounds */
#define FORWARDER_SCAN_EVERY 10           /* rounds between two scans for the same waiting install */
#define FORWARDER_REPLY (1 << 20)

uint32_t sceLncUtilGetAppIdOfRunningBigApp(void);

static FILE *say_to;
static void say(const char *what, const char *id)
{
    if (!say_to) return;
    time_t now = time(NULL);
    struct tm parts;
    char when[32] = "";
    if (localtime_r(&now, &parts)) strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", &parts);
    fprintf(say_to, "%s tile %s: %s\n", when, id, what);
    fflush(say_to);
}

/* POST body to ShadowMountPlus's route; its answer's body (NUL-ended) in reply, or 0 when it does not answer. */
static int smp(const char *route, const char *body, char *reply, size_t size)
{
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return 0;
    struct timeval limit = {20, 0};   /* a scan can take a while */
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &limit, sizeof limit);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &limit, sizeof limit);
    struct sockaddr_in address;
    memset(&address, 0, sizeof address);
    address.sin_family = AF_INET;
    address.sin_port = htons(FORWARDER_SMP_PORT);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    char request[768];
    const int length = snprintf(request, sizeof request,
                                "POST %s HTTP/1.0\r\nHost: 127.0.0.1:%d\r\nContent-Type: application/json\r\n"
                                "Content-Length: %zu\r\n\r\n%s", route, FORWARDER_SMP_PORT, strlen(body), body);
    size_t got = 0;
    int ok = connect(fd, (struct sockaddr *)&address, sizeof address) == 0 && length > 0 && length < (int)sizeof request &&
             send(fd, request, (size_t)length, 0) == length;
    for (ssize_t n; ok && got + 1 < size && (n = recv(fd, reply + got, size - got - 1, 0)) > 0;) got += (size_t)n;
    close(fd);
    reply[got] = '\0';
    char *head = strstr(reply, "\r\n\r\n");
    if (!ok || strncmp(reply, "HTTP/", 5) || !head) return 0;
    memmove(reply, head + 4, strlen(head + 4) + 1);
    return 1;
}

static void remove_tree(const char *path)
{
    DIR *dir = opendir(path);
    if (dir) {
        for (struct dirent *entry; (entry = readdir(dir));) {
            if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
            char child[512];
            snprintf(child, sizeof child, "%s/%s", path, entry->d_name);
            struct stat status;
            if (lstat(child, &status) == 0 && S_ISDIR(status.st_mode)) remove_tree(child);
            else unlink(child);
        }
        closedir(dir);
    }
    rmdir(path);
}

/* A big app (a game, or MihawkOS) runs: nothing is installed or uninstalled under it. */
static int app_runs(void)
{
    const uint32_t app = sceLncUtilGetAppIdOfRunningBigApp();
    return app != 0 && app != 0xffffffffu;
}

int main(void)
{
    const int lock = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in address;
    memset(&address, 0, sizeof address);
    address.sin_family = AF_INET;
    address.sin_port = htons(FORWARDER_WORKER_PORT);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (lock < 0 || bind(lock, (struct sockaddr *)&address, sizeof address) != 0) return 0;   /* another one runs */
    say_to = fopen(FORWARDER_WORKER_LOG, "a");
    char *reply = malloc(FORWARDER_REPLY);
    if (!reply) return 1;
    int scanned_at = -FORWARDER_SCAN_EVERY;
    for (int round = 0; round < FORWARDER_ROUNDS; ++round) {
        int waiting = 0;
        const int busy = app_runs();
        for (int number = FORWARDER_FIRST_TILE; number <= FORWARDER_LAST_TILE; ++number) {
            char id[FORWARDER_ID_SIZE], folder[256], removal[300], install[300];
            snprintf(id, sizeof id, "PPSA%05d", number);
            snprintf(folder, sizeof folder, FORWARDER_HOMEBREW "/%s", id);
            snprintf(removal, sizeof removal, "%s/remove.pending", folder);
            snprintf(install, sizeof install, "%s/install.pending", folder);
            const int removing = access(removal, F_OK) == 0, installing = !removing && access(install, F_OK) == 0;
            if (!removing && !installing) continue;
            ++waiting;
            if (busy) {
                if (round % 20 == 0) say("waits: a game or app runs", id);
                continue;
            }
            if (!smp("/api/v1/games", "{\"include_size\":false}", reply, FORWARDER_REPLY) ||
                forwarder_reply_status(reply) != 0) {
                if (round % 20 == 0) say("waits: nothing answers on port 10101", id);
                continue;
            }
            const int installed = forwarder_reply_installed(reply, id);
            char body[320];
            if (installing) {
                if (installed) {
                    unlink(install);
                    say("installed", id);
                    --waiting;
                } else if (round - scanned_at >= FORWARDER_SCAN_EVERY) {
                    snprintf(body, sizeof body, "{\"path\":\"%s\"}", folder);
                    smp("/api/v1/manual/add", body, reply, FORWARDER_REPLY);
                    const int took = smp("/api/v1/scan", "{\"reset_attempts\":true}", reply, FORWARDER_REPLY) &&
                                     forwarder_reply_status(reply) == 0;
                    say(took ? "scan asked" : "waits: port 10101 did not take the scan", id);
                    scanned_at = round;
                }
                continue;
            }
            if (installed) {
                snprintf(body, sizeof body, "{\"title_id\":\"%s\"}", id);
                if (!smp("/api/v1/games/uninstall", body, reply, FORWARDER_REPLY) || forwarder_reply_status(reply) != 0) {
                    if (round % 20 == 0) say("waits: port 10101 does not uninstall it yet", id);
                    continue;
                }
                say("uninstalled", id);
            }
            remove_tree(folder);
            say("removed", id);
            --waiting;
        }
        if (!waiting) break;
        sleep(3);
    }
    free(reply);
    if (say_to) fclose(say_to);
    close(lock);
    return 0;
}
