/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Native speeds on the console, with no Wine in the way, for comparing with tests/net-throughput-probe.c (a game
 * launcher's download ran at 2.5 MB/s on a 1 Gbps line, 2026-10-11): a payload sent through websrv's launcher
 * (tools/console-iobench.py), run only while no title runs (a payload tool during a title's run jams elfldr).
 *
 *   console-iobench HOST PORT [DIR]
 *
 * It receives from a sender at HOST:PORT (tools/run-net-throughput.py's, which sends zeros) for 4 s with 64 KiB and
 * with 1 MiB reads, then writes 128 MiB to DIR (default /data/ps5proton) with 64 KiB, 1 MiB and 8 MiB writes, each
 * followed by fsync, and reads it back. MB/s are decimal megabytes. */
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/param.h>
#include <sys/mount.h>
#include <pthread.h>
#include <signal.h>
#include <setjmp.h>

static void line(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
    printf("\n");
    fflush(stdout);
}

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

static void receive(const char *host, int port, size_t block, double seconds)
{
    struct sockaddr_in address = {0};
    int s = socket(AF_INET, SOCK_STREAM, 0);
    char *buffer = malloc(block);
    double total = 0, begin;
    address.sin_family = AF_INET;
    address.sin_port = htons((uint16_t)port);
    address.sin_addr.s_addr = inet_addr(host);
    if (s < 0 || !buffer || connect(s, (struct sockaddr *)&address, sizeof(address))) {
        line("recv %zu: connect failed errno=%d", block, errno);
        if (s >= 0) close(s);
        free(buffer);
        return;
    }
    begin = now();
    while (now() - begin < seconds) {
        ssize_t got = recv(s, buffer, block, 0);
        if (got <= 0) break;
        total += (double)got;
    }
    line("recv block=%zu KiB: %.1f MB/s", block >> 10, total / (now() - begin) / 1e6);
    close(s);
    free(buffer);
}

/* total bytes written in `block` writes to DIR/iobench.tmp; preallocated: the file is sized first (ftruncate), so a
 * write does not extend it; sync_each: fsync after every write. */
static void disk(const char *directory, size_t block, size_t total, int preallocated, int sync_each)
{
    char path[512];
    char *buffer = calloc(1, block);
    double begin, write_seconds, read_seconds;
    unsigned writes = 0;
    int fd;
    snprintf(path, sizeof(path), "%s/iobench.tmp", directory);
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0 || !buffer) {
        line("write block=%zu KiB in %s: open failed errno=%d", block >> 10, directory, errno);
        free(buffer);
        return;
    }
    if (preallocated && ftruncate(fd, (off_t)total)) line("ftruncate failed errno=%d", errno);
    begin = now();
    for (size_t done = 0; done < total; done += block) {
        if (write(fd, buffer, block) != (ssize_t)block) { line("write failed errno=%d", errno); break; }
        writes++;
        if (sync_each) fsync(fd);
    }
    fsync(fd);
    write_seconds = now() - begin;
    close(fd);
    fd = open(path, O_RDONLY);
    begin = now();
    if (fd >= 0) {
        while (read(fd, buffer, block) > 0) {}
        close(fd);
    }
    read_seconds = now() - begin;
    line("disk %s block=%zu KiB total=%zu MiB%s%s: write %.1f MB/s (%.2f ms per write), read back %.1f MB/s", directory,
         block >> 10, total >> 20, preallocated ? " preallocated" : " growing", sync_each ? " fsync-each" : "",
         (double)total / write_seconds / 1e6, write_seconds * 1000.0 / (writes ? writes : 1),
         (double)total / read_seconds / 1e6);
    unlink(path);
    free(buffer);
}


/* Writing through a shared mapping of a sized file, flushed once at the end. */
static void mapped(const char *directory, size_t block, size_t total)
{
    char path[512];
    double begin, seconds;
    snprintf(path, sizeof(path), "%s/iobench.tmp", directory);
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0 || ftruncate(fd, (off_t)total)) { line("mapped: open or size failed errno=%d", errno); if (fd >= 0) close(fd); return; }
    char *map = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) { line("mapped: mmap failed errno=%d", errno); close(fd); unlink(path); return; }
    char *buffer = calloc(1, block);
    begin = now();
    for (size_t done = 0; done < total; done += block) memcpy(map + done, buffer, block);
    double copied = now() - begin;
    msync(map, total, MS_SYNC);
    seconds = now() - begin;
    line("mapped %s block=%zu KiB total=%zu MiB: copy %.1f MB/s, with msync %.1f MB/s", directory, block >> 10, total >> 20,
         (double)total / copied / 1e6, (double)total / seconds / 1e6);
    munmap(map, total);
    close(fd);
    unlink(path);
    free(buffer);
}

