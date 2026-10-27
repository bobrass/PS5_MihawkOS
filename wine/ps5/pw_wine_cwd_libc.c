/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The C library names behind pw_wine_cwd. Each module is linked with
 * --wrap=<name> for every __wrap_<name> below (tools/build_wine_ps5.sh and
 * the Makefile derive the list from this file), so the module's own calls
 * come here and __real_<name> is the kernel's. Relative paths are resolved
 * against the module's directory; absolute paths pass through unchanged.
 * Every successful open records its path, without a system call, so that
 * fchdir() and the *at() calls can resolve against the descriptor.
 *
 * Only calls a title has linked are used (FW 12.02): libkernel_sys, where
 * openat(), fstatat(), mkdirat(), unlinkat(), renameat(), symlink(),
 * readlink(), link() and statfs() live, is not given to a title, and a
 * system call from title code kills it. The *at calls resolve to an absolute
 * path and use the libkernel call by path; symbolic links are virtual
 * (pw_wine_cwd.h); link() is EPERM and statfs() ENOSYS, as fstatfs() is in
 * pw_wine_compat. */
#include "pw_wine_cwd.h"
#include "pw_wine_fdv.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/vfs.h>
#else
#include <sys/mount.h>
#endif

int __real_open(const char *path, int flags, ...);
int __real_close(int fd);
int __real_stat(const char *path, struct stat *st);
int __real_lstat(const char *path, struct stat *st);
int __real_access(const char *path, int mode);
int __real_mkdir(const char *path, mode_t mode);
int __real_rmdir(const char *path);
int __real_unlink(const char *path);
int __real_rename(const char *from, const char *to);
int __real_chmod(const char *path, mode_t mode);
int __real_truncate(const char *path, off_t length);
int __real_utimes(const char *path, const struct timeval times[2]);
char *__real_realpath(const char *path, char *resolved);
FILE *__real_fopen(const char *path, const char *mode);
DIR *__real_opendir(const char *path);

/* A path as the caller named it (absolute, links unresolved) and as the
 * kernel gets it. */
struct resolved {
    char logical[PW_CWD_PATH_MAX], full[PW_CWD_PATH_MAX];
    int follow_last;
    int slash;   /* the caller's name ended in a slash: it must name a directory */
};

static int resolve_at(struct resolved *r, int dirfd, const char *path, int follow_last)
{
    size_t length = path ? strlen(path) : 0;

    r->slash = length > 1 && path[length - 1] == '/';
    /* POSIX: "link/" names what the link points to, even for lstat() and the calls that do not follow a last name. Wine asks for the
     * root of a drive that way ("dosdevices/c:/"), and a link that is only virtual is found by name, not by the kernel. */
    r->follow_last = follow_last || r->slash;
    follow_last = r->follow_last;
    if (pw_cwd_resolve_at(dirfd, AT_FDCWD, path, r->logical, sizeof(r->logical))) return -1;
    return pw_cwd_follow(r->logical, follow_last, r->full, sizeof(r->full));
}

static int resolve(struct resolved *r, const char *path, int follow_last)
{
    return resolve_at(r, AT_FDCWD, path, follow_last);
}

/* The directory part of an absolute path, "/" for a top-level name. */
static void parent_of(const char *path, char *out)
{
    const char *slash = strrchr(path, '/');
    size_t length = slash == path ? 1 : (size_t)(slash - path);

    memcpy(out, path, length);
    out[length] = 0;
}

static void table_of(const char *directory, char *out, size_t size)
{
    snprintf(out, size, "%s/%s", strcmp(directory, "/") ? directory : "", PW_CWD_LINK_TABLE);
}

/* Read the link table of every directory on the way to path not yet looked
 * at; how many links were new. */
static int load_tables(const char *path)
{
    char directory[PW_CWD_PATH_MAX], table[PW_CWD_PATH_MAX + sizeof(PW_CWD_LINK_TABLE)], text[4096];
    const char *slash = path;
    int added = 0;

    while ((slash = strchr(slash + 1, '/'))) {
        size_t length = slash == path ? 1 : (size_t)(slash - path);
        ssize_t got;
        int fd;

        memcpy(directory, path, length);
        directory[length] = 0;
        if (!pw_cwd_probe_directory(directory)) continue;
        table_of(directory, table, sizeof(table));
        if ((fd = __real_open(table, O_RDONLY)) < 0) continue;
        got = read(fd, text, sizeof(text));
        __real_close(fd);
        if (got > 0) added += pw_cwd_links_parse(directory, text, (size_t)got);
    }
    return added;
}

