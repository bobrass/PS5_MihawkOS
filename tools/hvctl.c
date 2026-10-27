/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Kernel and hypervisor read/write from a payload, for the hypervisor program
 * (docs/HYPERVISOR_PLAN.md, phase 2). Sent through websrv's launcher
 * (POST /elfldr, pipe=1); its output comes back to the sender.
 *
 * Modes (the whole args string is scanned, websrv may pass it unsplit):
 *   pmap                 dump the kernel pmap_store struct (0x200 bytes)
 *   read=0xADDR          read one 8-byte word
 *   dump=0xADDR len=N    dump N bytes (N multiple of 8, up to 0x1000)
 *   write=0xADDR value=0xV   write one word and read it back (verify)
 *
 * Writes are explicit and read back; nothing here patches a code path on its
 * own. Kernel text base is the 13.60 case: data base - 0x0CC0000
 * (PS5_PayloadSDK/crt/kernel.c); offsets are PS5Dev/Byepervisor-Reborn's 13.60
 * map (include/offsets/13_60.h).
 */
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysctl.h>

#include <ps5/kernel.h>

/* Direct memory (physical RAM) from libkernel, used by the PT-pool extender:
 * carve a contiguous block, register it as the pool's region 2. */
extern int sceKernelAllocateDirectMemory(long search_start, long search_end, size_t len,
                                         size_t alignment, int memory_type, long *phys_out);
extern int sceKernelReleaseDirectMemory(long start, size_t len);
extern void sceKernelDebugOutText(int channel, const char *text);
extern int sceKernelMapDirectMemory(void **addr, size_t len, int prot, int flags,
                                    long offset, size_t alignment);
extern long sceKernelGetDirectMemorySize(void);

#define O_PMAP_STORE      0x3B48FF8

static uint64_t g_text_base;

static uint64_t ktext(uint64_t off) { return g_text_base + off; }
static uint64_t kread64(uint64_t addr) { return kernel_getlong((intptr_t)addr); }
static void kwrite64(uint64_t addr, uint64_t value) { kernel_setlong((intptr_t)addr, value); }
static void kwrite32(uint64_t addr, uint32_t value) { kernel_copyin(&value, (intptr_t)addr, 4); }

/* Pool entry for a type (docs/HYPERVISOR.md goal round 2): +0x20 head, +0x28
 * node array, +0x30 region-2 node base, +0x40/+0x48 physical bases,
 * +0x50/+0x54 page counts, +0x58 limit, +0x5c used. */
#define PT_TABLE 0x3B47E90ULL
static uint64_t pt_entry(unsigned type) { return ktext(PT_TABLE + (uint64_t)type * 0x68); }

static unsigned pt_free_count(unsigned type)
{
    const uint64_t e = pt_entry(type);
    const uint64_t arr = kread64(e + 0x28);
    const uint32_t pages1 = (uint32_t)kread64(e + 0x50);
    const uint32_t pages2 = (uint32_t)(kread64(e + 0x50) >> 32);
    const uint64_t hi = arr + (uint64_t)(pages1 + pages2) * 0x30;
    uint64_t node = kread64(e + 0x20);
    unsigned n = 0;
    /* Follow the whole chain: a region-2 node lives outside [arr,hi), so do not
     * bound the walk by the array; count it so the report stays meaningful. */
    while (node && n < 200000) {
        (void)pages2;
        (void)hi;
        node = kread64(node);
        n++;
    }
    return n;
}

static void pt_report(unsigned type, const char *tag)
{
    const uint64_t e = pt_entry(type);
    const uint64_t q50 = kread64(e + 0x50);
    const uint64_t q58 = kread64(e + 0x58);
    printf("  [%s] head=0x%llx tail=0x%llx phy1=0x%llx phy2=0x%llx pages1=%u pages2=%u limit=%u used=%u free=%u\n",
           tag, (unsigned long long)kread64(e + 0x20), (unsigned long long)kread64(e + 0x30),
           (unsigned long long)kread64(e + 0x40), (unsigned long long)kread64(e + 0x48),
           (uint32_t)q50, (uint32_t)(q50 >> 32),
           (uint32_t)q58, (uint32_t)(q58 >> 32), pt_free_count(type));
}

/* Direct-map base: 0xffff800000000000 | (slot<<39) | (idx<<30), with slot and
 * idx the two u32 globals the pmap's dmap helpers use (0x3B49294/0x3B49298). */
static uint64_t pt_dmap_base(void)
{
    const uint64_t a = kread64(ktext(0x3B49294));
    const uint64_t b = kread64(ktext(0x3B49298));
    return 0xffff800000000000ULL | ((uint64_t)(uint32_t)a << 39) | ((uint64_t)(uint32_t)b << 30);
}


/* --- VA->PA (goal rounds 6-8) -------------------------------------------
 * allproc is at text+0x3589E80.  The proc/vmspace field offsets are discovered
 * once per run by accepting the pair whose page-table walk maps the target VA;
 * a successful walk is proven by reading the same bytes back through the
 * translated physical address.  pmap+0x20 holds the kernel VA of the PML4
 * (find_pml4e). */

/* Last-level entry (or large-page entry) for va, plus the level it came from.
 * Level 3 = 1 GiB page, 2 = 2 MiB page, 1 = 4 KiB PTE. */
static uint64_t pt_entry_for(uint64_t l4, uint64_t dmap, uint64_t va, int *level)
{
    const uint64_t e4 = kread64(l4 + (((va >> 39) & 0x1ff) * 8));
    if (!(e4 & 0x50000000000001ULL)) return 0;
    const uint64_t e3 = kread64(dmap + (e4 & 0xffffffffff000ULL) + (((va >> 30) & 0x1ff) * 8));
    if (!(e3 & 0x50000000000001ULL)) return 0;
    if (e3 & 0x80) { *level = 3; return e3; }
    const uint64_t e2 = kread64(dmap + (e3 & 0xffffffffff000ULL) + (((va >> 21) & 0x1ff) * 8));
    if (!(e2 & 0x50000000000001ULL)) return 0;
    if (e2 & 0x80) { *level = 2; return e2; }
    const uint64_t e1 = kread64(dmap + (e2 & 0xffffffffff000ULL) + (((va >> 12) & 0x1ff) * 8));
    *level = 1;
    return e1;
}

static void pt_show_entry(const char *what, uint64_t e, int level)
{
    printf("%-28s %016llx lvl=%d P=%d RW=%d US=%d PWT=%d PCD=%d A=%d D=%d PS=%d G=%d PAT=%d NX=%d pfn=0x%llx\n",
           what, (unsigned long long)e, level,
           (int)(e & 1), (int)((e >> 1) & 1), (int)((e >> 2) & 1), (int)((e >> 3) & 1),
           (int)((e >> 4) & 1), (int)((e >> 5) & 1), (int)((e >> 6) & 1), (int)((e >> 7) & 1),
           (int)((e >> 8) & 1), (int)((e >> 12) & 1), (int)((e >> 63) & 1),
           (unsigned long long)((e & 0xffffffffff000ULL) >> 12));
}

static int pt_walk(uint64_t l4, uint64_t dmap, uint64_t va, uint64_t *pa_out)
{
    const uint64_t e4 = kread64(l4 + (((va >> 39) & 0x1ff) * 8));
    if (!(e4 & 0x50000000000001ULL)) return 0;
    const uint64_t l3 = dmap + (e4 & 0xffffffffff000ULL);
    const uint64_t e3 = kread64(l3 + (((va >> 30) & 0x1ff) * 8));
    if (!(e3 & 0x50000000000001ULL)) return 0;
    const uint64_t l2 = dmap + (e3 & 0xffffffffff000ULL);
    const uint64_t e2 = kread64(l2 + (((va >> 21) & 0x1ff) * 8));
    if (!(e2 & 0x50000000000001ULL)) return 0;
    if (e2 & 0x80) {   /* 2 MiB page */
        *pa_out = (e2 & 0xfffffffe00000ULL) | (va & 0x1fffffULL);
        return 1;
    }
    const uint64_t l1 = dmap + (e2 & 0xffffffffff000ULL);
    const uint64_t e1 = kread64(l1 + (((va >> 12) & 0x1ff) * 8));
    if (!(e1 & 0x50000000000001ULL)) return 0;
    *pa_out = (e1 & 0xffffffffff000ULL) | (va & 0xfffULL);
    return 1;
}


/* Is the last-level entry a 2 MiB page?  e2 bit 7 = PS. */
static int pt_is_2m(uint64_t l4, uint64_t dmap, uint64_t va)
{
    const uint64_t e4 = kread64(l4 + (((va >> 39) & 0x1ff) * 8));
    if (!(e4 & 0x50000000000001ULL)) return 0;
    const uint64_t e3 = kread64(dmap + (e4 & 0xffffffffff000ULL) + (((va >> 30) & 0x1ff) * 8));
    if (!(e3 & 0x50000000000001ULL)) return 0;
    const uint64_t e2 = kread64(dmap + (e3 & 0xffffffffff000ULL) + (((va >> 21) & 0x1ff) * 8));
    return (e2 & 0x50000000000081ULL) == 0x50000000000081ULL || (e2 & 0x81) == 0x81;
}