struct writer { const char *directory; int index; size_t block, total; double seconds; };

static void *writer_main(void *argument)
{
    struct writer *w = argument;
    char path[512];
    char *buffer = calloc(1, w->block);
    snprintf(path, sizeof(path), "%s/iobench-%d.tmp", w->directory, w->index);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    double begin = now();
    for (size_t done = 0; fd >= 0 && done < w->total; done += w->block)
        if (write(fd, buffer, w->block) != (ssize_t)w->block) break;
    if (fd >= 0) { fsync(fd); close(fd); }
    w->seconds = now() - begin;
    unlink(path);
    free(buffer);
    return NULL;
}

static void parallel(const char *directory, size_t block, size_t each, int threads)
{
    pthread_t id[8];
    struct writer w[8];
    double begin = now();
    for (int i = 0; i < threads; i++) {
        w[i] = (struct writer){directory, i, block, each, 0};
        pthread_create(&id[i], NULL, writer_main, &w[i]);
    }
    for (int i = 0; i < threads; i++) pthread_join(id[i], NULL);
    line("parallel %d writers, block=%zu KiB, %zu MiB each: %.1f MB/s together", threads, block >> 10, each >> 20,
         (double)threads * (double)each / (now() - begin) / 1e6);
}


static void filesystem(const char *directory)
{
    struct statfs info;
    if (statfs(directory, &info)) { line("statfs %s failed errno=%d", directory, errno); return; }
    line("fs %s: type=%s from=%s flags=%#llx iosize=%llu bsize=%llu blocks=%llu bfree=%lld bavail=%lld = %llu MiB available",
         directory, info.f_fstypename, info.f_mntfromname, (unsigned long long)info.f_flags,
         (unsigned long long)info.f_iosize, (unsigned long long)info.f_bsize, (unsigned long long)info.f_blocks,
         (long long)info.f_bfree, (long long)info.f_bavail,
         (unsigned long long)(info.f_bavail > 0 ? (unsigned long long)info.f_bavail * info.f_bsize >> 20 : 0));
}

/* 4 KiB writes opened with different flags. */
static void flags(const char *directory, int extra_flags, const char *label)
{
    char path[512];
    char buffer[4096] = {0};
    double begin;
    snprintf(path, sizeof(path), "%s/iobench.tmp", directory);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | extra_flags, 0644);
    if (fd < 0) { line("flags %s: open failed errno=%d", label, errno); return; }
    begin = now();
    for (int i = 0; i < 2048; i++) if (write(fd, buffer, sizeof(buffer)) != (ssize_t)sizeof(buffer)) break;
    double seconds = now() - begin;
    close(fd);
    unlink(path);
    line("flags %s: 8 MiB in 4 KiB writes: %.1f MB/s (%.2f ms per write)", label, 8.0 * 1048576 / seconds / 1e6, seconds * 1000 / 2048);
}


/* The mapped-write design: append `block` bytes at a time to a file by extending it with ftruncate and copying into a
 * shared mapping of a window that is moved as the file grows. Reports the cost of each part. */
