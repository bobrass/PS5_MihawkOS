/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pw_prx_elf.h"
#include <string.h>

/* ELF64 constants, kept local: the core must build on any host, including
 * ones without <elf.h>. All fields are read with memcpy so the input may be
 * arbitrarily aligned. */
enum {
    ELF64_EHDR_SIZE = 64, ELF64_PHDR_SIZE = 56, ELF64_SYM_SIZE = 24, ELF64_RELA_SIZE = 24,
    ET_DYN = 3, ET_SCE_MODULE = 0xfe18, EM_X86_64 = 62, PT_LOAD = 1, PT_DYNAMIC = 2,
    PF_X = 1, PF_W = 2,
    DT_NULL = 0, DT_PLTRELSZ = 2, DT_STRTAB = 5, DT_SYMTAB = 6, DT_RELA = 7, DT_RELASZ = 8,
    DT_RELAENT = 9, DT_STRSZ = 10, DT_SYMENT = 11, DT_REL = 17, DT_RELSZ = 18,
    DT_PLTREL = 20, DT_JMPREL = 23,
    R_X86_64_NONE = 0, R_X86_64_64 = 1, R_X86_64_GLOB_DAT = 6, R_X86_64_JUMP_SLOT = 7,
    R_X86_64_RELATIVE = 8, SHN_UNDEF = 0
};

typedef struct PwPrxElfSegment {
    uint64_t offset, vaddr, filesz, memsz;
    uint32_t flags;
} PwPrxElfSegment;

static uint16_t read16(const void *at)
{
    uint16_t value;
    memcpy(&value, at, sizeof value);
    return value;
}
static uint32_t read32(const void *at)
{
    uint32_t value;
    memcpy(&value, at, sizeof value);
    return value;
}
static uint64_t read64(const void *at)
{
    uint64_t value;
    memcpy(&value, at, sizeof value);
    return value;
}
static int64_t read64s(const void *at)
{
    int64_t value;
    memcpy(&value, at, sizeof value);
    return value;
}
static int file_range(uint64_t offset, uint64_t bytes, size_t size)
{
    return offset <= (uint64_t)size && bytes <= (uint64_t)size - offset;
}
static int image_contains(const PwPrxElfImage *image, uintptr_t address, size_t bytes)
{
    for (uint32_t i = 0; i < image->segment_count; i++) {
        uintptr_t start = (uintptr_t)image->segments[i].address;
        size_t size = image->segments[i].size;
        if (address >= start && address - start <= size && bytes <= size - (address - start)) return 1;
    }
    return 0;
}
static int image_range(const PwPrxElfImage *image, uint64_t vaddr, size_t bytes, uintptr_t *out)
{
    uintptr_t address;
    if (vaddr > UINTPTR_MAX - image->base) return PW_PRX_ELF_ERR_OVERFLOW;
    address = image->base + (uintptr_t)vaddr;
    if (!image_contains(image, address, bytes)) return PW_PRX_ELF_ERR_RELOCATION;
    if (out) *out = address;
    return PW_PRX_ELF_OK;
}

/* st_value of a defined symbol is the linked vaddr; an undefined symbol is
 * resolved through the caller, which owns the "NID#library#module" mapping. */
static int symbol_address(const PwPrxElfImage *image, uintptr_t symtab, uintptr_t strtab,
                          uint64_t strtab_size, uint32_t index, PwPrxElfResolve resolve,
                          void *opaque, uintptr_t *out)
{
    const uint8_t *symbol;
    uintptr_t entry;
    uint32_t name;
    uint16_t section;
    uint64_t value;
    size_t length;
    const char *text, *hash, *end;
    char plain[PW_PRX_ELF_MAX_NAME], library[PW_PRX_ELF_MAX_NAME];

    if (!index) return PW_PRX_ELF_ERR_RELOCATION;
    if (index > (UINTPTR_MAX - symtab) / ELF64_SYM_SIZE) return PW_PRX_ELF_ERR_OVERFLOW;
    entry = symtab + (uintptr_t)index * ELF64_SYM_SIZE;
    if (!image_contains(image, entry, ELF64_SYM_SIZE)) return PW_PRX_ELF_ERR_RELOCATION;
    symbol = (const uint8_t *)entry;
    name = read32(symbol);
    section = read16(symbol + 6);
    value = read64(symbol + 8);
    if (section != SHN_UNDEF) {
        if (value > UINTPTR_MAX - image->base) return PW_PRX_ELF_ERR_OVERFLOW;
        *out = image->base + (uintptr_t)value;
        return PW_PRX_ELF_OK;
    }
    if (!resolve || !strtab || !strtab_size || name >= strtab_size)
        return PW_PRX_ELF_ERR_RELOCATION;
    text = (const char *)(strtab + name);
    end = (const char *)memchr(text, 0, (size_t)(strtab_size - name));
    if (!end || end == text) return PW_PRX_ELF_ERR_RELOCATION;
    length = (size_t)(end - text);
    hash = memchr(text, '#', length);
    if (hash) {
        const char *library_start = hash + 1;
        const char *library_end = memchr(library_start, '#', (size_t)(end - library_start));
        size_t name_length = (size_t)(hash - text);
        size_t library_length = library_end ? (size_t)(library_end - library_start)
                                            : (size_t)(end - library_start);
        if (!name_length || name_length >= sizeof plain || library_length >= sizeof library)
            return PW_PRX_ELF_ERR_LIMIT;
        memcpy(plain, text, name_length);
        plain[name_length] = 0;
        memcpy(library, library_start, library_length);
        library[library_length] = 0;
        *out = resolve(opaque, plain, library);
    } else {
        if (length >= sizeof plain) return PW_PRX_ELF_ERR_LIMIT;
        memcpy(plain, text, length);
        plain[length] = 0;
        *out = resolve(opaque, plain, NULL);
    }
    return *out ? PW_PRX_ELF_OK : PW_PRX_ELF_ERR_UNRESOLVED;
}