static int pt_find_pmap(uint64_t target, uint64_t dmap, uint64_t *pmap_out,
                        uint64_t *l4_out, unsigned long long *po, unsigned long long *pv)
{
    const uint64_t allproc = ktext(0x3589E80ULL);
    const int pid = (int)getpid();
    uint64_t proc = kread64(allproc);
    int found = 0;
    for (unsigned n = 0; proc && n < 500; n++) {
        if ((int)(uint32_t)(kread64(proc + 0xb8) >> 32) == pid) { found = 1; break; }
        proc = kread64(proc);
    }
    if (!found) return 0;
    for (unsigned long long o = 0; o < 0x600; o += 8) {
        const uint64_t vs = kread64(proc + o);
        if ((vs >> 48) != 0xffff || (vs & 0x7)) continue;
        for (unsigned long long v = 0; v < 0x600; v += 8) {
            const uint64_t pm = kread64(vs + v);
            if ((pm >> 48) != 0xffff || (pm & 0x7)) continue;
            const uint64_t l4 = kread64(pm + 0x20);
            if ((l4 >> 48) != 0xffff || (l4 & 0xfff)) continue;
            /* the PML4 sits in the direct map: only read it there, never an arbitrary kernel pointer */
            if (l4 < dmap || l4 >= dmap + 0x400000000ULL) continue;
            uint64_t pa = 0;
            if (!pt_walk(l4, dmap, target, &pa)) continue;
            *pmap_out = pm; *l4_out = l4; *po = o; *pv = v;
            return 1;
        }
    }
    return 0;
}

/* Read a kernel range into user memory: page-table pages only, so a caller must bound the physical
 * addresses it dereferences to RAM (the console's 16 GiB) before calling this. */
static int kread_bytes(uint64_t kaddr, void *buf, size_t len)
{
    return kernel_copyout((intptr_t)kaddr, buf, len);
}

/* Present 2 MiB windows of one process, by how many of their 512 pages are mapped and how they are mapped. */
typedef struct WinStats {
    unsigned windows, ps2m, ps1g;
    unsigned occ[5]; /* 1-16, 17-128, 129-511, 512, present-but-empty table */
    unsigned long long pages;
} WinStats;

#define RAM_LIMIT 0x400000000ULL /* 16 GiB: a page-table page is always below it, a device mapping is not */

static void win_walk(uint64_t l4, uint64_t dmap, WinStats *st, unsigned top, uint64_t *sparse, unsigned *sparse_n)
{
    uint64_t e4buf[512], pdpt[512], pd[512], pt[512];

    if ((l4 >> 48) == 0 || (l4 & 0xfff) || kread_bytes(l4, e4buf, sizeof e4buf)) return;
    for (unsigned i4 = 0; i4 < 512; i4++) {
        const uint64_t e4 = e4buf[i4];
        const uint64_t pdpt_pa = e4 & 0xffffffffff000ULL;
        if (!(e4 & 1) || pdpt_pa >= RAM_LIMIT || (pdpt_pa & 0xfff)) continue;
        if (kread_bytes(dmap + pdpt_pa, pdpt, sizeof pdpt)) continue;
        for (unsigned i3 = 0; i3 < 512; i3++) {
            const uint64_t e3 = pdpt[i3];
            const uint64_t pd_pa = e3 & 0xffffffffff000ULL;
            if (!(e3 & 1) || pd_pa >= RAM_LIMIT || (pd_pa & 0xfff)) continue;
            if (e3 & 0x80) { st->ps1g++; st->windows += 512; st->occ[3] += 512; st->pages += 512ULL * 512; continue; }
            if (kread_bytes(dmap + pd_pa, pd, sizeof pd)) continue;
            for (unsigned i2 = 0; i2 < 512; i2++) {
                const uint64_t e2 = pd[i2];
                const uint64_t pt_pa = e2 & 0xffffffffff000ULL;
                if (!(e2 & 1) || pt_pa >= RAM_LIMIT || (pt_pa & 0xfff)) continue;
                st->windows++;
                if (e2 & 0x80) {
                    st->ps2m++;
                    st->occ[3]++;
                    st->pages += 512;
                    continue;
                }
                if (kread_bytes(dmap + pt_pa, pt, sizeof pt)) continue;
                unsigned n = 0;
                for (unsigned i1 = 0; i1 < 512; i1++) n += !!(pt[i1] & 1);
                st->pages += n;
                if (!n) st->occ[4]++;
                else if (n <= 16) st->occ[0]++;
                else if (n <= 128) st->occ[1]++;
                else if (n < 512) st->occ[2]++;
                else st->occ[3]++;
                if (top && n && sparse_n && *sparse_n < top) {
                    const unsigned at = *sparse_n;
                    sparse[at * 2] = ((uint64_t)i4 << 39) | ((uint64_t)i3 << 30) | ((uint64_t)i2 << 21);
                    sparse[at * 2 + 1] = n;
                    (*sparse_n)++;
                }
            }
        }
    }
}

/* The pmap of one proc: vmspace and pmap are both kernel pointers in the proc's fields; the candidate is
 * accepted only when its page tables yield present windows with in-RAM page-table pages. */
static int proc_pmap(uint64_t proc, uint64_t dmap, uint64_t *pmap_out, uint64_t *l4_out)
{
    if (!proc) return 0;
    for (unsigned long long o = 0; o < 0x600; o += 8) {
        const uint64_t vs = kread64(proc + o);
        if ((vs >> 48) != 0xffff || (vs & 0x7) || vs >= 0xffffffffff000000ULL) continue;
        for (unsigned long long v = 0; v < 0x600; v += 8) {
            const uint64_t pm = kread64(vs + v);
            if ((pm >> 48) != 0xffff || (pm & 0x7) || pm >= 0xffffffffff000000ULL) continue;
            const uint64_t l4 = kread64(pm + 0x20);
            if ((l4 >> 48) != 0xffff || (l4 & 0xfff)) continue;
            /* the PML4 sits in the direct map: only read it there, never an arbitrary kernel pointer */
            if (l4 < dmap || l4 >= dmap + 0x400000000ULL) continue;
            WinStats probe;
            memset(&probe, 0, sizeof probe);
            win_walk(l4, dmap, &probe, 0, NULL, NULL);
            if (probe.windows >= 4) {
                *pmap_out = pm;
                *l4_out = l4;
                return 1;
            }
        }
    }
    return 0;
}

/* A printable name in a proc's first bytes (p_comm), for the title and its payload children. */
static void proc_name(uint64_t proc, char *out, size_t len)
{
    unsigned char buf[0x400];
    out[0] = 0;
    if (kread_bytes(proc, buf, sizeof buf)) return;
    for (unsigned i = 0x40; i + 4 < sizeof buf; i++) {
        unsigned j = 0;
        while (j < 20 && i + j < sizeof buf && buf[i + j] >= 0x20 && buf[i + j] < 0x7f) j++;
        if (j >= 4 && (i + j >= sizeof buf || buf[i + j] == 0)) {
            if (j > len - 1) j = (unsigned)len - 1;
            memcpy(out, buf + i, j);
            out[j] = 0;
            return;
        }
    }
}

static void dump(uint64_t addr, unsigned len)
{
    for (unsigned off = 0; off < len; off += 8)
        printf("%llx: %016llx\n", (unsigned long long)(addr + off),
               (unsigned long long)kread64(addr + off));
}

static unsigned long long parse_hex(const char *token, const char *key)
{
    const char *at = strstr(token, key);
    if (!at) return 0;
    return strtoull(at + strlen(key), NULL, 0);
}

/* Panic-hook stub, copied into the text cave at ktext(0x121EB0) and reached by
 * a 5-byte E9 at the PRINTF entry (ktext(0x4ADD60)). It logs the caller (the
 * return address on the stack), the format pointer and the first two args into
 * a 64-entry ring at ktext(0x2D39000)+0x100, then replays the five overwritten
 * prologue bytes and jumps to PRINTF+5. The two 8-byte sentinels (0xAA scratch,
 * 0xBB PRINTF+5) are patched by the installer. */
static const unsigned char hook_stub[] = {
    0x4c,0x8b,0x1c,0x24, 0x50,0x51,0x52,0x56,0x57,
    0x41,0x50,0x41,0x51,0x41,0x52,0x41,0x53,
    0x9c,
    0x48,0xb8, 0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,
    0x48,0x8b,0x08, 0x48,0xff,0xc1, 0x48,0x89,0x08,
    0x48,0x83,0xe1,0x3f, 0x48,0xc1,0xe1,0x05,
    0x48,0x8d,0x4c,0x08,0x10,
    0x4c,0x89,0x19, 0x48,0x89,0x79,0x08,
    0x48,0x89,0x71,0x10, 0x48,0x89,0x51,0x18,
    0x9d,
    0x41,0x5b,0x41,0x5a,0x41,0x59,0x41,0x58,
    0x5f,0x5e,0x5a,0x59,0x58,
    0x55,0x48,0x89,0xe5,0x53,
    0x48,0xb8, 0xbb,0xbb,0xbb,0xbb,0xbb,0xbb,0xbb,0xbb,
    0xff,0xe0
};

/* Allocator hook stub (goal round 11), assembled from
 * build/kmalloc_stub.s.  Entered instead of PRINTF's first bytes; it replays
 * those bytes and jumps to PRINTF+5.  Sentinels: 0xAA scratch, 0xCC allocator,
 * 0xBB PRINTF+5. */
