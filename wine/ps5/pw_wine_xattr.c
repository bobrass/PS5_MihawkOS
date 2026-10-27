/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _GNU_SOURCE
#include "pw_wine_xattr.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifndef ENODATA
#define ENODATA ENOATTR
#endif

struct key { uint64_t dev, ino; };

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static char store[1024];
static struct key *keys;          /* inodes the store holds something for, sorted */
static size_t count, capacity;
static struct timespec seen;      /* the store's modification time when keys were read */
static unsigned temporaries;

static int compare(const void *a, const void *b)
{
    const struct key *x = a, *y = b;
    if (x->dev != y->dev) return x->dev < y->dev ? -1 : 1;
    return x->ino < y->ino ? -1 : x->ino > y->ino;
}

static struct key *find(struct key k)
{
    return count ? bsearch(&k, keys, count, sizeof(*keys), compare) : NULL;
}

static int parse(const char *name, struct key *out)
{
    unsigned long long dev, ino;
    int used = 0;
    if (sscanf(name, "%llx-%llx.%n", &dev, &ino, &used) != 2 || !used) return 0;
    out->dev = dev;
    out->ino = ino;
    return 1;
}

static void add(struct key k)
{
    if (find(k)) return;
    if (count == capacity) {
        size_t more = capacity ? capacity * 2 : 64;
        struct key *grown = realloc(keys, more * sizeof(*keys));
        if (!grown) return;   /* the next reload tries again */
        keys = grown;
        capacity = more;
    }
    keys[count++] = k;
    qsort(keys, count, sizeof(*keys), compare);
}

/* Read the store's names again when it may have changed since they were read. */
static void refresh(void)
{
    struct stat st;
    struct timespec now;
    if (stat(store, &st)) return;
    clock_gettime(CLOCK_REALTIME, &now);
    if (st.st_mtim.tv_sec == seen.tv_sec && st.st_mtim.tv_nsec == seen.tv_nsec && now.tv_sec - st.st_mtim.tv_sec >= 2)
        return;
    DIR *dir = opendir(store);
    if (!dir) return;
    count = 0;
    for (struct dirent *entry; (entry = readdir(dir));) {
        struct key k;
        if (parse(entry->d_name, &k)) add(k);
    }
    closedir(dir);
    seen = st.st_mtim;
}

static int has(struct key k)
{
    refresh();
    return find(k) != NULL;
}

static int path_of(char *out, size_t size, struct key k, const char *name)
{
    if (strchr(name, '/')) { errno = EINVAL; return -1; }
    int n = snprintf(out, size, "%s/%llx-%llx.%s", store, (unsigned long long)k.dev, (unsigned long long)k.ino, name);
    if (n < 0 || (size_t)n >= size) { errno = ENAMETOOLONG; return -1; }
    return 0;
}

int pw_wine_xattr_init(const char *store_dir)
{
    if (!store_dir || strlen(store_dir) >= sizeof(store) - 1) { errno = EINVAL; return -1; }
    if (mkdir(store_dir, 0755) && errno != EEXIST) return -1;
    pthread_mutex_lock(&lock);
    strcpy(store, store_dir);
    seen.tv_sec = seen.tv_nsec = -1;
    refresh();
    pthread_mutex_unlock(&lock);
    return 0;
}

ssize_t pw_wine_xattr_get(dev_t dev, ino_t ino, const char *name, void *value, size_t size)
{
    struct key k = { dev, ino };
    char path[1280];
    ssize_t got = -1;
    pthread_mutex_lock(&lock);
    if (!store[0]) errno = ENOTSUP;
    else if (!has(k)) errno = ENODATA;
    else if (!path_of(path, sizeof(path), k, name)) {
        int fd = open(path, O_RDONLY);
        if (fd < 0) errno = errno == ENOENT ? ENODATA : errno;
        else {
            struct stat st;
            if (fstat(fd, &st)) got = -1;
            else if (!size) got = st.st_size;
            else if ((size_t)st.st_size > size) errno = ERANGE;
            else got = read(fd, value, st.st_size);
            close(fd);
        }
    }
    pthread_mutex_unlock(&lock);
    return got;
}

int pw_wine_xattr_set(dev_t dev, ino_t ino, const char *name, const void *value, size_t size)
{
    struct key k = { dev, ino };
    char path[1280], temporary[1280];
    int result = -1;
    pthread_mutex_lock(&lock);
    if (!store[0]) errno = ENOTSUP;
    else if (!path_of(path, sizeof(path), k, name)) {
        snprintf(temporary, sizeof(temporary), "%s/.new-%d-%u", store, (int)getpid(), temporaries++);
        int fd = open(temporary, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) {
            ssize_t put = write(fd, value, size);
            int closed = close(fd);
            if (put == (ssize_t)size && !closed && !rename(temporary, path)) {
                add(k);
                result = 0;
            } else {
                int saved = errno;
                unlink(temporary);
                errno = saved ? saved : EIO;
            }
        }
    }
    pthread_mutex_unlock(&lock);
    return result;
}

int pw_wine_xattr_remove(dev_t dev, ino_t ino, const char *name)
{
    struct key k = { dev, ino };
    char path[1280];
    int result = -1;
    pthread_mutex_lock(&lock);
    if (!store[0]) errno = ENOTSUP;
    else if (!has(k)) errno = ENODATA;
    else if (!path_of(path, sizeof(path), k, name)) {
        if (!unlink(path)) result = 0;
        else if (errno == ENOENT) errno = ENODATA;
    }
    pthread_mutex_unlock(&lock);
    return result;
}

void pw_wine_xattr_forget(dev_t dev, ino_t ino)
{
    struct key k = { dev, ino };
    char prefix[64], path[1280];
    pthread_mutex_lock(&lock);
    if (store[0] && has(k)) {
        int length = snprintf(prefix, sizeof(prefix), "%llx-%llx.", (unsigned long long)k.dev, (unsigned long long)k.ino);
        DIR *dir = opendir(store);
        if (dir) {
            for (struct dirent *entry; (entry = readdir(dir));)
                if (!strncmp(entry->d_name, prefix, length) &&
                    snprintf(path, sizeof(path), "%s/%s", store, entry->d_name) < (int)sizeof(path))
                    unlink(path);
            closedir(dir);
        }
        struct key *found = find(k);
        if (found) {
            memmove(found, found + 1, (count - (size_t)(found - keys) - 1) * sizeof(*keys));
            count--;
        }
    }
    pthread_mutex_unlock(&lock);
}
