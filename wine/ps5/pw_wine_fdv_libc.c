/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Every libc call of Wine's Unix side that takes a descriptor, wrapped with
 * lld --wrap (tools/build-wine-ps5.sh derives the list from this file and
 * pw_wine_cwd_libc.c) so that the call pins its file for the duration and a
 * file the open-file budget parked is reopened first (pw_wine_fdv.h). The
 * wrapped calls reach __real_<name>, the kernel's.
 *
 * open(), close() and rename() are in pw_wine_cwd_libc.c, which resolves the
 * path they take. A descriptor that is not a tracked file (a socket, a pipe,
 * a device) costs one lookup per call. */
#include "pw_wine_fdv.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/event.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <unistd.h>

const struct pw_wine_fdv *pw_wine_fdv;

void pw_module_share(const void *table)
{
    pw_wine_fdv = table;
}

enum { PW_FDV_PINS = 64 };

static int enter(int fd, uint32_t *pin)
{
    const struct pw_wine_fdv *fdv = pw_wine_fdv;

    *pin = 0;
    return fdv ? fdv->enter(fd, pin) : 0;
}

static void leave(uint32_t pin)
{
    int saved;

    if (!pin) return;
    saved = errno;
    pw_wine_fdv->leave(pin);
    errno = saved;
}

ssize_t __real_read(int fd, void *buffer, size_t bytes);
ssize_t __real_write(int fd, const void *buffer, size_t bytes);
ssize_t __real_pread(int fd, void *buffer, size_t bytes, off_t offset);
ssize_t __real_pwrite(int fd, const void *buffer, size_t bytes, off_t offset);
ssize_t __real_readv(int fd, const struct iovec *vector, int count);
ssize_t __real_writev(int fd, const struct iovec *vector, int count);
off_t __real_lseek(int fd, off_t offset, int whence);
int __real_fstat(int fd, struct stat *status);
int __real_fstatvfs(int fd, struct statvfs *status);
int __real_fsync(int fd);
int __real_ftruncate(int fd, off_t length);
int __real_fchmod(int fd, mode_t mode);
int __real_futimes(int fd, const struct timeval times[2]);
int __real_ioctl(int fd, unsigned long request, ...);
int __real_fcntl(int fd, int command, ...);
void *__real_mmap(void *address, size_t length, int protection, int flags, int fd, off_t offset);
int __real_dup(int fd);
FILE *__real_fdopen(int fd, const char *mode);
int __real_poll(struct pollfd *fds, nfds_t count, int timeout);
int __real_select(int count, fd_set *readers, fd_set *writers, fd_set *errors, struct timeval *timeout);
int __real_kevent(int queue, const struct kevent *changes, int change_count, struct kevent *events,
                  int event_count, const struct timespec *timeout);
ssize_t __real_sendmsg(int socket, const struct msghdr *message, int flags);
ssize_t __real_recvmsg(int socket, struct msghdr *message, int flags);

ssize_t __wrap_read(int fd, void *buffer, size_t bytes)
{
    uint32_t pin;
    ssize_t result;

    if (enter(fd, &pin)) return -1;
    result = __real_read(fd, buffer, bytes);
    leave(pin);
    return result;
}

ssize_t __wrap_write(int fd, const void *buffer, size_t bytes)
{
    uint32_t pin;
    ssize_t result;

    if (enter(fd, &pin)) return -1;
    result = __real_write(fd, buffer, bytes);
    leave(pin);
    return result;
}

ssize_t __wrap_pread(int fd, void *buffer, size_t bytes, off_t offset)
{
    uint32_t pin;
    ssize_t result;

    if (enter(fd, &pin)) return -1;
    result = __real_pread(fd, buffer, bytes, offset);
    leave(pin);
    return result;
}

ssize_t __wrap_pwrite(int fd, const void *buffer, size_t bytes, off_t offset)
{
    uint32_t pin;
    ssize_t result;

    if (enter(fd, &pin)) return -1;
    result = __real_pwrite(fd, buffer, bytes, offset);
    leave(pin);
    return result;
}

ssize_t __wrap_readv(int fd, const struct iovec *vector, int count)
{
    uint32_t pin;
    ssize_t result;

    if (enter(fd, &pin)) return -1;
    result = __real_readv(fd, vector, count);
    leave(pin);
    return result;
}