static void mapped_append(const char *directory, size_t block, size_t total, size_t window)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/iobench.tmp", directory);
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    char *buffer = malloc(block);
    char *map = NULL;
    size_t map_start = 0, size = 0;
    double truncating = 0, copying = 0, mapping = 0, begin = now(), t;
    unsigned mappings = 0;
    if (fd < 0 || !buffer) { line("mapped_append: open failed errno=%d", errno); return; }
    memset(buffer, 0x5a, block);
    while (size < total) {
        t = now();
        if (ftruncate(fd, (off_t)(size + block))) { line("ftruncate failed errno=%d", errno); break; }
        truncating += now() - t;
        t = now();
        if (!map || size + block > map_start + window) {
            if (map) munmap(map, window);
            map_start = size & ~(size_t)16383;
            map = mmap(NULL, window, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)map_start);
            mappings++;
            if (map == MAP_FAILED) { line("mmap failed errno=%d", errno); map = NULL; break; }
        }
        mapping += now() - t;
        t = now();
        memcpy(map + (size - map_start), buffer, block);
        copying += now() - t;
        size += block;
    }
    double done = now() - begin;
    if (map) munmap(map, window);
    t = now();
    fsync(fd);
    double synced = now() - t;
    line("mapped_append block=%zu KiB window=%zu MiB total=%zu MiB: %.1f MB/s without the final flush (%.2f ms ftruncate, "
         "%.2f ms mmap, %.3f ms copy per write; %u mappings); final fsync %.2f s", block >> 10, window >> 20, total >> 20,
         (double)size / done / 1e6, truncating * 1000.0 / (double)(size / block), mapping * 1000.0 / (double)(size / block),
         copying * 1000.0 / (double)(size / block), mappings, synced);
    close(fd);
    unlink(path);
    free(buffer);
}

/* What another opener sees of a mapped write that has not been flushed: its size and bytes through a second descriptor
 * (and through pread), right after, with no msync; and the mapping seeing a pwrite from another descriptor. */
static void coherence(const char *directory)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/iobench.tmp", directory);
    int writer = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    int reader = open(path, O_RDONLY);
    size_t block = 65536;
    char *buffer = malloc(block), *check = malloc(block);
    int sees_size = 0, sees_bytes = 0, map_sees_pwrite = 0, read_after_shrink = 1;
    if (writer < 0 || reader < 0) { line("coherence: open failed errno=%d", errno); return; }
    for (size_t i = 0; i < block; i++) buffer[i] = (char)(i * 7 + 1);
    ftruncate(writer, (off_t)(2 * block));
    char *map = mmap(NULL, 2 * block, PROT_READ | PROT_WRITE, MAP_SHARED, writer, 0);
    if (map == MAP_FAILED) { line("coherence: mmap failed errno=%d", errno); return; }
    memcpy(map, buffer, block);                       /* no msync */
    struct stat st;
    sees_size = !fstat(reader, &st) && st.st_size == (off_t)(2 * block);
    memset(check, 0, block);
    sees_bytes = pread(reader, check, block, 0) == (ssize_t)block && !memcmp(check, buffer, block);
    /* a pwrite through another descriptor, seen through the mapping */
    memset(buffer, 0x33, block);
    pwrite(reader == writer ? writer : open(path, O_WRONLY), buffer, block, (off_t)block);
    map_sees_pwrite = !memcmp(map + block, buffer, block);
    line("coherence %s: reader sees the size %s, the bytes %s; the mapping sees another descriptor's pwrite %s", directory,
         sees_size ? "yes" : "NO", sees_bytes ? "yes" : "NO", map_sees_pwrite ? "yes" : "NO");
    (void)read_after_shrink;
    munmap(map, 2 * block);
    close(writer);
    close(reader);
    unlink(path);
    free(buffer);
    free(check);
}

