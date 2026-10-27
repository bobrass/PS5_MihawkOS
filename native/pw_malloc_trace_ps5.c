/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Who asks Sony's libc heap for memory (docs/PLAN.md, the browser helper's start-up jump: its libc heap grows from 5 to 400 MiB at 12-15 s, all of it in
 * use, and no mapping our walk lists shows it). WINE_PS5_MALLOC_TRACE=1: a thread watches the process's module list and, in every Wine module the kernel
 * loaded (the staged "<name>.prx.elf" tells where its allocation imports sit), replaces the malloc, calloc, realloc, free, posix_memalign and aligned_alloc
 * slots with the counting versions below. Requests are added up by the caller's return address; every 32 MiB asked for, the six callers that asked for the
 * most are printed with their module and offset. The counting runs inside any allocation of a module: no lock, no allocation of its own, errno kept. */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include "../wine/ps5/pw_wine_prx.h"

extern size_t malloc_usable_size(void *);

enum { SLOTS = 1024, STEP_MIB = 1, TOP = 6, MODULES = 64, SEEN = 256 };
static struct { uintptr_t caller; uint64_t bytes, count; } table[SLOTS];
static int enabled;
static uint64_t total_asked, next_report = (uint64_t)STEP_MIB << 20;
static int64_t in_use;
static struct { char name[48]; uintptr_t lo, hi; } modules[MODULES];
static volatile unsigned module_count;

static void report(void)
{
    char line[900];
    unsigned best[TOP] = {0};
    int saved = errno;
    for (unsigned rank = 0; rank < TOP; rank++) {
        uint64_t most = 0;
        for (unsigned i = 0; i < SLOTS; i++) {
            int taken = 0;
            for (unsigned r = 0; r < rank; r++) taken |= best[r] == i;
            if (!taken && table[i].caller && table[i].bytes > most) { most = table[i].bytes; best[rank] = i; }
        }
    }
    int n = snprintf(line, sizeof(line), "malloc-trace: asked %llu MiB, in use %lld MiB; top callers:", (unsigned long long)(total_asked >> 20),
                     (long long)(__atomic_load_n(&in_use, __ATOMIC_RELAXED) >> 20));
    for (unsigned r = 0; r < TOP && n > 0 && (size_t)n < sizeof(line) - 160; r++) {
        const uintptr_t caller = table[best[r]].caller;
        if (!caller) continue;
        const char *module = "?";
        uintptr_t offset = caller;
        for (unsigned i = 0; i < module_count; i++)
            if (caller >= modules[i].lo && caller < modules[i].hi) { module = modules[i].name; offset = caller - modules[i].lo; }
        n += snprintf(line + n, sizeof(line) - (size_t)n, " %s+%#lx=%llu MiB/%llu", module, (unsigned long)offset,
                      (unsigned long long)(table[best[r]].bytes >> 20), (unsigned long long)table[best[r]].count);
    }
    if (n > 0 && (size_t)n < sizeof(line) - 1) { line[n++] = '\n'; line[n] = 0; fputs(line, stderr); }
    errno = saved;
}

static void big_request(void *caller, size_t bytes)
{
    char line[200];
    const int saved = errno;
    const char *module = "?";
    uintptr_t offset = (uintptr_t)caller;
    for (unsigned i = 0; i < module_count; i++)
        if ((uintptr_t)caller >= modules[i].lo && (uintptr_t)caller < modules[i].hi) { module = modules[i].name; offset = (uintptr_t)caller - modules[i].lo; }
    const int n = snprintf(line, sizeof(line), "malloc-trace: request of %zu KiB from %s+%#lx\n", bytes >> 10, module, (unsigned long)offset);
    if (n > 0) { line[(size_t)n < sizeof(line) ? (size_t)n : sizeof(line) - 1] = 0; fputs(line, stderr); }
    errno = saved;
}

