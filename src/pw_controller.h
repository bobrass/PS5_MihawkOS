/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PW_CONTROLLER_H
#define PW_CONTROLLER_H
/* The controller Windows programs see (MihawkOS's Settings, saved beside controls.txt as controller.txt).
 *
 * The DualSense used to reach programs twice: as XInput's controller 0, a wired Xbox 360 controller (Wine patch 0470),
 * and as a DualSense HID device (patch 0870). The owner saw Steam take both (2026-10-26): its button glyphs swapped
 * between PlayStation and Xbox in a menu. So programs now see one, the player's choice:
 * - PW_CONTROLLER_XBOX_360 (the default: what XInput reported to every game before): XInput's 045e:028e, no HID device;
 * - PW_CONTROLLER_XBOX_ONE: XInput's 045e:02ea (an Xbox One S controller), no HID device;
 * - PW_CONTROLLER_DUALSENSE: the DualSense HID device alone; XInput reports no controller, as on Windows, so a game that
 *   only knows XInput sees none.
 * The title passes it on as WINE_PS5_CONTROLLER (the word below) and sets WINE_PS5_HID only for the DualSense.
 * The file holds one line, "controller <word>"; a missing or unreadable file means the default. Save is atomic (a
 * temporary file, fsync, rename): a failed save leaves the previous file. Functions return errno values. */
#ifdef __cplusplus
extern "C" {
#endif
enum { PW_CONTROLLER_XBOX_360, PW_CONTROLLER_XBOX_ONE, PW_CONTROLLER_DUALSENSE, PW_CONTROLLER_KINDS };
/* "xbox-360", "xbox-one", "dualsense"; NULL for another number. */
const char *pw_controller_word(int kind);
/* The kind a word names; -1 for another word or NULL. A game's or a test's own WINE_PS5_CONTROLLER wins over the setting. */
int pw_controller_kind(const char *word);
/* What Settings shows: "Xbox 360 controller", "Xbox One controller", "DualSense". */
const char *pw_controller_name(int kind);
/* *kind: the saved choice; PW_CONTROLLER_XBOX_360 when the file is missing (ENOENT) or not understood (EINVAL). */
int pw_controller_load(int *kind, const char *path);
int pw_controller_save(int kind, const char *path);
#ifdef __cplusplus
}
#endif
#endif
