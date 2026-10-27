/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PW_SETUP_H
#define PW_SETUP_H

/* The first-launch guided setup's portable part (docs/GUIDED_SETUP.md): its steps, when it shows, its languages and
 * their text, and the record it keeps (/data/ps5proton/setup.txt). The launcher draws it
 * (examples/ps5proton/kit/setup_tour.cpp); tests/test_pw_setup.c checks every rule here.
 *
 * - It shows when the record has no done=1 (the first launch), when the runtime or the mihawk prefix is missing, or
 *   when it is asked for (Settings, or the first-run marker a test sets). Never without /data: then nothing can be
 *   written, and the launcher's own complaint says why.
 * - A language is listed only once its text is complete: every key, in characters its face carries (the launcher's,
 *   baked with the kit's "european" set by ps5/ui/setup-kit.sh, or its own CJK face).
 * - The chosen language is the setup's text, and the Windows locale (and so the keyboard layout) of a mihawk prefix
 *   made after it: the title pins it in the prefix at creation (native/proton_wine.cpp). */

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PW_SETUP_NONE = -1,
    PW_SETUP_WELCOME = 0,
    PW_SETUP_LANGUAGE,
    PW_SETUP_METHOD,      /* USB drive, FTP, or the Internet (coming soon) */
    PW_SETUP_USB,         /* how to prepare the drive, then the drive's check */
    PW_SETUP_INSTALLING,
    PW_SETUP_PREPARING,   /* the first Mihawk OS session, which builds the prefix and ends by itself */
    PW_SETUP_DONE,
    PW_SETUP_FTP,         /* USB's alternative: the pieces copied into /data/ps5proton/install over FTP, then that folder's
                           * check. Last, so the steps' numbers in logs and test expectations stay as they were. */
    PW_SETUP_STEPS
} PwSetupStep;

typedef struct PwSetupFacts {
    int data;      /* /data is granted */
    int done;      /* the record says the setup was completed or skipped */
    int runtime;   /* the Wine runtime is installed */
    int prefix;    /* the mihawk prefix exists */
    int asked;     /* Settings or the first-run marker asked for the whole tour */
    int returning; /* back from the first Mihawk OS session the tour started (its marker, setup-preparing) */
} PwSetupFacts;

/* Where the tour starts, or PW_SETUP_NONE. Back from its first session, it goes on at Done, or at Preparing again
 * when the session ended before the prefix was made. */
PwSetupStep pw_setup_start(const PwSetupFacts *facts);
/* The step after `step` once it is finished: the install is skipped when the runtime is there, the first session
 * when the prefix is. */
PwSetupStep pw_setup_next(PwSetupStep step, const PwSetupFacts *facts);
/* Where Back goes from `step`; PW_SETUP_NONE where there is no going back (installing, done). */
PwSetupStep pw_setup_back(PwSetupStep step);

