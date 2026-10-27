// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// The WebUI in the launcher (src/pw_webui.hpp, src/pw_webui_http.hpp): started when the launcher shows with /data
// granted, on port 6767, and gone with the launcher's process when a game starts (sceSystemServiceLoadExec), so it
// never holds memory during a game. The launcher shows its address and a QR code (library page, Settings > Web UI).
#include "../../src/pw_webui.hpp"
#include <string>

// Starts it (once); false and why when /data is not granted or the port cannot be opened. themes, music: the
// launcher's own, which the page offers (PwWebuiContext).
bool proton_webui_start(std::string *why, std::vector<PwWebuiChoice> themes, std::vector<PwWebuiChoice> music);
// "http://<address>:6767" while it runs and the console has an IPv4 address, else "". Shown on screen only: the
// address never goes to a log (AGENTS.md rule 5).
std::string proton_webui_url();
// The data folder the WebUI serves (/data/ps5proton; the host reference's own beside its app): covers/ and games/ there.
std::string proton_webui_data_root();
// Counts what the page changed ("settings", "library"): the launcher reloads them when the count moves.
unsigned proton_webui_changes();
