// SPDX-License-Identifier: GPL-3.0-or-later
// The WebUI on the PC (tools/run-webui-host.sh): the same routes and HTTP glue as the title's, over a data folder and
// a title folder given on the command line, with the library read from data/games/*/game.json as the launcher lists it
// (its complaints are the launcher's alone: none here). For the page's own checks in a browser, never on the console.
//
//   webui-host DATA TITLE [PORT]
#include "../src/pw_webui_http.hpp"
#include "json.hpp"
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <fstream>
#include <unistd.h>

static volatile sig_atomic_t stopping;

int main(int argc, char **argv)
{
    if (argc < 3) {
        std::fprintf(stderr, "usage: webui-host DATA TITLE [PORT]\n");
        return 2;
    }
    PwWebuiContext context;
    context.data_root = argv[1];
    context.title_root = argv[2];
    context.token = pw_webui_http_nonce();
    context.version = "host";
    context.themes = {{"gloss", "Gloss"}, {"aero", "Aero"}, {"tiles", "Tiles"}};
    context.music = {{"bubble-garden", "Bubble Garden"}, {"glass-meadow", "Glass Meadow"}, {"sky-lounge", "Sky Lounge"}};
    context.games = [root = context.data_root]() {
        std::vector<PwWebuiGame> games(1);
        games[0].id = "mihawk";
        games[0].name = "Mihawk OS";
        games[0].detail = "The Windows desktop";
        if (DIR *dir = opendir((root + "/games").c_str())) {
            while (dirent *entry = readdir(dir)) {
                if (entry->d_name[0] == '.') continue;
                std::ifstream file(root + "/games/" + entry->d_name + "/game.json");
                const auto json = nlohmann::json::parse(file, nullptr, false);
                PwWebuiGame game;
                game.id = entry->d_name;
                game.name = json.is_object() ? json.value("name", game.id) : game.id;
                game.detail = json.is_object() ? json.value("detail", std::string()) : std::string();
                game.executable = json.is_object() ? json.value("executable", std::string()) : std::string();
                game.prefix = json.is_object() ? json.value("prefix", std::string()) : std::string();
                game.arguments = json.is_object() ? json.value("arguments", std::string()) : std::string();
                game.directory = json.is_object() ? json.value("directory", std::string()) : std::string();
                game.desktop = json.is_object() ? json.value("desktop", true) : true;
                game.launcher = json.is_object() ? json.value("launcher", true) : true;
                if (json.is_object() && json.count("env") && json["env"].is_array())
                    for (const auto &word : json["env"])
                        if (word.is_string()) game.env.push_back(word.get<std::string>());
                if (game.executable.empty()) game.complaint = "game.json names no executable";
                games.push_back(game);
            }
            closedir(dir);
        }
        return games;
    };
    context.changed = [](const char *what) { std::printf("changed: %s\n", what); std::fflush(stdout); };
    // The store over this PC's curl (the console's is the title's own HTTPS); WEBUI_HOST_OFFLINE=1 leaves it unset
    if (!std::getenv("WEBUI_HOST_OFFLINE"))
        context.fetch = [](const std::string &url, size_t limit, std::string &body, std::string &why) {
            if (url.find('\'') != std::string::npos) return why = "bad address", false;
            const std::string command = "curl -fsSL --max-time 25 --max-filesize " + std::to_string(limit) + " '" + url + "'";
            FILE *pipe = popen(command.c_str(), "r");
            if (!pipe) return why = "curl did not start", false;
            body.clear();
            char buffer[65536];
            for (size_t got; (got = fread(buffer, 1, sizeof buffer, pipe)) > 0;) body.append(buffer, got);
            if (pclose(pipe) != 0) return why = "the store refused or did not answer", false;
            return true;
        };
    // A ShadowMountPlus that answers (WEBUI_HOST_FAKE_SMP=1), for the tiles page on this PC: it takes every tile
    context.homebrew_root = context.data_root + "/homebrew";
    if (std::getenv("WEBUI_HOST_FAKE_SMP"))
        context.shadowmount = [](const std::string &route, const std::string &, std::string &reply, std::string &) {
            reply = route == "/api/v1/version" ? R"({"status":0,"shadowmount_version":"fake"})"
                  : route == "/api/v1/games"   ? R"({"status":0,"games":[]})"
                  : route == "/api/v1/games/uninstall" ? R"({"status":1,"error":"busy"})" : R"({"status":0})";
            return true;
        };
    if (std::getenv("WEBUI_HOST_FAKE_SMP"))
        context.tile_worker = [](std::string &) { std::printf("tiles' worker sent\n"); std::fflush(stdout); return true; };
    const unsigned short port = argc > 3 ? (unsigned short)std::atoi(argv[3]) : 6767;
    std::string why;
    if (!pw_webui_http_start(context, port, &why)) {
        std::fprintf(stderr, "webui-host: %s\n", why.c_str());
        return 1;
    }
    std::printf("webui-host: http://127.0.0.1:%u/\n", port);
    std::fflush(stdout);
    std::signal(SIGINT, [](int) { stopping = 1; });
    std::signal(SIGTERM, [](int) { stopping = 1; });
    while (!stopping) pause();
    pw_webui_http_stop();
    return 0;
}
