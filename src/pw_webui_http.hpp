// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// The WebUI over HTTP (src/pw_webui.hpp has the routes): GNU libmicrohttpd (tools/build-webui-http.sh), a pool of 4
// threads, at most 32 connections, as PS5_RetroArch's WebUI runs it on the same SDK.
// - Only requests addressed to the console's own address and port are answered (Host, and Origin when sent): a page
//   elsewhere cannot reach it through the browser (DNS rebinding).
// - A write (anything but GET and HEAD) carries X-Mihawk-Token, the per-start nonce; a body is at most 64 KiB, but an
//   upload (PUT /api/upload) streams into <file>.part at its offset and becomes <file> once its last byte arrives.
// - Every answer: no caching, nosniff, no referrer, and a Content-Security-Policy that allows the page's own files only.
// The routes run one at a time (a mutex): they read and write a few small files.
#include "pw_webui.hpp"
#include <string>

// Serves context on port, every interface. False (why: what failed) when it cannot listen.
bool pw_webui_http_start(const PwWebuiContext &context, unsigned short port, std::string *why);
void pw_webui_http_stop();
bool pw_webui_http_running();
// A fresh token: 32 hex digits from the system's random source.
std::string pw_webui_http_nonce();
