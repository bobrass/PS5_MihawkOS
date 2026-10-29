/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pw_memory_helper.h"
#ifdef PW_MEMORY_HELPER_CENSUS
#include "pw_memory_file.h"
#define PW_MEMORY_SECTION_DIRECTORY "/data/ps5proton/run/shared-sections"
#endif
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#ifdef PW_MEMORY_HELPER_PHYSICAL
#include "../native/pw_physical_shared.h"
#endif
#ifdef PW_MEMORY_HELPER_DMEM
#include "../native/pw_dmem_share.h"
#endif

#define PW_MEMORY_MAGIC 0x504d4831u   /* "PMH1" */
enum { PW_MEMORY_ALLOC = 1, PW_MEMORY_TOUCH = 2, PW_MEMORY_CENSUS = 3, PW_MEMORY_ALLOC_SHARED = 4, PW_MEMORY_DMEM_ALLOC = 5,
       PW_MEMORY_DMEM_FREE = 6, PW_MEMORY_DMEM_GRANT = 7, PW_MEMORY_STALL = 8 };
struct pw_memory_request { uint32_t magic, kind; uint64_t from, to; };
struct pw_memory_reply { uint32_t magic, kind; int32_t status, reserved; uint64_t value; };

static int send_with_fd(int control, const void *data, size_t size, int fd)
{
    char space[CMSG_SPACE(sizeof(int))];
    struct iovec io = {(void *)data, size};
    struct msghdr message;
    memset(&message, 0, sizeof(message));
    memset(space, 0, sizeof(space));
    message.msg_iov = &io;
    message.msg_iovlen = 1;
    if (fd >= 0) {
        message.msg_control = space;
        message.msg_controllen = sizeof(space);
        struct cmsghdr *header = CMSG_FIRSTHDR(&message);
        header->cmsg_level = SOL_SOCKET;
        header->cmsg_type = SCM_RIGHTS;
        header->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(header), &fd, sizeof(fd));
    }
    ssize_t sent;
    do sent = sendmsg(control, &message, MSG_NOSIGNAL);
    while (sent < 0 && errno == EINTR);
    return sent == (ssize_t)size ? 0 : -1;
}

/* One message, and the descriptor it carries, if any (*fd -1 when none). 0 at the other end's close. */
static ssize_t receive_with_fd(int control, void *data, size_t size, int *fd)
{
    char space[CMSG_SPACE(sizeof(int))];
    struct iovec io = {data, size};
    struct msghdr message;
    memset(&message, 0, sizeof(message));
    message.msg_iov = &io;
    message.msg_iovlen = 1;
    message.msg_control = space;
    message.msg_controllen = sizeof(space);
    *fd = -1;
    ssize_t got;
    do got = recvmsg(control, &message, 0);
    while (got < 0 && errno == EINTR);
    for (struct cmsghdr *header = got > 0 ? CMSG_FIRSTHDR(&message) : NULL; header;
         header = CMSG_NXTHDR(&message, header)) {
        if (header->cmsg_level != SOL_SOCKET || header->cmsg_type != SCM_RIGHTS) continue;
        int received;
        memcpy(&received, CMSG_DATA(header), sizeof(received));
        if (*fd < 0) *fd = received;
        else close(received);
    }
    return got;
}

static int make_object(uint64_t bytes)
{
    if (!bytes || bytes > INT64_MAX) { errno = EINVAL; return -1; }
    int fd;
#ifdef SHM_ANON
    fd = shm_open(SHM_ANON, O_RDWR, 0600);
#else
    /* A host build (the tests): a named object, unlinked at once. */
    static unsigned serial;
    char name[64];
    snprintf(name, sizeof(name), "/pw-memory-%d-%u", (int)getpid(), ++serial);
    fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd >= 0) shm_unlink(name);
#endif
    if (fd < 0) goto fallback;
    if (ftruncate(fd, (off_t)bytes)) {
        const int saved = errno;
        close(fd);
        errno = saved;
        goto fallback;
    }
    return fd;