/* Many small files, one per download piece: create, write 1 MiB in 64 KiB blocks, close; plain writes against mapped. */
static void many_files(const char *directory, int mapped_way, unsigned files)
{
    char path[512];
    char *buffer = calloc(1, 65536);
    double begin = now();
    for (unsigned f = 0; f < files; f++) {
        snprintf(path, sizeof(path), "%s/iobench-%u.tmp", directory, f);
        int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) break;
        if (!mapped_way) {
            for (int i = 0; i < 16; i++) write(fd, buffer, 65536);
        } else {
            ftruncate(fd, 1u << 20);
            char *map = mmap(NULL, 1u << 20, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
            if (map != MAP_FAILED) { for (int i = 0; i < 16; i++) memcpy(map + i * 65536, buffer, 65536); munmap(map, 1u << 20); }
        }
        close(fd);
    }
    double seconds = now() - begin;
    line("many_files %s: %u files of 1 MiB in %.2f s = %.1f MB/s", mapped_way ? "mapped" : "write()", files, seconds,
         (double)files * 1048576.0 / seconds / 1e6);
    for (unsigned f = 0; f < files; f++) { snprintf(path, sizeof(path), "%s/iobench-%u.tmp", directory, f); unlink(path); }
    free(buffer);
}


/* Dirty pages in the cache: append `total` bytes through a mapping without flushing, wait, then flush and wait: the kernel's
 * VM Stats line in the klog (every 30 s) shows whether they count against the system's resident limit. */
static void pressure(const char *directory, size_t total)
{
    char path[512];
    char *buffer = calloc(1, 65536);
    snprintf(path, sizeof(path), "%s/iobench.tmp", directory);
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    size_t size = 0, window = 4u << 20, map_start = 0;
    char *map = NULL;
    if (fd < 0) { line("pressure: open failed errno=%d", errno); return; }
    line("pressure: appending %zu MiB through a mapping, no flush", total >> 20);
    double begin = now();
    while (size < total) {
        ftruncate(fd, (off_t)(size + 65536));
        if (!map || size + 65536 > map_start + window) {
            if (map) munmap(map, window);
            map_start = size & ~(size_t)16383;
            map = mmap(NULL, window, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)map_start);
        }
        memset(map + (size - map_start), 0x5a, 65536);
        size += 65536;
    }
    if (map) munmap(map, window);
    line("pressure: appended in %.2f s; dirty, waiting 35 s", now() - begin);
    sleep(35);
    begin = now();
    fsync(fd);
    line("pressure: fsync took %.2f s; waiting 35 s", now() - begin);
    sleep(35);
    close(fd);
    unlink(path);
    free(buffer);
    line("pressure: done");
}


static sigjmp_buf fault_jump;
static void fault_handler(int sig) { siglongjmp(fault_jump, sig); }

