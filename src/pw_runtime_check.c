/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pw_runtime_check.h"
#include <string.h>

/* The text between leading and trailing white space: its start and length. */
static const char *trimmed(const char *text, size_t *length)
{
    if (!text) { *length = 0; return ""; }
    while (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n') text++;
    size_t n = strlen(text);
    while (n && (text[n - 1] == ' ' || text[n - 1] == '\t' || text[n - 1] == '\r' || text[n - 1] == '\n')) n--;
    *length = n;
    return text;
}

PwRuntimeState pw_runtime_state(int ntdll_present, const char *installed, const char *pinned)
{
    if (!ntdll_present) return PW_RUNTIME_MISSING;
    size_t pinned_length, installed_length;
    const char *pin = trimmed(pinned, &pinned_length), *have = trimmed(installed, &installed_length);
    if (!pinned_length) return PW_RUNTIME_THIS_BUILD;   /* a title built without its pin (ps5/tools/build.sh alone) */
    return installed_length == pinned_length && !memcmp(pin, have, pinned_length) ? PW_RUNTIME_THIS_BUILD
                                                                                    : PW_RUNTIME_OTHER_BUILD;
}
