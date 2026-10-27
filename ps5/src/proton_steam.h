/*
 * MihawkOS - the launcher's Steam option (src/pw_steam.h says what it is and why it is shaped so).
 *
 * The launcher asks nothing of the network until the user presses CROSS on it. Then, in a thread of its own, the title
 * downloads Valve's installer over its own TLS (proton_fetch.h) into /data/ps5proton/downloads, checks that it is a
 * plausible signed Windows installer, and the launcher restarts into a session that runs it silently in the Mihawk
 * prefix. That session ends when the installer does; the launcher then finds the client installed, and CROSS starts
 * it for the sign-in (a session that ends itself once the sign-in is saved: native/proton_wine.cpp).
 *
 * Copyright (C) 2026 Mihawk
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef PROTON_STEAM_H
#define PROTON_STEAM_H

#include <stdint.h>

#include <string>
#include <vector>

#include "proton_launch.h"
#include "../../src/pw_steam.h"

struct ProtonSteam {
	enum Phase { idle, downloading, downloaded, failed } phase{ idle };
	bool installed{ false };   /* the client's program is in the prefix */
	bool signed_in{ false };   /* its loginusers.vdf holds a user whose sign-in is remembered */
	std::string persona;       /* that user's display name, when signed in */
	std::string message;       /* one sentence for the screen */
	uint64_t bytes{ 0 };       /* downloaded so far */
	int fetch_status{ 0 };     /* failed: the last try's PW_HTTPS_* status (src/pw_https.h), 0 when the file was the fault */
};

/* What there is now: the files are read again each call (a restart or an install changes them), the download's
 * progress comes from its thread. */
ProtonSteam proton_steam_state();
/* Start the download of the installer, if none is running and the client is not installed; returns at once. */
void proton_steam_download();
/* The download is over but the installer could not be started: the row fails (CROSS tries again), with `message`. */
void proton_steam_fail(const std::string &message);
/* The catalog entry to restart into, written under games/ first: the silent installer run (installer true, once the
 * download is checked) or the client's own window (for the sign-in). false, with `problem` set, when it cannot be made. */
bool proton_steam_game(ProtonGame *out, bool installer, std::string *problem, bool big_picture = false);
/* The launcher's words for the option, so the rest of the launcher names nothing itself. */
const char *proton_steam_title();
PwSteamStep proton_steam_step(const ProtonSteam &state);   /* src/pw_steam.h's step for what the launcher read */
const char *proton_steam_subtitle(const ProtonSteam &state);
/* The paragraphs of the launcher's side panel for the option, for the state it is in. */
std::vector<std::string> proton_steam_panel(const ProtonSteam &state);

#endif