/* The text keys. Each language's table has every one, in this order (pw_setup.c). */
typedef enum {
    PW_TEXT_WELCOME_TITLE,
    PW_TEXT_WELCOME_BODY,
    PW_TEXT_GET_STARTED,
    PW_TEXT_SKIP_SETUP,
    PW_TEXT_LANGUAGE_TITLE,
    PW_TEXT_LANGUAGE_BODY,
    PW_TEXT_METHOD_TITLE,
    PW_TEXT_METHOD_BODY,
    PW_TEXT_METHOD_USB,
    PW_TEXT_METHOD_USB_BODY,
    PW_TEXT_METHOD_FTP,
    PW_TEXT_METHOD_FTP_BODY,
    PW_TEXT_METHOD_INTERNET,
    PW_TEXT_METHOD_INTERNET_BODY,
    PW_TEXT_COMING_SOON,
    PW_TEXT_METHOD_INSTALLED,
    PW_TEXT_CONTINUE,
    PW_TEXT_QR_CAPTION,
    PW_TEXT_USB_TITLE,
    PW_TEXT_USB_STEP1,
    PW_TEXT_USB_STEP2,
    PW_TEXT_USB_STEP3,
    PW_TEXT_USB_STEP4,
    PW_TEXT_CHECK_DRIVE,
    PW_TEXT_FOUND,             /* {1}: the size */
    PW_TEXT_INSTALL,
    PW_TEXT_RETRY,
    PW_TEXT_FTP_TITLE,
    PW_TEXT_FTP_STEP1,
    PW_TEXT_FTP_STEP2,
    PW_TEXT_FTP_STEP3,
    PW_TEXT_FTP_STEP4,
    PW_TEXT_CHECK_FILES,
    PW_TEXT_FTP_FOUND,         /* {1}: the size */
    PW_TEXT_ERR_FTP_EMPTY,
    PW_TEXT_ERR_FTP_PARTIAL,   /* {1} pieces of {2} */
    PW_TEXT_FTP_INSTALLING_BODY,
    PW_TEXT_ERR_FTP_READ,
    PW_TEXT_ERR_FTP_DAMAGED,
    PW_TEXT_ERR_NO_STICK,
    PW_TEXT_ERR_NO_FOLDER,
    PW_TEXT_ERR_PARTIAL,       /* {1} pieces of {2} */
    PW_TEXT_ERR_NO_SPACE,      /* {1} needed, {2} free */
    PW_TEXT_ERR_READ,
    PW_TEXT_ERR_DAMAGED,
    PW_TEXT_ERR_FULL,
    PW_TEXT_ERR_WRITE,
    PW_TEXT_ERR_CANCELLED,
    PW_TEXT_ERR_BUILD,
    PW_TEXT_INSTALLING_TITLE,
    PW_TEXT_INSTALLING_BODY,
    PW_TEXT_INSTALLING_CHECKING,
    PW_TEXT_INSTALLING_STATS,  /* {1} of {2} MB, {3} MB/s */
    PW_TEXT_INSTALLED,
    PW_TEXT_STOP,
    PW_TEXT_PREPARE_TITLE,
    PW_TEXT_PREPARE_BODY,
    PW_TEXT_PREPARE_NOTE,
    PW_TEXT_PREPARE_START,
    PW_TEXT_PREPARE_FAILED,
    PW_TEXT_BUILD_TITLE,       /* the first session's cover (PwSetupBuild) */
    PW_TEXT_BUILD_BODY,
    PW_TEXT_BUILD_SETTINGS,    /* its stages, in order */
    PW_TEXT_BUILD_DOTNET,
    PW_TEXT_BUILD_FINISHING,
    PW_TEXT_DONE_TITLE,
    PW_TEXT_DONE_BODY,
    PW_TEXT_ENTER,
    PW_TEXT_HINT_SELECT,
    PW_TEXT_HINT_BACK,
    PW_TEXT_HINT_MOVE,
    PW_TEXT_STEP_OF,           /* {1} of {2} */
    PW_TEXT_GIGABYTES,         /* {1}: the number, written with PW_TEXT_DECIMAL */
    PW_TEXT_DECIMAL,
    /* The Steam guide (src/pw_steam_guide.h): the launcher's pages, then the screens its sessions draw */
    PW_TEXT_STEAM_TITLE,
    PW_TEXT_STEAM_BODY,
    PW_TEXT_STEAM_STEP_DOWNLOAD, /* the intro's four stages: name, then a line */
    PW_TEXT_STEAM_STEP_DOWNLOAD_BODY,
    PW_TEXT_STEAM_STEP_INSTALL,
    PW_TEXT_STEAM_STEP_INSTALL_BODY,
    PW_TEXT_STEAM_STEP_UPDATE,
    PW_TEXT_STEAM_STEP_UPDATE_BODY,
    PW_TEXT_STEAM_STEP_SIGNIN,
    PW_TEXT_STEAM_STEP_SIGNIN_BODY,
    PW_TEXT_STEAM_NOTE,
    PW_TEXT_STEAM_START,
    PW_TEXT_STEAM_NOT_NOW,
    PW_TEXT_STEAM_DOWNLOADING,
    PW_TEXT_STEAM_DOWNLOADING_BODY,
    PW_TEXT_STEAM_MEGABYTES, /* {1} of about {2} MB */
    PW_TEXT_STEAM_STARTING,
    PW_TEXT_STEAM_FAIL_TITLE,
    PW_TEXT_STEAM_ERR_OFFLINE, /* why the guide stopped (PwSteamGuideFailure) */
    PW_TEXT_STEAM_ERR_CLOCK,
    PW_TEXT_STEAM_ERR_DOWNLOAD,
    PW_TEXT_STEAM_ERR_INSTALL,
    PW_TEXT_STEAM_ERR_STALLED,
    PW_TEXT_STEAM_ERR_CANCELLED,
    PW_TEXT_STEAM_ERR_TIMEOUT,
    PW_TEXT_STEAM_ERR_GUARD,
    PW_TEXT_STEAM_ERR_ENDED,
    PW_TEXT_STEAM_INSTALLING_TITLE, /* the session's screens (title-drawn) */
    PW_TEXT_STEAM_INSTALLING_BODY,
    PW_TEXT_STEAM_UPDATING_TITLE,
    PW_TEXT_STEAM_UPDATING_BODY,
    PW_TEXT_STEAM_DOWNLOADED, /* {1} MB */
    PW_TEXT_STEAM_ELAPSED, /* {1}: minutes:seconds */
    PW_TEXT_STEAM_SLOW,
    PW_TEXT_STEAM_SCAN_TITLE,
    PW_TEXT_STEAM_SCAN_BODY,
    PW_TEXT_STEAM_CANCEL_NOTE,
    PW_TEXT_STEAM_SIGNED_IN,
    PW_TEXT_STEAM_SIGNED_IN_AS, /* {1}: the Steam display name */
    PW_TEXT_STEAM_REOPEN_BODY,
    PW_TEXT_STEAM_DONE_BODY,
    PW_TEXT_STEAM_OPENING_IN, /* {1} seconds */
    PW_TEXT_COUNT
} PwSetupText;