fallback:
#ifdef PW_MEMORY_HELPER_CENSUS
    (void)pw_memory_file_collect(PW_MEMORY_SECTION_DIRECTORY);
    return pw_memory_file_alloc(PW_MEMORY_SECTION_DIRECTORY, bytes);
#else
    return -1;
#endif
}

static int make_shared_object(uint64_t bytes)
{
#ifdef PW_MEMORY_HELPER_PHYSICAL
    if (bytes > SIZE_MAX) { errno = EINVAL; return -1; }
    int fd = pw_physical_shared_create((size_t)bytes);
    if (fd >= 0) return fd;
    static int logged;
    if (!logged++) fprintf(stderr, "wine-ps5: physical shared allocation refused: errno %d; ordinary backing retained\n", errno);
#endif
    return make_object(bytes);
}

/* Grow to `to` if needed, then read and write back one byte in every page of [from, to). */
static int touch_object(int fd, uint64_t from, uint64_t to)
{
    struct stat status;
    const long page = sysconf(_SC_PAGESIZE) > 0 ? sysconf(_SC_PAGESIZE) : 4096;
    if (from > to || fstat(fd, &status)) {
        errno = EINVAL;
        return -1;
    }
    if ((uint64_t)status.st_size < to && ftruncate(fd, (off_t)to)) return -1;
    const uint64_t start = from & ~(uint64_t)(page - 1);
    if (start == to) return 0;
    volatile unsigned char *map = mmap(NULL, (size_t)(to - start), PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)start);
    if (map == MAP_FAILED) return -1;
    for (uint64_t at = 0; at < to - start; at += (uint64_t)page) map[at] = map[at];
    munmap((void *)map, (size_t)(to - start));
    return 0;
}