/* After a lookup failed for want of a directory, read the tables on the way
 * and resolve again. 1 when the call is worth repeating; 0 keeps errno.
 * Both ways are read: as named, and as followed, since a table below a link
 * is in the link's target (C:\users\steamuser is dosdevices/c: -> drive_c,
 * then drive_c/users' table; src/pw_wine_profile.h). The callers repeat
 * while a reload finds more, so a chain of links is read link by link. */
static int reload(struct resolved *r)
{
    int saved = errno;

    if ((saved != ENOENT && saved != ENOTDIR) || !(load_tables(r->logical) + load_tables(r->full)) ||
        pw_cwd_follow(r->logical, r->follow_last, r->full, sizeof(r->full))) {
        errno = saved;
        return 0;
    }
    return 1;
}

/* Rewrite the table of the directory holding a link, or remove it once the
 * directory has none left. */
static int save_table(const char *link)
{
    char directory[PW_CWD_PATH_MAX], table[PW_CWD_PATH_MAX + sizeof(PW_CWD_LINK_TABLE)];
    char next[sizeof(table) + 4], text[4096];
    ssize_t size;
    int fd, status;

    parent_of(link, directory);
    if ((size = pw_cwd_links_format(directory, text, sizeof(text))) < 0) return -1;
    table_of(directory, table, sizeof(table));
    if (!size) return __real_unlink(table) && errno != ENOENT ? -1 : 0;
    snprintf(next, sizeof(next), "%s.new", table);
    if ((fd = __real_open(next, O_WRONLY | O_CREAT | O_TRUNC, 0644)) < 0) return -1;
    status = write(fd, text, (size_t)size) == size ? 0 : -1;
    if (__real_close(fd)) status = -1;
    return status ? -1 : __real_rename(next, table);
}

/* lstat() of a virtual link: its directory's device, owner and times. */
static int link_stat(const char *full, ssize_t target_length, struct stat *st)
{
    char directory[PW_CWD_PATH_MAX];
    struct stat parent;
    size_t hash = 5381;

    parent_of(full, directory);
    if (__real_stat(directory, &parent)) return -1;
    while (*full) hash = hash * 33 + (unsigned char)*full++;
    *st = parent;
    st->st_mode = S_IFLNK | 0777;
    st->st_nlink = 1;
    st->st_size = target_length;
    st->st_ino = (ino_t)(hash | 1);
    return 0;
}

static int is_link(const char *full, ssize_t *length)
{
    char target[PW_CWD_PATH_MAX];
    int saved = errno;

    *length = pw_cwd_link_target(full, target, sizeof(target));
    errno = saved;
    return *length >= 0;
}

/* POSIX: a name that ends in a slash names a directory, and a file that is not one answers ENOTDIR. The console's
 * file system opens and stats "file/" as it does "file"; Windows code relies on the refusal ("file\\" is an invalid
 * name there), so it is given here, for every caller. */
static int refuse_slash_on_file(const struct resolved *r, const struct stat *st)
{
    if (!r->slash || S_ISDIR(st->st_mode)) return 0;
    errno = ENOTDIR;
    return -1;
}

static int lstat_resolved(struct resolved *r, struct stat *st)
{
    ssize_t length;
    int result;

    do {
        if (is_link(r->full, &length)) return link_stat(r->full, length, st);
    } while ((result = __real_lstat(r->full, st)) && reload(r));
    return result ? result : refuse_slash_on_file(r, st);
}

static int stat_resolved(struct resolved *r, struct stat *st)
{
    int result;

    while ((result = __real_stat(r->full, st)) && reload(r)) {}
    return result ? result : refuse_slash_on_file(r, st);
}

