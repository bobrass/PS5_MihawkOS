/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "../src/pw_memory_helper.h"
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static uint64_t now_ms(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000;
}

/* A memory helper that reads requests and never answers. */
struct silent { int fd; unsigned requests; };
static void *silent_helper(void *opaque)
{
    struct silent *silent = opaque;
    char request[64];
    while (recv(silent->fd, request, sizeof(request), 0) > 0) __atomic_add_fetch(&silent->requests, 1, __ATOMIC_RELAXED);
    return NULL;
}

int main(void)
{
    alarm(60);   /* a request that waits for good fails the test instead of hanging it */
    int pair[2];
    assert(!socketpair(AF_UNIX, SOCK_SEQPACKET, 0, pair));
    const pid_t helper = fork();
    assert(helper >= 0);
    if (!helper) {
        close(pair[0]);
        const unsigned long served = pw_memory_helper_serve(pair[1]);
        _exit(served == 12 ? 0 : 20 + (int)served);
    }
    close(pair[1]);
    const int control = pair[0];

    /* An object made by the helper: the asked size, zero, writable by the title. */
    const int fd = pw_memory_alloc(control, 65536);
    assert(fd >= 0);
    struct stat status;
    assert(!fstat(fd, &status) && status.st_size == 65536);
    unsigned char *map = mmap(NULL, 65536, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    assert(map != MAP_FAILED && map[0] == 0 && map[65535] == 0);
    map[0] = 0x5a;
    map[40000] = 0xa5;
    unsigned char byte = 0x62, readback = 0;
    assert(pwrite(fd, &byte, 1, 17) == 1);
    assert(map[17] == byte && pread(fd, &readback, 1, 17) == 1 && readback == byte);

    /* Touching keeps what is there. */
    assert(!pw_memory_touch(control, fd, 0, 65536));
    assert(map[0] == 0x5a && map[40000] == 0xa5);

    /* Touching beyond the end grows the object: the new part is zero, the old part kept. */
    assert(!pw_memory_touch(control, fd, 65536, 131072));
    assert(!fstat(fd, &status) && status.st_size == 131072);
    unsigned char *grown = mmap(NULL, 131072, PROT_READ, MAP_SHARED, fd, 0);
    assert(grown != MAP_FAILED && grown[0] == 0x5a && grown[40000] == 0xa5 && grown[100000] == 0);
    munmap(grown, 131072);
    munmap(map, 65536);

    /* Errors come back as errno, and the helper goes on. */
    assert(pw_memory_touch(control, fd, 8192, 4096) == -1 && errno == EINVAL);
    assert(pw_memory_touch(control, -1, 0, 4096) == -1 && errno == EBADF);   /* not sent: no request */
    close(fd);
    const int second = pw_memory_alloc(control, 16384);
    assert(second >= 0);
    close(second);

    /* Explicit memory-only requests use the same SCM_RIGHTS protocol and keep
     * coherence/growth. Ordinary requests must retain their byte-I/O contract. */
    const int shared = pw_memory_alloc_shared(control, 65536);
    assert(shared >= 0);
    map = mmap(NULL, 65536, PROT_READ | PROT_WRITE, MAP_SHARED, shared, 0);
    assert(map != MAP_FAILED && map[0] == 0 && map[65535] == 0);
    map[0] = 0x51;
    assert(!pw_memory_touch(control, shared, 65536, 131072));
    grown = mmap(NULL, 131072, PROT_READ, MAP_SHARED, shared, 0);
    assert(grown != MAP_FAILED && grown[0] == 0x51 && grown[100000] == 0);
    munmap(grown, 131072); munmap(map, 65536); close(shared);
    assert(pw_memory_alloc(control, 0) == -1 && errno == EINVAL);

    /* A host build takes no census (only the helper loader does, tools/build-payload-loader.sh): it says so, and goes
     * on serving. */
    assert(pw_memory_census_page(control, 1234) == -1 && errno == ENOSYS);

    /* Direct memory is the console's: a host build says so and goes on serving. */
    int64_t offset = -1;
    assert(pw_memory_dmem_alloc(control, 16384, 1, 2, &offset) == -1 && errno == ENOSYS);
    assert(pw_memory_dmem_free(control, 0, 16384) == -1 && errno == ENOSYS);
    assert(pw_memory_dmem_grant(control, 0, 16384, 3) == -1 && errno == ENOSYS);
    assert(pw_memory_dmem_grant(control, 0, 16384, 0) == -1 && errno == EINVAL);   /* not sent: no pid */
    assert(pw_memory_dmem_alloc(control, 16384, 1, 2, NULL) == -1 && errno == EINVAL);   /* not sent: no request */
    assert(pw_memory_stall(control, 60001) == -1 && errno == EINVAL);                    /* not sent: too long */
    assert(!pw_memory_dead(control));   /* a status the helper reports never marks the socket */

    /* The helper ends when the title's end closes, having answered every request. */
    close(control);
    int result = 0;
    assert(waitpid(helper, &result, 0) == helper && WIFEXITED(result));
    assert(WEXITSTATUS(result) == 0);

    /* The census page: nothing to read before the first census, then what was written; a copy torn by a write in
     * progress is not taken. */
    static struct pw_memory_census page;
    struct pw_memory_census census, read;
    assert(pw_memory_census_read(&page, &read) == -1 && errno == EAGAIN);
    memset(&census, 0, sizeof(census));
    census.magic = PW_MEMORY_CENSUS_MAGIC;
    census.taken_ms = 123456;
    census.system = 870ull << 20;
    census.helpers = 600ull << 20;
    census.largest = 300ull << 20;
    census.largest_pid = 222;
    census.processes = 90;
    census.helper_count = 6;
    census.helpers_mapped = 810ull << 20;
    census.page_tables = 8192;
    census.page_tables_free = 3600;
    pw_memory_census_write(&page, &census);
    assert(!pw_memory_census_read(&page, &read));
    assert(read.system == census.system && read.helpers == census.helpers && read.largest == census.largest &&
           read.largest_pid == 222 && read.processes == 90 && read.helper_count == 6 && read.taken_ms == 123456 &&
           read.status == 0 && read.sequence == 2);
    assert(read.helpers_mapped == 810ull << 20 && read.page_tables == 8192 && read.page_tables_free == 3600);
    /* Payloads: not started by SceSysCore (pid 30 here) and not "Sce..." (Sony's services, our helpers). */
    assert(pw_memory_is_payload(40, 30, "kstuff.elf") && pw_memory_is_payload(1, 30, "payload"));
    assert(pw_memory_is_payload(77, 30, "shadowmountplus.elf") && pw_memory_is_payload(1, 30, "web-file-mgr"));
    assert(!pw_memory_is_payload(40, 30, "SceSpZeroConf") && !pw_memory_is_payload(30, 30, "SceShellUI"));
    assert(!pw_memory_is_payload(30, 30, "orbis_audiod.elf") && !pw_memory_is_payload(30, 30, "eboot.bin"));
    assert(!pw_memory_is_payload(40, 30, "") && !pw_memory_is_payload(1, 30, NULL) && !pw_memory_is_payload(40, 0, "kstuff.elf"));
    census.system = 900ull << 20;
    pw_memory_census_write(&page, &census);
    assert(!pw_memory_census_read(&page, &read) && read.system == 900ull << 20 && read.sequence == 4);
    page.sequence = 5;   /* a write in progress */
    assert(pw_memory_census_read(&page, &read) == -1 && errno == EAGAIN);

    /* A helper that never answers: the request fails within the deadline and marks the socket dead, and every later
     * request fails at once without being sent. The dead socket stays open, as the title keeps it. */
    pw_memory_set_reply_ms(200);
    int quiet[2];
    assert(!socketpair(AF_UNIX, SOCK_SEQPACKET, 0, quiet));
    struct silent silent = { quiet[1], 0 };
    pthread_t listener;
    assert(!pthread_create(&listener, NULL, silent_helper, &silent));
    int64_t offset2 = -1;
    uint64_t started = now_ms();
    assert(pw_memory_dmem_alloc(quiet[0], 16384, 1, 2, &offset2) == -1 && errno == ETIMEDOUT);
    const uint64_t waited = now_ms() - started;
    assert(waited >= 150 && waited < 2000 && pw_memory_dead(quiet[0]));
    started = now_ms();
    assert(pw_memory_alloc(quiet[0], 65536) == -1 && errno == ENOTCONN);
    assert(pw_memory_dmem_free(quiet[0], 0, 16384) == -1 && errno == ENOTCONN);
    assert(now_ms() - started < 100);
    shutdown(quiet[1], SHUT_RDWR);
    pthread_join(listener, NULL);
    close(quiet[1]);
    assert(silent.requests == 1);

    /* The real helper, asked to sleep past the deadline: ETIMEDOUT and marked dead; its late answer stays unread and
     * nothing more is sent. */
    int slow_pair[2];
    assert(!socketpair(AF_UNIX, SOCK_SEQPACKET, 0, slow_pair));
    const pid_t slow = fork();
    assert(slow >= 0);
    if (!slow) {
        close(slow_pair[0]);
        _exit(pw_memory_helper_serve(slow_pair[1]) == 1 ? 0 : 30);
    }
    close(slow_pair[1]);
    started = now_ms();
    assert(pw_memory_stall(slow_pair[0], 600) == -1 && errno == ETIMEDOUT && pw_memory_dead(slow_pair[0]));
    assert(now_ms() - started < 2000);
    usleep(800000);   /* the late answer is in the socket now */
    assert(pw_memory_alloc(slow_pair[0], 65536) == -1 && errno == ENOTCONN);
    /* Nothing the helper made is mapped here, so this dead socket may close: the helper answered only the stall. */
    close(slow_pair[0]);
    assert(waitpid(slow, &result, 0) == slow && WIFEXITED(result) && WEXITSTATUS(result) == 0);
    pw_memory_set_reply_ms(0);

    /* A helper that ended between requests: the send fails (EPIPE) and marks the socket dead, so the title's callers
     * see ENOTCONN and never make the helpers' memory themselves (patch 1126's ps5_touch, graphics patch 0012). */
    int gone[2];
    assert(!socketpair(AF_UNIX, SOCK_SEQPACKET, 0, gone));
    close(gone[1]);
    assert(pw_memory_alloc(gone[0], 65536) == -1 && errno == ENOTCONN && pw_memory_dead(gone[0]));
    assert(pw_memory_touch(gone[0], gone[0], 0, 65536) == -1 && errno == ENOTCONN);
    close(gone[0]);
    puts("pw_memory_helper: ok");
    return 0;
}
