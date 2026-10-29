/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PW_RUNTIME_CHECK_H
#define PW_RUNTIME_CHECK_H
/*
 * Whether the Wine runtime on the console is the one this title was built with: the title folder pins the runtime's
 * stamp (runtime-stamp, tools/stage-wine.py: a digest of every file the runtime installs), and the runtime carries its
 * own copy, written last by its install (src/pw_pack.h).
 *
 * Why every start asks it: a tester's console ran Alpha 4's title on a runtime and helper from another build
 * (2026-10-29, errno 92 on firmware 11.00). The launcher's library checked the stamp, but a start from a home-screen
 * tile or shortcut goes straight into a profile and never passed there, and the guided setup counted any ntdll.prx as
 * an installed runtime. Now proton_run_wine refuses a runtime that is not this build's before any helper starts, and
 * the setup tour's install step offers this build's runtime for it, as for a missing one.
 */
#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PW_RUNTIME_THIS_BUILD = 0, /* installed, and its stamp is the title's (or the title pins none: a development build) */
    PW_RUNTIME_MISSING,        /* no ntdll.prx: never installed */
    PW_RUNTIME_OTHER_BUILD     /* installed, but its stamp is another build's, or missing (an install cut short) */
} PwRuntimeState;

/* ntdll_present: the runtime's ntdll.prx is there. installed and pinned: the two stamps' text as read (NULL or "" for
 * none); surrounding white space does not count. */
PwRuntimeState pw_runtime_state(int ntdll_present, const char *installed, const char *pinned);

#ifdef __cplusplus
}
#endif
#endif