ssize_t __wrap_writev(int fd, const struct iovec *vector, int count)
{
    uint32_t pin;
    ssize_t result;

    if (enter(fd, &pin)) return -1;
    result = __real_writev(fd, vector, count);
    leave(pin);
    return result;
}

off_t __wrap_lseek(int fd, off_t offset, int whence)
{
    uint32_t pin;
    off_t result;

    if (enter(fd, &pin)) return -1;
    result = __real_lseek(fd, offset, whence);
    leave(pin);
    return result;
}

int __wrap_fstat(int fd, struct stat *status)
{
    uint32_t pin;
    int result;

    if (enter(fd, &pin)) return -1;
    result = __real_fstat(fd, status);
    leave(pin);
    return result;
}

int __wrap_fstatvfs(int fd, struct statvfs *status)
{
    uint32_t pin;
    int result;

    if (enter(fd, &pin)) return -1;
    result = __real_fstatvfs(fd, status);
    leave(pin);
    return result;
}

int __wrap_fsync(int fd)
{
    uint32_t pin;
    int result;

    if (enter(fd, &pin)) return -1;
    result = __real_fsync(fd);
    leave(pin);
    return result;
}

int __wrap_ftruncate(int fd, off_t length)
{
    uint32_t pin;
    int result;

    if (enter(fd, &pin)) return -1;
    result = __real_ftruncate(fd, length);
    leave(pin);
    return result;
}

int __wrap_fchmod(int fd, mode_t mode)
{
    uint32_t pin;
    int result;

    if (enter(fd, &pin)) return -1;
    result = __real_fchmod(fd, mode);
    leave(pin);
    return result;
}

int __wrap_futimes(int fd, const struct timeval times[2])
{
    uint32_t pin;
    int result;

    if (enter(fd, &pin)) return -1;
    result = __real_futimes(fd, times);
    leave(pin);
    return result;
}

int __wrap_ioctl(int fd, unsigned long request, ...)
{
    va_list args;
    void *argument;
    uint32_t pin;
    int result;

    va_start(args, request);
    argument = va_arg(args, void *);
    va_end(args);
    if (enter(fd, &pin)) return -1;
    result = __real_ioctl(fd, request, argument);
    leave(pin);
    return result;
}

/* A lock belongs to the process's file, not to a descriptor number: closing
 * any descriptor of the file drops it, so a locked file is never parked. */
int __wrap_fcntl(int fd, int command, ...)
{
    va_list args;
    long argument;
    uint32_t pin;
    int result;

    va_start(args, command);
    argument = va_arg(args, long);
    va_end(args);
    if (enter(fd, &pin)) return -1;
    if (pin && (command == F_SETLK || command == F_SETLKW || command == F_GETLK)) pw_wine_fdv->keep(pin);
#ifdef F_DUP2FD
    if (pin && (command == F_DUP2FD
#ifdef F_DUP2FD_CLOEXEC
                || command == F_DUP2FD_CLOEXEC
#endif
                ))
        pw_wine_fdv->forget((int)argument);
#endif
    result = __real_fcntl(fd, command, argument);
    if (pin && (command == F_DUPFD || command == F_DUPFD_CLOEXEC)) {
        if (result < 0 && errno == EMFILE && pw_wine_fdv->make_room()) result = __real_fcntl(fd, command, argument);
        if (result >= 0) pw_wine_fdv->dup_note(fd, result);
    }
#ifdef F_DUP2FD
    else if (pin && result >= 0 && (command == F_DUP2FD
#ifdef F_DUP2FD_CLOEXEC
                                    || command == F_DUP2FD_CLOEXEC
#endif
                                    ))
        pw_wine_fdv->dup_note(fd, result);
#endif
    leave(pin);
    return result;
}

int __wrap_dup(int fd)
{
    uint32_t pin;
    int result;

    if (enter(fd, &pin)) return -1;
    result = __real_dup(fd);
    if (pin) {
        if (result < 0 && errno == EMFILE && pw_wine_fdv->make_room()) result = __real_dup(fd);
        if (result >= 0) pw_wine_fdv->dup_note(fd, result);
    }
    leave(pin);
    return result;
}

/* Told where each mapping made without MAP_FIXED landed; set by ntdll's section pager (pw_wine_section_ps5.c), never
 * in the server. It must not lock or change errno: it runs inside any mmap, the heap's included. */
void (*pw_wine_mmap_placed)(void *, size_t);