static void note(void *caller, size_t bytes)
{
    if (bytes >= ((size_t)1 << 20)) big_request(caller, bytes);
    const uintptr_t key = (uintptr_t)caller;
    unsigned at = (unsigned)((key >> 3) * 2654435761u) % SLOTS;
    for (unsigned probe = 0; probe < SLOTS; probe++, at = (at + 1) % SLOTS) {
        uintptr_t seen = __atomic_load_n(&table[at].caller, __ATOMIC_RELAXED);
        if (!seen && __atomic_compare_exchange_n(&table[at].caller, &seen, key, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) seen = key;
        if (seen == key) {
            __atomic_add_fetch(&table[at].bytes, bytes, __ATOMIC_RELAXED);
            __atomic_add_fetch(&table[at].count, 1, __ATOMIC_RELAXED);
            break;
        }
    }
    const uint64_t total = __atomic_add_fetch(&total_asked, bytes, __ATOMIC_RELAXED);
    uint64_t due = __atomic_load_n(&next_report, __ATOMIC_RELAXED);
    if (total >= due && __atomic_compare_exchange_n(&next_report, &due, due + ((uint64_t)STEP_MIB << 20), 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
        report();
}

static void *got(void *pointer, size_t bytes, void *caller)
{
    if (pointer) {
        __atomic_add_fetch(&in_use, (int64_t)malloc_usable_size(pointer), __ATOMIC_RELAXED);
        note(caller, bytes);
    }
    return pointer;
}
static void *t_malloc(size_t bytes) { return got(malloc(bytes), bytes, __builtin_return_address(0)); }
static void *t_calloc(size_t count, size_t bytes) { return got(calloc(count, bytes), count * bytes, __builtin_return_address(0)); }
static void *t_realloc(void *old, size_t bytes)
{
    const int64_t before = old ? (int64_t)malloc_usable_size(old) : 0;
    void *pointer = realloc(old, bytes);
    if (pointer) {
        __atomic_add_fetch(&in_use, (int64_t)malloc_usable_size(pointer) - before, __ATOMIC_RELAXED);
        note(__builtin_return_address(0), bytes);
    }
    return pointer;
}
static void t_free(void *pointer)
{
    if (pointer) __atomic_sub_fetch(&in_use, (int64_t)malloc_usable_size(pointer), __ATOMIC_RELAXED);
    free(pointer);
}
static int t_posix_memalign(void **out, size_t alignment, size_t bytes)
{
    const int status = posix_memalign(out, alignment, bytes);
    if (!status) got(*out, bytes, __builtin_return_address(0));
    return status;
}
static void *t_aligned_alloc(size_t alignment, size_t bytes) { return got(aligned_alloc(alignment, bytes), bytes, __builtin_return_address(0)); }


static const char *patching_module;


static uintptr_t replacement(const char *symbol)
{
    if (!strncmp(symbol, "gQX+4GDQjpM#", 12)) return (uintptr_t)t_malloc;
    if (!strncmp(symbol, "2X5agFjKxMc#", 12)) return (uintptr_t)t_calloc;
    if (!strncmp(symbol, "Y7aJ1uydPMo#", 12)) return (uintptr_t)t_realloc;
    if (!strncmp(symbol, "tIhsqj0qsFE#", 12)) return (uintptr_t)t_free;
    if (!strncmp(symbol, "cVSk9y8URbc#", 12)) return (uintptr_t)t_posix_memalign;
    if (!strncmp(symbol, "2Btkg8k24Zg#", 12)) return (uintptr_t)t_aligned_alloc;
    return 0;
}

/* The import slots of a module the loader wired sit in a segment it cannot write: make the page writable for the one store, then put its protection back. */
static int store_slot(uintptr_t slot, uintptr_t wanted, const PwPrxSegment *segments, uint32_t segment_count)
{
    const uintptr_t page = slot & ~(uintptr_t)0x3fff;
    int protection = PROT_READ | PROT_WRITE;
    for (uint32_t i = 0; i < segment_count; i++)
        if (slot >= (uintptr_t)segments[i].address && slot < (uintptr_t)segments[i].address + segments[i].size) protection = (int)segments[i].protection;
    if (!(protection & PROT_WRITE) && mprotect((void *)page, 0x4000, protection | PROT_WRITE)) return 0;
    __atomic_store_n((uintptr_t *)slot, wanted, __ATOMIC_RELEASE);
    if (!(protection & PROT_WRITE)) (void)mprotect((void *)page, 0x4000, protection);
    return 1;
}

/* Patch one module: `bytes` is its staged ELF, `base` where its virtual address 0 is in memory. Returns the slots replaced. */
static unsigned patch_module(const unsigned char *bytes, size_t size, uintptr_t base, const PwPrxSegment *segments, uint32_t segment_count)
{
    if (size < 64 || memcmp(bytes, "\177ELF", 4) || bytes[4] != 2) return 0;
    const uint64_t phoff = *(const uint64_t *)(bytes + 0x20);
    const uint16_t phentsize = *(const uint16_t *)(bytes + 0x36), phnum = *(const uint16_t *)(bytes + 0x38);
    if (phentsize != 56 || phnum > 32 || phoff + (uint64_t)phentsize * phnum > size) return 0;
    uint64_t dyn_offset = 0, dyn_size = 0;
    for (unsigned i = 0; i < phnum; i++) {
        const unsigned char *ph = bytes + phoff + (size_t)i * phentsize;
        if (*(const uint32_t *)ph == 2) { dyn_offset = *(const uint64_t *)(ph + 8); dyn_size = *(const uint64_t *)(ph + 32); }
    }
    if (!dyn_offset || dyn_offset + dyn_size > size) return 0;
    uint64_t rela = 0, rela_size = 0, jmprel = 0, jmprel_size = 0, symtab = 0, strtab = 0;
    for (uint64_t at = dyn_offset; at + 16 <= dyn_offset + dyn_size; at += 16) {
        const uint64_t tag = *(const uint64_t *)(bytes + at), value = *(const uint64_t *)(bytes + at + 8);
        if (!tag) break;
        if (tag == 7) rela = value; else if (tag == 8) rela_size = value; else if (tag == 23) jmprel = value;
        else if (tag == 2) jmprel_size = value; else if (tag == 6) symtab = value; else if (tag == 5) strtab = value;
    }
    /* virtual address -> file offset through the PT_LOAD that holds it */
    #define V2F(v, out) do { (out) = 0; for (unsigned p = 0; p < phnum; p++) { const unsigned char *q = bytes + phoff + (size_t)p * phentsize; \
        const uint64_t off = *(const uint64_t *)(q + 8), va = *(const uint64_t *)(q + 16), fs = *(const uint64_t *)(q + 32); \
        if (*(const uint32_t *)q == 1 && (v) >= va && (v) < va + fs) { (out) = off + (v) - va; break; } } } while (0)
    uint64_t sym_file = 0, str_file = 0;
    V2F(symtab, sym_file); V2F(strtab, str_file);
    if (!sym_file || !str_file) return 0;
    /* An import is named "NID#library token#module token": the token's alphabet index is the id of a DT_SCE_IMPORT_LIB entry (0x61000049: id in the
     * top 16 bits, the library's name at the low 32 in the string table). Wine's modules take malloc from ntdll's heap (library "ntdll"), only
     * the ones that name libSceLibcInternal reach Sony's heap: those are the ones counted, the rest must stay as they are. */
    int libc_library[64] = {0};
    for (uint64_t at = dyn_offset; at + 16 <= dyn_offset + dyn_size; at += 16) {
        const uint64_t tag = *(const uint64_t *)(bytes + at), value = *(const uint64_t *)(bytes + at + 8);
        if (!tag) break;
        if (tag != 0x61000049 && tag != 0x61000015) continue;
        const uint64_t id = (value >> 48) & 0xffff, name_offset = value & 0xffffffff;
        if (id < 64 && str_file + name_offset + 24 < size) {
            if (strstr((const char *)bytes + str_file + name_offset, "LibcInternal")) libc_library[id] = 1;
        }
    }
    unsigned patched = 0;
    for (int table_index = 0; table_index < 2; table_index++) {
        uint64_t start = table_index ? jmprel : rela, length = table_index ? jmprel_size : rela_size, file = 0;
        if (!start || !length) continue;
        V2F(start, file);
        if (!file || file + length > size) continue;
        for (uint64_t at = 0; at + 24 <= length; at += 24) {
            const uint64_t offset = *(const uint64_t *)(bytes + file + at), info = *(const uint64_t *)(bytes + file + at + 8);
            const uint32_t type = (uint32_t)info, index = (uint32_t)(info >> 32);
            if (!index || (type != 1 && type != 6 && type != 7)) continue;
            if (sym_file + (uint64_t)(index + 1) * 24 > size) continue;
            const uint32_t name = *(const uint32_t *)(bytes + sym_file + (uint64_t)index * 24);
            if (str_file + name + 16 > size) continue;
            const char *symbol_text = (const char *)bytes + str_file + name;
            const uintptr_t wanted = enabled ? replacement(symbol_text) : 0;
            static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-";
            const char *token = symbol_text[11] == '#' ? strchr(alphabet, symbol_text[12]) : NULL;
            if (wanted && token && libc_library[token - alphabet] && store_slot(base + offset, wanted, segments, segment_count)) patched++;
        }
    }
    return patched;
}


/* Where Sony's libc asks the kernel for the memory its heap grows by: its import of sceKernelMapNamedFlexibleMemory is replaced (offsets of firmware
 * 10.01's libSceLibcInternal, read from its decrypted ELF: the import's slot, and malloc's own address to find the module's base). A growth of 4 MiB or
 * more is logged with its size and the return addresses found on the calling thread's stack that fall in a known module, which names the code that
 * asked malloc for it even where no frame pointers exist. */
enum { LIBC_MALLOC_OFFSET = 0x5f30, LIBC_MAP_SLOT = 0x1aac28, LIBC_SPAN = 0x200000 };
extern int sceKernelMapNamedFlexibleMemory(void **address, size_t length, int protection, int flags, const char *name);
extern void *malloc(size_t);
static uintptr_t libc_base;
static uintptr_t carrier_lo, carrier_hi;

static int t_map_named(void **address, size_t length, int protection, int flags, const char *name)
{
    const int status = sceKernelMapNamedFlexibleMemory(address, length, protection, flags, name);
    /* Chunks are small, so count them all and print a summary every 32 MiB grown; the first few big ones and every 32 MiB step carry a stack. */
    static uint64_t grown, calls, next_step = (uint64_t)32 << 20;
    const uint64_t total = __atomic_add_fetch(&grown, length, __ATOMIC_RELAXED);
    const uint64_t count = __atomic_add_fetch(&calls, 1, __ATOMIC_RELAXED);
    uint64_t due = __atomic_load_n(&next_step, __ATOMIC_RELAXED);
    const int step = total >= due && __atomic_compare_exchange_n(&next_step, &due, due + ((uint64_t)32 << 20), 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED);
    if (step || length >= ((size_t)1 << 20) || count <= 3) {
        char line[900];
        int saved = errno;
        int n = snprintf(line, sizeof(line), "malloc-trace: libc heap grows by %zu KiB (call %llu, %llu MiB so far, status %#x, at %p, name \"%s\"); stack:", length >> 10, (unsigned long long)count, (unsigned long long)(total >> 20), (unsigned)status,
                         address ? *address : NULL, name ? name : "");
        volatile uintptr_t anchor = 0;
        const uintptr_t *word = (const uintptr_t *)&anchor;
        unsigned found = 0;
        for (unsigned i = 0; i < 384 && found < 14 && n > 0 && (size_t)n < sizeof(line) - 90; i++) {
            const uintptr_t value = word[i];
            const char *module = NULL;
            uintptr_t offset = 0;
            if (value >= libc_base && value < libc_base + LIBC_SPAN) { module = "libc"; offset = value - libc_base; }
            else if (value >= carrier_lo && value < carrier_hi) { module = "carrier"; offset = value - carrier_lo; }
            else for (unsigned m = 0; m < module_count; m++)
                if (value >= modules[m].lo && value < modules[m].hi) { module = modules[m].name; offset = value - modules[m].lo; }
            if (!module) continue;
            n += snprintf(line + n, sizeof(line) - (size_t)n, " %s+%#lx", module, (unsigned long)offset);
            found++;
        }
        if (n > 0 && (size_t)n < sizeof(line) - 1) { line[n++] = '\n'; line[n] = 0; fputs(line, stderr); }
        errno = saved;
    }
    return status;
}

static void hook_libc(void)
{
    libc_base = (uintptr_t)malloc - LIBC_MALLOC_OFFSET;
    carrier_lo = (uintptr_t)hook_libc - 0x300000;
    carrier_hi = (uintptr_t)hook_libc + 0x300000;
    uintptr_t *slot = (uintptr_t *)(libc_base + LIBC_MAP_SLOT);
    fprintf(stderr, "malloc-trace: libc base %#lx, map slot holds %#lx (the kernel's is %#lx)\n", (unsigned long)libc_base, (unsigned long)*slot,
            (unsigned long)(uintptr_t)sceKernelMapNamedFlexibleMemory);
    if (*slot != (uintptr_t)sceKernelMapNamedFlexibleMemory) { fprintf(stderr, "malloc-trace: the slot is not the kernel's call; libc left alone\n"); return; }
    /* libc's import table is read-only once the kernel has bound it: make its page writable for the one store, then put the protection back. */
    void *page = (void *)((uintptr_t)slot & ~(uintptr_t)0x3fff);
    if (mprotect(page, 0x4000, PROT_READ | PROT_WRITE)) { fprintf(stderr, "malloc-trace: cannot make libc's import table writable (errno %d)\n", errno); return; }
    __atomic_store_n(slot, (uintptr_t)t_map_named, __ATOMIC_RELEASE);
    (void)mprotect(page, 0x4000, PROT_READ);
    fprintf(stderr, "malloc-trace: libc's heap growth is logged\n");
}

static char runtime_dir[256];

/* The loader calls this for each Wine module as it loads (native/proton_payload_child.c, payload_prepare_module): the module list of a payload
 * holds only its executable, so the loader's own hook is the place that sees each one, before the module starts. */
void pw_malloc_trace_module(const char *name, const PwPrxSegment *segments, uint32_t segment_count)
{
    char path[512];
    struct stat st;
    if (!enabled || !segment_count) return;
    patching_module = name;
    snprintf(path, sizeof(path), "%s/%s.elf", runtime_dir, name);
    const int fd = open(path, O_RDONLY);
    if (fd < 0) { fprintf(stderr, "malloc-trace: %s: no staged file (errno %d)\n", name, errno); return; }
    unsigned char *bytes = fstat(fd, &st) ? NULL : malloc((size_t)st.st_size);
    if (bytes && read(fd, bytes, (size_t)st.st_size) == (ssize_t)st.st_size) {
        uintptr_t lo = ~(uintptr_t)0, hi = 0;
        for (uint32_t s = 0; s < segment_count; s++) {
            const uintptr_t a = (uintptr_t)segments[s].address;
            if (a < lo) lo = a;
            if (a + segments[s].size > hi) hi = a + segments[s].size;
        }
        const unsigned patched = patch_module(bytes, (size_t)st.st_size, lo, segments, segment_count);
        if (module_count < MODULES) {
            snprintf(modules[module_count].name, sizeof(modules[0].name), "%s", name);
            modules[module_count].lo = lo;
            modules[module_count].hi = hi;
            module_count++;
        }
        fprintf(stderr, "malloc-trace: %s at %#lx-%#lx: %u allocation imports replaced\n", name, (unsigned long)lo, (unsigned long)hi, patched);
    }
    free(bytes);
    close(fd);
}

/* Switch the trace on when WINE_PS5_MALLOC_TRACE=1; `directory` holds the staged "<name>.prx.elf" files (the folder of ntdll's). */
void pw_malloc_trace_start(const char *directory)
{
    const char *setting = getenv("WINE_PS5_MALLOC_TRACE");
    if (!directory) return;
    snprintf(runtime_dir, sizeof(runtime_dir), "%s", directory);
    if (!(setting && setting[0] == '1')) return;
    enabled = 1;
    fprintf(stderr, "malloc-trace: on, staged files in %s\n", runtime_dir);
    hook_libc();
}