/* A volume with no room (/system_tmp is full): does extending the file fail up front, or does the page-out fail later? */
static void full_volume(const char *directory)
{
    char path[512];
    struct sigaction action = {0}, old_bus, old_segv;
    snprintf(path, sizeof(path), "%s/iobench-full.tmp", directory);
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { line("full %s: open failed errno=%d", directory, errno); return; }
    char one[1] = {1};
    ssize_t plain = write(fd, one, 1);
    line("full %s: a 1-byte write() gives %zd errno=%d", directory, plain, plain < 0 ? errno : 0);
    errno = 0;
    int extended = ftruncate(fd, 4u << 20);
    line("full %s: ftruncate to 4 MiB gives %d errno=%d", directory, extended, extended ? errno : 0);
    if (!extended) {
        char *map = mmap(NULL, 4u << 20, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (map == MAP_FAILED) line("full %s: mmap failed errno=%d", directory, errno);
        else {
            action.sa_handler = fault_handler;
            sigaction(SIGBUS, &action, &old_bus);
            sigaction(SIGSEGV, &action, &old_segv);
            int signal_seen = sigsetjmp(fault_jump, 1);
            if (!signal_seen) {
                for (size_t at = 0; at < (4u << 20); at += 16384) map[at] = 1;
                line("full %s: touching all 4 MiB of the mapping raised no signal", directory);
                errno = 0;
                int synced = msync(map, 4u << 20, MS_SYNC);
                line("full %s: msync gives %d errno=%d; fsync gives %d errno=%d", directory, synced, synced ? errno : 0,
                     fsync(fd), errno);
            } else line("full %s: touching the mapping raised signal %d (SIGBUS=%d)", directory, signal_seen, SIGBUS);
            sigaction(SIGBUS, &old_bus, NULL);
            sigaction(SIGSEGV, &old_segv, NULL);
            munmap(map, 4u << 20);
        }
    }
    close(fd);
    unlink(path);
}


static unsigned char pattern_byte(size_t o) { return (unsigned char)(o * 131u + (o >> 8) * 7u + 1u); }

/* Mapped appends of arbitrary sizes (the file ends in the middle of a page): does another descriptor read back what was
 * written? variant 0: ftruncate(exact end) before each copy; 1: extend only to the next 16 KiB boundary and set the exact
 * size afterwards, after the copy; 2: pwrite the last byte first, then copy. */
static void unaligned(const char *directory, int variant, unsigned writes)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/iobench.tmp", directory);
    int writer = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    int reader = open(path, O_RDONLY);
    size_t size = 0, window = 1u << 20, map_start = 0;
    char *map = NULL;
    unsigned long state = 12345, bad_writes = 0, bad_bytes = 0, first_bad = (unsigned long)-1;
    double began = now();
    static unsigned char chunk[70000], check[70000];
    if (writer < 0 || reader < 0) { line("unaligned: open failed"); return; }
    for (unsigned w = 0; w < writes; w++) {
        state = state * 1664525u + 1013904223u;
        size_t n = 1 + ((state >> 8) % 70000);
        for (size_t i = 0; i < n; i++) chunk[i] = pattern_byte(size + i);
        size_t end = size + n;
        if (variant == 1) {
            size_t round = (end + 16383) & ~(size_t)16383;
            if (round > size && ftruncate(writer, (off_t)round)) { line("ftruncate failed"); break; }
        } else if (variant == 2) {
            unsigned char z = 0;
            if (pwrite(writer, &z, 1, (off_t)(end - 1)) != 1) { line("pwrite failed"); break; }
        } else if (variant == 6) {
            /* allocate exactly the new range (and set the size) in one call, then copy into the mapping */
            int rc = posix_fallocate(writer, (off_t)size, (off_t)n);
            if (rc) { line("posix_fallocate failed rc=%d", rc); break; }
        } else if (variant >= 4) {
            /* ftruncate to the exact end, then one real byte at the end of every allocation unit (16 KiB: 4, 64 KiB: 5)
             * the write newly reaches, the last one at the exact end */
            size_t unit = variant == 4 ? 16384 : 65536;
            unsigned char z = 0;
            if (ftruncate(writer, (off_t)end)) { line("ftruncate failed"); break; }
            for (size_t u = size ? (size - 1) / unit + 1 : 0; u <= (end - 1) / unit; u++) {
                size_t at = (u + 1) * unit - 1;
                if (at > end - 1) at = end - 1;
                if (pwrite(writer, &z, 1, (off_t)at) != 1) { line("pwrite failed"); break; }
            }
        } else if (ftruncate(writer, (off_t)end)) { line("ftruncate failed"); break; }
        size_t done = 0;
        while (done < n) {
            size_t at = size + done;
            size_t start = at & ~(window - 1);
            if (!map || start != map_start) {
                if (map) munmap(map, window);
                map = mmap(NULL, window, PROT_READ | PROT_WRITE, MAP_SHARED, writer, (off_t)start);
                map_start = start;
                if (map == MAP_FAILED) { line("mmap failed errno=%d", errno); return; }
            }
            size_t part = window - (at - start);
            if (part > n - done) part = n - done;
            memcpy(map + (at - start), chunk + done, part);
            done += part;
        }
        if (variant == 1 && ftruncate(writer, (off_t)end)) { line("final ftruncate failed"); break; }
        if (variant == 3) msync(map + (size - map_start), n < window ? n : window, MS_ASYNC);
        size = end;
        struct stat st;
        fstat(reader, &st);
        ssize_t got = pread(reader, check, n, (off_t)(end - n));
        unsigned long wrong = 0;
        if (got != (ssize_t)n || st.st_size != (off_t)end) wrong = n;
        else for (size_t i = 0; i < n; i++) if (check[i] != chunk[i]) { wrong++; if (first_bad == (unsigned long)-1) first_bad = (unsigned long)(end - n + i); }
        if (wrong) { bad_writes++; bad_bytes += wrong; }
    }
    line("unaligned variant %d: %u writes, %lu wrong writes (%lu bytes), first wrong offset %ld, file %zu bytes, %.1f MB/s",
         variant, writes, bad_writes, bad_bytes, (long)first_bad, size, (double)size / (now() - began) / 1e6);
    if (map) munmap(map, window);
    close(writer);
    close(reader);
    unlink(path);
}