static int apply_rela(PwPrxElfImage *image, uintptr_t table, uint64_t count, uintptr_t symtab,
                      uintptr_t strtab, uint64_t strtab_size, PwPrxElfResolve resolve,
                      void *opaque)
{
    for (uint64_t i = 0; i < count; i++) {
        uintptr_t entry = table + (uintptr_t)(i * ELF64_RELA_SIZE);
        uint64_t offset = read64((const void *)entry);
        uint64_t info = read64((const void *)(entry + 8));
        int64_t addend = read64s((const void *)(entry + 16));
        uint32_t type = (uint32_t)info, index = (uint32_t)(info >> 32);
        uintptr_t where, value;
        int status;

        if (type == R_X86_64_NONE) continue;
        if (image_range(image, offset, sizeof value, &where) != PW_PRX_ELF_OK)
            return PW_PRX_ELF_ERR_RELOCATION;
        if (type == R_X86_64_RELATIVE) {
            uint64_t add = (uint64_t)addend;
            if (add > UINTPTR_MAX - image->base) return PW_PRX_ELF_ERR_OVERFLOW;
            value = image->base + (uintptr_t)add;
        } else if (type == R_X86_64_64 || type == R_X86_64_GLOB_DAT ||
                   type == R_X86_64_JUMP_SLOT) {
            if (!symtab || !strtab) return PW_PRX_ELF_ERR_RELOCATION;
            if ((status = symbol_address(image, symtab, strtab, strtab_size, index, resolve,
                                         opaque, &value)) != PW_PRX_ELF_OK)
                return status;
            if (type == R_X86_64_64) value = (uintptr_t)(value + (uintptr_t)addend);
        } else {
            return PW_PRX_ELF_ERR_UNSUPPORTED;
        }
        memcpy((void *)where, &value, sizeof value);
    }
    return PW_PRX_ELF_OK;
}