int __wrap_chdir(const char *path)
{
    char folded[PW_CWD_PATH_MAX];
    struct resolved r;
    struct stat st;

    if (resolve(&r, path, 1) || pw_cwd_fold(r.logical, folded, sizeof(folded)) ||
        stat_resolved(&r, &st))
        return -1;
    if (!S_ISDIR(st.st_mode)) { errno = ENOTDIR; return -1; }
    return pw_cwd_set(folded);
}

int __wrap_fchdir(int fd)
{
    char path[PW_CWD_PATH_MAX];
    struct stat by_fd, by_path;

    if (fstat(fd, &by_fd)) return -1;
    if (!S_ISDIR(by_fd.st_mode)) { errno = ENOTDIR; return -1; }
    /* Another module may have closed and reused the number: the path must
     * still name the descriptor's directory. */
    {
        const int unknown = pw_cwd_fd_path(fd, path, sizeof(path)) != 0;
        const int gone = !unknown && __real_stat(path, &by_path) != 0;
        if (unknown || gone || by_path.st_dev != by_fd.st_dev || by_path.st_ino != by_fd.st_ino) {
            /* Rare, and the cause of a .NET game's "access denied" on a directory listing (2026-10-24): say which. */
            fprintf(stderr, "wine-ps5: fchdir(%d) refused: %s path=%s fd dev/ino %llu/%llu path dev/ino %llu/%llu\n", fd,
                    unknown ? "no recorded path" : gone ? "the recorded path is gone" : "the recorded path names another directory",
                    unknown ? "-" : path, (unsigned long long)by_fd.st_dev, (unsigned long long)by_fd.st_ino,
                    unknown || gone ? 0ull : (unsigned long long)by_path.st_dev, unknown || gone ? 0ull : (unsigned long long)by_path.st_ino);
            errno = EACCES;
            return -1;
        }
    }
    return pw_cwd_set(path);
}

char *__wrap_getcwd(char *buffer, size_t size)
{
    char path[PW_CWD_PATH_MAX];
    size_t length = pw_cwd_get(path, sizeof(path));

    if (!buffer) {
        if (!size) size = length + 1;
        if (!(buffer = malloc(size))) return NULL;
        if (size <= length) { free(buffer); errno = ERANGE; return NULL; }
    }
    else if (!size) { errno = EINVAL; return NULL; }
    if (size <= length) { errno = ERANGE; return NULL; }
    memcpy(buffer, path, length + 1);
    return buffer;
}

static int open_resolved(struct resolved *r, int flags, mode_t mode)
{
    int fd;

    if (r->slash && (flags & O_CREAT)) {   /* "name/" cannot be created as a file, and must not leave one behind */
        errno = EISDIR;
        return -1;
    }
    /* through the open-file budget: a file past the console's limit is
     * answered by parking idle ones (pw_wine_fdv.h) */
    while ((fd = pw_wine_fdv ? pw_wine_fdv->open(r->full, flags, mode) : __real_open(r->full, flags, mode)) < 0 &&
           reload(r)) {}
    if (fd >= 0 && r->slash) {
        struct stat st;

        if (fstat(fd, &st) || refuse_slash_on_file(r, &st)) {
            int saved = errno ? errno : ENOTDIR;

            __real_close(fd);
            errno = saved;
            return -1;
        }
    }
    if (fd >= 0) pw_cwd_track(fd, r->full);
    return fd;
}

int __wrap_open(const char *path, int flags, ...)
{
    struct resolved r;
    mode_t mode = 0;

    if (flags & O_CREAT) {
        va_list args;
        va_start(args, flags);
        mode = (mode_t)va_arg(args, int);
        va_end(args);
    }
    return resolve(&r, path, 1) ? -1 : open_resolved(&r, flags, mode);
}

int __wrap_openat(int dirfd, const char *path, int flags, ...)
{
    struct resolved r;
    mode_t mode = 0;

    if (flags & O_CREAT) {
        va_list args;
        va_start(args, flags);
        mode = (mode_t)va_arg(args, int);
        va_end(args);
    }
    return resolve_at(&r, dirfd, path, 1) ? -1 : open_resolved(&r, flags, mode);
}

