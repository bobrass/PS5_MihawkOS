/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PW_WINE_XATTR_H
#define PW_WINE_XATTR_H
/*
 * Extended attributes for Wine's Unix side where the file system has none, as on the console: Wine keeps a file's
 * DOS attributes (hidden, system) in user.DOSATTRIB and a reparse point in user.WINEREPARSE (dlls/ntdll/unix/file.c
 * xattr_*), and without them those were lost at once (ntdll file and directory, kernel32 file: 355 test lines,
 * 2026-10-25). Security descriptors have their own store in the server (patch 1108).
 *
 * Each value is a file STORE/<dev>-<ino>.<name>, written whole through a temporary and a rename (a reader never sees
 * half of one) and read whole. Keyed by the file's device and inode, a value follows its file through a reopen and a
 * rename, as Windows keeps a file's attributes, and not through a copy. An inode the kernel hands to a new file must
 * start empty: its creator calls pw_wine_xattr_forget.
 *
 * Asking about a file that has no attribute, the common case, costs no file system call while the store is quiet:
 * the store's names are kept in memory and read again when the store's modification time changes, or while it is
 * under two seconds old (the console's file times are too coarse to show two changes in a row). Every process keeps
 * its own copy; one that sets or removes a value sees it at once.
 */
#include <stddef.h>
#include <sys/types.h>

/* The store's folder, made if missing. 0, or -1 with errno; until it succeeds every call answers ENOTSUP. */
int pw_wine_xattr_init(const char *store_dir);
/* The value's size, copied into value when size holds it; -1 with ENODATA when there is none, ERANGE when size is
 * too small, as getxattr. */
ssize_t pw_wine_xattr_get(dev_t dev, ino_t ino, const char *name, void *value, size_t size);
/* 0, or -1 with errno. */
int pw_wine_xattr_set(dev_t dev, ino_t ino, const char *name, const void *value, size_t size);
/* 0, or -1 with ENODATA when there was none. */
int pw_wine_xattr_remove(dev_t dev, ino_t ino, const char *name);
/* Every value of an inode the kernel has given to a new file. */
void pw_wine_xattr_forget(dev_t dev, ino_t ino);

#endif