/* Several writers into ONE file at disjoint offsets (what several outstanding overlapped writes on one handle would be):
 * interleaved blocks, the file growing; `ordered` makes each thread wait its turn to start, so the writes begin in order. */
struct shared_writer { int fd; size_t block; unsigned blocks_each; unsigned thread, threads; };

static void *shared_writer_main(void *argument)
{
    struct shared_writer *w = argument;
    char *buffer = calloc(1, w->block);
    for (unsigned i = 0; i < w->blocks_each; i++) {
        off_t at = (off_t)(((size_t)i * w->threads + w->thread) * w->block);
        if (pwrite(w->fd, buffer, w->block, at) != (ssize_t)w->block) break;
    }
    free(buffer);
    return NULL;
}

static void parallel_one_file(const char *directory, size_t block, unsigned total_blocks, unsigned threads)
{
    char path[512];
    pthread_t id[16];
    struct shared_writer w[16];
    snprintf(path, sizeof(path), "%s/iobench.tmp", directory);
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    double begin = now();
    for (unsigned t = 0; t < threads; t++) {
        w[t] = (struct shared_writer){fd, block, total_blocks / threads, t, threads};
        pthread_create(&id[t], NULL, shared_writer_main, &w[t]);
    }
    for (unsigned t = 0; t < threads; t++) pthread_join(id[t], NULL);
    fsync(fd);
    double seconds = now() - begin;
    line("one file, %u writers, block=%zu KiB, %u MiB: %.1f MB/s", threads, block >> 10, (unsigned)((size_t)total_blocks * block >> 20),
         (double)total_blocks * (double)block / seconds / 1e6);
    close(fd);
    unlink(path);
}

static char selected[128] = "";
static int want(const char *name)
{
    /* a word is wanted when listed whole: "mapped" does not select "mapped_append" */
    if (!selected[0]) return 1;
    for (const char *at = selected; (at = strstr(at, name)); at += strlen(name))
        if ((at == selected || at[-1] == ',') && (at[strlen(name)] == 0 || at[strlen(name)] == ',')) return 1;
    return 0;
}

/* Name operations, 20 of each, per call in microseconds: what a Windows program's create, rename, delete, mkdir and
 * rmdir cost in Wine was 10-16 ms on the console against 30-70 us on the PC (tests/lock-probe.c split, 2026-10-25);
 * this is the file system's own share. */
static void names(const char *directory)
{
    double t[7] = {0}, begin;
    char path[512], moved[512];
    struct stat st;
    for (int i = 0; i < 20; i++) {
        snprintf(path, sizeof(path), "%s/iobench-name%02d.tmp", directory, i);
        snprintf(moved, sizeof(moved), "%s/iobench-name%02d.mov", directory, i);
        begin = now(); int fd = open(path, O_RDWR | O_CREAT | O_EXCL, 0644); t[0] += now() - begin;
        begin = now(); if (fd >= 0) close(fd); t[1] += now() - begin;
        begin = now(); rename(path, moved); t[2] += now() - begin;
        begin = now(); unlink(moved); t[3] += now() - begin;
        snprintf(path, sizeof(path), "%s/iobench-dir%02d", directory, i);
        begin = now(); mkdir(path, 0755); t[4] += now() - begin;
        begin = now(); rmdir(path); t[5] += now() - begin;
        snprintf(path, sizeof(path), "%s/iobench-missing%02d", directory, i);
        begin = now(); stat(path, &st); t[6] += now() - begin;
    }
    line("names %s: per call, us: create %.0f, close %.0f, rename %.0f, unlink %.0f, mkdir %.0f, rmdir %.0f, missing stat %.0f",
         directory, t[0] / 20 * 1e6, t[1] / 20 * 1e6, t[2] / 20 * 1e6, t[3] / 20 * 1e6, t[4] / 20 * 1e6, t[5] / 20 * 1e6,
         t[6] / 20 * 1e6);
}

