/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PW_WINE_DSECTION_H
#define PW_WINE_DSECTION_H
#include <stddef.h>
#include <stdint.h>

/* Large anonymous sections in the console's direct memory, kept by the server (in the title).
 *
 * A Windows program's anonymous section (CreateFileMapping with no file) is RAM on Windows. On the console the large
 * ones were a sparse file on /data paged through a 64 MiB cache, and a program whose working set in one is larger
 * thrashed the disk: a Unity game in the earlier emulator experiment (removed) (its guest memory is one 14.5 GiB section) evicted and flushed 60,000 to
 * 77,000 pages per run, its own file reads took up to 2.3 s, frames stopped for 25 s and the console went down
 * (2026-10-14; record removed from the tree; in git history). /data is also not coherent between mappings of a sparse file's holes
 * (pw_wine_blocks.h), which the cache's aliases rely on.
 *
 * Here a section is identified by its backing file (device and inode; the file stays only as that identity) and its
 * memory is direct memory in 2 MiB chunks, allocated and zeroed when a process first asks for a page of the chunk.
 * Every process that asks is granted every chunk (the memory helper owns them: native/pw_dmem_share.h), so each alias
 * in each process maps the same RAM. The kernel is reached only through PwWineDsectionOps, so the policy is host-tested.
 *
 * Lock: dsection_lock guards the table. No op runs under it but alive(), a kill(pid, 0) that never blocks. A request
 * that must ask the memory helper (a chunk's first page, or a process's first page of the section) marks the section
 * busy, drops the lock, asks, and publishes the answer under the lock. Requests for chunks that exist are answered
 * meanwhile; one that needs the busy section waits up to wait_ms (ETIMEDOUT). Before, the lock was held across every
 * helper call, on the helpers' monitor threads and on the server thread at release, so one stalled helper call wedged
 * every helper's pager and then the server (2026-10-14).
 *
 * Lifetime: the server's release only marks a section. Windows views outlive the section's handle, and a helper's pager
 * remembers a section's chunks for its whole life (pw_wine_dmap.c), so the chunks go back in pw_wine_dsection_collect
 * once every process admitted to the section has ended; until then the section still answers them. A process is
 * admitted when it maps a view (pw_wine_section_add asks for the first chunk), not at its first touch: a view mapped
 * and left untouched while the section was released would otherwise find it gone and fall back to the file. The backing
 * descriptor is held as long, so (dev, ino) names no other section while any process may map the chunks.
 * shortcut: chunks go back only when their processes end; a program that keeps making and dropping large sections
 * holds their memory and the table's slots until it ends (new ones then keep their file); free per view if one does. */

enum {
    PW_WINE_DSECTION_CHUNK = 2u << 20,
    PW_WINE_DSECTION_MIN_BYTES = 8u << 20,     /* and the views the demand pager maps (patch 0962): Steam's two 25 MiB
                                                * sections were 50 MiB of flexible memory in each of its processes */
    PW_WINE_DSECTION_SECTIONS = 64,
    PW_WINE_DSECTION_PIDS = 16,
    PW_WINE_DSECTION_WAIT_MS = 8000,           /* under a helper pager's 10 s wait for its answer (pw_wine_process.c) */
};

/* The smallest section the title keeps in direct memory, and a helper's view the pager maps from it: PW_WINE_DSECTION_MIN_BYTES,
 * or what WINE_PS5_DSECTION_MIN_MIB asks (8 to 256 MiB; anything else is refused and 8 stays). Helpers inherit the setting. An experiment (2026-10-24): the browser's smaller anonymous sections, the
 * ones Chromium shares between its processes, count against the 1 GiB flexible pool as physical shared memory. Read once. */
uint64_t pw_wine_dsection_min_bytes(void);
/* The setting's parse, for a test: bytes, or 0 when the text is missing or outside 8..256. */
uint64_t pw_wine_dsection_parse_min_mib(const char *text);

typedef struct PwWineDsectionOps {
    void *context;
    /* Direct memory of bytes, granted to pid as well as the caller (which zeroes it). 0 with *offset, or -1. */
    int (*allocate)(void *context, uint64_t bytes, int pid, int64_t *offset);
    /* Grant a range already allocated to one more pid. 0, or -1. */
    int (*grant)(void *context, int64_t offset, uint64_t bytes, int pid);
    /* Zero a fresh range (direct memory comes back as the last user left it). 0, or -1. */
    int (*zero)(void *context, int64_t offset, uint64_t bytes);
    /* Give a range back: 0, or -1 when it was not taken (it stays the memory helper's). Called once per range and
     * never retried: an answer that was lost may still come. */
    int (*release)(void *context, int64_t offset, uint64_t bytes);
    /* 0 only when pid has certainly ended. It runs under the lock, so it must not block. NULL: never ended. */
    int (*alive)(void *context, int pid);
    /* How long a request waits behind another in the same section; 0 is PW_WINE_DSECTION_WAIT_MS. */
    unsigned wait_ms;
} PwWineDsectionOps;

void pw_wine_dsection_init(const PwWineDsectionOps *ops);
/* A new anonymous section of size bytes backed by the file fd, whose identity is (dev, ino). fd is duplicated and the
 * copy held until the section's memory goes back; fd stays the caller's (-1: none held, for tests). 0, or -1 with
 * errno EINVAL (too small), EEXIST ((dev, ino) is still in the table), ENOSPC (the table is full), ENOMEM, or dup's
 * error: the section then keeps its file backing. */
int pw_wine_dsection_register(int fd, uint64_t dev, uint64_t ino, uint64_t size);
/* 1 when (dev, ino) is in the table, released or not. */
int pw_wine_dsection_known(uint64_t dev, uint64_t ino);
/* The direct memory behind offset of the section, for pid: the chunk's offset in direct memory, its offset in the
 * section and its size. 0, -1 with errno ENOENT (not a registered section; the caller maps the file), ERANGE (past
 * the end), EMFILE (PW_WINE_DSECTION_PIDS live processes are admitted), ETIMEDOUT (another request held the section
 * past wait_ms), or an op's error (ENOTCONN once the memory helper is marked dead). */
int pw_wine_dsection_page(uint64_t dev, uint64_t ino, uint64_t offset, int pid, int64_t *dmem,
                          uint64_t *chunk_offset, uint64_t *chunk_bytes);
/* The server destroyed the section: it is marked, and its chunks go back in pw_wine_dsection_collect. Never waits on
 * the memory helper. */
void pw_wine_dsection_release(uint64_t dev, uint64_t ino);
/* One pass of the title's collector: forget the ended processes of every section, then give back the chunks of each
 * released section no live process is admitted to and close its descriptor, holding no lock while the memory helper
 * is asked. *given and *lost (either may be NULL) count the chunks taken back and not. Returns how many released
 * sections still wait for a process to end. */
unsigned pw_wine_dsection_collect(uint64_t *given, uint64_t *lost);
/* Sections, chunks allocated now, grants made, failures. */
void pw_wine_dsection_stats(uint64_t *sections, uint64_t *chunks, uint64_t *grants, uint64_t *failures);

#endif
