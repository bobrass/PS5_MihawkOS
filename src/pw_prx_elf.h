/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PW_PRX_ELF_H
#define PW_PRX_ELF_H
/*
 * User-space loader for the unsigned Wine module ELFs (the staged
 * "<name>.prx.elf" files). A payload helper runs in a system carrier process,
 * and whether sceKernelLoadStartModule can load a fake-signed SELF PRX there
 * is up to the console's jailbreak tools, not the firmware: the kernel stopped
 * helpers at ntdll.prx's text (pager error, errno 92) on 11.00, 12.40, 13.40
 * and 13.59 while the title loaded the same files. So every helper maps the
 * unsigned ELF itself and registers the result with ntdll (pw_prx_elf_wanted).
 *
 * The core is pure: it parses an ELF64 little-endian image from a
 * caller-supplied buffer (the converter emits type 0xFE18, SCE's dynamic
 * module type; plain ET_DYN is accepted too) and asks the caller to map,
 * protect and resolve. There is no file I/O, no allocation and no execution
 * here; module_start is returned, never called. A failed load leaves the
 * mappings in `out` so the caller can release them (there is deliberately no
 * unmap callback).
 *
 * Callback contracts:
 *  - map maps at least [address, address+bytes) at `address`, rounding to
 *    platform pages as needed, and returns the address corresponding to the
 *    first byte. NULL or MAP_FAILED fails the load. Every segment must come
 *    back with the same bias (fixed mapping, or one shared relocation), or
 *    the load fails: RELA addends are linked virtual addresses.
 *  - protect applies a page protection after every relocation has been
 *    written. Segments may share a page at the platform's granularity
 *    (libfreetype's last two PT_LOADs do on 16 KiB pages), so the callback
 *    owns resolving overlapping requests. Nonzero fails the load.
 *  - resolve returns the address of an undefined symbol, or 0. `name` is the
 *    dynamic symbol name; the runtime's import form "NID#lib#mod" is split
 *    into name="NID" and library="lib" (NULL for a plain name). The library
 *    string is passed verbatim: the staged modules encode it as the one
 *    letter that their SCE_IMPORT_LIB dynamic tags map to a library name.
 *
 * Assumptions the console side must satisfy:
 *  - preferred_base is the would-be address of virtual address 0. Pass a
 *    nonzero, suitably aligned base: the staged modules are linked at 0.
 *  - The image's PT_LOADs use p_align 0x4000; map the whole segment span at
 *    its linked address and copy the bytes there. Map writable first, then
 *    protect after the load (text RX, data RW).
 *  - PRXDESC1 must lie in one of the first PW_PRX_MAX_SEGMENTS PT_LOADs:
 *    pw_prx_find_descriptor has that capacity and the loader passes it those.
 */
#include <stddef.h>
#include <stdint.h>
#include "../include/prospero_win.h"
#include "../wine/ps5/pw_wine_prx.h"

enum {
    /* The staged modules have five PT_LOADs (the last one, p_flags 0, carries
     * .dynamic and .dynstr); the kernel's module info reports four. */
    PW_PRX_ELF_MAX_SEGMENTS = 8,
    /* Longest dynamic symbol name or library field accepted by the splitter. */
    PW_PRX_ELF_MAX_NAME = 256,

    PW_PRX_ELF_OK = 0,
    PW_PRX_ELF_ERR_ARGUMENT = -1,     /* null or unusable argument */
    PW_PRX_ELF_ERR_FORMAT = -2,       /* not an ELF64 LE x86-64 ET_DYN/0xFE18 image */
    PW_PRX_ELF_ERR_TRUNCATED = -3,    /* a header or table is past `size` */
    PW_PRX_ELF_ERR_OVERFLOW = -4,     /* an address or size would wrap */
    PW_PRX_ELF_ERR_LIMIT = -5,        /* more PT_LOADs than the image holds */
    PW_PRX_ELF_ERR_MAP = -6,          /* map failed or changed the image bias */
    PW_PRX_ELF_ERR_RELOCATION = -7,   /* bad RELA entry, symbol or target */
    PW_PRX_ELF_ERR_UNRESOLVED = -8,   /* resolve returned 0 */
    PW_PRX_ELF_ERR_PROTECT = -9,      /* protect failed */
    PW_PRX_ELF_ERR_DESCRIPTOR = -10,  /* no valid PRXDESC1 in the image */
    PW_PRX_ELF_ERR_UNSUPPORTED = -11  /* DT_REL, or a relocation type we do not apply */
};

/* bytes of memory at address (or anywhere, the bias then applies to every segment) that reads as zero: a fresh
 * anonymous mapping. The loader writes only a segment's file bytes, never its zero-filled tail. */
typedef void *(*PwPrxElfMap)(void *opaque, uintptr_t address, size_t bytes, int prot);
typedef int (*PwPrxElfProtect)(void *opaque, uintptr_t address, size_t bytes, int prot);
typedef uintptr_t (*PwPrxElfResolve)(void *opaque, const char *name, const char *library);

typedef struct PwPrxElfImage {
    uintptr_t base;                 /* address of virtual address 0 */
    size_t span;                    /* end of the highest segment, from base */
    PwPrxSegment segments[PW_PRX_ELF_MAX_SEGMENTS];
    uint32_t segment_count;         /* mapped segments, valid even on failure */
    const PwPrxDescriptor *descriptor;
    int (*module_start)(size_t, const void *); /* NULL when not exported */
} PwPrxElfImage;

/* Map, copy, relocate, protect and find the descriptor of one module image.
 * Returns PW_PRX_ELF_OK or a negative status. On failure after mapping began,
 * out->segments[0..segment_count) names what the caller must release. */
int pw_prx_elf_load(const void *bytes, size_t size, uintptr_t preferred_base,
                    const PwPrxElfMap map, const PwPrxElfProtect protect,
                    const PwPrxElfResolve resolve, void *opaque,
                    PwPrxElfImage *out);

/* Whether a payload helper loads Wine's modules with this loader (1) or the kernel's (0), from its environment block
 * ("NAME=VALUE" strings, each NUL-ended, an empty one or `bytes` ending it). This loader on every firmware;
 * WINE_PS5_PRX_LOADER=0 asks for the kernel's, for an A/B on a console whose tools allow it. `reason`, when not NULL,
 * gets "default", "WINE_PS5_PRX_LOADER=0" or "WINE_PS5_PRX_LOADER=<value>" for the helper's log. */
int pw_prx_elf_wanted(const char *environment, size_t bytes, const char **reason);
#endif