/* The census page: the writer keeps sequence odd while it writes, so a reader can tell a torn copy and read again. */
void pw_memory_census_write(volatile struct pw_memory_census *page, const struct pw_memory_census *census)
{
    const uint32_t sequence = __atomic_load_n(&page->sequence, __ATOMIC_RELAXED);
    __atomic_store_n(&page->sequence, sequence + 1, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    page->taken_ms = census->taken_ms;
    page->system = census->system;
    page->helpers = census->helpers;
    page->largest = census->largest;
    page->largest_pid = census->largest_pid;
    page->processes = census->processes;
    page->helper_count = census->helper_count;
    page->status = census->status;
    page->page_tables = census->page_tables;
    page->page_tables_free = census->page_tables_free;
    page->helpers_mapped = census->helpers_mapped;
    page->payloads = census->payloads;
    page->payload_count = census->payload_count;
    page->magic = census->magic;
    __atomic_store_n(&page->sequence, sequence + 2, __ATOMIC_RELEASE);
}

int pw_memory_census_read(const volatile struct pw_memory_census *page, struct pw_memory_census *census)
{
    for (int attempt = 0; attempt < 16; ++attempt) {
        const uint32_t before = __atomic_load_n(&page->sequence, __ATOMIC_ACQUIRE);
        if (before & 1) continue;
        census->magic = page->magic;
        census->taken_ms = page->taken_ms;
        census->system = page->system;
        census->helpers = page->helpers;
        census->largest = page->largest;
        census->largest_pid = page->largest_pid;
        census->processes = page->processes;
        census->helper_count = page->helper_count;
        census->status = page->status;
        census->page_tables = page->page_tables;
        census->page_tables_free = page->page_tables_free;
        census->helpers_mapped = page->helpers_mapped;
        census->payloads = page->payloads;
        census->payload_count = page->payload_count;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (__atomic_load_n(&page->sequence, __ATOMIC_RELAXED) != before) continue;
        if (census->magic != PW_MEMORY_CENSUS_MAGIC) break;
        census->sequence = before;
        return 0;
    }
    errno = EAGAIN;
    return -1;
}

#ifdef PW_MEMORY_HELPER_CENSUS
/* Only the helper loader takes the census (tools/build-payload-loader.sh): a helper may list every process and ask
 * each one's figure, and nothing else that links this file imports the call. */
#include <pthread.h>
#include <stdlib.h>
#include <sys/sysctl.h>
#include <sys/user.h>
#include <time.h>

/* Two counts in 16 KiB pages, both written: what the process holds resident, and what it has mapped (SceShellCore's
 * FMEM pair). Without the second pointer it refuses (EFAULT) and writes neither (console-memory-2026-10-07-07). */
int sceKernelGetResidentFmemCount(int pid, uint64_t *resident, uint64_t *mapped);
/* The pool SceShellCore's VM Stats line prints as "page table CPU used/total" (8192 on the base PS5, 9216 on the Pro). */
int sceKernelGetPageTableStats(int *cpu_total, int *cpu_free, int *gpu_total, int *gpu_free);

static int census_take(int title_pid, struct pw_memory_census *census)
{
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PROC, 0};
    const uint64_t page = (uint64_t)sysconf(_SC_PAGESIZE);
    const int parent = (int)getppid();
    size_t bytes = 0;
    char *raw;

    if (sysctl(mib, 4, NULL, &bytes, NULL, 0) || !bytes) return -1;
    bytes += bytes / 4;   /* room for the processes started since */
    if (!(raw = malloc(bytes))) return -1;
    if (sysctl(mib, 4, raw, &bytes, NULL, 0)) {
        const int saved = errno;
        free(raw);
        errno = saved;
        return -1;
    }
    census->system = census->helpers = census->largest = census->helpers_mapped = census->payloads = 0;
    census->payload_count = 0;
    /* SceSysCore's pid first: what it starts is Sony's (pw_memory_is_payload). */
    int syscore = 0;
    for (const char *cursor = raw; cursor + sizeof(int) <= raw + bytes;) {
        struct kinfo_proc record;
        int size;
        memcpy(&size, cursor, sizeof(size));
        if (size <= 0 || cursor + size > raw + bytes) break;
        memset(&record, 0, sizeof(record));
        memcpy(&record, cursor, (size_t)size < sizeof(record) ? (size_t)size : sizeof(record));
        cursor += size;
        if (!strncmp(record.ki_comm, "SceSysCore", 10)) syscore = record.ki_pid;
    }
    census->largest_pid = 0;
    census->processes = census->helper_count = 0;
    /* The console's records are ki_structsize apart (1096 bytes, the SDK's 1088); the fields read come first
     * (tools/console-processes.c). */
    for (const char *cursor = raw; cursor + sizeof(int) <= raw + bytes;) {
        struct kinfo_proc record;
        uint64_t pages = 0, mapped = 0;
        int size;

        memcpy(&size, cursor, sizeof(size));
        if (size <= 0 || cursor + size > raw + bytes) break;
        memset(&record, 0, sizeof(record));
        memcpy(&record, cursor, (size_t)size < sizeof(record) ? (size_t)size : sizeof(record));
        cursor += size;
        /* One that ended since the listing has no figure, and is left out. */
        if (record.ki_pid == title_pid || sceKernelGetResidentFmemCount(record.ki_pid, &pages, &mapped)) continue;
        census->system += pages * page;
        census->processes++;
        if (record.ki_pid != syscore && pw_memory_is_payload(record.ki_ppid, syscore, record.ki_comm)) {
            census->payloads += pages * page;
            census->payload_count++;
        }
        if (record.ki_ppid == parent) {
            census->helpers += pages * page;
            census->helpers_mapped += mapped * page;
            census->helper_count++;
            if (pages * page > census->largest) {
                census->largest = pages * page;
                census->largest_pid = record.ki_pid;
            }
        }
    }
    free(raw);
    return 0;
}

/* As the kernel gave them: the title decides what a reading is worth (pw_page_table_known) and names a bad one. */
static void census_page_tables(struct pw_memory_census *census)
{
    int total = 0, spare = 0, gpu_total = 0, gpu_free = 0;
    const int tables = !sceKernelGetPageTableStats(&total, &spare, &gpu_total, &gpu_free) && total > 0;
    census->page_tables = tables ? (uint32_t)total : 0;
    census->page_tables_free = tables ? (uint32_t)spare : 0;
}

