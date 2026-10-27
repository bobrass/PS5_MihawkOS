// SPDX-License-Identifier: GPL-3.0-or-later
// The WebUI in the launcher: ps5/src/proton_webui.h.
#include "proton_webui.h"
#include "proton_launch.h"
#include "proton_fetch.h"
#include "../../src/pw_webui_http.hpp"
#include <arpa/inet.h>
#include <atomic>
#include <sys/time.h>
#include <thread>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {
constexpr unsigned short kPort = 6767;
std::atomic<unsigned> changes{0};
bool started;

// The address other machines reach the console at: the source address of a route to the internet. A UDP socket
// connected to a public address sends nothing; getsockname names the interface the system would use.
std::string console_address()
{
    const int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return {};
    sockaddr_in target{};
    target.sin_family = AF_INET;
    target.sin_port = htons(53);
    inet_pton(AF_INET, "1.1.1.1", &target.sin_addr);
    sockaddr_in local{};
    socklen_t size = sizeof local;
    char text[INET_ADDRSTRLEN] = "";
    const bool ok = connect(fd, reinterpret_cast<sockaddr *>(&target), sizeof target) == 0 &&
                    getsockname(fd, reinterpret_cast<sockaddr *>(&local), &size) == 0 && local.sin_addr.s_addr != 0 &&
                    inet_ntop(AF_INET, &local.sin_addr, text, sizeof text);
    close(fd);
    return ok ? std::string(text) : std::string();
}

// One POST to the user's ShadowMountPlus (its API on 127.0.0.1:10101, as PS5_RetroArch's tiles use it): HTTP/1.0, the
// answer read to the end, 5 s for each step.
bool shadowmount(const std::string &route, const std::string &body, std::string &reply, std::string &why)
{
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return why = "no socket", false;
    timeval limit{5, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &limit, sizeof limit);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &limit, sizeof limit);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(10101);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    std::string answer;
    bool ok = connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof address) == 0;
    if (ok) {
        const std::string request = "POST " + route + " HTTP/1.0\r\nHost: 127.0.0.1:10101\r\nContent-Type: application/json\r\n"
                                    "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
        ok = send(fd, request.data(), request.size(), 0) == ssize_t(request.size());
        char buffer[16384];
        for (ssize_t got; ok && answer.size() < (16u << 20) && (got = recv(fd, buffer, sizeof buffer, 0)) > 0;)
            answer.append(buffer, size_t(got));
    }
    close(fd);
    const size_t head = answer.find("\r\n\r\n");
    if (!ok || answer.compare(0, 5, "HTTP/") != 0 || head == std::string::npos) return why = "no answer", false;
    reply = answer.substr(head + 4);
    return true;
}
}  // namespace

std::string proton_webui_data_root()
{
#if defined(PS5_HOST_REFERENCE)
    return std::string(PS5_APP_ROOT) + "/data";   // the host reference keeps its files beside the app
#else
    return PROTON_DATA_ROOT;
#endif
}

bool proton_webui_start(std::string *why, std::vector<PwWebuiChoice> themes, std::vector<PwWebuiChoice> music)
{
    if (started) return true;
#if !defined(PS5_HOST_REFERENCE)
    if (!proton_data_grant()) {
        if (why) *why = "/data is not granted";
        return false;
    }
#endif
    const std::string data = proton_webui_data_root();
    PwWebuiContext context;
    context.data_root = data;
    context.title_root = proton_app_root();
    context.token = pw_webui_http_nonce();
    context.version = PS5_TITLE_NAME;
    context.themes = std::move(themes);
    context.music = std::move(music);
    context.games = []() {
        std::vector<PwWebuiGame> out;
        for (const ProtonGame &game : proton_read_games(nullptr)) {
            PwWebuiGame entry;
            entry.id = game.id;
            entry.name = game.name;
            entry.detail = game.detail;
            entry.executable = game.executable;
            entry.arguments = game.arguments;
            entry.directory = game.directory;
            entry.prefix = game.prefix;
            entry.env = game.env;
            entry.desktop = game.desktop;
            entry.launcher = game.launcher;
            entry.x86_32 = game.x86_32;
            entry.complaint = game.complaint;
            out.push_back(entry);
        }
        return out;
    };
    context.changed = [](const char *) { changes.fetch_add(1, std::memory_order_release); };
    context.shadowmount = shadowmount;
    // The tiles' worker to the console's ELF loader (127.0.0.1:9021), as a tile sends its launch payload
    context.tile_worker = [root = std::string(proton_app_root())](std::string &why) {
        FILE *file = fopen((root + "/forwarder/worker.elf").c_str(), "rb");
        if (!file) return why = "the tiles' worker is not in the title folder", false;
        std::string elf;
        char buffer[65536];
        for (size_t got; (got = fread(buffer, 1, sizeof buffer, file)) > 0;) elf.append(buffer, got);
        fclose(file);
        const int fd = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in loader{};
        loader.sin_family = AF_INET;
        loader.sin_port = htons(9021);
        loader.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        bool ok = fd >= 0 && !elf.empty() && connect(fd, reinterpret_cast<sockaddr *>(&loader), sizeof loader) == 0;
        for (size_t sent = 0; ok && sent < elf.size();) {
            const ssize_t n = send(fd, elf.data() + sent, elf.size() - sent, 0);
            ok = n > 0;
            sent += ok ? size_t(n) : 0;
        }
        if (fd >= 0) close(fd);
        if (!ok) why = "the ELF loader did not take it";
        return ok;
    };
    // Media from the store: the title's own HTTPS (certificates and hostnames checked; ps5/src/proton_fetch.h)
    context.fetch = [](const std::string &url, size_t limit, std::string &body, std::string &why) {
        body.clear();
        const ProtonFetch got = proton_fetch(url, limit, [&body](const void *data, size_t size) {
            body.append(static_cast<const char *>(data), size);
            return true;
        });
        if (got.ok && got.http == 200) return true;
        why = !got.error.empty() ? got.error : "HTTP " + std::to_string(got.http);
        return false;
    };
    started = pw_webui_http_start(context, kPort, why);
    // The tiles a removal waits on (src/pw_webui.hpp): finished away from the launcher's thread, ShadowMountPlus may be slow
    if (started) {
        std::thread([context]() {
            pw_webui_forwarders_finish(context, [](const std::string &line) { proton_say("webui: %s", line.c_str()); });
        }).detach();
    }
    return started;
}

std::string proton_webui_url()
{
    if (!started) return {};
    static std::string address;
    static unsigned asked;
    // A test's pictures name a made-up address (PROTON_WEBUI_ADDRESS), never this machine's own
    if (const char *shown = getenv("PROTON_WEBUI_ADDRESS"); shown && *shown) address = shown;
    if (address.empty() && asked++ % 300 == 0) address = console_address();   // a cable plugged in later is found
    return address.empty() ? std::string() : "http://" + address + ":" + std::to_string(kPort);
}

unsigned proton_webui_changes() { return changes.load(std::memory_order_acquire); }