/* WINE_PS5_MAPLOG_MIB=N: every mmap of N MiB or more through this wrap is logged with where it landed and who called
 * (resolve the caller against the unstripped module), to name the owner of a large anonymous range the kernel's map shows
 * (docs/PLAN.md, the browser helper's start-up jump). Off when unset. Runs inside any mmap: no allocation, no lock. */
static long maplog_bytes = -1;

static void maplog(void *result, size_t length, int protection, int flags, int fd, void *caller)
{
    char line[200];
    int saved = errno;

    if (maplog_bytes < 0) {
        const char *setting = getenv("WINE_PS5_MAPLOG_MIB");
        maplog_bytes = setting ? atol(setting) << 20 : 0;
    }
    if (maplog_bytes && length >= (size_t)maplog_bytes) {
        int n = snprintf(line, sizeof(line), "wine-ps5: maplog %p +%zu MiB prot %#x flags %#x fd %d caller %p\n", result,
                         length >> 20, protection, flags, fd, caller);
        if (n > 0) (void)write(2, line, (size_t)n < sizeof(line) ? (size_t)n : sizeof(line) - 1);
    }
    errno = saved;
}

void *__wrap_mmap(void *address, size_t length, int protection, int flags, int fd, off_t offset)
{
    uint32_t pin = 0;
    void *result;

    if (!(flags & MAP_ANON) && fd >= 0 && enter(fd, &pin)) return MAP_FAILED;
    result = __real_mmap(address, length, protection, flags, fd, offset);
    leave(pin);
    maplog(result, length, protection, flags, fd, __builtin_return_address(0));
    if (result != MAP_FAILED && !(flags & MAP_FIXED) && pw_wine_mmap_placed) pw_wine_mmap_placed(result, length);
    return result;
}

/* FILE streams do their own system calls on the descriptor, outside any
 * wrap: the file stays open. */
FILE *__wrap_fdopen(int fd, const char *mode)
{
    uint32_t pin;
    FILE *result;

    if (enter(fd, &pin)) return NULL;
    if (pin) pw_wine_fdv->keep(pin);
    result = __real_fdopen(fd, mode);
    leave(pin);
    return result;
}

int __wrap_poll(struct pollfd *fds, nfds_t count, int timeout)
{
    uint32_t pins[PW_FDV_PINS];
    unsigned held = 0, i;
    int result;

    if (pw_wine_fdv && count <= PW_FDV_PINS) {
        for (i = 0; i < count; i++) {
            if (fds[i].fd < 0) continue;
            if (enter(fds[i].fd, &pins[held])) {
                while (held) leave(pins[--held]);
                return -1;
            }
            if (pins[held]) held++;
        }
    }
    result = __real_poll(fds, count, timeout);
    while (held) leave(pins[--held]);
    return result;
}

int __wrap_select(int count, fd_set *readers, fd_set *writers, fd_set *errors, struct timeval *timeout)
{
    uint32_t pins[PW_FDV_PINS];
    unsigned held = 0;
    int fd, result;

    if (pw_wine_fdv) {
        for (fd = 0; fd < count && fd < FD_SETSIZE; fd++) {
            if (!((readers && FD_ISSET(fd, readers)) || (writers && FD_ISSET(fd, writers)) ||
                  (errors && FD_ISSET(fd, errors))))
                continue;
            if (held == PW_FDV_PINS) break;
            if (enter(fd, &pins[held])) {
                while (held) leave(pins[--held]);
                return -1;
            }
            if (pins[held]) held++;
        }
    }
    result = __real_select(count, readers, writers, errors, timeout);
    while (held) leave(pins[--held]);
    return result;
}

/* A file registered with a kqueue is never parked: the registration belongs
 * to the file, and the queue would report on a socket. */
int __wrap_kevent(int queue, const struct kevent *changes, int change_count, struct kevent *events,
                  int event_count, const struct timespec *timeout)
{
    int i;

    if (pw_wine_fdv) {
        for (i = 0; i < change_count; i++) {
            uint32_t pin;

            if ((changes[i].flags & EV_ADD) == 0 || (changes[i].filter != EVFILT_READ &&
                                                      changes[i].filter != EVFILT_WRITE &&
                                                      changes[i].filter != EVFILT_VNODE))
                continue;
            if (enter((int)changes[i].ident, &pin)) return -1;
            if (pin) {
                pw_wine_fdv->keep(pin);
                leave(pin);
            }
        }
    }
    return __real_kevent(queue, changes, change_count, events, event_count, timeout);
}