/* The C library opens by itself: wineserver loads the registry with
 * fopen("system.reg") after entering the prefix. */
FILE *__wrap_fopen(const char *path, const char *mode)
{
    struct resolved r;
    FILE *file;

    if (resolve(&r, path, 1)) return NULL;
    while (!(file = __real_fopen(r.full, mode)) && reload(&r)) {}
    return file;
}

int __wrap_close(int fd)
{
    if (fd == 3 && getenv("WINE_PS5_CHILD")) {
        int saved=errno;
        fprintf(stderr, "wine-ps5: control descriptor close native=%d caller=%p\n",
                (int)getpid(), __builtin_return_address(0));
        errno=saved;
    }
    pw_cwd_forget(fd);
    return pw_wine_fdv ? pw_wine_fdv->close(fd) : __real_close(fd);
}

int __wrap_stat(const char *path, struct stat *st)
{
    struct resolved r;
    return resolve(&r, path, 1) ? -1 : stat_resolved(&r, st);
}

int __wrap_lstat(const char *path, struct stat *st)
{
    struct resolved r;
    return resolve(&r, path, 0) ? -1 : lstat_resolved(&r, st);
}

int __wrap_fstatat(int dirfd, const char *path, struct stat *st, int flags)
{
    struct resolved r;
    int follow = !(flags & AT_SYMLINK_NOFOLLOW);

    if (resolve_at(&r, dirfd, path, follow)) return -1;
    return follow ? stat_resolved(&r, st) : lstat_resolved(&r, st);
}

int __wrap_access(const char *path, int mode)
{
    struct resolved r;
    struct stat st;

    if (resolve(&r, path, 1)) return -1;
    while (__real_access(r.full, mode)) {
        if (reload(&r)) continue;
        if (errno != EPERM || __real_stat(r.full, &st)) return -1;
        if (pw_cwd_mode_allows(st.st_mode, mode)) return refuse_slash_on_file(&r, &st) ? -1 : 0;
        errno = EACCES;
        return -1;
    }
    if (r.slash && !__real_stat(r.full, &st) && refuse_slash_on_file(&r, &st)) return -1;
    return 0;
}

static int mkdir_resolved(struct resolved *r, mode_t mode)
{
    int result;

    while ((result = __real_mkdir(r->full, mode)) && reload(r)) {}
    return result;
}

int __wrap_mkdir(const char *path, mode_t mode)
{
    struct resolved r;
    return resolve(&r, path, 0) ? -1 : mkdir_resolved(&r, mode);
}

int __wrap_mkdirat(int dirfd, const char *path, mode_t mode)
{
    struct resolved r;
    return resolve_at(&r, dirfd, path, 0) ? -1 : mkdir_resolved(&r, mode);
}

static int rmdir_resolved(struct resolved *r)
{
    int result;

    while ((result = __real_rmdir(r->full)) && reload(r)) {}
    return result;
}

/* unlink() of a virtual link forgets it and rewrites its table. */
static int unlink_resolved(struct resolved *r)
{
    char folded[PW_CWD_PATH_MAX];
    ssize_t length;
    int result;

    do {
        if (is_link(r->full, &length)) {
            if (pw_cwd_fold(r->full, folded, sizeof(folded)) || pw_cwd_link_remove(folded)) return -1;
            return save_table(folded);
        }
    } while ((result = __real_unlink(r->full)) && reload(r));
    return result;
}

int __wrap_rmdir(const char *path)
{
    struct resolved r;
    return resolve(&r, path, 0) ? -1 : rmdir_resolved(&r);
}

int __wrap_unlink(const char *path)
{
    struct resolved r;
    return resolve(&r, path, 0) ? -1 : unlink_resolved(&r);
}

int __wrap_unlinkat(int dirfd, const char *path, int flags)
{
    struct resolved r;

    if (resolve_at(&r, dirfd, path, 0)) return -1;
    return flags & AT_REMOVEDIR ? rmdir_resolved(&r) : unlink_resolved(&r);
}

static int rename_resolved(struct resolved *from, struct resolved *to)
{
    int result;

    while ((result = __real_rename(from->full, to->full)) && (reload(from) | reload(to))) {}
    if (!result && pw_wine_fdv) pw_wine_fdv->renamed(from->full, to->full);
    return result;
}