int main(int argc, char **argv)
{
    char host[64] = "";
    int port = 0;
    const char *directory = "/data/ps5proton";
    /* websrv may pass the send's args as the whole argv, unsplit and with no program name first: take every word of
     * every argument, the host being the one with three dots, then the port, then the directory. */
    int stage = 0;
    for (int i = 0; i < argc; i++) {
        char text[512];
        if (!argv[i]) continue;
        snprintf(text, sizeof(text), "%s", argv[i]);
        for (char *word = strtok(text, " "); word; word = strtok(NULL, " ")) {
            if (stage == 0 && strchr(word, '.') && strchr(strchr(word, '.') + 1, '.') && word[0] != '/') {
                snprintf(host, sizeof(host), "%s", word);
                stage = 1;
            } else if (stage == 1 && word[0] >= '0' && word[0] <= '9') {
                port = atoi(word);
                stage = 2;
            } else if (stage == 2 && word[0] == '/') {
                directory = strdup(word);
                stage = 3;
            } else if (stage >= 2 && !strncmp(word, "only=", 5)) {
                snprintf(selected, sizeof(selected), "%s", word + 5);
            }
        }
    }
    line("console-iobench: host=%s port=%d dir=%s", host, port, directory);
    if (host[0] && port) {
        receive(host, port, 65536, 4.0);
        receive(host, port, 1u << 20, 4.0);
    }
    if (want("names")) {
        names(directory);
        names("/data/ps5proton/prefixes/probe/drive_c/users/Mihawk/AppData/Local/Temp");
        names("/data");
        names("/user/data");
    }
    if (want("fs")) { filesystem("/data/ps5proton"); filesystem("/data"); filesystem("/user/data"); filesystem("/"); }
    if (want("flags")) {
        flags(directory, 0, "default");
        flags(directory, O_APPEND, "O_APPEND");
        flags(directory, O_DIRECT, "O_DIRECT");
        flags(directory, O_SYNC, "O_SYNC");
    }
    if (want("mapped")) { mapped(directory, 4096, 32u << 20); mapped(directory, 65536, 32u << 20); }
    if (want("parallel")) {
        parallel(directory, 4096, 8u << 20, 1);
        parallel(directory, 4096, 8u << 20, 4);
        parallel(directory, 4096, 8u << 20, 8);
        parallel(directory, 65536, 16u << 20, 4);
    }
    if (want("mapped_append")) {
        mapped_append(directory, 65536, 64u << 20, 4u << 20);
        mapped_append(directory, 65536, 64u << 20, 1u << 20);
        mapped_append(directory, 16384, 32u << 20, 4u << 20);
        mapped_append(directory, 4096, 16u << 20, 4u << 20);
    }
    if (want("one_file")) {
        for (unsigned t = 1; t <= 8; t *= 2) parallel_one_file(directory, 65536, 512, t);
        for (unsigned t = 1; t <= 8; t *= 2) parallel_one_file(directory, 16384, 1024, t);
        for (unsigned t = 1; t <= 16; t *= 4) parallel_one_file(directory, 262144, 128, t);
    }
    if (want("unaligned")) { for (int v = 2; v <= 6; v += (v == 2 ? 4 : 1)) unaligned(directory, v, 600); }
    if (want("full")) { full_volume("/system_tmp");  }
    if (want("pressure")) pressure(directory, 192u << 20);
    if (want("coherence")) { coherence(directory); coherence("/user/data"); }
    if (want("many_files")) { many_files(directory, 0, 24); many_files(directory, 1, 24); }
    if (want("sizes")) for (size_t block = 65536; block <= (1u << 20); block *= 4) disk(directory, block, 32u << 20, 0, 0);
    line("console-iobench done");
    return 0;
}