struct census_state { volatile struct pw_memory_census *page; int title_pid; };

static void *census_loop(void *opaque)
{
    const struct census_state *state = opaque;
    struct pw_memory_census census;

    memset(&census, 0, sizeof(census));
    /* The processes every half second, the page tables every 100 ms: a game's start took 200 of them a second
     * (re3-steam-base-2026-10-28), and their guard needs a reading each tenth of that. */
    for (unsigned beat = 0;; ++beat) {
        if (beat % 5 == 0) {
            struct pw_memory_census fresh = census;
            struct timespec now;

            if (census_take(state->title_pid, &fresh)) {
                census.status = errno ? errno : EIO;
            } else {
                clock_gettime(CLOCK_MONOTONIC, &now);
                census = fresh;
                census.taken_ms = (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000;
                census.status = 0;
                census.magic = PW_MEMORY_CENSUS_MAGIC;
            }
        }
        census_page_tables(&census);
        if (census.magic == PW_MEMORY_CENSUS_MAGIC) pw_memory_census_write(state->page, &census);
        usleep(100000);
    }
    return NULL;
}

/* The page, made and its writer started at the first request; each request gets its own descriptor of it. */
static int census_page(int title_pid)
{
    static struct census_state state;
    static int object = -1;

    if (object < 0) {
        const size_t bytes = (size_t)sysconf(_SC_PAGESIZE);
        pthread_attr_t attributes;
        pthread_t thread;
        void *map;
        int fd, error;

        if ((fd = make_shared_object(bytes)) < 0) return -1;
        map = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (map == MAP_FAILED) {
            const int saved = errno;
            close(fd);
            errno = saved;
            return -1;
        }
        state.page = map;
        state.title_pid = title_pid;
        pthread_attr_init(&attributes);
        pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
        pthread_attr_setstacksize(&attributes, 256u << 10);
        /* The scheduling the loader gave the helper's serving thread, whatever libkernel's default for a new thread is. */
        pthread_attr_setinheritsched(&attributes, PTHREAD_INHERIT_SCHED);
        error = pthread_create(&thread, &attributes, census_loop, &state);
        pthread_attr_destroy(&attributes);
        if (error) {
            munmap(map, bytes);
            close(fd);
            errno = error;
            return -1;
        }
        object = fd;
    }
    return dup(object);
}
#else
static int census_page(int title_pid)
{
    (void)title_pid;
    errno = ENOSYS;
    return -1;
}
#endif

unsigned long pw_memory_helper_serve(int control)
{
    unsigned long served = 0;
#ifdef PW_MEMORY_HELPER_CENSUS
    (void)pw_memory_file_collect(PW_MEMORY_SECTION_DIRECTORY);
#endif
    for (;;) {
        struct pw_memory_request request;
        int fd;
        const ssize_t got = receive_with_fd(control, &request, sizeof(request), &fd);
        if (got <= 0) break;
        struct pw_memory_reply reply = {PW_MEMORY_MAGIC, 0, EINVAL, 0, 0};
        int answer = -1;
        if (got == (ssize_t)sizeof(request) && request.magic == PW_MEMORY_MAGIC) {
            reply.kind = request.kind;
            if (request.kind == PW_MEMORY_ALLOC && fd < 0) {
                answer = make_object(request.to);
                reply.status = answer < 0 ? errno : 0;
            } else if (request.kind == PW_MEMORY_ALLOC_SHARED && fd < 0) {
                answer = make_shared_object(request.to);
                reply.status = answer < 0 ? errno : 0;
            } else if (request.kind == PW_MEMORY_TOUCH && fd >= 0) {
                reply.status = touch_object(fd, request.from, request.to) ? errno : 0;
            } else if (request.kind == PW_MEMORY_DMEM_ALLOC && fd < 0) {
#ifdef PW_MEMORY_HELPER_DMEM
                int64_t offset = -1;
                if (pw_dmem_share_alloc(request.from, (int)(request.to >> 32), (int)(uint32_t)request.to, &offset)) reply.status = errno;
                else { reply.status = 0; reply.value = (uint64_t)offset; }
#else
                reply.status = ENOSYS;
#endif
            } else if (request.kind == PW_MEMORY_DMEM_GRANT && fd < 0) {
#ifdef PW_MEMORY_HELPER_DMEM
                reply.status = pw_dmem_share_grant((int64_t)request.from, (uint32_t)request.to, (int)(request.to >> 32)) ? errno : 0;
#else
                reply.status = ENOSYS;
#endif
            } else if (request.kind == PW_MEMORY_DMEM_FREE && fd < 0) {
#ifdef PW_MEMORY_HELPER_DMEM
                reply.status = pw_dmem_share_free((int64_t)request.from, request.to) ? errno : 0;
#else
                reply.status = ENOSYS;
#endif
            } else if (request.kind == PW_MEMORY_CENSUS && fd < 0) {
                answer = census_page((int)request.from);
                reply.status = answer < 0 ? errno : 0;
            } else if (request.kind == PW_MEMORY_STALL && fd < 0) {
                const uint64_t ms = request.to < 60000 ? request.to : 60000;
                struct timespec left = {(time_t)(ms / 1000), (long)(ms % 1000) * 1000000};
                while (nanosleep(&left, &left) && errno == EINTR) {}
                reply.status = 0;
            }
        }
        if (fd >= 0) close(fd);
        const int failed = send_with_fd(control, &reply, sizeof(reply), answer);
        if (answer >= 0) close(answer);
        if (failed) break;
        ++served;
    }
#ifdef PW_MEMORY_HELPER_CENSUS
    (void)pw_memory_file_collect(PW_MEMORY_SECTION_DIRECTORY);
#endif
    return served;
}

/* Sockets marked dead (see the header), as control + 1; 0 is a free slot.
 * shortcut: 8 slots, for the one socket the title has; a ninth dead socket is refused only for the call that found it
 * dead; grow it if a process ever holds more helper sockets. */
static int dead_sockets[8];
static int reply_ms = PW_MEMORY_REPLY_MS;

int pw_memory_dead(int control)
{
    if (control < 0) return 0;
    for (unsigned i = 0; i < sizeof(dead_sockets) / sizeof(dead_sockets[0]); ++i)
        if (__atomic_load_n(&dead_sockets[i], __ATOMIC_ACQUIRE) == control + 1) return 1;
    return 0;
}

static void mark_dead(int control)
{
    for (unsigned i = 0; i < sizeof(dead_sockets) / sizeof(dead_sockets[0]); ++i) {
        int free_slot = 0;
        if (__atomic_compare_exchange_n(&dead_sockets[i], &free_slot, control + 1, 0, __ATOMIC_RELEASE, __ATOMIC_RELAXED))
            return;
    }
}

void pw_memory_set_reply_ms(int milliseconds) { reply_ms = milliseconds > 0 ? milliseconds : PW_MEMORY_REPLY_MS; }

/* 1 once control is readable (an answer, or the helper's close), 0 with errno ETIMEDOUT after milliseconds. */
static int readable_within(int control, int milliseconds)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    const int64_t deadline = (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000 + milliseconds;
    for (;;) {
        clock_gettime(CLOCK_MONOTONIC, &now);
        const int64_t left = deadline - ((int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000);
        struct pollfd wait = {control, POLLIN, 0};
        const int ready = poll(&wait, 1, left > 0 ? (int)left : 0);
        if (ready > 0) return 1;
        if (ready == 0 || errno != EINTR) {
            errno = ETIMEDOUT;
            return 0;
        }
    }
}

static int ask_value(int control, uint32_t kind, int fd, uint64_t from, uint64_t to, int *received, uint64_t *value)
{
    const struct pw_memory_request request = {PW_MEMORY_MAGIC, kind, from, to};
    struct pw_memory_reply reply;
    int answer;
    if (pw_memory_dead(control)) {
        errno = ENOTCONN;
        return -1;
    }
    if (send_with_fd(control, &request, sizeof(request), fd)) {
        /* The helper's end is gone (it ended between requests): dead, as for a missed deadline. Not EBADF, which a bad
         * carried descriptor gives with the helper still alive. */
        if (errno == EPIPE || errno == ECONNRESET || errno == ENOTCONN) {
            mark_dead(control);
            errno = ENOTCONN;
        }
        return -1;
    }
    if (!readable_within(control, kind == PW_MEMORY_CENSUS ? PW_MEMORY_FIRST_REPLY_MS : reply_ms)) {
        mark_dead(control);
        return -1;
    }
    if (receive_with_fd(control, &reply, sizeof(reply), &answer) != (ssize_t)sizeof(reply) ||
        reply.magic != PW_MEMORY_MAGIC || reply.kind != kind) {
        if (answer >= 0) close(answer);
        mark_dead(control);
        errno = EPROTO;
        return -1;
    }
    if (reply.status) {
        if (answer >= 0) close(answer);
        errno = reply.status;
        return -1;
    }
    if (value) *value = reply.value;
    if (received) *received = answer;
    else if (answer >= 0) close(answer);
    return 0;
}

static int ask(int control, uint32_t kind, int fd, uint64_t from, uint64_t to, int *received)
{
    return ask_value(control, kind, fd, from, to, received, NULL);
}

int pw_memory_alloc(int control, uint64_t bytes)
{
    int fd = -1;
    if (ask(control, PW_MEMORY_ALLOC, -1, 0, bytes, &fd)) return -1;
    if (fd < 0) errno = EPROTO;
    return fd;
}

int pw_memory_alloc_shared(int control, uint64_t bytes)
{
    int fd = -1;
    if (ask(control, PW_MEMORY_ALLOC_SHARED, -1, 0, bytes, &fd)) return -1;
    if (fd < 0) errno = EPROTO;
    return fd;
}

int pw_memory_touch(int control, int fd, uint64_t from, uint64_t to)
{
    if (fd < 0) {
        errno = EBADF;
        return -1;
    }
    return ask(control, PW_MEMORY_TOUCH, fd, from, to, NULL);
}

int pw_memory_census_page(int control, int title_pid)
{
    int fd = -1;
    if (ask(control, PW_MEMORY_CENSUS, -1, (uint64_t)title_pid, 0, &fd)) return -1;
    if (fd < 0) errno = EPROTO;
    return fd;
}

int pw_memory_dmem_alloc(int control, uint64_t bytes, int pid_a, int pid_b, int64_t *offset)
{
    uint64_t value = 0;
    if (!offset) {
        errno = EINVAL;
        return -1;
    }
    if (ask_value(control, PW_MEMORY_DMEM_ALLOC, -1, bytes, ((uint64_t)(uint32_t)pid_a << 32) | (uint32_t)pid_b, NULL, &value))
        return -1;
    *offset = (int64_t)value;
    return 0;
}

int pw_memory_dmem_grant(int control, int64_t offset, uint64_t bytes, int pid)
{
    if (pid <= 0 || !bytes || bytes > UINT32_MAX) {
        errno = EINVAL;
        return -1;
    }
    return ask(control, PW_MEMORY_DMEM_GRANT, -1, (uint64_t)offset, ((uint64_t)(uint32_t)pid << 32) | bytes, NULL);
}

int pw_memory_dmem_free(int control, int64_t offset, uint64_t bytes)
{
    return ask(control, PW_MEMORY_DMEM_FREE, -1, (uint64_t)offset, bytes, NULL);
}

int pw_memory_stall(int control, uint32_t milliseconds)
{
    if (milliseconds > 60000) {
        errno = EINVAL;
        return -1;
    }
    return ask(control, PW_MEMORY_STALL, -1, 0, milliseconds, NULL);
}