typedef struct PwSetupLanguage {
    const char *tag;            /* BCP 47, the record's value: "fr-FR" */
    const char *name;           /* in its own language: "Français" */
    const char *flag;           /* its flag in ps5/assets/flags (lipis/flag-icons' code): "fr" */
    const char *locale;         /* the Unix locale Wine reads for it: "fr_FR.UTF-8" */
    const char *font;           /* its face in the title's assets/setup/fonts, or NULL for the launcher's own */
    const char *const *text;    /* PW_TEXT_COUNT strings, or NULL until its translation lands */
} PwSetupLanguage;

/* Every planned language, with or without text; *count is set. */
const PwSetupLanguage *pw_setup_languages(size_t *count);
/* The language a record's tag names, if it has text; NULL otherwise. */
const PwSetupLanguage *pw_setup_language(const char *tag);
/* The default: English (US). */
const PwSetupLanguage *pw_setup_default_language(void);
const char *pw_setup_text(const PwSetupLanguage *language, PwSetupText key);
/* The key a step of the install shows on its route: on the FTP route (ftp nonzero) each text that speaks of the USB
 * drive (the page, its check, what was found, the install's note and errors) becomes its FTP twin, which speaks of
 * /data/ps5proton/install; every other key, and every key on the USB route, is itself. */
PwSetupText pw_setup_route_text(int ftp, PwSetupText key);
/* text with {1}..{9} replaced by args[0..]; a missing argument leaves the braces. Returns the length written,
 * truncated to size - 1. */
size_t pw_setup_fill(char *out, size_t size, const char *text, const char *const *args, size_t count);

/* bytes as the language writes gigabytes, to two decimals: "1.19 GB", "1,19 Go". */
size_t pw_setup_gigabytes(const PwSetupLanguage *language, unsigned long long bytes, char *out, size_t size);

/* The record, /data/ps5proton/setup.txt: "language=<tag>" and "done=1" lines; any other line is ignored. */
typedef struct PwSetupRecord {
    char language[16];
    int done;
} PwSetupRecord;
/* 0, or the errno (ENOENT: none yet); *out is zeroed first, so a missing or unreadable record reads as not done. */
int pw_setup_record_load(PwSetupRecord *out, const char *path);
/* Written beside it and renamed into place after an fsync; 0 or the errno. */
int pw_setup_record_save(const PwSetupRecord *record, const char *path);
/* The first session ends once the prefix is made: wineboot (patch 0952) writes .update-timestamp after the update,
 * ending in " wow64". It writes it through msvcrt in text mode, "1734871457 wow64\r\n": an LF-only strip never
 * matched, and the first session of first-install-unattended-2026-10-19-03 never ended. Trims trailing CR, LF and
 * spaces from stamp in place; 1 when it is made. */
int pw_setup_prefix_made(char *stamp);

/* The first session's cover (docs/GUIDED_SETUP.md, "Preparing"): what the user sees instead of a black desktop while
 * wineboot makes the prefix. The launcher draws its words when Start is pressed and keeps that frame
 * (PW_SETUP_COVER_NAME, in /data/ps5proton); the title shows it through the GPU broker until the session ends, with a mark for each
 * stage and the time since the start at the places below (1920x1080, the frame's own pixels). The stages follow the
 * processes wineboot starts; first-install-unattended-2026-10-19-04 took 75 s: wine.inf's settings from the start, Wine
 * Mono from about 15 s (control.exe appwiz.cpl install_mono, then msiexec and its helpers, each command line naming
 * mono), the rest from about 58 s (iexplore /RegServer, the 32-bit settings), and the prefix made. */
typedef enum {
    PW_SETUP_BUILD_SETTINGS,
    PW_SETUP_BUILD_DOTNET,
    PW_SETUP_BUILD_FINISHING,
    PW_SETUP_BUILD_READY /* set by the title once pw_setup_prefix_made */
} PwSetupBuild;
/* The stage once a process with this command line (NULL: none known) has started; never earlier than stage. */
PwSetupBuild pw_setup_build_next(PwSetupBuild stage, const char *command_line);
#define PW_SETUP_COVER_NAME "setup-cover.ppm"
enum {
    PW_SETUP_COVER_WIDTH = 1920,
    PW_SETUP_COVER_HEIGHT = 1080,
    PW_SETUP_COVER_MARK_X = 394,   /* each stage's mark: its centre, 22 px in radius; its words start at x 446 */
    PW_SETUP_COVER_ROW_Y = 520,    /* the first stage's centre */
    PW_SETUP_COVER_ROW_STEP = 84
};
/* Draws the stage marks over frame (PW_SETUP_COVER_WIDTH x HEIGHT, 0xAARRGGBB: the display's B, G, R, A bytes), after
 * copying their square back from picture, the frame without them: a green disc with a check for each stage done, a
 * turning ring of dots for the current one (tick: the frame's number; the title draws ten a second), a grey ring for
 * each to come. Nothing outside that square is written. */
void pw_setup_cover_draw(unsigned *frame, const unsigned *picture, PwSetupBuild stage, unsigned tick);

#ifdef __cplusplus
}
#endif

#endif
