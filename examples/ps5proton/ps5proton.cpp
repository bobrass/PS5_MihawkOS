/*
 * PS5_Proton - game library and the template UI kit's thirty themes.
 * Derived from PS5_VulkanTemplate's Samples menu and Themes picker.
 * Copyright (C) 2026 Mihawk
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "kit.hpp"
#include "launcher_music.hpp"
#include "setup_tour.hpp"
#include "proton_webui.h"
#include "../../src/pw_qrcodegen.h"
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sys/stat.h>

namespace {
class VulkanExample : public ps5ui::KitExample
{
	std::vector<ProtonGame> games;
	std::string problem;
	std::string sessionNote;   // why the last session failed, or that it never came back
	std::string noticesNote;   // what the last session said on screen
	std::string payloadsNote;  // the startup tip: what the user's payloads held at the last session's start
	bool payloadShown{ false };     // the payload checklist's page: failing, all there is until it passes; at a fresh start always
	int payloadPicture{ -1 };       // frames until the checklist's picture is taken (the payload-check-show flag); -1: none
	hui::ui::ListView library, themes, settingsList, controlsList, buttonList;
	pw_controls controls{};
	std::string controlsNotice;
	int gameMode{1};
	int highRefresh{1};   // Settings > 120 Hz output (src/pw_high_refresh.h)
	int detailedLogs{0};   // Settings > Logging (src/pw_detailed_logs.h)
	int controller{PW_CONTROLLER_XBOX_360};   // Settings > Controller (src/pw_controller.h)
	unsigned webuiSeen{0};   // the WebUI's change count the launcher last reloaded at (ps5/src/proton_webui.h)
	std::string qrText;      // the QR code drawn, and its modules (pw_qrcodegen)
	std::vector<uint8_t> qrModules;
	int qrSize{0};
	std::map<std::string, std::pair<long long, uint32_t>> covers;   // entry id: its header art's time and texture
	enum class Page { library, aurelia, themes, settings, controls, buttons };

	// The Aurelia page's library: the snapshot a session wrote (native/proton_wine.cpp aurelia_save_library), so the
	// page renders without starting the client. account is the name its sign-in reported, if any.
	struct AureliaEntry { unsigned app_id; std::string name; unsigned minutes; bool update; };
	std::vector<AureliaEntry> aureliaEntries;
	std::string aureliaAccount;
	std::string aureliaNote;
	bool aureliaOpenTried{ false };
	int aureliaFocus{ 0 };
	float aureliaScroll{ 0.0f };   // the shelf's leftmost tile, animated
	hui::ui::ListView aureliaList;
	Page page{Page::library};
	unsigned mouseAction{0};
	hui::gfx::DrawList scene;
	hui::ui::Feedback feedback;
	ps5ui::HoldToLeave leaving;
	bool dataAvailable{ false };
	float clock{ 0.0f }, applied{ 0.0f };
	int selectedTheme{ 0 };
	ProtonInstall install;   // Settings > Install runtime (proton_install.h)
	// The first-launch guided setup (setup_tour.hpp), while it shows
	std::unique_ptr<SetupTour> tour;
	PwSetupFacts setupFacts{};
	PwSetupStep setupStep{ PW_SETUP_NONE };
	bool setupSimulated{ false };
	bool setupUnattended{ false };   // proton_setup_unattended: a scripted run with the real engine and its sessions
	// MihawkOS's own music (launcher_music.hpp): R1 or Settings > Music switches it; the name shows for a moment
	Jukebox music;
	float musicShown{ 0.0f };
	ProtonUpdate updates;    // Settings > Check for updates (proton_update.h)
	bool drawnOnce{ false };
	// The store client's set-up (ps5/src/proton_steam.h), shown as a row of the Aurelia page's Accounts block
	ProtonSteam store;
	std::string clientNote;
	float clientPoll{ 0.0f };
	float steamPoll{ 0.0f };             // the Steam guide's next read of the client's state
	bool clientTestTried{ false }; bool clientTest{ false };
	bool clientInstallQueued{ false };
	bool aureliaAccounts{ false };       // the Aurelia page shows its Accounts rows instead of the shelf
	int clientRow{ -1 };                 // the library's Steam row (a default entry, not a catalog entry: its tag is -1)
	bool fromAccounts{ false };          // the press came from the Aurelia page's rows: the session returns there
	std::vector<ProtonGame> allGames;    // the catalog before the library's filter: the Aurelia page's entries are in it
	std::string runtimeBlocked;          // why no session can start now (the runtime or /data), or empty

	const hui::ui::Theme &theme() const
	{
		return hui::ui::themes()[page == Page::themes ? themes.focus() : selectedTheme];
	}

	void syncThemes()
	{
		for (int i = 0; i < (int)themes.items().size(); ++i)
			themes.item(i).badge = i == selectedTheme ? "IN USE" : "";
	}

	void openThemes()
	{
		page = Page::themes;
		themes.set_focus(selectedTheme);
		themes.enter();
		syncThemes();
	}

	// What the page is showing, written where a run leaves it: a page opened, its game count, the account and the
	// focused row (the console's klog is not readable from a recorder on every boot, so the page proves itself here).
	void aureliaMark(const char *state, const char *note)
	{
		const std::string path = std::string(PROTON_DATA_ROOT) + "/aurelia-page.txt", temporary = path + ".tmp";
		FILE *file = fopen(temporary.c_str(), "w");
		if (!file) return;
		fprintf(file, "state=%s games=%zu account=%s focus=%d of=%zu note=%s\n", state, aureliaEntries.size(),
			aureliaAccount.empty() ? "(none)" : aureliaAccount.c_str(), aureliaFocus, aureliaEntries.size(),
			note ? note : "");
		fclose(file);
		rename(temporary.c_str(), path.c_str());
	}

	// The Aurelia page: the session's snapshot of the account's library, drawn in the launcher's theme, and the two
	// commands the page can run (sign-in, refresh) queued as ordinary game restarts with their own arguments.
	void openAurelia(bool accounts = false)
	{
		aureliaEntries.clear();
		aureliaAccount.clear();
		aureliaNote.clear();
		std::ifstream account(std::string(PROTON_DATA_ROOT) + "/aurelia-account.txt");
		if (account) std::getline(account, aureliaAccount);
		std::ifstream snapshot(std::string(PROTON_DATA_ROOT) + "/aurelia-library.txt");
		for (std::string line; std::getline(snapshot, line); ) {
			unsigned app_id = 0, minutes = 0;
			int update = 0;
			char name[160] = {};
			if (sscanf(line.c_str(), "%u\t%159[^\t]\t%u\t%d", &app_id, name, &minutes, &update) >= 2)
				aureliaEntries.push_back({app_id, name, minutes, update != 0});
		}
		// The Accounts rows: the library account (what lists and installs) and the store client (the official one, which a
		// game that calls the store's API runs on; set up once).
		std::vector<hui::ui::ListItem> accounts_rows(2);
		accounts_rows[0].title = "Library account";
		accounts_rows[0].tag = 0;
		accounts_rows[1].title = "Steam client";
		accounts_rows[1].tag = 1;
		aureliaList.set_items(accounts_rows);
		aureliaList.set_focus(0);
		aureliaAccounts = accounts || aureliaEntries.empty();
		aureliaFocus = 0;
		aureliaScroll = 0.0f;
		syncAccounts();
		if (aureliaEntries.empty())
			aureliaNote = aureliaAccount.empty() ? "Sign in to Aurelia to see your library."
				: "The library is empty; press TRIANGLE to refresh it.";
		else if (aureliaAccount.empty())
			aureliaNote = "Signed in; the account name shows after the next sign-in.";
		proton_say("launcher: Aurelia page, %zu game(s) from the session snapshot, account %s", aureliaEntries.size(),
			aureliaAccount.empty() ? "(none)" : aureliaAccount.c_str());
		page = Page::aurelia;
		aureliaMark("open", aureliaNote.c_str());
	}

	ProtonGame *aureliaProfile()
	{
		for (auto &game : allGames)
			if (game.id == "aurelia") return &game;
		return nullptr;
	}

	// Queues a session that runs the client with these arguments, through the ordinary game restart.
	void aureliaCommand(const char *arguments, const char *reason)
	{
		if (ps5.frameBudget) return;
		ProtonGame *game = aureliaProfile();
		if (!game) {
			aureliaNote = "The Aurelia entry is missing from the game catalog.";
			return;
		}
		if (sessionBlocked()) return;
		// The launcher comes back to this page instead of its library: the startup hook below reopens it.
		if (FILE *flag = fopen((std::string(PROTON_DATA_ROOT) + "/aurelia-open").c_str(), "w")) fclose(flag);
		ProtonGame queued = *game;
		queued.arguments = arguments;
		aureliaNote = std::string(arguments).find("login") != std::string::npos ? "Opening the sign-in screen..."
			: "Refreshing the library...";
		proton_queue_restart(&queued, proton_launch().cycle, reason);
		quit = true;
	}

	// A game the client installed and the catalog lists under its app id, launched the launcher's own way.
	ProtonGame *aureliaInstalled(const AureliaEntry &entry)
	{
		const std::string id = std::to_string(entry.app_id);
		for (auto &game : allGames)
			if (game.id == id) return &game;
		return nullptr;
	}

	void syncMusic()
	{
		settingsList.item(5).subtitle = music.title() ? music.title() : "Off";
		settingsList.item(5).badge = music.title() ? "ON" : "OFF";
	}

	void nextMusic()
	{
		music.next();
		musicShown = 2.5f;
		feedback.play(hui::audio::Cue::toggle);
		syncMusic();
	}

	void syncGameMode()
	{
		settingsList.item(1).subtitle = gameMode ?
			"On: full-screen games take the screen directly and the desktop unloads while they play" :
			"Off: every game is shown inside the desktop, which stays loaded";
		settingsList.item(1).badge = gameMode ? "ON" : "OFF";
	}

	void toggleGameMode()
	{
		const int changed = !gameMode;
		// Scripted UI runs preview the choice without overwriting the player's file.
		const int error = ps5.frameBudget ? 0 : pw_game_mode_save(changed, proton_game_mode_path().c_str());
		if (error) {
			controlsNotice = "Could not save Game Mode. Your previous choice is unchanged.";
			proton_say("Game Mode: save failed (%d)", error);
		} else {
			gameMode = changed;
			controlsNotice = gameMode ? "Game Mode on. Applies from the next launch." : "Game Mode off. Applies from the next launch.";
			feedback.play(hui::audio::Cue::saved);
			proton_say("Game Mode: %s %s", ps5.frameBudget ? "preview" : "saved", gameMode ? "on" : "off");
		}
		syncGameMode();
	}

	void syncHighRefresh()
	{
		settingsList.item(10).subtitle = highRefresh ? "On: games can run at up to 120 frames a second on a 120 Hz TV" :
			"Off: the screen stays at 60 Hz";
		settingsList.item(10).badge = highRefresh ? "ON" : "OFF";
	}

	void toggleHighRefresh()
	{
		const int changed = !highRefresh;
		// Scripted UI runs preview the choice without overwriting the player's file.
		const int error = ps5.frameBudget ? 0 : pw_high_refresh_save(changed, proton_high_refresh_path().c_str());
		if (error) {
			controlsNotice = "Could not save the 120 Hz setting. Your previous choice is unchanged.";
			proton_say("120 Hz: save failed (%d)", error);
		} else {
			highRefresh = changed;
			controlsNotice = highRefresh ? "120 Hz on. Applies from the next launch." : "120 Hz off. Applies from the next launch.";
			feedback.play(hui::audio::Cue::saved);
			proton_say("120 Hz: %s %s", ps5.frameBudget ? "preview" : "saved", highRefresh ? "on" : "off");
		}
		syncHighRefresh();
	}

	void syncLogging()
	{
		settingsList.item(7).subtitle = detailedLogs ? "On: each session's logs go to one folder, for a report" :
			"Off: only the usual logs are kept";
		settingsList.item(7).badge = detailedLogs ? "ON" : "OFF";
	}

	void toggleLogging()
	{
		const int changed = !detailedLogs;
		// Scripted UI runs preview the choice without overwriting the player's file.
		const int error = ps5.frameBudget ? 0 : pw_detailed_logs_save(changed, proton_logging_path().c_str());
		if (error) {
			controlsNotice = "Could not save Logging. Your previous choice is unchanged.";
			proton_say("Logging: save failed (%d)", error);
		} else {
			detailedLogs = changed;
			controlsNotice = detailedLogs ? "Logging on. Applies from the next session." : "Logging off. Applies from the next session.";
			feedback.play(hui::audio::Cue::saved);
			proton_say("Logging: %s %s", ps5.frameBudget ? "preview" : "saved", detailedLogs ? "on" : "off");
		}
		syncLogging();
	}

	void syncController()
	{
		settingsList.item(8).subtitle = std::string("Games and Steam see ") +
			(controller == PW_CONTROLLER_DUALSENSE ? "a " : "an ") + pw_controller_name(controller);
		settingsList.item(8).badge = controller == PW_CONTROLLER_DUALSENSE ? "DUALSENSE" : controller == PW_CONTROLLER_XBOX_ONE ?
			"XBOX ONE" : "XBOX 360";
	}

	// CROSS takes the next choice: Xbox 360, Xbox One, DualSense.
	void nextController()
	{
		const int changed = (controller + 1) % PW_CONTROLLER_KINDS;
		// Scripted UI runs preview the choice without overwriting the player's file.
		const int error = ps5.frameBudget ? 0 : pw_controller_save(changed, proton_controller_path().c_str());
		if (error) {
			controlsNotice = "Could not save the controller. Your previous choice is unchanged.";
			proton_say("Controller: save failed (%d)", error);
		} else {
			controller = changed;
			controlsNotice = std::string(pw_controller_name(controller)) + " chosen. Applies from the next launch.";
			feedback.play(hui::audio::Cue::saved);
			proton_say("Controller: %s %s", ps5.frameBudget ? "preview" : "saved", pw_controller_word(controller));
		}
		syncController();
	}

	void syncUpdates()
	{
		updates = proton_update_state();
		const char *badges[] = { "", "CHECKING", "NEWEST", "AVAILABLE", "DOWNLOADING", "READY", "FAILED" };
		settingsList.item(6).badge = badges[updates.phase];
		settingsList.item(6).subtitle = updates.version.empty() ? "On request only" : PS5_TITLE_NAME " " + updates.version;
	}

	void syncInstall()
	{
		install = proton_install_state();
		const char *badges[] = { "", "READY", "CHECKING", "INSTALLING", "DONE", "FAILED" };
		settingsList.item(3).badge = badges[install.phase];
		settingsList.item(3).subtitle = install.phase == ProtonInstall::running ? "Installing" :
			install.phase == ProtonInstall::done ? "Installed" : "From /data/ps5proton/install or a USB stick";
	}

	void syncControls()
	{
		for (unsigned i = 0; i < 3; ++i)
			controlsList.item(i).subtitle = pw_controls_button_label(controls.button[i]);
	}

	// The library's rows: the Steam row first, then each game; first: the focus starts on the first game below Steam.
	void syncLibraryRows(bool first)
	{
		std::vector<hui::ui::ListItem> rows;
		for (size_t i = 0; i < games.size(); ++i) {
			hui::ui::ListItem item;
			item.title = games[i].name;
			item.subtitle = games[i].detail;
			item.badge = games[i].complaint.empty() ? "" : games[i].x86_32 ? "32-BIT" : "UNAVAILABLE";
			item.disabled = !games[i].complaint.empty();
			item.tag = (int)i;
			rows.push_back(item);
		}
		if (dataAvailable) {
			// The default entry: Steam is always in the library. CROSS downloads and installs it from Valve when it is missing, opens
			// its sign-in once, and from then on runs it in Big Picture (src/pw_steam.h's steps).
			hui::ui::ListItem clientItem;
			clientItem.title = "Steam";
			clientItem.tag = -1;
			clientRow = 0;
			rows.insert(rows.begin(), clientItem);   // first: a default entry. The focus starts on the first game below it.
		}
		const int focus = library.focus();
		library.set_items(rows);
		if (first && clientRow == 0 && rows.size() > 1) library.set_focus(1);
		else if (!first && !rows.empty()) library.set_focus(std::min(focus, (int)rows.size() - 1));
		syncAccounts();
	}

	// What the WebUI changed (ps5/src/proton_webui.h): the settings and the library, read again as at the start.
	void reloadFromWebui()
	{
		pw_controls_load(&controls, proton_controls_path().c_str());
		pw_game_mode_load(&gameMode, proton_game_mode_path().c_str());
		pw_high_refresh_load(&highRefresh, proton_high_refresh_path().c_str());
		pw_detailed_logs_load(&detailedLogs, proton_logging_path().c_str());
		pw_controller_load(&controller, proton_controller_path().c_str());
		syncControls();
		syncGameMode();
		syncHighRefresh();
		syncLogging();
		syncController();
		// The theme and the music the page chose (title folder hui/theme.txt, hui/music.txt)
		std::string id;
		hui::save::read_file(proton_title_path(PS5_APP_ROOT "/hui/theme.txt"), &id, 64);
		while (!id.empty() && (id.back() == '\n' || id.back() == '\r' || id.back() == ' ')) id.pop_back();
		const auto looks = hui::ui::themes();
		for (int i = 0; i < (int)looks.size(); ++i)
			if (id == looks[i].id && i != selectedTheme) {
				selectedTheme = i;
				ps5ui::set_active_theme(i);
				syncThemes();
			}
		if (music.remembered() != music.piece()) {
			music.play(music.remembered(), false);
			musicShown = 2.5f;
			syncMusic();
		}
		loadGames();
		syncLibraryRows(false);
		proton_say("webui: the launcher reloaded its settings and library");
	}

	// The library page's Web UI block, under the panel's words: its code and its address, where the eye finds it first.
	void drawWebuiBlock(hui::ui::Painter &paint, const hui::ui::Theme &look)
	{
		const std::string url = proton_webui_url();
		if (url.empty()) return;
		drawQr(url, 1240, 778, 164);
		paint.heading("Web UI", 1426, 806, 28, look.text);
		float y = 846;
		for (const auto &line : kit.fonts.regular.font->wrap(url, 21, 350.0f)) {
			paint.body(line, 1426, y, 21, look.text);
			y += 30;
		}
		for (const auto &line : kit.fonts.regular.font->wrap("Scan it with your phone to add games, back up saves and change settings.", 19, 350.0f)) {
			paint.body(line, 1426, y + 4, 19, look.text_muted);
			y += 27;
		}
	}

	// The entry's header art (the WebUI saves it: covers/<id>-header.img), uploaded again only when its file changes; 0: none.
	uint32_t coverTexture(const std::string &id)
	{
		const std::string path = proton_webui_data_root() + "/covers/" + id + "-header.img";
		struct stat status;
		const long long when = stat(path.c_str(), &status) == 0 ? (long long)status.st_mtime : 0;
		auto found = covers.find(id);
		if (found != covers.end() && found->second.first == when) return found->second.second;
		if (found != covers.end() && found->second.second) kit.renderer.destroy_texture(found->second.second);
		uint32_t texture = 0;
		std::string data;
		if (when && hui::save::read_file(path, &data, 4u << 20)) {
			int w = 0, h = 0, channels = 0;
			stbi_uc *rgba = stbi_load_from_memory((const stbi_uc *)data.data(), (int)data.size(), &w, &h, &channels, 4);
			if (rgba) {
				texture = kit.renderer.create_texture(w, h, rgba);
				stbi_image_free(rgba);
			}
		}
		covers[id] = { when, texture };
		return texture;
	}

	// A QR code of text, black modules on a white card size wide, at x, y; encoded once for each text (pw_qrcodegen).
	void drawQr(const std::string &text, float x, float y, float size)
	{
		if (text != qrText) {
			static uint8_t code[qrcodegen_BUFFER_LEN_MAX], temp[qrcodegen_BUFFER_LEN_MAX];
			qrText = text;
			qrSize = 0;
			qrModules.clear();
			if (qrcodegen_encodeText(text.c_str(), temp, code, qrcodegen_Ecc_MEDIUM, qrcodegen_VERSION_MIN, qrcodegen_VERSION_MAX,
					qrcodegen_Mask_AUTO, true)) {
				qrSize = qrcodegen_getSize(code);
				for (int row = 0; row < qrSize; ++row)
					for (int col = 0; col < qrSize; ++col) qrModules.push_back(qrcodegen_getModule(code, col, row) ? 1 : 0);
			}
		}
		if (!qrSize) return;
		scene.rounded_rect({ x, y, size, size }, size * 0.06f, hui::gfx::Color::rgb(0xffffff));
		const float quiet = size * 0.07f, module = (size - 2 * quiet) / qrSize;
		for (int row = 0; row < qrSize; ++row)
			for (int col = 0; col < qrSize; ++col)
				if (qrModules[size_t(row * qrSize + col)])
					scene.rounded_rect({ x + quiet + col * module, y + quiet + row * module, module + 0.5f, module + 0.5f }, 0,
						hui::gfx::Color::rgb(0x1b2430));
	}

	// The two Accounts rows' words, from what the files and the download thread say now (src/pw_steam.h's step words).
	void syncAccounts()
	{
		store = proton_steam_state();
		if (clientRow >= 0) {
			auto &row = library.item(clientRow);
			const PwSteamStep step = proton_steam_step(store);
			row.subtitle = pw_steam_step_subtitle(step);
			row.badge = pw_steam_step_badge(step);
		}
		if (aureliaList.items().size() < 2) return;
		auto &library_row = aureliaList.item(0);
		library_row.subtitle = aureliaAccount.empty() ? "Not signed in. Press CROSS, then scan the code with your phone"
			: "Signed in as " + aureliaAccount;
		library_row.badge = aureliaAccount.empty() ? "SIGN IN" : "SIGNED IN";
		const PwSteamStep step = proton_steam_step(store);
		auto &client_row = aureliaList.item(1);
		client_row.subtitle = pw_steam_step_subtitle(step);
		client_row.badge = pw_steam_step_badge(step);
	}

	// A session started from the Accounts rows comes back to the Aurelia page, on those rows (the startup hook reads this).
	void returnToAccounts()
	{
		if (!fromAccounts) return;   // pressed on the library's row: back to the library
		if (FILE *flag = fopen(PROTON_DATA_ROOT "/aurelia-open", "w")) {
			fputs("accounts\n", flag);
			fclose(flag);
		}
	}

	// CROSS on the Steam client row: until it is signed in, the Steam guide (src/pw_steam_guide.h) takes the user through
	// the download, the install and the sign-in; signed in, Steam opens in Big Picture.
	void pressSteam()
	{
		if (ps5.frameBudget && !clientTest) return;   // a scripted run asks nothing of the network
		syncAccounts();
		const PwSteamStep step = proton_steam_step(store);
		if (step != PW_STEAM_STEP_SIGNED_IN) {
			if (sessionBlocked()) return;   // the note says why
			clientNote.clear();
			returnToAccounts();
			// A test's press (the client-press flag) takes the guide past its first page: Start is pressed for it
			const bool busy = step == PW_STEAM_STEP_DOWNLOADING || step == PW_STEAM_STEP_INSTALLING;
			startSteamGuide(busy || clientTest ? PW_STEAM_PAGE_DOWNLOAD : PW_STEAM_PAGE_INTRO, PW_STEAM_FAIL_NONE);
			if (clientTest && !busy) proton_steam_download();
		} else if (sessionBlocked()) {
			// the note says why
		} else {
			ProtonGame client;
			std::string problem;
			if (proton_steam_game(&client, false, &problem, true)) {
				proton_say("launcher: %s in Big Picture", proton_steam_title());
				proton_queue_restart(&client, proton_launch().cycle, "steam");
				quit = true;
			} else {
				clientNote = std::string(proton_steam_title()) + " cannot start: " + problem;
			}
		}
		syncAccounts();
	}

	// A session cannot start while the runtime or /data is missing: say why on the page instead of restarting into a failure.
	bool sessionBlocked()
	{
		if (runtimeBlocked.empty()) return false;
		aureliaNote = clientNote = "Sessions cannot start: " + runtimeBlocked;
		return true;
	}

	// A press on an Accounts row: row 0 is the library account's QR sign-in, row 1 the store client's set-up.
	void accountAction(int row)
	{
		if (ps5.frameBudget) return;
		fromAccounts = true;
		if (row == 0) aureliaCommand("login --qr --json", "Aurelia sign-in");
		else pressSteam();
	}

	hui::ui::ListView &activeList()
	{
		switch (page) {
		case Page::aurelia: return aureliaList;
		case Page::themes: return themes;
		case Page::settings: return settingsList;
		case Page::controls: return controlsList;
		case Page::buttons: return buttonList;
		default: return library;
		}
	}

	void saveControls(const pw_controls &changed)
	{
		// Scripted UI runs preview choices without overwriting the player's file.
		const int error = ps5.frameBudget ? 0 : pw_controls_save(&changed, proton_controls_path().c_str());
		if (error) {
			controlsNotice = "Could not save controls. Your previous choices are unchanged.";
			proton_say("Controls: save failed (%d)", error);
		} else {
			controls = changed;
			controlsNotice = "Saved. Applies to every game you launch.";
			feedback.play(hui::audio::Cue::saved);
			proton_say("Controls: %s left=%s right=%s middle=%s", ps5.frameBudget ? "preview" : "saved", pw_controls_button_id(controls.button[0]),
				pw_controls_button_id(controls.button[1]), pw_controls_button_id(controls.button[2]));
		}
		syncControls();
	}

	void update(float dt)
	{
		feedback.clear();
		const hui::InputFrame input = ps5.frameBudget ? kit.scripted("ps5proton", ps5.framesDrawn) : kit.input();
		clock += dt;
		applied = std::max(0.0f, applied - dt);
		if (const unsigned changes = proton_webui_changes(); changes != webuiSeen) {
			webuiSeen = changes;
			reloadFromWebui();
		}
		// A one-shot test hook: with /data/ps5proton/aurelia-open present the page opens on the first frame, so a run
		// nobody sits at can prove the page (the marker it writes) without a controller.
		if (!aureliaOpenTried) {
			aureliaOpenTried = true;
			const std::string flag = std::string(PROTON_DATA_ROOT) + "/aurelia-open";
			if (access(flag.c_str(), F_OK) == 0) {
				std::string how;
				{
					std::ifstream content(flag);
					std::getline(content, how);
				}
				remove(flag.c_str());
				proton_say("launcher: the Aurelia-open flag opened the page%s", how == "accounts" ? " on its Accounts rows" : "");
				openAurelia(how == "accounts");
			}
		}
		// A one-shot test hook, like the Aurelia page's: with /data/ps5proton/client-press present, CROSS is pressed on the
		// option on the first frame, so a run nobody sits at can take the option through its steps. It works under a
		// frame budget too (the budget then ends the launcher, and a restart the option queued is carried out).
		if (!clientTestTried) {
			clientTestTried = true;
			if (access(PROTON_DATA_ROOT "/client-press", F_OK) == 0) {
				clientTest = true;
				remove(PROTON_DATA_ROOT "/client-press");
				proton_say("launcher: the client-press flag pressed CROSS on the option");
				pressSteam();
			}
		}
		const bool clientBusy = store.phase == ProtonSteam::downloading || store.phase == ProtonSteam::downloaded;
		if ((page == Page::aurelia || clientBusy) && (clientPoll -= dt) <= 0.0f) {
			clientPoll = 0.5f;
			syncAccounts();
			// The download is done and checked: restart into the session that runs the installer, once.
			if (store.phase == ProtonSteam::downloaded && !store.installed && !clientInstallQueued && (!ps5.frameBudget || clientTest)) {
				clientInstallQueued = true;
				ProtonGame installer;
				std::string problem;
				if (proton_steam_game(&installer, true, &problem)) {
					proton_say("launcher: installing from the downloaded installer");
					returnToAccounts();
					proton_queue_restart(&installer, proton_launch().cycle, "install");
					quit = true;
				} else {
					clientNote = "The installer cannot run: " + problem;
					clientInstallQueued = false;
					proton_steam_fail("The installer cannot run: " + problem);   // the row offers CROSS to try again
				}
			}
		}
		if (page == Page::library && input.is_pressed(hui::Action::west)) {
			openThemes();
		} else if (page == Page::library && input.is_pressed(hui::Action::north)) {
			page = Page::settings;
			settingsList.enter();
		// Aurelia is parked (the owner, 2026-10-24): its page stays (the aurelia-open test hook still opens it), but R2 no
		// longer does and the hints no longer name it.
		} else if (page == Page::aurelia && input.is_pressed(hui::Action::west)) {
			aureliaCommand("login --qr --json", "Aurelia sign-in");
		} else if (page == Page::aurelia && input.is_pressed(hui::Action::north)) {
			aureliaCommand("list --json", "Aurelia refresh");

		} else if (page == Page::aurelia && aureliaAccounts) {
			// The Accounts rows. CIRCLE goes back to the shelf, or to the library when there is no shelf to go to.
			const auto event = aureliaList.handle(input, feedback);
			if (event == hui::ui::Event::cancelled || input.is_pressed(hui::Action::back)) {
				if (aureliaEntries.empty()) page = Page::library;
				else aureliaAccounts = false;
			} else if (event == hui::ui::Event::activated) {
				accountAction(aureliaList.focus());
			}
		} else if (page == Page::aurelia) {
			if (input.nav == hui::Direction::up) {
				aureliaAccounts = true;
			} else if (input.nav == hui::Direction::left) {
				aureliaFocus = std::max(0, aureliaFocus - 1);
			} else if (input.nav == hui::Direction::right) {
				aureliaFocus = std::min(int(aureliaEntries.size()) - 1, aureliaFocus + 1);
			} else if (input.is_pressed(hui::Action::back)) {
				page = Page::library;
			} else if (input.is_pressed(hui::Action::confirm)) {
				const AureliaEntry &entry = aureliaEntries[size_t(aureliaFocus)];
				if (ProtonGame *installed = aureliaInstalled(entry)) {
					if (!installed->complaint.empty()) {
						aureliaNote = installed->complaint;   // the library refuses such a game; so does the page
					} else {
						proton_say("launcher: Aurelia plays %s (%u)", entry.name.c_str(), entry.app_id);
						proton_queue_restart(installed, proton_launch().cycle, "aurelia play");
						quit = true;
					}
				} else {
					char command[96];
					snprintf(command, sizeof(command), "install %u --json", entry.app_id);
					aureliaCommand(command, "Aurelia install");
				}
			}
		} else {
			const auto event = activeList().handle(input, feedback);
			if (event == hui::ui::Event::cancelled || input.is_pressed(hui::Action::back)) {
				if (page == Page::buttons) page = Page::controls;
				else if (page == Page::controls) page = Page::settings;
				else page = Page::library;
			} else if (event == hui::ui::Event::activated) {
				switch (page) {
				case Page::themes:
					selectedTheme = themes.focus();
					ps5ui::set_active_theme(selectedTheme);
					syncThemes();
					applied = 2.5f;
					feedback.play(hui::audio::Cue::saved);
					proton_say("launcher theme: %s", theme().id);
					break;
				case Page::settings:
					if (settingsList.focus() == 5) {
						nextMusic();
					} else if (settingsList.focus() == 4) {
						setupFacts = SetupTour::facts(dataAvailable, true, &setupSimulated);
						startSetup(PW_SETUP_WELCOME);
					} else if (settingsList.focus() == 1) {
						toggleGameMode();
					} else if (settingsList.focus() == 10) {
						toggleHighRefresh();
					} else if (settingsList.focus() == 7) {
						toggleLogging();
					} else if (settingsList.focus() == 8) {
						nextController();
					} else if (settingsList.focus() == 3) {
						// Look again each time (a stick may have been plugged in), then start when the pieces are all there.
						if (!ps5.frameBudget && proton_install_find().phase == ProtonInstall::ready) proton_install_start();
						syncInstall();
					} else if (settingsList.focus() == 6) {
						// One press a step: look, then download what was found, then restart into it.
						const ProtonUpdate::Phase now = proton_update_state().phase;
						if (ps5.frameBudget || now == ProtonUpdate::checking || now == ProtonUpdate::downloading) {
							// a check or a download is running; a scripted run asks nothing of the network
						} else if (now == ProtonUpdate::available) {
							proton_update_download();
						} else if (now == ProtonUpdate::ready) {
							proton_update_restart();
						} else {
							proton_update_check();
						}
						syncUpdates();
					} else if (settingsList.focus() == 0) {
						page = Page::controls;
						controlsList.enter();
					}  // About (2) is read where it is
					break;
				case Page::controls:
					if (controlsList.focus() == 3) {
						pw_controls defaults;
						pw_controls_defaults(&defaults);
						saveControls(defaults);
					} else {
						mouseAction = controlsList.focus();
						page = Page::buttons;
						buttonList.set_focus(controls.button[mouseAction]);
						buttonList.enter();
					}
					break;
				case Page::buttons: {
					pw_controls changed = controls;
					pw_controls_assign(&changed, mouseAction, buttonList.focus());
					saveControls(changed);
					page = Page::controls;
					break;
				}
				case Page::library:
					if (!ps5.frameBudget) {
						const int index = library.items()[library.focus()].tag;
						if (index < 0) {
							fromAccounts = false;
							pressSteam();
							break;
						}
						proton_queue_restart(&games[index], proton_launch().cycle, "chosen");
						quit = true;
					}
					break;
				case Page::aurelia:   // handled above: the Accounts rows and the shelf have their own branches
					break;
				}
			}
		}
		leaving.label = "Close " PS5_TITLE_NAME;
		if (!ps5.frameBudget && leaving.update(dt, (ps5_pad().held & PAD_OPTIONS) != 0)) quit = true;
		if (page == Page::aurelia) {
			static int marked = -1;
			if (marked != aureliaFocus) {
				marked = aureliaFocus;
				aureliaMark("focus", aureliaNote.c_str());
			}
		}
		for (auto *list : {&library, &aureliaList, &themes, &settingsList, &controlsList, &buttonList}) {
			list->style.theme = theme();
			list->update(dt);
		}
		musicShown = std::max(0.0f, musicShown - dt);
		music.pump(dt);
		if (page == Page::settings) {
			syncInstall();
			syncUpdates();
		}
		// The launcher has drawn: this title started once, so a previous title an update replaced can go.
		if (!drawnOnce && ps5.framesDrawn > 0) {
			drawnOnce = true;
			proton_update_started();
		}
		kit.play(feedback, theme().sounds);
		kit.tick(dt);
	}

	// What plays, for a moment after it changes: a note and the piece's name, top right
	void drawMusic(const hui::ui::Theme &look)
	{
		if (musicShown <= 0.0f) return;
		const float shown = std::min(1.0f, std::min(musicShown / 0.3f, (2.5f - musicShown) / 0.2f + 0.0001f));
		const std::string label = music.title() ? std::string("Music: ") + music.title() : std::string("Music off");
		const float w = kit.fonts.semibold.measure(label, 24) + 92;
		const hui::gfx::Rect plate{ 1800 - w, 40 - (1.0f - shown) * 16.0f, w, 60 };
		scene.push_opacity(shown);
		scene.shadow(plate, 30, 22, look.shadow.a > 0 ? look.shadow.with_alpha(0.45f) : hui::gfx::Color::rgb(0x000000, 0.35f));
		scene.rounded_rect(plate, 30, look.surface.with_alpha(0.97f));
		const float x = plate.x + 34, cy = plate.y + 30;   // an eighth note: a head, a stem, a flag
		const auto ink = music.title() ? look.accent : look.text_muted;
		scene.circle(x - 4, cy + 8, 7, ink);
		scene.line(x + 2.5f, cy + 8, x + 2.5f, cy - 13, 3, ink);
		scene.line(x + 2.5f, cy - 13, x + 11, cy - 6, 3, ink);
		hui::ui::text(scene, kit.fonts.semibold, label, plate.x + 62, cy + 8, 24, look.text);
		scene.pop_opacity();
	}

	void paragraph(hui::ui::Painter &paint, const std::string &text, float x, float &y,
		float size, hui::gfx::Color ink)
	{
		for (const auto &line : kit.fonts.regular.font->wrap(text, size, 500.0f)) {
			paint.body(line, x, y, size, ink);
			y += size * 1.45f;
		}
	}

	// The shelf: one row of large tiles, the focused one lifted and ringed, the row scrolling to keep it in view. The
	// theme's surface, radius, accent and ink style it, so all 30 looks carry over.
	void drawAureliaShelf()
	{
		const auto &look = theme();
		const float tile_w = 250, tile_h = 330, gap = 26, left = 120, top = 300, text_w = tile_w - 32;
		const int visible = 5;
		const int count = int(aureliaEntries.size());
		if (!count) return;
		const float target = std::max(0, aureliaFocus - 2);
		aureliaScroll += (target - aureliaScroll) * 0.25f;
		for (int i = 0; i < count; ++i) {
			const float offset = float(i) - aureliaScroll;
			if (offset < -0.7f || offset > visible - 0.3f) continue;
			const bool focused = i == aureliaFocus;
			const float x = left + offset * (tile_w + gap), lift = focused ? 10.0f : 0.0f;
			const float w = tile_w + (focused ? 18.0f : 0.0f), h = tile_h + (focused ? 18.0f : 0.0f);
			const hui::gfx::Rect tile{ x - (focused ? 9.0f : 0.0f), top - lift, w, h };
			if (focused) scene.rounded_rect(tile, look.radius_card, look.accent);
			scene.rounded_rect({ tile.x + (focused ? 2.0f : 0.0f), tile.y + (focused ? 2.0f : 0.0f), w - (focused ? 4.0f : 0.0f),
				h - (focused ? 4.0f : 0.0f) }, look.radius_card, focused ? look.surface_high : look.surface);
			const auto &entry = aureliaEntries[size_t(i)];
			const auto name = kit.fonts.semibold.font->wrap(entry.name, 26, text_w);
			float y = tile.y + 54;
			for (size_t line = 0; line < name.size() && line < 3; ++line, y += 34)
				hui::ui::text(scene, kit.fonts.semibold, name[line], x + 16, y, 26, look.text);
			hui::ui::text(scene, kit.fonts.regular, std::to_string(entry.minutes) + " min", x + 16, tile.y + h - 26, 20,
				look.text_muted);
			if (entry.update)
				hui::ui::text(scene, kit.fonts.regular, "UPDATE", x + 16, tile.y + h - 56, 18, look.accent);
			if (aureliaInstalled(entry))
				hui::ui::text(scene, kit.fonts.regular, "INSTALLED", x + 16, tile.y + h - 86, 18, look.success);
		}
	}

	void compose()
	{
		const auto &look = theme();
		if (settingsList.items().size() > 9) {
			const std::string url = proton_webui_url();
			settingsList.item(9).subtitle = url.empty() ? "Starts once the PS5 is on your network" : url;
		}
		const auto glass = kit.renderer.glass_texture();
		scene.clear();
		hui::ui::Canvas canvas{ scene, kit.fonts, glass, clock };
		hui::ui::Painter paint(scene, kit.fonts, look, glass);
		const bool picking = page == Page::themes;
		const bool configuring = page != Page::library && !picking && page != Page::aurelia;
		const char *titles[] = {"Left mouse click", "Right mouse click", "Middle mouse click"};
		const char *heading = page == Page::aurelia ? "Aurelia" : picking ? "Themes" : page == Page::settings ? "Settings" :
			page == Page::controls ? "Controls" : page == Page::buttons ? titles[mouseAction] : PS5_TITLE_NAME;
		paint.heading(heading, 120, 145, 64, paint.page_text());
		const std::string subtitle = page == Page::aurelia
			? (aureliaAccounts ? std::string("Accounts: your library, and the Steam client your games run on")
				: aureliaAccount.empty() ? std::string("Steam, without a browser: sign in with the app on your phone")
				: std::string("Signed in as ") + aureliaAccount + " \u00b7 your library")
			: picking ? std::string("Find your look. Preview any theme, then press CROSS to keep it.")
			: configuring ? std::string("Choose how your controller operates the mouse.")
			: std::string("Your Windows game library");
		paint.body(subtitle, 122, 195, 24, paint.page_text_muted());
		char count[64];
		if (page == Page::aurelia && aureliaAccounts) snprintf(count, sizeof(count), "Accounts");
		else if (page == Page::aurelia) snprintf(count, sizeof(count), "%zu game%s", aureliaEntries.size(), aureliaEntries.size() == 1 ? "" : "s");
		else if (picking) snprintf(count, sizeof(count), "%02d / %02zu", themes.focus() + 1, themes.items().size());
		else if (configuring) snprintf(count, sizeof(count), "Settings / Controls");
		else if (!dataAvailable) snprintf(count, sizeof(count), "Library unavailable");
		else snprintf(count, sizeof(count), "%zu game%s", games.size(), games.size() == 1 ? "" : "s");
		paint.label(count, 1800, 145, 24, paint.page_text_muted(), hui::gfx::Align::right);
		if (page == Page::aurelia && !aureliaAccounts) drawAureliaShelf();
		else activeList().draw(canvas);

		const hui::gfx::Rect panel{ 1200, 230, 600, 740 };
		paint.panel(panel);
		float y = 300;
		if (picking) {
			paint.heading(look.name, 1240, y, 42, look.text);
			y += 48;
			paragraph(paint, look.family, 1240, y, 24, look.text_muted);
			y += 35;
			paragraph(paint, look.summary, 1240, y, 24, look.text_muted);
			paint.label("COLOR PALETTE", 1240, 535, 20, look.text_muted);
			const char *labels[] = { "Background", "Panels", "Text", "Accent" };
			const hui::gfx::Color colors[] = { look.page, look.surface, look.text, look.accent };
			for (int i = 0; i < 4; ++i) {
				const float x = 1240.0f + i * 130.0f;
				scene.rounded_rect({ x, 560, 110, 90 }, 8, look.outline);
				scene.rounded_rect({ x + 1, 561, 108, 88 }, 7, colors[i]);
				paint.body(labels[i], x, 684, 19, look.text_muted);
			}
			paint.body(selectedTheme == themes.focus() ? "Current theme" : "Preview", 1240, 805, 26, look.text);
			if (applied > 0) paint.body("Theme applied", 1240, 855, 24, look.success);
			ps5ui::draw_hints(scene, kit.fonts, look,
				{ { hui::ui::Button::dpad, "Browse" }, { hui::ui::Button::cross, "Use theme" },
				  { hui::ui::Button::circle, "Back" } }, 120, 1040);
		} else if (page == Page::settings && settingsList.focus() == 1) {
			paint.heading("Game Mode", 1240, y, 38, look.text);
			y += 65;
			paragraph(paint, "On: a full-screen game's frames go straight to the screen, and the Mihawk desktop unloads while it plays.",
				1240, y, 24, look.text_muted);
			y += 30;
			paragraph(paint, "Off: every game is shown inside the desktop, which stays loaded. Use it if a game's screen stays black.",
				1240, y, 24, look.text_muted);
			y += 30;
			paragraph(paint, controlsNotice, 1240, y, 24, look.text);
			ps5ui::draw_hints(scene, kit.fonts, look,
				{ { hui::ui::Button::dpad, "Choose" }, { hui::ui::Button::cross, gameMode ? "Turn off" : "Turn on" },
				  { hui::ui::Button::circle, "Back" } }, 120, 1040);
		} else if (page == Page::settings && settingsList.focus() == 10) {
			paint.heading("120 Hz output", 1240, y, 38, look.text);
			y += 65;
			paragraph(paint, "On: games can run at up to 120 frames a second, where your TV supports 120 Hz. In a game's own "
				"display settings, choose 120 Hz too.", 1240, y, 24, look.text_muted);
			y += 30;
			paragraph(paint, "Off: the screen stays at 60 Hz. Use it if your TV shows no picture or flickers when a game "
				"starts. This is one setting for every game.", 1240, y, 24, look.text_muted);
			y += 30;
			paragraph(paint, controlsNotice, 1240, y, 24, look.text);
			ps5ui::draw_hints(scene, kit.fonts, look,
				{ { hui::ui::Button::dpad, "Choose" }, { hui::ui::Button::cross, highRefresh ? "Turn off" : "Turn on" },
				  { hui::ui::Button::circle, "Back" } }, 120, 1040);
		} else if (page == Page::settings && settingsList.focus() == 2) {
			// The Appropriate Legal Notices GPLv3 asks an interactive program to show (LICENSE, section 0); wording approved
			// by the owner on 2026-10-18.
			paint.heading("About " PS5_TITLE_NAME, 1240, y, 38, look.text);
			y += 65;
			paragraph(paint, PS5_TITLE_NAME ". Copyright (C) 2026 Mihawk and the authors credited in NOTICE.md.", 1240, y, 22,
				look.text_muted);
			y += 22;
			paragraph(paint, "This program is free software under the GNU General Public License, version 3 or later. It comes "
				"with ABSOLUTELY NO WARRANTY. You may share and change it under that licence's terms.", 1240, y, 22, look.text_muted);
			y += 22;
			paragraph(paint, "Licence texts: /data/homebrew/PPSA99170/licenses (this title) and " PROTON_DATA_ROOT
				"/runtime/ge11-7/share/licenses (the Windows runtime).", 1240, y, 22, look.text_muted);
			y += 22;
			paragraph(paint, "Source: github.com/mihawk-99/PS5_MihawkOS", 1240, y, 22, look.text_muted);
			y += 22;
			paragraph(paint, "Not affiliated with Sony, Microsoft or Valve.", 1240, y, 22, look.text_muted);
			ps5ui::draw_hints(scene, kit.fonts, look,
				{ { hui::ui::Button::dpad, "Choose" }, { hui::ui::Button::circle, "Back" } }, 120, 1040);
		} else if (page == Page::settings && settingsList.focus() == 3) {
			paint.heading("Runtime", 1240, y, 38, look.text);
			y += 65;
			paragraph(paint, "Windows programs run on a Wine runtime of about 1.2 GB. Install it, or repair it, from its pieces "
				"in " PROTON_DATA_ROOT "/install or in a mihawkos folder on a USB stick.", 1240, y, 24, look.text_muted);
			y += 30;
			const auto &done = install.progress;
			if (install.phase == ProtonInstall::checking || install.phase == ProtonInstall::running) {
				const float share = done.to_write ? std::min(1.0f, (float)done.written / (float)done.to_write) : 0.0f;
				scene.rounded_rect({ 1240, y, 520, 18 }, 9, look.outline);
				scene.rounded_rect({ 1240, y, std::max(18.0f, 520 * share), 18 }, 9, look.accent);
				y += 50;
				char line[160];
				snprintf(line, sizeof(line), "%.0f of %.0f MB, %.1f MB/s, piece %zu of %zu",
					done.written / 1e6, done.to_write / 1e6, install.seconds > 0 ? done.written / 1e6 / install.seconds : 0.0,
					done.chunks_done, done.chunks_needed);
				paragraph(paint, install.phase == ProtonInstall::checking ? "Checking what is installed already" : line,
					1240, y, 24, look.text);
				y += 30;
				paragraph(paint, "You can keep the console on this screen. Closing " PS5_TITLE_NAME " stops the install; "
					"it goes on from where it stopped next time.", 1240, y, 22, look.text_muted);
			} else {
				paragraph(paint, install.message.empty() ? "Press CROSS to look for the runtime's pieces." : install.message,
					1240, y, 24, install.phase == ProtonInstall::failed ? look.danger :
					install.phase == ProtonInstall::done ? look.success : look.text);
			}
			ps5ui::draw_hints(scene, kit.fonts, look,
				{ { hui::ui::Button::dpad, "Choose" }, { hui::ui::Button::cross, "Install" },
				  { hui::ui::Button::circle, "Back" } }, 120, 1040);
		} else if (page == Page::settings && settingsList.focus() == 5) {
			paint.heading("Music", 1240, y, 38, look.text);
			y += 65;
			paragraph(paint, "MihawkOS's own music, made for it: Bubble Garden, Glass Meadow and Sky Lounge, quietly looping under "
				"the menus and the guided setup. Press CROSS, or R1 in the library, for the next one or for silence.", 1240, y, 24,
				look.text_muted);
			y += 30;
			paragraph(paint, music.title() ? std::string("Playing: ") + music.title() : std::string("Off"), 1240, y, 24, look.text);
			ps5ui::draw_hints(scene, kit.fonts, look,
				{ { hui::ui::Button::dpad, "Choose" }, { hui::ui::Button::cross, "Next" },
				  { hui::ui::Button::circle, "Back" } }, 120, 1040);
		} else if (page == Page::settings && settingsList.focus() == 4) {
			paint.heading("Guided setup", 1240, y, 38, look.text);
			y += 65;
			paragraph(paint, "Run the first-launch tour again: your language, installing the Windows runtime, and the first start "
				"of Mihawk OS. What is installed already is kept.", 1240, y, 24, look.text_muted);
			ps5ui::draw_hints(scene, kit.fonts, look,
				{ { hui::ui::Button::dpad, "Choose" }, { hui::ui::Button::cross, "Start" },
				  { hui::ui::Button::circle, "Back" } }, 120, 1040);
		} else if (page == Page::settings && settingsList.focus() == 6) {
			paint.heading("Updates", 1240, y, 38, look.text);
			y += 65;
			paragraph(paint, PS5_TITLE_NAME " looks for a newer release only when you ask. A release is installed only when it is "
				"signed with " PS5_TITLE_NAME "'s key, and it is switched in the next time " PS5_TITLE_NAME " starts.",
				1240, y, 24, look.text_muted);
			y += 30;
			const auto &done = updates.progress;
			if (updates.phase == ProtonUpdate::downloading) {
				const float share = done.to_write ? std::min(1.0f, (float)done.written / (float)done.to_write) : 0.0f;
				scene.rounded_rect({ 1240, y, 520, 18 }, 9, look.outline);
				scene.rounded_rect({ 1240, y, std::max(18.0f, 520 * share), 18 }, 9, look.accent);
				y += 50;
				char line[160];
				snprintf(line, sizeof(line), "%.0f of %.0f MB, piece %zu of %zu", done.written / 1e6, done.to_write / 1e6,
					done.chunks_done, done.chunks_needed);
				paragraph(paint, line, 1240, y, 24, look.text);
			} else {
				paragraph(paint, updates.message.empty() ? "Press CROSS to check for a newer release." : updates.message, 1240,
					y, 24, updates.phase == ProtonUpdate::failed ? look.danger :
					updates.phase == ProtonUpdate::ready ? look.success : look.text);
			}
			ps5ui::draw_hints(scene, kit.fonts, look,
				{ { hui::ui::Button::dpad, "Choose" },
				  { hui::ui::Button::cross, updates.phase == ProtonUpdate::available ? "Download" :
					updates.phase == ProtonUpdate::ready ? "Restart" : "Check" },
				  { hui::ui::Button::circle, "Back" } }, 120, 1040);
		} else if (page == Page::settings && settingsList.focus() == 7) {
			paint.heading("Logging", 1240, y, 38, look.text);
			y += 65;
			paragraph(paint, "On: every session writes all of " PS5_TITLE_NAME "'s logs, in more detail, to its own folder in "
				PW_DETAILED_LOGS_ROOT ". To report a problem, zip that session's folder and post it; its README.txt says what to "
				"check first.", 1240, y, 24, look.text_muted);
			y += 30;
			paragraph(paint, "Sessions write more while it is on: turn it off when you are done. The 5 newest folders are kept, "
				"at most 96 MB each.", 1240, y, 24, look.text_muted);
			y += 30;
			paragraph(paint, controlsNotice, 1240, y, 24, look.text);
			ps5ui::draw_hints(scene, kit.fonts, look,
				{ { hui::ui::Button::dpad, "Choose" }, { hui::ui::Button::cross, detailedLogs ? "Turn off" : "Turn on" },
				  { hui::ui::Button::circle, "Back" } }, 120, 1040);
		} else if (page == Page::settings && settingsList.focus() == 9) {
			const std::string url = proton_webui_url();
			paint.heading("Web UI", 1240, y, 38, look.text);
			y += 50;
			if (url.empty()) {
				paragraph(paint, "Connect the PS5 to your network: the Web UI then opens here, with its address and a code to scan.",
					1240, y, 24, look.text_muted);
			} else {
				drawQr(url, 1240, y, 250);
				y += 296;
				paint.heading(url, 1240, y, 30, look.text);
				y += 44;
				paragraph(paint, "Scan the code with your phone, or type the address in a browser on your home network. It walks "
					"you through adding a game from your computer, gives each game its picture and its own settings, puts games "
					"on the PS5 home screen, backs up your saves and changes these settings.", 1240, y, 22, look.text_muted);
			}
			ps5ui::draw_hints(scene, kit.fonts, look,
				{ { hui::ui::Button::dpad, "Choose" }, { hui::ui::Button::circle, "Back" } }, 120, 1040);
		} else if (page == Page::settings && settingsList.focus() == 8) {
			paint.heading("Controller", 1240, y, 38, look.text);
			y += 65;
			paragraph(paint, "Which controller Windows games and Steam see. Your DualSense is shown to them as one controller, "
				"so menus show one set of buttons.", 1240, y, 24, look.text_muted);
			y += 30;
			paragraph(paint, "Xbox 360 or Xbox One: every game that supports a controller sees it. Steam shows Xbox buttons.",
				1240, y, 24, look.text_muted);
			y += 30;
			paragraph(paint, "DualSense: Steam and games made for it show PlayStation buttons. Games that only know Xbox "
				"controllers see none.", 1240, y, 24, look.text_muted);
			y += 30;
			paragraph(paint, controlsNotice, 1240, y, 24, look.text);
			ps5ui::draw_hints(scene, kit.fonts, look,
				{ { hui::ui::Button::dpad, "Choose" }, { hui::ui::Button::cross, "Change" },
				  { hui::ui::Button::circle, "Back" } }, 120, 1040);
		} else if (configuring) {
			paint.heading("Mouse controls", 1240, y, 38, look.text);
			y += 65;
			paragraph(paint, "Move the pointer with the touchpad. Choose a controller button for each mouse click.",
				1240, y, 24, look.text_muted);
			y += 30;
			paragraph(paint, "Assigned buttons send mouse clicks instead of gamepad presses. Each button can have one assignment.",
				1240, y, 24, look.text_muted);
			y += 30;
			paragraph(paint, "Choices are saved for all games. Restore defaults to use the touchpad for left click.",
				1240, y, 24, look.text_muted);
			y += 30;
			paragraph(paint, controlsNotice, 1240, y, 24, look.text);
			ps5ui::draw_hints(scene, kit.fonts, look,
				{ { hui::ui::Button::dpad, "Choose" }, { hui::ui::Button::cross, "Select" },
				  { hui::ui::Button::circle, "Back" } }, 120, 1040);
		} else if (page == Page::aurelia && aureliaAccounts) {
			scene.push_clip({ 1240, 250, 520, 700 });
			if (aureliaList.focus() == 0) {
				paint.heading("Library account", 1240, y, 34, look.text);
				y += 55;
				paragraph(paint, "What lists your games and installs them: no browser, so it costs little memory. Sign in with the "
					"Steam app on your phone: scan the code on the screen.", 1240, y, 22, look.text_muted);
				y += 150;
				paragraph(paint, aureliaAccount.empty() ? "Not signed in." : "Signed in as " + aureliaAccount + ".", 1240, y, 22, look.text);
			} else {
				paint.heading("Steam client", 1240, y, 34, look.text);
				y += 45;
				// What went wrong first: below the long paragraphs it fell outside the panel.
				if (!clientNote.empty()) paragraph(paint, clientNote, 1240, y, 22, look.danger);
				else if (store.phase == ProtonSteam::failed) paragraph(paint, store.message, 1240, y, 22, look.danger);
				for (const std::string &line : proton_steam_panel(store)) {
					paragraph(paint, line, 1240, y, 22, look.text_muted);
					y += 14;
				}
			}
			scene.pop_clip();
			float noteY = 880;
			if (!aureliaNote.empty() && aureliaList.focus() == 0) paragraph(paint, aureliaNote, 1240, noteY, 22, look.text);
			ps5ui::draw_hints(scene, kit.fonts, look,
				{ { hui::ui::Button::dpad, "Choose" }, { hui::ui::Button::cross, "Select" },
				  { hui::ui::Button::circle, "Back" } }, 120, 1040);
		} else if (page == Page::aurelia) {
			if (!aureliaEntries.empty()) {
				const auto &entry = aureliaEntries[size_t(aureliaFocus)];
				paint.heading(entry.name, 1240, y, 34, look.text);
				y += 55;
				char line[128];
				snprintf(line, sizeof(line), "App %u  \u00b7  %u min played%s", entry.app_id, entry.minutes,
					entry.update ? "  \u00b7  update" : "");
				paragraph(paint, line, 1240, y, 24, look.text_muted);
				y += 32;
				const bool installed = aureliaInstalled(entry) != nullptr;
				paragraph(paint, installed ? "Installed: CROSS plays it." : "Not installed yet.", 1240, y, 24,
					installed ? look.success : look.text_muted);
				y += 32;
			}
			if (!aureliaNote.empty()) paragraph(paint, aureliaNote, 1240, y, 22, look.text);
			y += 60;
			paragraph(paint, "Press UP for your accounts: the library account and the Steam client.", 1240, y, 22, look.text_muted);
			ps5ui::draw_hints(scene, kit.fonts, look,
				{ { hui::ui::Button::dpad, "Browse" }, { hui::ui::Button::cross, "Play" },
				  { hui::ui::Button::square, "Sign in" }, { hui::ui::Button::triangle, "Refresh" },
				  { hui::ui::Button::circle, "Back" } }, 120, 1040);
		} else {
			scene.push_clip({ 1240, 250, 520, 500 });
			// The library always holds Mihawk OS (proton_read_games), so a reason a game cannot start is its complaint.
			const auto &item = library.items()[library.focus()];
			// The entry's art from the WebUI, across the panel's top, the words below it
			if (item.tag >= 0) {
				if (const uint32_t cover = coverTexture(games[item.tag].id)) {
					scene.image(cover, { 1240, 252, 520, 243 }, hui::gfx::kFullUv, hui::gfx::Color::rgb(0xffffff), 12);
					y = 545;
				}
			}
			paragraph(paint, item.title, 1240, y, 34, look.text);
			y += 30;
			if (item.tag < 0) {
				if (!clientNote.empty()) paragraph(paint, clientNote, 1240, y, 22, look.danger);
				else if (store.phase == ProtonSteam::failed) paragraph(paint, store.message, 1240, y, 22, look.danger);
				for (const std::string &line : proton_steam_panel(store)) {
					paragraph(paint, line, 1240, y, 22, look.text_muted);
					y += 14;
				}
			} else {
				paragraph(paint, item.subtitle, 1240, y, 24, look.text_muted);
				for (const std::string *note : { &games[item.tag].complaint, &sessionNote, &noticesNote }) {
					if (note->empty()) continue;
					y += 30;
					paragraph(paint, *note, 1240, y, 22, look.danger);
				}
				if (!payloadsNote.empty()) {
					y += 30;
					paragraph(paint, payloadsNote, 1240, y, 22, look.text_muted);
				}
			}
			scene.pop_clip();
			drawWebuiBlock(paint, look);
			ps5ui::draw_hints(scene, kit.fonts, look,
				{ { hui::ui::Button::dpad, "Choose" }, { hui::ui::Button::cross, "Select" },
				  { hui::ui::Button::square, "Themes" }, { hui::ui::Button::triangle, "Settings" },
				  { hui::ui::Button::r1, "Music" },
				  { hui::ui::Button::options, "Hold to close" } }, 120, 1040);
		}
		drawMusic(look);
		leaving.draw(scene, kit.fonts, look);
		auto backdrop = look.backdrop;
		backdrop.time = clock;
		kit.renderer.begin();
		kit.renderer.backdrop(backdrop);
		kit.renderer.glass();
		kit.renderer.draw(scene);
	}

	void selftestStep()
	{
		if (ps5.frameBudget || !proton_selftest_active()) return;
		const auto &launch = proton_launch();
		proton_selftest_note(launch.game ? "game" : "launcher", launch.cycle);
		if (launch.game) {
			proton_queue_restart(nullptr, launch.cycle + 1, "selftest");
			quit = true;
		} else if (launch.cycle < 2 && !games.empty()) {
			proton_queue_restart(&games[0], launch.cycle, "selftest");
			quit = true;
		} else {
			proton_say("selftest: done after %u cycle(s)", launch.cycle);
			remove(proton_title_path("/app0/restart-check").c_str());
		}
	}

public:
	VulkanExample()
	{
		title = PS5_TITLE_NAME;
		name = "ps5proton";
		settings.overlay = false;
		ps5.ownOverlay = true;
		dataAvailable = proton_data_grant(); // Cached startup result; never retries after threads start.
		// The checklist shows whenever it fails; prepare() adds a passing one at a start from the home screen
		payloadShown = !proton_payloads_check().pass;
		// A one-shot test hook, like aurelia-open: with /data/ps5proton/payload-check-show present the checklist shows even
		// when it passes, and its third frame is saved as payload-check.ppm beside the flag, for a picture of the screen.
		if (dataAvailable && remove(PROTON_DATA_ROOT "/payload-check-show") == 0) {
			payloadShown = true;
			payloadPicture = 3;
			proton_say("launcher: the payload-check-show flag shows the checklist");
		}
		loadGames();
		// A failed session's reason (ended=), or a session that never came back to say one (its marker is still there).
		const std::string marker = proton_title_path(PROTON_SESSION_MARKER);
		if (FILE *file = fopen(marker.c_str(), "rb")) {
			char name[128] = {};
			const size_t got = fread(name, 1, sizeof(name) - 1, file);
			fclose(file);
			std::string game(name, got);
			game.erase(std::remove(game.begin(), game.end(), '\n'), game.end());
			sessionNote = "The last session" + (game.empty() ? std::string() : " (" + game + ")") +
				" did not end normally: it crashed, froze or lost power. Its log: " PROTON_DATA_ROOT "/logs/title-last.log";
			remove(marker.c_str());
		}
		if (!proton_launch().ended.empty()) sessionNote = "Last session ended: " + proton_launch().ended;
		if (!sessionNote.empty()) proton_say("launcher: %s", sessionNote.c_str());
		// The memory plan's startup tip (docs/PLAN.md, Part 4): on the launcher's own screen, never over a game.
		if (FILE *file = fopen(PROTON_PAYLOADS_FILE, "r")) {
			unsigned long long mib = 0;
			if (fscanf(file, "%llu", &mib) == 1 && mib >= 64)
				payloadsNote = "Payloads are using about " + std::to_string(mib) + " MiB; closing them frees room for games.";
			fclose(file);
			if (!payloadsNote.empty()) proton_say("launcher: %s", payloadsNote.c_str());
		}
		const std::string notices = proton_title_path(PROTON_SESSION_NOTICES);
		if (FILE *file = fopen(notices.c_str(), "rb")) {
			char line[512];
			while (fgets(line, sizeof(line), file)) {
				std::string text(line);
				text.erase(std::remove(text.begin(), text.end(), '\n'), text.end());
				if (!text.empty()) noticesNote += (noticesNote.empty() ? "During the last session: " : " ") + text;
			}
			fclose(file);
			remove(notices.c_str());
			if (!noticesNote.empty()) proton_say("launcher: %s", noticesNote.c_str());
		}
	}

	// The library: the catalog, filtered as the launcher shows it, each entry's complaint. Called again when the WebUI
	// changes an entry.
	void loadGames()
	{
		games = proton_read_games(&problem);
		allGames = games;   // the Aurelia page plays and installs from the whole catalog, not the library's filtered list
		proton_say("launcher: %zu game(s); %s", games.size(), problem.c_str());
		// The library lists every game (the owner, 2026-10-26: a game added in the WebUI belongs in it), but an entry whose
		// game.json says "launcher": false: that one starts from the desktop's shortcut, a tile or a profile only.
		// A file named library-show-all in the data folder lists those too.
		struct stat showAll;
		if (!(dataAvailable && stat(PROTON_DATA_ROOT "/library-show-all", &showAll) == 0)) {
			const size_t installed = games.size();
			games.erase(std::remove_if(games.begin(), games.end(), [](const ProtonGame &game) {
				return game.id != "mihawk-os" && !game.launcher; }), games.end());
			proton_say("launcher: library shows %zu of %zu (every entry but those with \"launcher\": false; "
				"%s/library-show-all shows them all)", games.size(), installed, PROTON_DATA_ROOT);
		}
		// Checked here, before LoadExec: a game that cannot start is shown why instead of being started.
		const std::string blocked = dataAvailable ? proton_runtime_problem() : proton_data_problem();
		runtimeBlocked = blocked;
		for (auto &game : games)
			if (game.complaint.empty()) game.complaint = blocked;
		for (auto &game : allGames)
			if (game.complaint.empty()) game.complaint = blocked;
		if (!blocked.empty()) proton_say("launcher: games cannot start: %s", blocked.c_str());
	}

	void prepare() override
	{
		VulkanExampleBase::prepare();
		// The guided setup: on the first launch, or with the runtime or the mihawk prefix missing (src/pw_setup.h). A
		// test run shows it only when asked: by the first-run marker, or on the PC by
		// PROTON_SETUP_TEST=<step>[:<fake install's scenario>][@<share held>], which shows a step for a picture.
		const char *setupTest = getenv("PROTON_SETUP_TEST");
		setupUnattended = proton_setup_unattended(ps5.frameBudget != 0);
		if (setupUnattended) proton_say("setup: unattended (scripted presses, real engine)");
		setupFacts = SetupTour::facts(dataAvailable || setupTest, false, &setupSimulated);
		setupStep = setupTest ? (PwSetupStep)atoi(setupTest) : pw_setup_start(&setupFacts);
		if (ps5.frameBudget && !setupTest && !setupSimulated && !setupUnattended) setupStep = PW_SETUP_NONE;
		if (setupStep != PW_SETUP_NONE) unlink(SetupTour::preparingMarker);
		prepareKit({ .music = false, .covers = false });
		music.init(kit.mixer, getAssetPath() + "music", ps5.frameBudget != 0);
		music.play(music.remembered(), false, 2.5f);
		selectedTheme = ps5ui::active_theme_index();
		{
			std::string why;
			std::vector<PwWebuiChoice> looks, pieces;
			for (const auto &look : hui::ui::themes()) looks.push_back({ look.id, look.name });
			for (int i = 0; i < Jukebox::kPieces; ++i) pieces.push_back({ Jukebox::kIds[i], Jukebox::kTitles[i] });
			if (proton_webui_start(&why, std::move(looks), std::move(pieces))) proton_say("webui: listening on port 6767");   // it refuses without /data
			else proton_say("webui: not started: %s", why.c_str());
		}
		syncLibraryRows(true);
		std::vector<hui::ui::ListItem> rows;
		for (const auto &look : hui::ui::themes()) {
			hui::ui::ListItem item;
			item.title = look.name;
			item.subtitle = look.family;
			rows.push_back(item);
		}
		themes.set_items(rows);
		rows.clear();
		hui::ui::ListItem item;
		item.title = "Controls";
		item.subtitle = "Mouse button assignments";
		rows.push_back(item);
		item = {};
		item.title = "Game Mode";
		rows.push_back(item);
		item = {};
		item.title = "About " PS5_TITLE_NAME;
		item.subtitle = "Licence, warranty and source";
		rows.push_back(item);
		item = {};
		item.title = "Install runtime";
		rows.push_back(item);
		item = {};
		item.title = "Guided setup";
		item.subtitle = "Language, install and the first start, step by step";
		rows.push_back(item);
		item = {};
		item.title = "Music";
		rows.push_back(item);
		item = {};
		item.title = "Check for updates";
		rows.push_back(item);
		item = {};
		item.title = "Logging";
		rows.push_back(item);
		item = {};
		item.title = "Controller";
		rows.push_back(item);
		item = {};
		item.title = "Web UI";
		item.subtitle = "Your library and settings in a browser";
		rows.push_back(item);
		item = {};
		item.title = "120 Hz output";
		rows.push_back(item);
		settingsList.set_items(rows);
		rows.clear();
		for (const char *label : {"Left mouse click", "Right mouse click", "Middle mouse click", "Restore defaults"}) {
			item = {};
			item.title = label;
			rows.push_back(item);
		}
		controlsList.set_items(rows);
		rows.clear();
		for (unsigned i = 0; i < PW_CONTROLS_BUTTONS; ++i) {
			item = {};
			item.title = pw_controls_button_label(i);
			rows.push_back(item);
		}
		buttonList.set_items(rows);
		if (ps5.frameBudget) pw_controls_defaults(&controls);
		else pw_controls_load(&controls, proton_controls_path().c_str());
		syncControls();
		if (!ps5.frameBudget) pw_game_mode_load(&gameMode, proton_game_mode_path().c_str());
		syncGameMode();
		if (!ps5.frameBudget) pw_high_refresh_load(&highRefresh, proton_high_refresh_path().c_str());
		syncHighRefresh();
		if (!ps5.frameBudget) pw_detailed_logs_load(&detailedLogs, proton_logging_path().c_str());
		syncLogging();
		if (!ps5.frameBudget) pw_controller_load(&controller, proton_controller_path().c_str());
		syncController();
		syncInstall();
		syncMusic();
		syncUpdates();
		for (auto *list : { &library, &aureliaList, &themes, &settingsList, &controlsList, &buttonList }) {
			list->style.panel = true;
			list->style.row_height = 84;
			list->style.title_size = 28;
			list->style.subtitle_size = 20;
			list->set_bounds({ 120, 230, 1020, 740 });
			list->enter();
		}
		syncThemes();
		proton_say("launcher UI: %zu themes, active=%s", themes.items().size(), theme().id);
		// A passing checklist shows at every start from the home screen (cycle 0); a return from a game and a test run's
		// launcher skip it. The frame budget is known here, not in the constructor: a scripted launcher case (controls-menu,
		// setup-tour, steam-guide-pages) met a passing page there and failed (alpha5-base-2026-10-29-02).
		if (proton_launch().cycle == 0 && !ps5.frameBudget) payloadShown = true;
		if (payloadShown) startPayloadCheck();
		else if (setupStep != PW_SETUP_NONE) startSetup(setupStep);
		else if (getenv("PROTON_STEAM_GUIDE_TEST")) steamGuideTest(getenv("PROTON_STEAM_GUIDE_TEST"));
		else if (ps5.frameBudget && startShowcase()) {}
		else if (dataAvailable && !ps5.frameBudget) resumeSteamGuide();
		prepared = true;
	}

	// The tour and its sky (the setup and the Steam guide draw over the same one)
	void makeTour()
	{
		tour = std::make_unique<SetupTour>();
		const VkPipelineShaderStageCreateInfo stages[2] = {
			loadShader(getShadersPath() + "ps5proton/aero.vert.spv", VK_SHADER_STAGE_VERTEX_BIT),
			loadShader(getShadersPath() + "ps5proton/aero.frag.spv", VK_SHADER_STAGE_FRAGMENT_BIT) };
		if (!tour->sky.init(device, physicalDevice, vulkanDevice->queueFamilyIndices.graphics, renderPass, pipelineCache, stages,
				maxConcurrentFrames))
			proton_say("setup: the sky's pipeline failed; the theme's backdrop stands in");
	}

	void closeTour()
	{
		vkDeviceWaitIdle(device);
		tour->sky.release();
		tour.reset();
		page = Page::library;
	}

	// ---- The payload checklist (src/pw_payload_check.h): a page in the tour's look ----
	void startPayloadCheck()
	{
		if (proton_payloads().pass) proton_say("launcher: the payload checklist shows a pass");
		else proton_say("launcher: the payload checklist refuses entry: %s", proton_payloads_problem().c_str());
		makeTour();
		tour->beginPayloads(kit, getAssetPath(), proton_payloads(), proton_payload_facts(), proton_payloads_problem());
	}

	// Check again shows the new report (a pass then counts down visibly); Continue, or a pass's countdown, closes the page
	// into the guided setup when it is due, else the launcher
	void payloadCheckOutcome()
	{
		if (tour->outcome == SetupTour::Outcome::payload_recheck) {
			proton_payloads_check();
			tour->payloads(proton_payloads(), proton_payload_facts(), proton_payloads_problem(), true);
			return;
		}
		if (tour->outcome == SetupTour::Outcome::payload_close) {
			proton_say("launcher: closed from the payload checklist");
			quit = true;
			return;
		}
		if (tour->outcome != SetupTour::Outcome::payload_continue) return;
		proton_say("launcher: the payload checklist passes");
		payloadShown = false;
		closeTour();
		if (setupStep != PW_SETUP_NONE) startSetup(setupStep);
	}

	// ---- The Steam guide (src/pw_steam_guide.h): its launcher pages are the tour's Steam mode ----
	void startSteamGuide(PwSteamPage page, PwSteamFailure failure, bool starting = false)
	{
		makeTour();
		tour->beginSteam(kit, getAssetPath(), page, failure, ps5.frameBudget != 0 && !clientTest, starting);
		steamPoll = 0.0f;
	}

	// The launcher starts where the guide's last session left it: the sign-in session after the install, the done page
	// after a sign-in, a failed page with the reason
	void resumeSteamGuide()
	{
		PwSteamGuideRecord record;
		if (pw_steam_guide_record_load(&record, PW_STEAM_GUIDE_RECORD) != 0) return;
		const ProtonSteam now = proton_steam_state();
		PwSteamFailure failure;
		const PwSteamResume resume = pw_steam_guide_resume(&record, now.installed, now.signed_in, &failure);
		proton_say("steam guide: back from its %s session (%s): resume %d, failure %d",
			record.stage == PW_STEAM_STAGE_INSTALL ? "install" : "sign-in", pw_steam_guide_result_word(record.result), (int)resume,
			(int)failure);
		switch (resume) {
		case PW_STEAM_RESUME_NOTHING: unlink(PW_STEAM_GUIDE_RECORD); break;
		case PW_STEAM_RESUME_SIGNIN:
			startSteamGuide(PW_STEAM_PAGE_DOWNLOAD, PW_STEAM_FAIL_NONE, true);
			tour->outcome = SetupTour::Outcome::steam_signin;
			break;
		case PW_STEAM_RESUME_DONE: startSteamGuide(PW_STEAM_PAGE_DONE, PW_STEAM_FAIL_NONE); break;
		case PW_STEAM_RESUME_FAILED: startSteamGuide(PW_STEAM_PAGE_FAILED, failure); break;
		}
	}

	// A test's pictures of the guide's pages on the console (tools/run-launcher.py --steam-guide-pictures): the hook
	// /data/ps5proton/steam-guide-showcase names a language ("1": all ten); every page in it is held 24 frames and the
	// 20th kept as /data/ps5proton/steam-guide-pictures/<page>-<language>.ppm.
	std::vector<std::pair<std::string, std::string>> showcase;   // (page as steamGuideTest takes it, language)
	size_t showcaseItem{ 0 };
	uint32_t showcaseFrame{ 0 };
	bool startShowcase()
	{
		char word[32] = {};
		FILE *hook = fopen(PROTON_DATA_ROOT "/steam-guide-showcase", "r");
		if (!hook) return false;
		if (!fgets(word, sizeof(word), hook)) word[0] = 0;
		fclose(hook);
		unlink(PROTON_DATA_ROOT "/steam-guide-showcase");
		word[strcspn(word, "\r\n")] = 0;
		size_t count = 0;
		const PwSetupLanguage *all = pw_setup_languages(&count);
		for (size_t i = 0; i < count; ++i)
			if (all[i].text && (!strcmp(word, "1") || !strcmp(word, all[i].tag)))
				for (const char *page : { "intro", "download@0.45", "starting", "failed:offline", "done#Mihawk" })
					showcase.push_back({ page, all[i].tag });
		mkdir(PROTON_DATA_ROOT "/steam-guide-pictures", 0777);
		proton_say("steam guide showcase: %zu pages", showcase.size());
		if (showcase.empty()) return false;
		steamGuideTest(showcase[0].first, showcase[0].second.c_str());
		return true;
	}
	void showcaseStep()
	{
		if (showcase.empty() || showcaseItem >= showcase.size()) return;
		if (++showcaseFrame == 20) {
			std::string name = showcase[showcaseItem].first.substr(0, showcase[showcaseItem].first.find_first_of(":@#"));
			ps5.capturePath = std::string(PROTON_DATA_ROOT "/steam-guide-pictures/") + name + "-" + showcase[showcaseItem].second +
				".ppm";
			proton_say("steam guide showcase: picture %zu is %s in %s", showcaseItem + 1, name.c_str(), showcase[showcaseItem].second.c_str());
		} else if (showcaseFrame >= 24 && showcaseItem + 1 < showcase.size()) {
			++showcaseItem;
			showcaseFrame = 0;
			closeTour();
			steamGuideTest(showcase[showcaseItem].first, showcase[showcaseItem].second.c_str());
		}
	}

	// PROTON_STEAM_GUIDE_TEST=<page>[:<failure>][@<share downloaded>][#<display name>]: a page held for a picture on
	// the PC (pages intro, download, starting, failed, done; failures as pw_steam_guide.h names them, lower case)
	void steamGuideTest(const std::string &test, const char *language = getenv("PROTON_SETUP_LANGUAGE"))
	{
		static const char *const pages[] = { "intro", "download", "failed", "done" };
		static const char *const failures[] = { "none", "offline", "clock", "download", "install", "stalled", "cancelled",
			"timeout", "guard", "ended" };
		const std::string name = test.substr(0, test.find_first_of(":@#"));
		PwSteamPage page = PW_STEAM_PAGE_INTRO;
		for (int i = 0; i < 4; ++i) if (name == pages[i]) page = (PwSteamPage)i;
		if (name == "starting") page = PW_STEAM_PAGE_DOWNLOAD;
		PwSteamFailure failure = PW_STEAM_FAIL_NONE;
		if (const size_t colon = test.find(':'); colon != std::string::npos) {
			const std::string word = test.substr(colon + 1, test.find_first_of("@#", colon) - colon - 1);
			for (int i = 0; i < 10; ++i) if (word == failures[i]) failure = (PwSteamFailure)i;
		}
		startSteamGuide(page, failure, name == "starting");
		const size_t at = test.find('@'), hash = test.find('#');
		tour->holdSteam(at == std::string::npos ? 0.0f : (float)atof(test.c_str() + at + 1),
			hash == std::string::npos ? nullptr : test.c_str() + hash + 1);
		if (language) tour->preview_language(language);
	}

	// The guide's facts, a few times a second (the client's files are read each time: proton_steam_state)
	void pollSteamGuide(float dt)
	{
		if ((steamPoll -= dt) > 0.0f) return;
		steamPoll = 0.25f;
		const ProtonSteam now = proton_steam_state();
		SetupTour::SteamFacts facts;
		facts.downloading = now.phase == ProtonSteam::downloading;
		facts.downloaded = now.phase == ProtonSteam::downloaded;
		facts.failed = now.phase == ProtonSteam::failed;
		facts.installed = now.installed;
		facts.signed_in = now.signed_in;
		facts.bytes = now.bytes;
		facts.fetch_status = now.fetch_status;
		facts.persona = now.persona;
		tour->steam(facts);
	}

	// What the guide asked for: the download, a session (its record written first, so the launcher knows on its return
	// where it was), Big Picture, or back to the library
	void steamGuideOutcome()
	{
		const auto outcome = tour->outcome;
		const bool live = !ps5.frameBudget || clientTest;
		if (outcome == SetupTour::Outcome::none) return;
		if (outcome == SetupTour::Outcome::steam_download) {
			tour->outcome = SetupTour::Outcome::none;
			if (live) proton_steam_download();
			return;
		}
		if (outcome == SetupTour::Outcome::library) {
			if (live) unlink(PW_STEAM_GUIDE_RECORD);
			closeTour();
			syncAccounts();
			return;
		}
		if (!live) return;   // a test's picture holds its page
		ProtonGame game;
		std::string problem;
		PwSteamGuideRecord record{};
		bool ok = false;
		const char *why = "steam", *said = "launcher: Steam in Big Picture";   // the words tools/run-steam-option.py follows
		if (outcome == SetupTour::Outcome::steam_install) {
			record.stage = PW_STEAM_STAGE_INSTALL;
			ok = proton_steam_game(&game, true, &problem);
			why = "install";
			said = "launcher: installing from the downloaded installer";
		} else if (outcome == SetupTour::Outcome::steam_signin) {
			record.stage = PW_STEAM_STAGE_SIGNIN;
			ok = proton_steam_game(&game, false, &problem);
			why = "sign-in";
			said = "launcher: Steam sign-in session";
		} else {
			unlink(PW_STEAM_GUIDE_RECORD);
			ok = proton_steam_game(&game, false, &problem, true);
		}
		if (ok && !runtimeBlocked.empty()) ok = false, problem = "sessions cannot start: " + runtimeBlocked;
		if (!ok) {
			proton_say("steam guide: the session cannot start: %s", problem.c_str());
			tour->failSteam(outcome == SetupTour::Outcome::steam_install ? PW_STEAM_FAIL_INSTALL : PW_STEAM_FAIL_ENDED);
			return;
		}
		if (record.stage) {
			const int error = pw_steam_guide_record_save(&record, PW_STEAM_GUIDE_RECORD);
			if (error) proton_say("steam guide: record not saved (%d)", error);
		}
		proton_say("%s", said);
		proton_queue_restart(&game, proton_launch().cycle, why);
		quit = true;
	}

	void startSetup(PwSetupStep step)
	{
		makeTour();
		// The installer's engine (proton_install.h); a test (PROTON_SETUP_TEST on the PC) or the first-run marker plays the
		// install with the fake one, so nothing of the console's runtime is written.
		const char *test = getenv("PROTON_SETUP_TEST");
		std::string scenario = test && strchr(test, ':') ? strchr(test, ':') + 1 : "ok";
		scenario = scenario.substr(0, scenario.find('@'));
		std::unique_ptr<SetupEngine> engine;
		if (test || setupSimulated) engine = std::make_unique<FakeSetupEngine>(scenario);
		else engine = std::make_unique<InstallSetupEngine>();
		tour->begin(kit, getAssetPath(), setupFacts, step, setupSimulated, ps5.frameBudget != 0 && !setupUnattended,
			std::move(engine));
		if (test && step == PW_SETUP_INSTALLING) tour->fake()->find(), tour->fake()->start();
		if (test && strstr(test, "@")) tour->fake()->hold(atof(strstr(test, "@") + 1));
		if (const char *language = getenv("PROTON_SETUP_LANGUAGE"); test && language) tour->preview_language(language);
		if (test && step == PW_SETUP_PREPARING && scenario == "cover") tour->cover();
	}

	void renderSetup()
	{
		showcaseStep();   // may put the next page's tour in place
		static const int gloss = [] {
			const auto all = hui::ui::themes();
			for (size_t i = 0; i < all.size(); ++i) if (!strcmp(all[i].id, "gloss")) return (int)i;
			return 0;
		}();
		const auto &look = hui::ui::themes()[gloss];
		const hui::InputFrame input = ps5.frameBudget ? kit.scripted("ps5proton", ps5.framesDrawn) : kit.input();
		if (tour->steamMode() && (!ps5.frameBudget || clientTest)) pollSteamGuide(std::min(frameTimer, 0.05f));
		tour->update(std::min(frameTimer, 0.05f), input, ps5.frameBudget ? nullptr : &ps5_pad());
		music.pump(std::min(frameTimer, 0.05f));
		scene.clear();
		tour->compose(scene, look);
		kit.renderer.begin();
		if (!tour->sky.ready()) {
			auto backdrop = look.backdrop;
			backdrop.time = tour->clock();
			kit.renderer.backdrop(backdrop);
		}
		kit.renderer.draw(scene);
		buildKitCommandBuffer([&](VkCommandBuffer cmd) { tour->sky.draw(cmd, width, height, tour->clock(), tour->calm()); },
			[&](VkCommandBuffer cmd) { tour->sky.reset(cmd, currentBuffer); });
		// Start's frame shows the first session's cover: kept for the title to show while the prefix is made (src/pw_setup.h).
		if (tour->outcome == SetupTour::Outcome::prepare && (!ps5.frameBudget || setupUnattended))
			ps5.capturePath = PROTON_DATA_ROOT "/" PW_SETUP_COVER_NAME;
		if (payloadPicture >= 0 && payloadPicture-- == 0) ps5.capturePath = PROTON_DATA_ROOT "/payload-check.ppm";
		submitFrame();
		if (tour->payloadMode()) {
			payloadCheckOutcome();
			return;
		}
		if (tour->steamMode()) {
			steamGuideOutcome();
			return;
		}
		const auto outcome = tour->outcome;
		if (outcome == SetupTour::Outcome::none) return;
		const auto mihawk = std::find_if(games.begin(), games.end(), [](const ProtonGame &game) { return game.id == "mihawk-os"; });
		if (outcome != SetupTour::Outcome::library && (!ps5.frameBudget || setupUnattended) && mihawk != games.end()) {
			proton_queue_restart(&*mihawk, proton_launch().cycle, outcome == SetupTour::Outcome::prepare ? "setup-prepare" : "setup");
			quit = true;
			return;
		}
		closeTour();
	}

	void render() override
	{
		if (!prepared) return;
		prepareFrame();
		if (tour) {
			renderSetup();
			return;
		}
		update(std::min(frameTimer, 0.05f));
		compose();
		buildKitCommandBuffer();
		submitFrame();
		selftestStep();
	}

	~VulkanExample() override
	{
		if (device) vkDeviceWaitIdle(device);
		if (tour) tour->sky.release();
		// The audio thread reads the music's rings: it stops before they go (KitExample's own release comes after
		// this class's members are destroyed)
		kit.release();
	}
};
} // namespace
VULKAN_EXAMPLE_MAIN()