static const unsigned char kmalloc_stub[] = {
    0x53,0x50,0x51,0x52,0x56,0x57,0x41,0x50,0x41,0x51,0x41,0x52,0x41,0x53,0x9c,
    0x48,0xbb,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,
    0x48,0x83,0x3b,0x01,0x75,0x3d,
    0x48,0xc7,0x03,0x03,0x00,0x00,0x00,
    0x48,0x8b,0x7b,0x08,
    0xbe,0x00,0x10,0x00,0x00,
    0xba,0x01,0x01,0x00,0x00,
    0x31,0xc9,0x45,0x31,0xc0,0x45,0x31,0xc9,
    0x6a,0x06,0x6a,0x00,
    0x49,0xbb,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,
    0x41,0xff,0xd3,
    0x48,0x83,0xc4,0x10,
    0x48,0x89,0x43,0x18,
    0x48,0xc7,0x03,0x02,0x00,0x00,0x00,
    0x9d,
    0x41,0x5b,0x41,0x5a,0x41,0x59,0x41,0x58,
    0x5f,0x5e,0x5a,0x59,0x58,0x5b,
    0x55,0x48,0x89,0xe5,0x53,
    0x49,0xbb,0xbb,0xbb,0xbb,0xbb,0xbb,0xbb,0xbb,
    0x41,0xff,0xe3
};

static void hook_patch(unsigned char *bytes, unsigned len, unsigned char fill, uint64_t value)
{
    for (unsigned i = 0; i + 8 <= len; i++) {
        unsigned j = 0;
        while (j < 8 && bytes[i + j] == fill) j++;
        if (j == 8) { memcpy(bytes + i, &value, 8); return; }
    }
}