int pw_prx_elf_load(const void *bytes, size_t size, uintptr_t preferred_base,
                    const PwPrxElfMap map, const PwPrxElfProtect protect,
                    const PwPrxElfResolve resolve, void *opaque, PwPrxElfImage *out)
{
    PwPrxElfSegment loads[PW_PRX_ELF_MAX_SEGMENTS];
    uint32_t load_count = 0;
    uint64_t phoff, dynamic_offset = 0, dynamic_size = 0;
    uint64_t rela_vaddr = 0, rela_size = 0, symtab_vaddr = 0, strtab_vaddr = 0, strtab_size = 0;
    uint64_t jmprel_vaddr = 0, jmprel_size = 0, rel_size = 0;
    int have_dynamic = 0, have_rela = 0, have_symtab = 0, have_strtab = 0, have_jmprel = 0;
    uintptr_t bias = 0, high = 0, rela = 0, symtab = 0, strtab = 0, jmprel = 0;
    uint64_t rela_count = 0, jmprel_count = 0;
    uint32_t phnum;
    const uint8_t *elf = bytes;
    int status;

    if (!bytes || !size || !map || !protect || !out) return PW_PRX_ELF_ERR_ARGUMENT;
    memset(out, 0, sizeof(*out));
    if (sizeof(uintptr_t) < sizeof(uint64_t) || sizeof(size_t) < sizeof(uint64_t))
        return PW_PRX_ELF_ERR_UNSUPPORTED;
    if (size < ELF64_EHDR_SIZE) return PW_PRX_ELF_ERR_TRUNCATED;
    if (memcmp(elf, "\x7f" "ELF", 4) || elf[4] != 2 || elf[5] != 1 || elf[6] != 1 ||
        (read16(elf + 16) != ET_DYN && read16(elf + 16) != ET_SCE_MODULE) ||
        read16(elf + 18) != EM_X86_64 || read32(elf + 20) != 1 ||
        read16(elf + 54) != ELF64_PHDR_SIZE)
        return PW_PRX_ELF_ERR_FORMAT;
    phoff = read64(elf + 32);
    phnum = read16(elf + 56);
    if (!phnum) return PW_PRX_ELF_ERR_FORMAT;
    if (!file_range(phoff, (uint64_t)phnum * ELF64_PHDR_SIZE, size))
        return PW_PRX_ELF_ERR_TRUNCATED;

    for (uint32_t i = 0; i < phnum; i++) {
        const uint8_t *ph = elf + phoff + (uint64_t)i * ELF64_PHDR_SIZE;
        uint32_t type = read32(ph), flags = read32(ph + 4);
        uint64_t offset = read64(ph + 8), vaddr = read64(ph + 16);
        uint64_t filesz = read64(ph + 32), memsz = read64(ph + 40);

        if (type == PT_LOAD) {
            if (load_count == PW_PRX_ELF_MAX_SEGMENTS) return PW_PRX_ELF_ERR_LIMIT;
            if (!memsz || filesz > memsz || memsz > UINT32_MAX) return PW_PRX_ELF_ERR_FORMAT;
            if (!file_range(offset, filesz, size)) return PW_PRX_ELF_ERR_TRUNCATED;
            if (vaddr > UINTPTR_MAX - preferred_base ||
                memsz > UINTPTR_MAX - (preferred_base + (uintptr_t)vaddr))
                return PW_PRX_ELF_ERR_OVERFLOW;
            loads[load_count++] = (PwPrxElfSegment){ offset, vaddr, filesz, memsz, flags };
        } else if (type == PT_DYNAMIC) {
            if (!file_range(offset, filesz, size)) return PW_PRX_ELF_ERR_TRUNCATED;
            if (!filesz || filesz % 16) return PW_PRX_ELF_ERR_FORMAT;
            if (!have_dynamic) {
                have_dynamic = 1;
                dynamic_offset = offset;
                dynamic_size = filesz;
            }
        }
    }
    if (!load_count || !have_dynamic) return PW_PRX_ELF_ERR_FORMAT;

    for (uint32_t i = 0; i < load_count; i++) {
        PwPrxElfSegment *load = &loads[i];
        uintptr_t desired = preferred_base + (uintptr_t)load->vaddr;
        void *mapped = map(opaque, desired, (size_t)load->memsz,
                           PW_PROT_READ | PW_PROT_WRITE);
        uintptr_t got, delta;
        int prot = PW_PROT_READ;

        if (!mapped || mapped == (void *)(uintptr_t)-1) return PW_PRX_ELF_ERR_MAP;
        got = (uintptr_t)mapped;
        delta = got - desired;
        if (out->segment_count && delta != bias) return PW_PRX_ELF_ERR_MAP;
        if (!out->segment_count) bias = delta;
        /* The rest of memsz is already zero (PwPrxElfMap): writing it made every page resident, ntdll's 11.4 MiB of
         * zero-filled data in every helper (fmem-steam-kinds-2026-10-25-01). */
        memcpy(mapped, elf + (size_t)load->offset, (size_t)load->filesz);
        if (load->flags & PF_W) prot |= PW_PROT_WRITE;
        if (load->flags & PF_X) prot |= PW_PROT_EXEC;
        out->segments[out->segment_count] = (PwPrxSegment){ mapped, (uint32_t)load->memsz,
                                                            (uint32_t)prot };
        out->segment_count++;
        if (load->memsz > UINTPTR_MAX - got) return PW_PRX_ELF_ERR_OVERFLOW;
        if (got + (uintptr_t)load->memsz > high) high = got + (uintptr_t)load->memsz;
    }
    if (bias > UINTPTR_MAX - preferred_base) return PW_PRX_ELF_ERR_OVERFLOW;
    out->base = preferred_base + bias;
    if (high < out->base) return PW_PRX_ELF_ERR_OVERFLOW;
    out->span = (size_t)(high - out->base);

    for (uint64_t entry = 0; entry < dynamic_size; entry += 16) {
        const uint8_t *at = elf + (size_t)dynamic_offset + (size_t)entry;
        int64_t tag = read64s(at);
        uint64_t value = read64(at + 8);

        switch (tag) {
        case DT_NULL:
            break;
        case DT_RELA: rela_vaddr = value; have_rela = 1; break;
        case DT_RELASZ: rela_size = value; break;
        case DT_RELAENT:
            if (value != ELF64_RELA_SIZE) return PW_PRX_ELF_ERR_FORMAT;
            break;
        case DT_SYMTAB: symtab_vaddr = value; have_symtab = 1; break;
        case DT_SYMENT:
            if (value != ELF64_SYM_SIZE) return PW_PRX_ELF_ERR_FORMAT;
            break;
        case DT_STRTAB: strtab_vaddr = value; have_strtab = 1; break;
        case DT_STRSZ: strtab_size = value; break;
        case DT_JMPREL: jmprel_vaddr = value; have_jmprel = 1; break;
        case DT_PLTRELSZ: jmprel_size = value; break;
        case DT_PLTREL:
            if (value != DT_RELA) return PW_PRX_ELF_ERR_UNSUPPORTED;
            break;
        case DT_REL:
            break;
        case DT_RELSZ: rel_size = value; break;
        default:
            break;
        }
    }
    if (rel_size) return PW_PRX_ELF_ERR_UNSUPPORTED;
    if (rela_size % ELF64_RELA_SIZE || jmprel_size % ELF64_RELA_SIZE)
        return PW_PRX_ELF_ERR_FORMAT;
    if (have_rela || rela_size) {
        if (!rela_size) {
            have_rela = 0;
        } else {
            if (!have_rela) return PW_PRX_ELF_ERR_FORMAT;
            if (image_range(out, rela_vaddr, (size_t)rela_size, &rela) != PW_PRX_ELF_OK)
                return PW_PRX_ELF_ERR_RELOCATION;
            rela_count = rela_size / ELF64_RELA_SIZE;
        }
    }
    if (have_jmprel || jmprel_size) {
        if (!jmprel_size) {
            have_jmprel = 0;
        } else {
            if (!have_jmprel) return PW_PRX_ELF_ERR_FORMAT;
            if (image_range(out, jmprel_vaddr, (size_t)jmprel_size, &jmprel) != PW_PRX_ELF_OK)
                return PW_PRX_ELF_ERR_RELOCATION;
            jmprel_count = jmprel_size / ELF64_RELA_SIZE;
        }
    }
    if (have_symtab && image_range(out, symtab_vaddr, ELF64_SYM_SIZE, &symtab) != PW_PRX_ELF_OK)
        return PW_PRX_ELF_ERR_RELOCATION;
    if (have_strtab && strtab_size &&
        image_range(out, strtab_vaddr, (size_t)strtab_size, &strtab) != PW_PRX_ELF_OK)
        return PW_PRX_ELF_ERR_RELOCATION;

    if (rela_count) {
        status = apply_rela(out, rela, rela_count, symtab, strtab, strtab_size, resolve, opaque);
        if (status != PW_PRX_ELF_OK) return status;
    }
    if (jmprel_count) {
        int inside = have_rela && rela_size && jmprel_vaddr >= rela_vaddr &&
                     jmprel_vaddr - rela_vaddr <= rela_size &&
                     jmprel_size <= rela_size - (jmprel_vaddr - rela_vaddr);
        if (!inside) {
            status = apply_rela(out, jmprel, jmprel_count, symtab, strtab, strtab_size,
                                resolve, opaque);
            if (status != PW_PRX_ELF_OK) return status;
        }
    }

    for (uint32_t i = 0; i < out->segment_count; i++) {
        if (protect(opaque, (uintptr_t)out->segments[i].address, out->segments[i].size,
                    (int)out->segments[i].protection) != 0)
            return PW_PRX_ELF_ERR_PROTECT;
    }

    {
        uint32_t scan = out->segment_count;
        if (scan > PW_PRX_MAX_SEGMENTS) scan = PW_PRX_MAX_SEGMENTS;
        if (pw_prx_find_descriptor(out->segments, scan, &out->descriptor) != PW_PRX_OK)
            return PW_PRX_ELF_ERR_DESCRIPTOR;
    }
    out->module_start =
        (int (*)(size_t, const void *))pw_prx_lookup(out->descriptor, "module_start");
    return PW_PRX_ELF_OK;
}

int pw_prx_elf_wanted(const char *environment, size_t bytes, const char **reason)
{
    static const char name[] = "WINE_PS5_PRX_LOADER=";
    const size_t name_length = sizeof(name) - 1;
    if (reason) *reason = "default";
    for (size_t at = 0; environment && at < bytes && environment[at];) {
        const char *entry = environment + at;
        const size_t length = strnlen(entry, bytes - at);
        if (length == bytes - at) break;   /* an unended last entry is not read (reason is printed with %s) */
        if (length > name_length && !strncmp(entry, name, name_length)) {
            if (reason) *reason = entry;
            return !(length == name_length + 1 && entry[name_length] == '0');
        }
        at += length + 1;
    }
    return 1;
}