/* Descriptors passed to the other end of a socket: the files are in flight
 * until the receiver has them, and the receiver's numbers join the file. */
ssize_t __wrap_sendmsg(int socket, const struct msghdr *message, int flags)
{
    const struct pw_wine_fdv *fdv = pw_wine_fdv;
    uint32_t pins[PW_FDV_PINS];
    unsigned held = 0, i;
    ssize_t result;
    int saved;

    if (fdv && message->msg_control && message->msg_controllen >= sizeof(struct cmsghdr)) {
        struct cmsghdr *header;

        for (header = CMSG_FIRSTHDR(message); header; header = CMSG_NXTHDR((struct msghdr *)message, header)) {
            size_t count, n;

            if (header->cmsg_level != SOL_SOCKET || header->cmsg_type != SCM_RIGHTS) continue;
            count = (header->cmsg_len - CMSG_LEN(0)) / sizeof(int);
            for (n = 0; n < count && held < PW_FDV_PINS; n++) {
                int fd;

                memcpy(&fd, CMSG_DATA(header) + n * sizeof(int), sizeof(fd));
                if (fdv->sending(fd, &pins[held])) {
                    saved = errno;
                    while (held) fdv->sent(pins[--held], 0);
                    errno = saved;
                    return -1;
                }
                if (pins[held]) held++;
            }
        }
    }
    result = __real_sendmsg(socket, message, flags);
    saved = errno;
    for (i = 0; i < held; i++) fdv->sent(pins[i], result >= 0);
    errno = saved;
    return result;
}

ssize_t __wrap_recvmsg(int socket, struct msghdr *message, int flags)
{
    const struct pw_wine_fdv *fdv = pw_wine_fdv;
    ssize_t result = __real_recvmsg(socket, message, flags);

    if (result >= 0 && fdv && message->msg_control && message->msg_controllen >= sizeof(struct cmsghdr)) {
        struct cmsghdr *header;
        int saved = errno;

        for (header = CMSG_FIRSTHDR(message); header; header = CMSG_NXTHDR(message, header)) {
            size_t count, n;

            if (header->cmsg_level != SOL_SOCKET || header->cmsg_type != SCM_RIGHTS) continue;
            count = (header->cmsg_len - CMSG_LEN(0)) / sizeof(int);
            for (n = 0; n < count; n++) {
                int fd;

                memcpy(&fd, CMSG_DATA(header) + n * sizeof(int), sizeof(fd));
                fdv->received(fd);
            }
        }
        errno = saved;
    }
    return result;
}

/* A file lent to another process (the server answering a child, patch 0929). The
 * console counts a file once however many processes hold a descriptor of it, so
 * the copy costs nothing while the server's own descriptor is open; but a copy
 * still held when the server parks the file would keep the old open file (and
 * its position) alive beside the reopened one. The file is pinned instead of
 * counted in flight: *loan holds the pin until pw_wine_fdv_loan_end, which the
 * server calls at the receiving thread's next request, after the child has
 * closed its copy. A plain transfer stayed in flight for good, since no receiver
 * in this process ever claimed it, and such a file could never be parked. */
ssize_t pw_wine_fdv_sendmsg_loan(int socket, const struct msghdr *message, int flags, unsigned int *loan)
{
    const struct pw_wine_fdv *fdv = pw_wine_fdv;
    uint32_t pin = 0;
    ssize_t result;
    int saved;

    *loan = 0;
    if (fdv && message->msg_control && message->msg_controllen >= sizeof(struct cmsghdr)) {
        struct cmsghdr *header = CMSG_FIRSTHDR(message);

        if (header && header->cmsg_level == SOL_SOCKET && header->cmsg_type == SCM_RIGHTS &&
            header->cmsg_len == CMSG_LEN(sizeof(int))) {
            int fd;

            memcpy(&fd, CMSG_DATA(header), sizeof(fd));
            if (fdv->enter(fd, &pin)) return -1;
        } else {
            /* Not the one descriptor the server sends: the ordinary wrapper's accounting. */
            return __wrap_sendmsg(socket, message, flags);
        }
    }
    result = __real_sendmsg(socket, message, flags);
    saved = errno;
    if (result >= 0) *loan = pin;
    else leave(pin);
    errno = saved;
    return result;
}

void pw_wine_fdv_loan_end(unsigned int loan)
{
    leave(loan);
}