int main(int argc, char **argv)
{
    char line[512] = "";
    unsigned len;

    g_text_base = (uint64_t)KERNEL_ADDRESS_DATA_BASE - 0x0CC0000ULL;

    for (int i = 0; i < argc; i++) {
        if (argv[i]) {
            strncat(line, argv[i], sizeof line - strlen(line) - 2);
            strncat(line, " ", sizeof line - strlen(line) - 2);
        }
    }

    setvbuf(stdout, NULL, _IONBF, 0);
    printf("hvctl: kernel .text base 0x%llx args [%s]\n", (unsigned long long)g_text_base, line);

    if (strstr(line, "scanobj")) {
        const uint64_t start = strstr(line, "start=") ? parse_hex(line, "start=") : (uint64_t)KERNEL_ADDRESS_DATA_BASE;
        len = (unsigned)parse_hex(line, "len=");
        if (!len) len = 0x4000000;
        if (len > 0x8000000) len = 0x8000000;
        uint8_t buf[0x1000];
        unsigned hits = 0;
        printf("scanobj 0x%llx..0x%llx\n", (unsigned long long)start, (unsigned long long)(start + len));
        for (uint64_t addr = start; addr < start + len; addr += sizeof buf) {
            if (kernel_copyout((intptr_t)addr, buf, sizeof buf)) {
                printf("scanobj: copyout failed at 0x%llx\n", (unsigned long long)addr);
                return 1;
            }
            for (unsigned i = 0; i + 0x2d0 <= sizeof buf; i += 4) {
                const uint16_t type = *(uint16_t *)(buf + i + 4);
                if (type != 1 && type != 2 && type != 5) continue;
                if (*(uint32_t *)(buf + i + 0x29c) != 0x2000) continue;
                if (*(uint32_t *)(buf + i + 0x154) != 0x2000) continue;
                if (*(uint32_t *)(buf + i + 0x2ac) != 0x10000) continue;
                if (*(uint32_t *)(buf + i + 0x164) != 0x10000) continue;
                if (*(uint32_t *)(buf + i + 0x2b4) != 0x800) continue;
                const uint64_t at = addr + i;
                printf("obj 0x%llx type=%u lim1=%x lim2=%x wm1=%x wm2=%x\n",
                       (unsigned long long)at, type,
                       *(uint32_t *)(buf + i + 0x29c), *(uint32_t *)(buf + i + 0x154),
                       *(uint32_t *)(buf + i + 0x2b4), *(uint32_t *)(buf + i + 0x16c));
                if (++hits >= 16) return 0;
            }
        }
        printf("scanobj done, %u hits\n", hits);
        return 0;
    }

    if (strstr(line, "find=")) {
        const uint32_t target = (uint32_t)parse_hex(line, "find=");
        const uint64_t start = strstr(line, "start=") ? parse_hex(line, "start=") : (uint64_t)KERNEL_ADDRESS_DATA_BASE;
        len = (unsigned)parse_hex(line, "len=");
        if (!len) len = 0x3B49000;
        if (len > 0x4000000) len = 0x4000000;
        uint8_t buf[0x1000];
        unsigned hits = 0;
        printf("find 0x%x in 0x%llx..0x%llx\n", target, (unsigned long long)start, (unsigned long long)(start + len));
        for (uint64_t addr = start; addr < start + len; addr += sizeof buf) {
            if (kernel_copyout((intptr_t)addr, buf, sizeof buf)) {
                printf("copyout failed at 0x%llx\n", (unsigned long long)addr);
                return 1;
            }
            for (unsigned i = 0; i + 8 <= sizeof buf; i += 4) {
                const uint32_t v = *(uint32_t *)(buf + i);
                if (v != target) continue;
                const uint64_t at = addr + i;
                printf("hit 0x%llx (u32), neighbors:", (unsigned long long)at);
                for (int k = -8; k <= 8; k += 4) {
                    uint32_t n = 0;
                    kernel_copyout((intptr_t)(at + k), &n, sizeof n);
                    printf(" %x", n);
                }
                printf("\n");
                if (++hits >= 20) return 0;
            }
        }
        printf("find done, %u hits\n", hits);
        return 0;
    }

    if (strstr(line, "scan")) {
        const uint64_t start = strstr(line, "start=") ? parse_hex(line, "start=") : (uint64_t)KERNEL_ADDRESS_DATA_BASE;
        len = (unsigned)parse_hex(line, "len=");
        if (!len) len = 0x20000;
        if (len > 0x4000000) len = 0x4000000;
        uint8_t buf[0x1000];
        unsigned hits = 0;
        printf("scan 0x%llx..0x%llx\n", (unsigned long long)start, (unsigned long long)(start + len));
        for (uint64_t addr = start; addr < start + len; addr += sizeof buf) {
            if (kernel_copyout((intptr_t)addr, buf, sizeof buf)) {
                printf("copyout failed at 0x%llx\n", (unsigned long long)addr);
                return 1;
            }
            for (unsigned i = 0; i + 16 <= sizeof buf; i += 4) {
                const uint32_t a0 = *(uint32_t *)(buf + i);
                const uint32_t a2 = *(uint32_t *)(buf + i + 8);
                if (a0 == 0x2000 && a2 == 0x2000) {
                    const uint64_t at = addr + i;
                    printf("hit 0x%llx:", (unsigned long long)at);
                    for (int k = -16; k <= 24; k += 4) {
                        uint32_t v = 0;
                        kernel_copyout((intptr_t)(at + k), &v, sizeof v);
                        printf(" %x", v);
                    }
                    printf("\n");
                    if (++hits >= 24) return 0;
                }
            }
        }
        printf("scan done, %u hits\n", hits);
        return 0;
    }

    if (strstr(line, "dmemtest")) {
        const long dmsize = sceKernelGetDirectMemorySize();
        printf("dmemtest: pool size=0x%lx (%ld)\n", dmsize, dmsize);
        long phys = 0;
        const int rc = sceKernelAllocateDirectMemory(0, dmsize, 0x200000, 0x200000, 3, &phys);
        printf("dmemtest: allocate 2MiB rc=%d offset=0x%lx\n", rc, phys);
        if (!rc) {
            void *va = NULL;
            const int mr = sceKernelMapDirectMemory(&va, 0x200000, 3, 0, phys, 0x200000);
            printf("dmemtest: map rc=%d va=%p\n", mr, va);
            if (!mr && va) {
                volatile uint64_t *w = (volatile uint64_t *)va;
                w[0] = 0x1122334455667788ULL;
                w[1] = 0x99aabbccddeeff00ULL;
                __sync_synchronize();
                const uint64_t dmap = pt_dmap_base();
                const long bases[3] = { 0x2000000000L, 0, 0x1000000000L };
                for (int i = 0; i < 3; i++) {
                    const long p = (long)((unsigned long)phys - (unsigned long)bases[i]);
                    uint64_t a;
                    memcpy(&a, &p, 8);
                    const uint64_t addr = dmap + (uint64_t)a;
                    printf("dmemtest: base=0x%lx phys=0x%lx dmap=0x%llx lo=%016llx hi=%016llx\n",
                           bases[i], (unsigned long)a, (unsigned long long)addr,
                           (unsigned long long)kread64(addr), (unsigned long long)kread64(addr + 8));
                }
                printf("dmemtest: va[0]=0x%llx\n", (unsigned long long)w[0]);
                /* Find the block's CPU physical address: the direct-memory
                 * offset is a GPU-aperture address, so scan 2 MiB-aligned
                 * physical addresses for the magic we wrote at block offset 0. */
                unsigned found = 0;
                for (unsigned long long pa = 0; pa < 0x400000000ULL; pa += 0x200000) {
                    const uint64_t addr = dmap + pa;
                    if (kread64(addr) != 0x1122334455667788ULL) continue;
                    printf("dmemtest: magic at phys=0x%llx (offset 0x%lx, delta 0x%llx)\n",
                           pa, phys, (unsigned long long)((unsigned long long)phys - pa));
                    if (++found >= 4) break;
                }
                printf("dmemtest: scan matches=%u\n", found);
            }
            printf("dmemtest: release rc=%d\n", sceKernelReleaseDirectMemory(phys, 0x200000));
        }
        printf("dmemtest: done\n");
        return 0;
    }

    if (strstr(line, "kmalloc") && !strstr(line, "kmallocfree")) {
        /* Call the contiguous kernel allocator (candidate text+0x503AF0) from a
         * PRINTF hook.  The only reliable kernel print on this console is the
         * one emitted while a process exits, hence the two phases:
         *   kmallocarm  size=...   install, arm, exit (exit print fires it)
         *   kmallocread            read the result, restore PRINTF, verify
         * The stub replays PRINTF's prologue and is harmless while disarmed. */
        const uint64_t printf_addr = g_text_base + 0x4ADD60ULL;
        const uint64_t cave = g_text_base + 0x121EB0ULL;
        const uint64_t scratch = g_text_base + 0x2D39000ULL + 0x100ULL;
        const uint64_t alloc = g_text_base + 0x503AF0ULL;
        const uint64_t dmap = pt_dmap_base();
        const int arm_only = strstr(line, "kmallocarm") != NULL;
        const int read_only = strstr(line, "kmallocread") != NULL;

        if (read_only) {
            const uint32_t cmd = (uint32_t)kread64(scratch);
            const uint64_t res = kread64(scratch + 0x18);
            const uint64_t orig8 = kread64(scratch + 0x20);
            unsigned char back[5];
            memcpy(back, &orig8, 5);
            kernel_copyin(back, (intptr_t)printf_addr, 5);
            printf("kmallocread: cmd=%u result=0x%llx printf restored to %016llx\n",
                   cmd, (unsigned long long)res, (unsigned long long)kread64(printf_addr));
            if (!res) return 1;
            void *probe = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
            uint64_t pm = 0, l4 = 0, prev = 0;
            unsigned long long po = 0, pv = 0;
            if (probe != MAP_FAILED && pt_find_pmap((uint64_t)(uintptr_t)probe, dmap, &pm, &l4, &po, &pv)) {
                unsigned found = 0, contig = 0;
                for (unsigned i = 0; i < 16; i++) {
                    uint64_t pa = 0;
                    if (!pt_walk(l4, dmap, res + i * 0x1000ULL, &pa)) break;
                    found++;
                    if (i && pa == prev + 0x1000) contig++;
                    prev = pa;
                    printf("kmallocread: page %u PA 0x%llx\n", i, (unsigned long long)pa);
                }
                printf("kmallocread: walked %u pages, contiguous %u\n", found, contig);
            }
            if (probe != MAP_FAILED) munmap(probe, 0x1000);
            return 0;
        }

        const unsigned long long size = parse_hex(line, "size=") ? parse_hex(line, "size=") : 0x100000ULL;
        unsigned char stub[sizeof kmalloc_stub];
        memcpy(stub, kmalloc_stub, sizeof stub);
        hook_patch(stub, sizeof stub, 0xAA, scratch);
        hook_patch(stub, sizeof stub, 0xCC, alloc);
        hook_patch(stub, sizeof stub, 0xBB, printf_addr + 5);
        const uint64_t orig8 = kread64(printf_addr);
        kernel_copyin(stub, (intptr_t)cave, sizeof stub);
        kernel_copyin(&orig8, (intptr_t)(scratch + 0x20), 8);
        const uint32_t cmd = 1;
        kernel_copyin(&cmd, (intptr_t)scratch, 4);
        kernel_copyin(&size, (intptr_t)(scratch + 8), 8);
        unsigned char patch[5];
        patch[0] = 0xE9;
        const int32_t rel = (int32_t)(cave - (printf_addr + 5));
        memcpy(patch + 1, &rel, 4);
        kernel_copyin(patch, (intptr_t)printf_addr, 5);
        printf("kmalloc%s: size=0x%llx alloc=0x%llx cave=0x%llx scratch=0x%llx entry=%016llx orig=%016llx\n",
               arm_only ? "arm" : "", size, (unsigned long long)alloc,
               (unsigned long long)cave, (unsigned long long)scratch,
               (unsigned long long)kread64(printf_addr), (unsigned long long)orig8);
        if (arm_only) {
            printf("kmallocarm: armed; exiting so the exit print fires the hook\n");
            return 0;
        }

        uint32_t c = 0;
        for (int t = 0; t < 300; t++) {
            c = (uint32_t)kread64(scratch);
            if (c == 2) break;
            usleep(100000);
        }
        const uint64_t res = kread64(scratch + 0x18);
        unsigned char back[5];
        memcpy(back, &orig8, 5);
        kernel_copyin(back, (intptr_t)printf_addr, 5);
        printf("kmalloc: cmd=%u result=0x%llx (hook %s)\n", c,
               (unsigned long long)res, c == 2 ? "ran" : "TIMEOUT");
        if (!res) return 1;
        return 0;
    }

    if (strstr(line, "hvdmem")) {
        /* Placement data without reading the title: walk a process's page tables and report each 2 MiB window's
         * occupancy, which is what decides consolidation and 2 MiB pages (goal round 15). Read-only; only
         * page-table pages are dereferenced, and only below 16 GiB. */
        const uint64_t dmap = pt_dmap_base();
        const uint64_t allproc = ktext(0x3589E80ULL);
        const unsigned long long want_pid = parse_hex(line, "pid=");
        const unsigned raw_top = (unsigned)parse_hex(line, "top=");
        const unsigned top = raw_top ? raw_top : 8;
        uint64_t sparse[16 * 2];
        uint64_t proc = kread64(allproc);
        unsigned n = 0;
        for (; proc && n < 500; n++) {
            const int pid = (int)(uint32_t)(kread64(proc + 0xb8) >> 32);
            if (want_pid && (unsigned long long)pid != want_pid) { proc = kread64(proc); continue; }
            char name[24];
            proc_name(proc, name, sizeof name);
            uint64_t pm = 0, l4 = 0;
            if (!proc_pmap(proc, dmap, &pm, &l4)) { proc = kread64(proc); continue; }
            WinStats st;
            unsigned sparse_n = 0;
            memset(&st, 0, sizeof st);
            win_walk(l4, dmap, &st, top, sparse, &sparse_n);
            printf("hvdmem: pid=%d name=%s pmap=0x%llx l4=0x%llx windows=%u 2M=%u 1G=%u pages=%llu "
                   "occ[1-16]=%u [17-128]=%u [129-511]=%u [full]=%u [empty]=%u\n",
                   pid, name, (unsigned long long)pm, (unsigned long long)l4, st.windows, st.ps2m, st.ps1g,
                   st.pages, st.occ[0], st.occ[1], st.occ[2], st.occ[3], st.occ[4]);
            for (unsigned k = 0; k < sparse_n; k++)
                printf("hvdmem:   sparse window 0x%llx pages=%llu\n",
                       (unsigned long long)sparse[k * 2], (unsigned long long)sparse[k * 2 + 1]);
            if (want_pid) return 0;
            proc = kread64(proc);
        }
        return 0;
    }

    if (strstr(line, "attrs")) {
        /* Compare the page attributes of a direct-memory page and a normal RAM
         * page, both in their user mapping and in the kernel dmap.  Page tables
         * must be normal write-back memory; this check is otherwise read-only. */
        const uint64_t dmap = pt_dmap_base();
        void *anon = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
        if (anon == MAP_FAILED) { printf("attrs: anon mmap failed\n"); return 1; }
        *(volatile unsigned char *)anon = 1;
        long off = 0;
        const int rc = sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(),
                                                     0x200000, 0x200000, 3, &off);
        printf("attrs: anon=%p dmem rc=%d off=0x%lx\n", anon, rc, off);
        void *dm = NULL;
        if (!rc) sceKernelMapDirectMemory(&dm, 0x200000, 3, 0, off, 0x200000);
        printf("attrs: dmem va=%p\n", dm);
        uint64_t pm = 0, l4 = 0, pa_anon = 0, pa_dm = 0;
        unsigned long long po = 0, pv = 0;
        if (!pt_find_pmap((uint64_t)(uintptr_t)anon, dmap, &pm, &l4, &po, &pv)) {
            printf("attrs: no pmap\n"); return 1;
        }
        int lv;
        const uint64_t ua = pt_entry_for(l4, dmap, (uint64_t)(uintptr_t)anon, &lv);
        pt_show_entry("user PTE anon", ua, lv);
        if (pt_walk(l4, dmap, (uint64_t)(uintptr_t)anon, &pa_anon))
            pt_show_entry("dmap entry anon", pt_entry_for(l4, dmap, dmap + pa_anon, &lv), lv);
        if (dm) {
            const uint64_t ud = pt_entry_for(l4, dmap, (uint64_t)(uintptr_t)dm, &lv);
            pt_show_entry("user PTE dmem", ud, lv);
            if (pt_walk(l4, dmap, (uint64_t)(uintptr_t)dm, &pa_dm))
                pt_show_entry("dmap entry dmem", pt_entry_for(l4, dmap, dmap + pa_dm, &lv), lv);
            printf("attrs: pa_anon=0x%llx pa_dmem=0x%llx\n",
                   (unsigned long long)pa_anon, (unsigned long long)pa_dm);
        }
        if (!rc) sceKernelReleaseDirectMemory(off, 0x200000);
        munmap(anon, 0x1000);
        return 0;
    }

    if (strstr(line, "anonvtop")) {
        /* Ordinary anonymous memory is normal write-back RAM, which is what page
         * tables must live in.  If the kernel backs it with 2 MiB pages, each
         * gives 512 physically contiguous 4 KiB pages for region 2. */
        const uint64_t dmap = pt_dmap_base();
        unsigned long long pages = parse_hex(line, "pages=");
        if (!pages) pages = 1024;
        const size_t bytes = (size_t)pages * 0x1000;
        void *va = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
        printf("anonvtop: mmap %zu -> %p\n", bytes, va);
        if (va == MAP_FAILED) return 1;
        for (unsigned long long i = 0; i < pages; i++) ((volatile unsigned char *)va)[i * 0x1000] = 1;
        const uint64_t base = (uint64_t)(uintptr_t)va;
        uint64_t pm = 0, l4 = 0;
        unsigned long long po = 0, pv = 0;
        if (!pt_find_pmap(base, dmap, &pm, &l4, &po, &pv)) { printf("anonvtop: no pmap\n"); return 1; }
        uint64_t prev = 0, run = 0, best = 0, best_at = 0, cur_at = 0;
        unsigned found = 0, contig = 0, big = 0;
        for (unsigned long long i = 0; i < pages; i++) {
            uint64_t pa = 0;
            if (!pt_walk(l4, dmap, base + i * 0x1000, &pa)) continue;
            found++;
            if (pt_is_2m(l4, dmap, base + i * 0x1000)) big++;
            if (i == 0) { prev = pa; run = 1; cur_at = i; }
            else if (pa == prev + 0x1000) { contig++; run++; prev = pa; }
            else { if (run > best) { best = run; best_at = cur_at; } run = 1; prev = pa; cur_at = i; }
            if (i < 8) printf("anonvtop: page %llu VA 0x%llx -> PA 0x%llx\n", i,
                              (unsigned long long)(base + i * 0x1000), (unsigned long long)pa);
        }
        if (run > best) { best = run; best_at = cur_at; }
        printf("anonvtop: mapped %u/%llu 2M-entries %u contiguous steps %u longest run %llu at page %llu\n",
               found, pages, big, contig, (unsigned long long)best, (unsigned long long)best_at);
        if (best >= 512) printf("anonvtop: usable run for region 2\n");
        munmap(va, bytes);
        return 0;
    }

    if (strstr(line, "dmemvtop")) {
        /* Is a direct-memory mapping physically contiguous?  If yes it can feed
         * the pool's region 2.  Allocate, map, tag each page, translate every
         * page's PTE to a physical address in the same process. */
        const uint64_t dmap = pt_dmap_base();
        unsigned long long pages = parse_hex(line, "pages=");
        if (!pages) pages = 512;
        if (pages > 4096) pages = 4096;
        const size_t bytes = (size_t)pages * 0x1000;
        long off = 0;
        const int rc = sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(),
                                                     bytes, 0x200000, 3, &off);
        printf("dmemvtop: alloc %zu bytes rc=%d off=0x%lx\n", bytes, rc, off);
        if (rc) return 1;
        void *va = NULL;
        const int mr = sceKernelMapDirectMemory(&va, bytes, 3, 0, off, 0x200000);
        printf("dmemvtop: map rc=%d va=%p\n", mr, va);
        if (mr || !va) { sceKernelReleaseDirectMemory(off, bytes); return 1; }
        volatile unsigned char *w = (volatile unsigned char *)va;
        for (unsigned long long i = 0; i < pages; i++) w[i * 0x1000] = (unsigned char)(0xa0 + (i & 0xf));
        __sync_synchronize();

        const uint64_t target = (uint64_t)(uintptr_t)va;
        uint64_t pm = 0, l4 = 0;
        unsigned long long po = 0, pv = 0;
        if (!pt_find_pmap(target, dmap, &pm, &l4, &po, &pv)) {
            printf("dmemvtop: no pmap maps 0x%llx\n", (unsigned long long)target);
            sceKernelReleaseDirectMemory(off, bytes);
            return 1;
        }
        const uint64_t base = (uint64_t)(uintptr_t)va;
        uint64_t first = 0, prev = 0;
        unsigned found = 0, contig = 0, big = 0;
        for (unsigned long long i = 0; i < pages; i++) {
            uint64_t pa = 0;
            if (!pt_walk(l4, dmap, base + i * 0x1000, &pa)) continue;
            found++;
            if (pt_is_2m(l4, dmap, base + i * 0x1000)) big++;
            if (i == 0) first = pa;
            if (i && pa == prev + 0x1000) contig++;
            prev = pa;
            if (i < 8)
                printf("dmemvtop: page %llu VA 0x%llx -> PA 0x%llx tag=%02llx\n",
                       i, (unsigned long long)(base + i * 0x1000), (unsigned long long)pa,
                       (unsigned long long)(kread64(dmap + pa) & 0xff));
        }
        printf("dmemvtop: mapped %llu of %llu, first PA 0x%llx, contiguous steps %u, 2M-entries %u\n",
               (unsigned long long)found, pages, (unsigned long long)first, contig, big);
        if (found == pages && contig == pages - 1)
            printf("dmemvtop: CONTIGUOUS - region-2 usable\n");
        else
            printf("dmemvtop: not contiguous\n");
        sceKernelReleaseDirectMemory(off, bytes);
        printf("dmemvtop: released\n");
        return 0;
    }

    if (strstr(line, "vtop")) {
        const uint64_t dmap = pt_dmap_base();
        static unsigned char page[0x1000] __attribute__((aligned(0x1000)));
        memset(page, 0, sizeof page);
        memcpy(page, "VTOPMAGIC", 9);
        uint64_t target = parse_hex(line, "va=");
        if (!target) target = (uint64_t)(uintptr_t)page;
        uint64_t pm = 0, l4 = 0, pa = 0;
        unsigned long long po = 0, pv = 0;
        const int pid = (int)getpid();
        if (!pt_find_pmap(target, dmap, &pm, &l4, &po, &pv)) {
            printf("vtop: pid=%d no pmap maps 0x%llx (dmap 0x%llx)\n",
                   pid, (unsigned long long)target, (unsigned long long)dmap);
            return 1;
        }
        if (!pt_walk(l4, dmap, target, &pa)) { printf("vtop: walk failed\n"); return 1; }
        printf("vtop: pid=%d proc+0x%llx vmspace+0x%llx pmap=0x%llx l4=0x%llx\n",
               pid, po, pv, (unsigned long long)pm, (unsigned long long)l4);
        printf("vtop: VA 0x%llx -> PA 0x%llx dmap=0x%llx first=%016llx\n",
               (unsigned long long)target, (unsigned long long)pa,
               (unsigned long long)(dmap + pa), (unsigned long long)kread64(dmap + pa));
        return 0;
    }

    if (strstr(line, "growfill")) {
        /* Region-2 extender, second form (goal round 8).  A direct-memory block
         * is physically contiguous (proven by dmemvtop), so it can back page
         * tables: the node array lives in the block's first pages and region 2
         * is registered with its true physical base, obtained by walking our own
         * page tables.  Everything is unwound before the process exits. */
        setvbuf(stdout, NULL, _IONBF, 0);
        const uint64_t dmap = pt_dmap_base();
        const unsigned type = (unsigned)parse_hex(line, "type=");
        const int have_pages = strstr(line, "pages=") != NULL;
        const int have_count = strstr(line, "count=") != NULL;
        const int noreg = strstr(line, "noreg=1") != NULL;
        unsigned long long pages = parse_hex(line, "pages=");
        unsigned long long count = parse_hex(line, "count=");
        unsigned long long baseva = parse_hex(line, "base=");
        unsigned long long hold = parse_hex(line, "hold=");
        if (!have_pages) pages = 2048;
        if (!have_count) count = 4200;
        if (!baseva) baseva = 0x100000000ULL;
        const unsigned node_pages = (unsigned)((pages * 0x30 + 0xfff) / 0x1000);
        size_t bytes = (size_t)(pages + node_pages) * 0x1000;
        bytes = (bytes + 0x1fffff) & ~(size_t)0x1fffff;
        const uint64_t e = pt_entry(type);
        const uint64_t old_head = kread64(e + 0x20);

        long off = 0;
        const int rc = sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(),
                                                     bytes, 0x200000, 3, &off);
        printf("growfill: dmem %zu bytes rc=%d off=0x%lx\n", bytes, rc, off);
        if (rc) return 1;
        void *va = NULL;
        const int mr = sceKernelMapDirectMemory(&va, bytes, 3, 0, off, 0x200000);
        printf("growfill: map rc=%d va=%p node_pages=%u\n", mr, va, node_pages);
        if (mr || !va) { sceKernelReleaseDirectMemory(off, bytes); return 1; }

        /* Node i at VA + i*0x30 maps to PA(base2) + i*0x1000. */
        for (unsigned long long i = 0; i < pages; i++) {
            const uint64_t next = (i + 1 < pages) ? (uint64_t)(uintptr_t)va + (i + 1) * 0x30
                                                  : old_head;
            memcpy((char *)va + i * 0x30, &next, 8);
            ((unsigned char *)va)[i * 0x30 + 0x2c] = 0;   /* allocator zeroes it */
            ((unsigned char *)va)[i * 0x30 + 0x2d] = (unsigned char)type;
        }
        __sync_synchronize();
        uint64_t pm = 0, l4 = 0, pa0 = 0;
        unsigned long long po = 0, pv = 0;
        if (!pt_find_pmap((uint64_t)(uintptr_t)va, dmap, &pm, &l4, &po, &pv) ||
            !pt_walk(l4, dmap, (uint64_t)(uintptr_t)va, &pa0)) {
            printf("growfill: cannot translate the block\n");
            sceKernelReleaseDirectMemory(off, bytes);
            return 1;
        }
        const uint64_t V = dmap + pa0;
        const uint64_t base2 = pa0 + (uint64_t)node_pages * 0x1000;
        printf("growfill: block PA 0x%llx nodes V 0x%llx region2 0x%llx pages %llu\n",
               (unsigned long long)pa0, (unsigned long long)V,
               (unsigned long long)base2, pages);
        pt_report(type, "before");
        if (noreg) {
            printf("growfill: noreg control - block mapped but region2 not registered\n");
            pt_report(type, "noreg");
            sceKernelReleaseDirectMemory(off, bytes);
            return 0;
        }

        kwrite64(e + 0x48, base2);
        kwrite32(e + 0x54, (uint32_t)pages);
        kwrite64(e + 0x30, V);
        kwrite64(e + 0x20, V);
        pt_report(type, "region2-on");

        unsigned long long mapped = 0;
        for (unsigned long long i = 0; i < count; i++) {
            void *want = (void *)(uintptr_t)(baseva + i * 0x200000ULL);
            void *got = mmap(want, 0x1000, PROT_READ | PROT_WRITE,
                             MAP_ANON | MAP_PRIVATE | MAP_FIXED, -1, 0);
            if (got == MAP_FAILED) {
                printf("growfill: mmap failed i=%llu addr=0x%llx errno=%d\n", i,
                       (unsigned long long)(baseva + i * 0x200000ULL), errno);
                break;
            }
            *(volatile unsigned char *)got = 1;
            mapped++;
        }
        printf("growfill: mapped %llu/%llu\n", mapped, count);
        pt_report(type, "filled");
        if (hold) { printf("growfill: holding %llu s\n", hold); sleep((unsigned)hold); }

        unsigned long long un = 0;
        for (unsigned long long i = 0; i < mapped; i++)
            if (munmap((void *)(uintptr_t)(baseva + i * 0x200000ULL), 0x1000) == 0) un++;
        printf("growfill: unmapped %llu/%llu\n", un, mapped);

        /* Wait for every region-2 node to return to the free list. */
        const uint64_t vend = V + (uint64_t)pages * 0x30;
        unsigned found = 0, tries = 0;
        for (; tries < 40; tries++) {
            uint64_t cur = kread64(e + 0x20);
            found = 0;
            for (unsigned long long n = 0; cur && n < 200000; n++) {
                if (cur >= V && cur < vend) found++;
                cur = kread64(cur);
            }
            if (found == pages) break;
            sleep(1);
        }
        printf("growfill: region2 nodes back %u/%llu after %u tries\n", found, pages, tries);
        pt_report(type, "unmapped");
        if (found != pages) {
            printf("growfill: POOL STILL REGISTERED - keeping this process alive\n");
            for (;;) sleep(3600);
        }
        uint64_t head = kread64(e + 0x20), prev = 0, cur = head;
        for (unsigned long long n = 0; cur && n < 200000; n++) {
            uint64_t next = kread64(cur);
            if (cur >= V && cur < vend) {
                if (prev) kwrite64(prev, next); else head = next;
            } else {
                prev = cur;
            }
            cur = next;
        }
        kwrite64(e + 0x20, head);
        kwrite64(e + 0x30, 0);
        kwrite32(e + 0x54, 0);
        kwrite64(e + 0x48, 0);
        sceKernelReleaseDirectMemory(off, bytes);
        pt_report(type, "restored");
        printf("growfill: done\n");
        return 0;
    }

    if (strstr(line, "ptlog")) {
        /* Stream the page-table pool counters to klog from inside a payload: klog reaches the host live, so the
         * last sample before a panic survives (a CSV on the console's disk does not). */
        const unsigned ms = parse_hex(line, "ms=") ? parse_hex(line, "ms=") : 200;
        const unsigned secs = parse_hex(line, "secs=") ? parse_hex(line, "secs=") : 1200;
        const uint64_t e = pt_entry((unsigned)parse_hex(line, "type="));
        const unsigned long long n = (unsigned long long)secs * 1000ULL / (ms ? ms : 1);
        char b[96];
        for (unsigned long long i = 0; i < n; i++) {
            const uint64_t q58 = kread64(e + 0x58);
            snprintf(b, sizeof b, "PTLOG i=%llu used=%u limit=%u\n", i, (unsigned)(q58 >> 32), (unsigned)q58);
            sceKernelDebugOutText(0, b);
            usleep(ms * 1000);
        }
        printf("ptlog: done\n");
        return 0;
    }

    if (strstr(line, "ptsample")) {
        /* Sample the pool while a title runs; appends CSV to the guest's /data so
         * the host can fetch it after the run (goal round 13). */
        const unsigned secs = parse_hex(line, "secs=") ? parse_hex(line, "secs=") : 600;
        const unsigned ms = parse_hex(line, "interval=") ? parse_hex(line, "interval=") : 1000;
        const unsigned type = (unsigned)parse_hex(line, "type=");
        const uint64_t e = pt_entry(type);
        const unsigned long long n = (unsigned long long)secs * 1000ULL / (ms ? ms : 1);
        FILE *f = fopen("/data/ps5proton/poolsample.csv", "w");
        if (!f) { printf("ptsample: cannot open csv\n"); return 1; }
        fprintf(f, "t,used,limit,free\n");
        printf("ptsample: %llu samples every %u ms\n", n, ms);
        for (unsigned long long i = 0; i < n; i++) {
            const uint64_t q58 = kread64(e + 0x58);
            const uint32_t used = (uint32_t)(q58 >> 32);
            const uint32_t limit = (uint32_t)q58;
            fprintf(f, "%llu,%u,%u,%u\n", i, used, limit, pt_free_count(type));
            if ((i % 10) == 0) fflush(f);
            usleep(ms * 1000);
        }
        fclose(f);
        printf("ptsample: done\n");
        return 0;
    }

    if (strstr(line, "ptchurn")) {
        /* Does the kernel return a page table to the pool when its 2 MiB window
         * is unmapped while the process is still alive?  Map one 4 KiB page per
         * 2 MiB window, unmap some, and read the pool counters after each step
         * (goal round 13).  No kernel writes. */
        const unsigned type = (unsigned)parse_hex(line, "type=");
        const unsigned long long count = parse_hex(line, "count=") ? parse_hex(line, "count=")
                                                                   : 2000;
        const unsigned long long unmap_n = strstr(line, "unmap=")
                                               ? parse_hex(line, "unmap=") : count;
        const unsigned long long baseva = parse_hex(line, "base=") ? parse_hex(line, "base=")
                                                                   : 0x100000000ULL;
        const unsigned long long hold = parse_hex(line, "hold=");
        pt_report(type, "start");
        unsigned long long mapped = 0;
        for (unsigned long long i = 0; i < count; i++) {
            void *want = (void *)(uintptr_t)(baseva + i * 0x200000ULL);
            void *got = mmap(want, 0x1000, PROT_READ | PROT_WRITE,
                             MAP_ANON | MAP_PRIVATE | MAP_FIXED, -1, 0);
            if (got == MAP_FAILED) { printf("ptchurn: mmap failed at %llu\n", i); break; }
            *(volatile unsigned char *)got = 1;
            mapped++;
        }
        printf("ptchurn: mapped %llu\n", mapped);
        pt_report(type, "mapped");
        unsigned long long un = 0;
        for (unsigned long long i = 0; i < unmap_n && i < mapped; i++)
            if (munmap((void *)(uintptr_t)(baseva + i * 0x200000ULL), 0x1000) == 0) un++;
        printf("ptchurn: unmapped %llu\n", un);
        pt_report(type, "unmapped");
        if (hold) { printf("ptchurn: holding %llu s\n", hold); sleep((unsigned)hold); }
        (void)type;
        return 0;
    }

    if (strstr(line, "ptpool")) {
        /* Walk the per-type page-table pool (docs/HYPERVISOR.md, goal round 2).
         * Entry: text+0x3B47E90 + type*0x68.  +0x20 free-list head, +0x28 node
         * array base, +0x30 second-region base, +0x38 bitmap, +0x40/+0x48
         * physical bases, +0x50/+0x54 page counts, +0x58 limit, +0x5c used.
         * Nodes are 0x30 bytes; node->next is at +0.  The allocator (text+
         * 0x8F1E00) asserts "page table memory exhausted" when the head is
         * NULL, so the list length is the real capacity. */
        const unsigned type = (unsigned)parse_hex(line, "type=");
        const uint64_t entry = ktext(0x3B47E90ULL + (uint64_t)type * 0x68);
        const uint64_t head = kread64(entry + 0x20);
        const uint64_t arr = kread64(entry + 0x28);
        const uint64_t tail = kread64(entry + 0x30);
        const uint32_t pages1 = (uint32_t)kread64(entry + 0x50);
        const uint32_t pages2 = (uint32_t)(kread64(entry + 0x50) >> 32);
        const uint32_t limit = (uint32_t)kread64(entry + 0x58);
        const uint32_t used = (uint32_t)(kread64(entry + 0x58) >> 32);
        printf("ptpool type %u entry 0x%llx\n", type, (unsigned long long)entry);
        printf("  head 0x%llx arr 0x%llx tail 0x%llx phy1 0x%llx phy2 0x%llx\n",
               (unsigned long long)head, (unsigned long long)arr,
               (unsigned long long)tail, (unsigned long long)kread64(entry + 0x40),
               (unsigned long long)kread64(entry + 0x48));
        printf("  pages1 %u pages2 %u limit %u used %u nodes %u\n",
               pages1, pages2, limit, used, pages1 + pages2);
        if (arr) {
            const uint64_t lo = arr;
            const uint64_t hi = arr + (uint64_t)(pages1 + pages2) * 0x30;
            uint64_t node = head, prev = 0;
            unsigned count = 0, bad = 0;
            while (node && count < 200000) {
                if (node < lo || node >= hi || ((node - lo) % 0x30)) { bad++; break; }
                prev = node;
                node = kread64(node);
                count++;
            }
            printf("  free-list: %u nodes, last 0x%llx, out-of-range %u, terminated %d\n",
                   count, (unsigned long long)prev, bad, node == 0);
            printf("  capacity check: used+free = %u (nodes %u)\n",
                   used + count, pages1 + pages2);
        }
        return 0;
    }

    if (strstr(line, "lsdev=")) {
        const char *pp = strstr(line, "lsdev=");
        char path[160] = "";
        sscanf(pp + 6, "%159s", path);
        if (!path[0]) return 2;
        DIR *dir = opendir(path);
        if (!dir) { printf("lsdev: opendir %s error %d\n", path, errno); return 1; }
        struct dirent *de;
        while ((de = readdir(dir)) != NULL)
            printf("dev %s/%s\n", path, de->d_name);
        closedir(dir);
        return 0;
    }

    if (strstr(line, "stat=")) {
        const char *pp = strstr(line, "stat=");
        char path[160] = "";
        sscanf(pp + 5, "%159s", path);
        struct stat st;
        if (stat(path, &st)) { printf("stat %s error %d\n", path, errno); return 1; }
        printf("stat %s mode=%o rdev=%#llx size=%lld\n", path, (unsigned)st.st_mode,
               (unsigned long long)st.st_rdev, (long long)st.st_size);
        return 0;
    }

    if (strstr(line, "sysctl=")) {
        const char *pp = strstr(line, "sysctl=");
        char name[160] = "";
        sscanf(pp + 7, "%159s", name);
        uint8_t buf[256];
        size_t size = sizeof buf;
        memset(buf, 0, sizeof buf);
        if (sysctlbyname(name, buf, &size, NULL, 0)) {
            printf("sysctl %s error %d\n", name, errno);
            return 1;
        }
        printf("sysctl %s size=%zu\n", name, size);
        for (size_t i = 0; i < size && i < 128; i += 8) {
            uint64_t v = 0;
            memcpy(&v, buf + i, 8);
            printf("  %02zx: %016llx\n", i, (unsigned long long)v);
        }
        return 0;
    }

    if (strstr(line, "msgbuffile")) {
        static char buf[1 << 20];
        size_t size = sizeof buf;
        int error = sysctlbyname("kern.msgbuf", buf, &size, NULL, 0);
        if (error) { printf("kern.msgbuf: error %d\n", errno); return 1; }
        const char *pp = strstr(line, "path=");
        char path[160] = "";
        if (pp) sscanf(pp + 5, "%159s", path);
        if (!path[0]) { printf("msgbuffile: no path\n"); return 2; }
        FILE *f = fopen(path, "wb");
        if (!f) { printf("msgbuffile: open %s failed\n", path); return 1; }
        fwrite(buf, 1, size, f);
        fclose(f);
        printf("msgbuffile: wrote %zu bytes to %s\n", size, path);
        return 0;
    }

    if (strstr(line, "msgbuf")) {
        static char buf[1 << 20];
        size_t size = sizeof buf;
        len = (unsigned)parse_hex(line, "len=");
        if (!len) len = 0x2000;
        int error = sysctlbyname("kern.msgbuf", buf, &size, NULL, 0);
        if (error) {
            error = errno;
            printf("kern.msgbuf: error %d\n", error);
            return 1;
        }
        if (len > size) len = (unsigned)size;
        printf("kern.msgbuf: %zu bytes (showing %u)\n", size, len);
        fwrite(buf, 1, len, stdout);
        return 0;
    }

    if (strstr(line, "hookinstall")) {
        const uint64_t printf_addr = g_text_base + 0x4ADD60ULL;
        const uint64_t cave = g_text_base + 0x121EB0ULL;
        const uint64_t scratch = g_text_base + 0x2D39000ULL + 0x100ULL;
        unsigned char stub[sizeof hook_stub];
        memcpy(stub, hook_stub, sizeof stub);
        hook_patch(stub, sizeof stub, 0xAA, scratch);
        hook_patch(stub, sizeof stub, 0xBB, printf_addr + 5);
        const uint64_t original = kread64(printf_addr);
        unsigned char patch[5];
        patch[0] = 0xE9;
        int32_t rel = (int32_t)(cave - (printf_addr + 5));
        memcpy(patch + 1, &rel, 4);
        kernel_copyin(&original, (intptr_t)(scratch + 0x800), 5);
        kernel_copyin(stub, (intptr_t)cave, sizeof stub);
        kernel_copyin(patch, (intptr_t)printf_addr, 5);
        const uint64_t after = kread64(printf_addr);
        const uint64_t cave8 = kread64(cave);
        const uint64_t expect = (uint64_t)0xE9 | ((uint64_t)(uint32_t)rel << 8);
        printf("hook: printf=0x%llx cave=0x%llx scratch=0x%llx rel=0x%x\n",
               (unsigned long long)printf_addr, (unsigned long long)cave,
               (unsigned long long)scratch, (unsigned)rel);
        printf("hook: entry before=0x%llx after=0x%llx cave_first=0x%llx %s\n",
               (unsigned long long)original, (unsigned long long)after, (unsigned long long)cave8,
               (after & 0xFFFFFFFFFFULL) == expect ? "entry-ok" : "ENTRY-MISMATCH");
        return (after & 0xFFFFFFFFFFULL) == expect ? 0 : 1;
    }

    /* hookpt: raise the page-table pool limit at its creation. The pool
     * initializer FUN_0x951ed0c0(type, base, ..., limit) takes the limit as
     * its sixth argument (r9d). The first five bytes (55 48 89 e5 41) end
     * mid-instruction, so the cave replays the full six-byte prologue
     * (push rbp; mov rbp,rsp; push r15) and jumps back to target+6. */
    if (strstr(line, "hookpt") && !strstr(line, "hookptrestore")) {
        const uint64_t target = g_text_base + 0x8ED0C0ULL;
        const uint64_t cave = g_text_base + 0x122000ULL;
        const uint64_t scratch = g_text_base + 0x2D39000ULL + 0x100ULL;
        unsigned char stub[17] = {
            0x41,0xb9,0x00,0x80,0x00,0x00,   /* mov r9d, 0x8000 */
            0x55,0x48,0x89,0xe5,0x41,0x57,   /* push rbp; mov rbp,rsp; push r15 */
            0xe9,0,0,0,0                      /* jmp target+6 */
        };
        int32_t rel = (int32_t)((target + 6) - (cave + sizeof stub));
        memcpy(stub + 13, &rel, 4);
        unsigned char saved[6];
        kernel_copyout((intptr_t)target, saved, sizeof saved);
        unsigned char patch[5] = {0xE9,0,0,0,0};
        int32_t jrel = (int32_t)(cave - (target + 5));
        memcpy(patch + 1, &jrel, 4);
        kernel_copyin(saved, (intptr_t)(scratch + 0x900), sizeof saved);
        kernel_copyin(stub, (intptr_t)cave, sizeof stub);
        kernel_copyin(patch, (intptr_t)target, sizeof patch);
        uint64_t after = kread64(target);
        uint64_t cave8 = kread64(cave);
        printf("hookpt: target=0x%llx cave=0x%llx rel=0x%x saved=%02x%02x%02x%02x%02x%02x\n",
               (unsigned long long)target, (unsigned long long)cave, (unsigned)jrel,
               saved[0],saved[1],saved[2],saved[3],saved[4],saved[5]);
        printf("hookpt: entry after=0x%llx cave=0x%llx %s\n",
               (unsigned long long)after, (unsigned long long)cave8,
               after == ((uint64_t)0xE9 | ((uint64_t)(uint32_t)jrel << 8)) ? "entry-ok" : "ENTRY-MISMATCH");
        return 0;
    }

    if (strstr(line, "hookptrestore")) {
        const uint64_t target = g_text_base + 0x8ED0C0ULL;
        const uint64_t scratch = g_text_base + 0x2D39000ULL + 0x100ULL;
        unsigned char saved[6];
        kernel_copyout((intptr_t)(scratch + 0x900), saved, sizeof saved);
        kernel_copyin(saved, (intptr_t)target, sizeof saved);
        printf("hookptrestore: target=0x%llx %02x%02x%02x%02x%02x%02x\n",
               (unsigned long long)target, saved[0],saved[1],saved[2],saved[3],saved[4],saved[5]);
        return 0;
    }

    if (strstr(line, "hookrestore")) {
        const uint64_t printf_addr = g_text_base + 0x4ADD60ULL;
        const uint64_t scratch = g_text_base + 0x2D39000ULL + 0x100ULL;
        const uint64_t saved = kread64(scratch + 0x800);
        kernel_copyin(&saved, (intptr_t)printf_addr, 5);
        const uint64_t after = kread64(printf_addr);
        printf("hookrestore: entry=0x%llx saved=0x%llx %s\n",
               (unsigned long long)after, (unsigned long long)saved,
               ((after ^ saved) & 0xFFFFFFFFFFULL) == 0 ? "restored" : "MISMATCH");
        return ((after ^ saved) & 0xFFFFFFFFFFULL) == 0 ? 0 : 1;
    }

    if (strstr(line, "hookrec")) {
        setvbuf(stdout, NULL, _IONBF, 0);
        const uint64_t scratch = g_text_base + 0x2D39000ULL + 0x100ULL;
        FILE *log = fopen("/data/ps5proton/hook-ring.log", "a");
        uint64_t last = kread64(scratch);
        if (log) { fprintf(log, "start count=%llu\n", (unsigned long long)last); fflush(log); fsync(fileno(log)); }
        printf("hookrec: scratch=0x%llx count=%llu log=%d\n", (unsigned long long)scratch,
               (unsigned long long)last, log ? 1 : 0);
        for (unsigned long long i = 0; i < 4000000ULL; i++) {
            const uint64_t c = kread64(scratch);
            if (c == last) continue;
            const uint64_t slot = scratch + 0x10 + ((c - 1) & 63) * 0x20;
            const uint64_t caller = kread64(slot);
            const uint64_t fmt = kread64(slot + 8);
            char msg[320];
            int n = snprintf(msg, sizeof msg, "hook[%llu] caller=0x%llx fmt=0x%llx a1=0x%llx a2=0x%llx",
                             (unsigned long long)c, (unsigned long long)caller, (unsigned long long)fmt,
                             (unsigned long long)kread64(slot + 0x10), (unsigned long long)kread64(slot + 0x18));
            if (n < 0) n = 0;
            if ((fmt >> 48) == 0xFFFF && n < (int)sizeof msg - 80) {
                char text[65];
                if (kernel_copyout((intptr_t)fmt, text, 64) == 0) {
                    text[64] = 0;
                    for (int q = 0; q < 64; q++) if (text[q] && (text[q] < 32 || text[q] > 126)) text[q] = '.';
                    n += snprintf(msg + n, sizeof msg - n, " fmt=%s", text);
                }
            }
            n += snprintf(msg + n, sizeof msg - n, "\n");
            fwrite(msg, 1, (size_t)n, stdout);
            if (log) { fwrite(msg, 1, (size_t)n, log); fflush(log); fsync(fileno(log)); }
            last = c;
        }
        if (log) fclose(log);
        printf("hookrec: done\n");
        return 0;
    }

    if (strstr(line, "hookdump")) {
        const uint64_t scratch = g_text_base + 0x2D39000ULL + 0x100ULL;
        const uint64_t count = kread64(scratch);
        printf("hookdump: scratch=0x%llx count=%llu\n", (unsigned long long)scratch,
               (unsigned long long)count);
        const uint64_t first = count > 64 ? count - 64 : 0;
        for (uint64_t k = first; k < count; k++) {
            const uint64_t slot = scratch + 0x10 + (k & 63) * 0x20;
            const uint64_t caller = kread64(slot);
            const uint64_t fmt = kread64(slot + 8);
            printf("hook[%llu] caller=0x%llx fmt=0x%llx a1=0x%llx a2=0x%llx",
                   (unsigned long long)k, (unsigned long long)caller, (unsigned long long)fmt,
                   (unsigned long long)kread64(slot + 0x10), (unsigned long long)kread64(slot + 0x18));
            if ((fmt >> 48) == 0xFFFF) {
                char text[65];
                if (kernel_copyout((intptr_t)fmt, text, 64) == 0) {
                    text[64] = 0;
                    for (int q = 0; q < 64; q++) if (text[q] && (text[q] < 32 || text[q] > 126)) text[q] = '.';
                    printf(" fmt=%s", text);
                }
            }
            printf("\n");
        }
        return 0;
    }

    if (strstr(line, "hookpoll")) {
        setvbuf(stdout, NULL, _IONBF, 0);
        const uint64_t scratch = g_text_base + 0x2D39000ULL + 0x100ULL;
        int udp = -1;
        struct sockaddr_in dst;
        const char *hp = strstr(line, "host=");
        if (hp) {
            char ip[64] = "";
            sscanf(hp + 5, "%63s", ip);
            int port = (int)parse_hex(line, "port=");
            if (!port) port = 9999;
            memset(&dst, 0, sizeof dst);
            dst.sin_family = AF_INET;
            dst.sin_port = htons((unsigned short)port);
            if (inet_pton(AF_INET, ip, &dst.sin_addr) == 1) udp = socket(AF_INET, SOCK_DGRAM, 0);
        }
        uint64_t last = kread64(scratch);
        printf("hookpoll: scratch=0x%llx count=%llu udp=%d\n", (unsigned long long)scratch,
               (unsigned long long)last, udp);
        if (udp >= 0) {
            char hb[128];
            int hn = snprintf(hb, sizeof hb, "hookpoll-start scratch=0x%llx count=%llu\n",
                              (unsigned long long)scratch, (unsigned long long)last);
            sendto(udp, hb, (size_t)hn, 0, (struct sockaddr *)&dst, sizeof dst);
        }
        for (unsigned long long i = 0; i < 4000000ULL; i++) {
            const uint64_t c = kread64(scratch);
            if (c == last) continue;
            const uint64_t slot = scratch + 0x10 + ((c - 1) & 63) * 0x20;
            const uint64_t caller = kread64(slot);
            const uint64_t fmt = kread64(slot + 8);
            char msg[320];
            int n = snprintf(msg, sizeof msg, "hook[%llu] caller=0x%llx fmt=0x%llx a1=0x%llx a2=0x%llx",
                             (unsigned long long)c, (unsigned long long)caller, (unsigned long long)fmt,
                             (unsigned long long)kread64(slot + 0x10), (unsigned long long)kread64(slot + 0x18));
            if (n < 0) n = 0;
            if ((fmt >> 48) == 0xFFFF && n < (int)sizeof msg - 80) {
                char s[65];
                if (kernel_copyout((intptr_t)fmt, s, 64) == 0) {
                    s[64] = 0;
                    for (int k = 0; k < 64; k++) if (s[k] && (s[k] < 32 || s[k] > 126)) s[k] = '.';
                    n += snprintf(msg + n, sizeof msg - n, " fmt=%s", s);
                }
            }
            n += snprintf(msg + n, sizeof msg - n, "\n");
            if (udp >= 0) sendto(udp, msg, (size_t)n, 0, (struct sockaddr *)&dst, sizeof dst);
            fwrite(msg, 1, (size_t)n, stdout);
            last = c;
        }
        printf("hookpoll: done\n");
        return 0;
    }

    if (strstr(line, "pt")) {
        extern int sceKernelGetPageTableStats(void *stats, size_t size);
        uint8_t buf[0x400];
        memset(buf, 0, sizeof buf);
        const int ret = sceKernelGetPageTableStats(buf, sizeof buf);
        printf("sceKernelGetPageTableStats ret %d\n", ret);
        for (unsigned off = 0; off < 0x100; off += 8)
            printf("%04x: %016llx\n", off, *(unsigned long long *)(buf + off));
        return 0;
    }

    if (strstr(line, "pmap")) {
        const uint64_t pmap = ktext(O_PMAP_STORE);
        printf("pmap_store 0x%llx\n", (unsigned long long)pmap);
        dump(pmap, 0x200);
        return 0;
    }

    if (strstr(line, "dumpfile=")) {
        const uint64_t addr = parse_hex(line, "dumpfile=");
        len = (unsigned)parse_hex(line, "len=");
        if (!len) len = 0x100000;
        if (len > 0x4000000) len = 0x4000000;
        const char *pp = strstr(line, "path=");
        char path[160] = "";
        if (pp) sscanf(pp + 5, "%159s", path);
        if (!path[0]) { printf("dumpfile: no path\n"); return 2; }
        FILE *f = fopen(path, "wb");
        if (!f) { printf("dumpfile: open %s failed\n", path); return 1; }
        uint8_t buf[0x1000];
        unsigned long long written = 0;
        for (uint64_t a = addr; a < addr + len; a += sizeof buf) {
            if (kernel_copyout((intptr_t)a, buf, sizeof buf)) {
                printf("dumpfile: copyout failed at 0x%llx\n", (unsigned long long)a);
                break;
            }
            fwrite(buf, 1, sizeof buf, f);
            written += sizeof buf;
        }
        fclose(f);
        printf("dumpfile: wrote %llu bytes to %s\n", written, path);
        return 0;
    }

    if (strstr(line, "dump=")) {
        const uint64_t addr = parse_hex(line, "dump=");
        len = (unsigned)parse_hex(line, "len=");
        if (!len) len = 0x100;
        if (len > 0x1000) len = 0x1000;
        printf("dump 0x%llx len 0x%x\n", (unsigned long long)addr, len);
        dump(addr, len);
        return 0;
    }

    if (strstr(line, "read=")) {
        const uint64_t addr = parse_hex(line, "read=");
        printf("read 0x%llx = 0x%llx\n", (unsigned long long)addr,
               (unsigned long long)kread64(addr));
        return 0;
    }

    if (strstr(line, "write32=")) {
        const uint64_t addr = parse_hex(line, "write32=");
        uint32_t value = (uint32_t)parse_hex(line, "value=");
        const uint32_t before = kernel_getint((intptr_t)addr);
        kernel_copyin(&value, (intptr_t)addr, sizeof value);
        const uint32_t after = kernel_getint((intptr_t)addr);
        printf("write32 0x%llx value 0x%x: before 0x%x after 0x%x %s\n",
               (unsigned long long)addr, value, before, after, after == value ? "verified" : "MISMATCH");
        return after == value ? 0 : 1;
    }

    if (strstr(line, "write=")) {
        const uint64_t addr = parse_hex(line, "write=");
        const uint64_t value = parse_hex(line, "value=");
        const uint64_t before = kread64(addr);
        kwrite64(addr, value);
        const uint64_t after = kread64(addr);
        printf("write 0x%llx value 0x%llx: before 0x%llx after 0x%llx %s\n",
               (unsigned long long)addr, (unsigned long long)value, (unsigned long long)before,
               (unsigned long long)after, after == value ? "verified" : "MISMATCH");
        return after == value ? 0 : 1;
    }

    printf("no mode: use pmap, read=0xADDR, dump=0xADDR len=N, or write=0xADDR value=0xV\n");
    return 2;
}