int __wrap_rename(const char *from, const char *to)
{
    struct resolved r_from, r_to;

    if (resolve(&r_from, from, 0) || resolve(&r_to, to, 0)) return -1;
    return rename_resolved(&r_from, &r_to);
}

int __wrap_renameat(int from_dirfd, const char *from, int to_dirfd, const char *to)
{
    struct resolved r_from, r_to;

    if (resolve_at(&r_from, from_dirfd, from, 0) || resolve_at(&r_to, to_dirfd, to, 0)) return -1;
    return rename_resolved(&r_from, &r_to);
}

/* The target is stored as given: a relative target stays relative to the
 * link, which is what Wine's "../drive_c" means. The link's directory must
 * exist and nothing may be at its name. */
static int symlink_resolved(const char *target, struct resolved *r)
{
    char folded[PW_CWD_PATH_MAX], directory[PW_CWD_PATH_MAX];
    struct stat st;

    load_tables(r->logical);
    if (pw_cwd_follow(r->logical, 0, r->full, sizeof(r->full)) ||
        pw_cwd_fold(r->full, folded, sizeof(folded)))
        return -1;
    if (!lstat_resolved(r, &st)) { errno = EEXIST; return -1; }
    if (errno != ENOENT) return -1;
    parent_of(folded, directory);
    if (__real_stat(directory, &st)) return -1;
    if (!S_ISDIR(st.st_mode)) { errno = ENOTDIR; return -1; }
    if (pw_cwd_link_add(folded, target)) return -1;
    if (!save_table(folded)) return 0;
    pw_cwd_link_remove(folded);
    return -1;
}

int __wrap_symlink(const char *target, const char *path)
{
    struct resolved r;
    return resolve(&r, path, 0) ? -1 : symlink_resolved(target, &r);
}

int __wrap_symlinkat(const char *target, int dirfd, const char *path)
{
    struct resolved r;
    return resolve_at(&r, dirfd, path, 0) ? -1 : symlink_resolved(target, &r);
}

/* Only virtual links can be read: anything else that exists is no link. */
ssize_t __wrap_readlink(const char *path, char *buffer, size_t size)
{
    struct resolved r;
    struct stat st;
    ssize_t length;
    int result;

    if (resolve(&r, path, 0)) return -1;
    do {
        if ((length = pw_cwd_link_target(r.full, buffer, size)) >= 0) return length;
    } while ((result = __real_lstat(r.full, &st)) && reload(&r));
    if (!result) errno = EINVAL;
    return -1;
}

int __wrap_link(const char *from, const char *to)
{
    (void)from;
    (void)to;
    errno = EPERM;
    return -1;
}

int __wrap_chmod(const char *path, mode_t mode)
{
    struct resolved r;
    int result;

    if (resolve(&r, path, 1)) return -1;
    while ((result = __real_chmod(r.full, mode)) && reload(&r)) {}
    return result;
}

int __wrap_truncate(const char *path, off_t length)
{
    struct resolved r;
    int result;

    if (resolve(&r, path, 1)) return -1;
    while ((result = __real_truncate(r.full, length)) && reload(&r)) {}
    return result;
}

int __wrap_utimes(const char *path, const struct timeval times[2])
{
    struct resolved r;
    int result;

    if (resolve(&r, path, 1)) return -1;
    while ((result = __real_utimes(r.full, times)) && reload(&r)) {}
    return result;
}

int __wrap_statfs(const char *path, struct statfs *buffer)
{
    (void)path;
    (void)buffer;
    errno = ENOSYS;
    return -1;
}

char *__wrap_realpath(const char *path, char *resolved_path)
{
    struct resolved r;
    char *result;

    if (resolve(&r, path, 1)) return NULL;
    while (!(result = __real_realpath(r.full, resolved_path)) && reload(&r)) {}
    return result;
}

DIR *__wrap_opendir(const char *path)
{
    struct resolved r;
    DIR *result;

    if (resolve(&r, path, 1)) return NULL;
    while (!(result = __real_opendir(r.full)) && reload(&r)) {}
    return result;
}
