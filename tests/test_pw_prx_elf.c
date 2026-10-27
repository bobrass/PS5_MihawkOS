/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "../src/pw_prx_elf.h"
#include <assert.h>
#include <elf.h>
#include <stdio.h>
#include <string.h>

/* A minimal module image: one PT_LOAD (text and data), a PT_DYNAMIC, a RELA
 * table with RELATIVE, GLOB_DAT and R_X86_64_64 entries, a separate JMPREL
 * table, an undefined NID import and a relocated PRXDESC1 descriptor. */
enum {
    ARENA_SIZE = 0x2000, IMAGE_SIZE = 0x2000,
    LOAD_OFFSET = 0x1000, LOAD_FILESZ = 0x4c0, LOAD_MEMSZ = 0x540,
    REL_SLOT = 0x100, GLOB_SLOT = 0x108, JUMP_SLOT = 0x110, WIDE_SLOT = 0x118,
    REL_ADDEND = 0x20,
    DYN_OFFSET = 0x200, DYN_COUNT = 12,
    RELA_OFFSET = 0x300, RELA_COUNT = 7,
    JMPREL_OFFSET = 0x3c0,
    SYM_OFFSET = 0x3e0, SYM_COUNT = 3,
    STR_OFFSET = 0x430,
    DESC_OFFSET = 0x470, DESC_NAMES = 0x4a0, DESC_NAMES_SECOND = 0x4ad,
    DESC_COUNT = 2
};

static _Alignas(16) uint8_t arena[ARENA_SIZE];
static uint8_t map_fill;   /* what test_map's memory holds: 0, or a sentinel the loader must not overwrite */
static _Alignas(16) uint8_t file[IMAGE_SIZE];

typedef struct Call { uintptr_t address; size_t bytes; int prot; } Call;
static Call map_log[4], protect_log[4];
static int map_calls, protect_calls, resolve_calls;
static int map_fail, protect_fail, resolve_fail;
static uintptr_t resolved_nid, resolved_plain;
static char seen_name[4][PW_PRX_ELF_MAX_NAME], seen_library[4][PW_PRX_ELF_MAX_NAME];
static int seen_has_library[4];

static void reset(void)
{
    map_calls = protect_calls = resolve_calls = 0;
    map_fail = protect_fail = resolve_fail = 0;
    memset(map_log, 0, sizeof map_log);
    memset(protect_log, 0, sizeof protect_log);
    memset(seen_name, 0, sizeof seen_name);
    memset(seen_library, 0, sizeof seen_library);
    memset(seen_has_library, 0, sizeof seen_has_library);
    memset(arena, 0xaa, sizeof arena);
}

static void *test_map(void *opaque, uintptr_t address, size_t bytes, int prot)
{
    (void)opaque;
    assert(map_calls < (int)(sizeof map_log / sizeof map_log[0]));
    map_log[map_calls++] = (Call){ address, bytes, prot };
    if (map_fail) return (void *)(uintptr_t)-1;
    /* A relocation bias: every segment comes back at arena + its linked
     * address. The image is linked at 0, so this is one shared bias. */
    if (address > (uintptr_t)ARENA_SIZE || bytes > (size_t)ARENA_SIZE - (size_t)address)
        return (void *)(uintptr_t)-1;
    memset(arena + address, map_fill, bytes);   /* a fresh mapping reads as zero (PwPrxElfMap) */
    return arena + address;
}
static void *fixed_map(void *opaque, uintptr_t address, size_t bytes, int prot)
{
    (void)opaque; (void)bytes; (void)prot;
    assert(map_calls < (int)(sizeof map_log / sizeof map_log[0]));
    map_log[map_calls++] = (Call){ address, bytes, prot };
    return (void *)address;
}
static int test_protect(void *opaque, uintptr_t address, size_t bytes, int prot)
{
    uintptr_t relative;
    (void)opaque;
    /* Protection runs after relocation: the RELATIVE slot is already fixed. */
    memcpy(&relative, arena + REL_SLOT, sizeof relative);
    assert(relative == (uintptr_t)arena + REL_ADDEND);
    assert(protect_calls < (int)(sizeof protect_log / sizeof protect_log[0]));
    protect_log[protect_calls++] = (Call){ address, bytes, prot };
    return protect_fail ? -1 : 0;
}
static uintptr_t test_resolve(void *opaque, const char *name, const char *library)
{
    (void)opaque;
    if (resolve_calls < 4) {
        snprintf(seen_name[resolve_calls], sizeof seen_name[0], "%s", name);
        seen_has_library[resolve_calls] = library != NULL;
        snprintf(seen_library[resolve_calls], sizeof seen_library[0], "%s",
                 library ? library : "");
    }
    resolve_calls++;
    if (resolve_fail) return 0;
    if (!strcmp(name, "NID12345678") && library && !strcmp(library, "libtest"))
        return resolved_nid;
    if (!strcmp(name, "plain_symbol") && !library) return resolved_plain;
    return 0;
}

static void set_rela(Elf64_Rela *rela, int index, uint64_t offset, uint32_t type,
                     uint32_t symbol, int64_t addend)
{
    rela[index].r_offset = offset;
    rela[index].r_info = ELF64_R_INFO((uint64_t)symbol, type);
    rela[index].r_addend = addend;
}

static void build_image(uint32_t load_flags, int with_descriptor)
{
    static const char strings[] = "\0NID12345678#libtest#modtest\0plain_symbol";
    Elf64_Ehdr *ehdr = (Elf64_Ehdr *)file;
    Elf64_Phdr *phdr = (Elf64_Phdr *)(file + sizeof(*ehdr));
    Elf64_Dyn *dyn;
    Elf64_Rela *rela;
    Elf64_Sym *sym;
    PwPrxDescriptor *descriptor;

    memset(file, 0, sizeof file);
    memcpy(ehdr->e_ident, ELFMAG, SELFMAG);
    ehdr->e_ident[EI_CLASS] = ELFCLASS64;
    ehdr->e_ident[EI_DATA] = ELFDATA2LSB;
    ehdr->e_ident[EI_VERSION] = EV_CURRENT;
    ehdr->e_type = ET_DYN;
    ehdr->e_machine = EM_X86_64;
    ehdr->e_version = EV_CURRENT;
    ehdr->e_phoff = sizeof(*ehdr);
    ehdr->e_ehsize = sizeof(*ehdr);
    ehdr->e_phentsize = sizeof(*phdr);
    ehdr->e_phnum = 2;
    phdr[0].p_type = PT_LOAD;
    phdr[0].p_flags = load_flags;
    phdr[0].p_offset = LOAD_OFFSET;
    phdr[0].p_vaddr = 0;
    phdr[0].p_filesz = LOAD_FILESZ;
    phdr[0].p_memsz = LOAD_MEMSZ;
    phdr[0].p_align = 0x1000;
    phdr[1].p_type = PT_DYNAMIC;
    phdr[1].p_flags = PF_R | PF_W;
    phdr[1].p_offset = LOAD_OFFSET + DYN_OFFSET;
    phdr[1].p_vaddr = DYN_OFFSET;
    phdr[1].p_filesz = DYN_COUNT * sizeof(Elf64_Dyn);
    phdr[1].p_memsz = phdr[1].p_filesz;
    memcpy(file + LOAD_OFFSET + 0x20, "code", 4);

    rela = (Elf64_Rela *)(file + LOAD_OFFSET + RELA_OFFSET);
    set_rela(rela, 0, REL_SLOT, R_X86_64_RELATIVE, 0, REL_ADDEND);
    set_rela(rela, 1, GLOB_SLOT, R_X86_64_GLOB_DAT, 1, 0);
    set_rela(rela, 2, WIDE_SLOT, R_X86_64_64, 2, 8);
    set_rela(rela, 3, DESC_OFFSET + 16, R_X86_64_RELATIVE, 0, DESC_NAMES);
    set_rela(rela, 4, DESC_OFFSET + 24, R_X86_64_RELATIVE, 0, 0x30);
    set_rela(rela, 5, DESC_OFFSET + 32, R_X86_64_RELATIVE, 0, DESC_NAMES_SECOND);
    set_rela(rela, 6, DESC_OFFSET + 40, R_X86_64_RELATIVE, 0, 0x34);
    rela = (Elf64_Rela *)(file + LOAD_OFFSET + JMPREL_OFFSET);
    set_rela(rela, 0, JUMP_SLOT, R_X86_64_JUMP_SLOT, 1, 0);

    sym = (Elf64_Sym *)(file + LOAD_OFFSET + SYM_OFFSET);
    sym[1].st_name = 1;
    sym[1].st_info = ELF64_ST_INFO(STB_GLOBAL, STT_FUNC);
    sym[1].st_shndx = SHN_UNDEF;
    sym[2].st_name = 1 + 28;
    sym[2].st_info = ELF64_ST_INFO(STB_GLOBAL, STT_FUNC);
    sym[2].st_shndx = SHN_UNDEF;
    assert(sizeof strings == 42);
    memcpy(file + LOAD_OFFSET + STR_OFFSET, strings, sizeof strings);

    dyn = (Elf64_Dyn *)(file + LOAD_OFFSET + DYN_OFFSET);
    dyn[0].d_tag = DT_RELA; dyn[0].d_un.d_ptr = RELA_OFFSET;
    dyn[1].d_tag = DT_RELASZ; dyn[1].d_un.d_val = RELA_COUNT * sizeof(Elf64_Rela);
    dyn[2].d_tag = DT_RELAENT; dyn[2].d_un.d_val = sizeof(Elf64_Rela);
    dyn[3].d_tag = DT_SYMTAB; dyn[3].d_un.d_ptr = SYM_OFFSET;
    dyn[4].d_tag = DT_SYMENT; dyn[4].d_un.d_val = sizeof(Elf64_Sym);
    dyn[5].d_tag = DT_STRTAB; dyn[5].d_un.d_ptr = STR_OFFSET;
    dyn[6].d_tag = DT_STRSZ; dyn[6].d_un.d_val = sizeof strings;
    dyn[7].d_tag = DT_JMPREL; dyn[7].d_un.d_ptr = JMPREL_OFFSET;
    dyn[8].d_tag = DT_PLTRELSZ; dyn[8].d_un.d_val = sizeof(Elf64_Rela);
    dyn[9].d_tag = DT_PLTREL; dyn[9].d_un.d_val = DT_RELA;
    dyn[10].d_tag = DT_NULL;
    dyn[11].d_tag = DT_NULL;
    assert((size_t)(SYM_OFFSET + SYM_COUNT * sizeof(Elf64_Sym)) <= STR_OFFSET);

    descriptor = (PwPrxDescriptor *)(file + LOAD_OFFSET + DESC_OFFSET);
    descriptor->magic = with_descriptor ? PW_PRX_MAGIC : 0;
    descriptor->version = PW_PRX_VERSION;
    descriptor->count = DESC_COUNT;
    descriptor->exports[0].name = (const char *)(uintptr_t)DESC_NAMES;
    descriptor->exports[0].address = (const void *)(uintptr_t)0x30;
    descriptor->exports[1].name = (const char *)(uintptr_t)DESC_NAMES_SECOND;
    descriptor->exports[1].address = (const void *)(uintptr_t)0x34;
    memcpy(file + LOAD_OFFSET + DESC_NAMES, "module_start", 13);
    memcpy(file + LOAD_OFFSET + DESC_NAMES_SECOND, "__wine_main", 12);
}

int main(void)
{
    PwPrxElfImage image;
    int status;

    /* Mapped with a relocation bias: the image comes back at arena+0. */
    reset();
    build_image(PF_R | PF_W | PF_X, 1);
    resolved_nid = (uintptr_t)arena + 0x1e0;
    resolved_plain = (uintptr_t)arena + 0x1f0;
    status = pw_prx_elf_load(file, sizeof file, 0, test_map, test_protect, test_resolve, NULL,
                             &image);
    assert(status == PW_PRX_ELF_OK);
    assert(image.base == (uintptr_t)arena && image.span == LOAD_MEMSZ);
    assert(image.segment_count == 1 && image.segments[0].address == arena &&
           image.segments[0].size == LOAD_MEMSZ &&
           image.segments[0].protection == (PW_PROT_READ | PW_PROT_WRITE | PW_PROT_EXEC));
    assert(map_calls == 1 && map_log[0].address == 0 && map_log[0].bytes == LOAD_MEMSZ &&
           map_log[0].prot == (PW_PROT_READ | PW_PROT_WRITE));
    assert(protect_calls == 1 && protect_log[0].address == (uintptr_t)arena &&
           protect_log[0].bytes == LOAD_MEMSZ &&
           protect_log[0].prot == (PW_PROT_READ | PW_PROT_WRITE | PW_PROT_EXEC));
    assert(!memcmp(arena + 0x20, "code", 4));
    assert(*(uintptr_t *)(arena + REL_SLOT) == (uintptr_t)arena + REL_ADDEND);
    assert(*(uintptr_t *)(arena + GLOB_SLOT) == resolved_nid);
    assert(*(uintptr_t *)(arena + JUMP_SLOT) == resolved_nid);
    assert(*(uintptr_t *)(arena + WIDE_SLOT) == resolved_plain + 8);
    for (size_t i = LOAD_FILESZ; i < LOAD_MEMSZ; i++) assert(arena[i] == 0);
    assert(image.descriptor == (const PwPrxDescriptor *)(arena + DESC_OFFSET));
    assert(pw_prx_lookup(image.descriptor, "module_start") == (const void *)(arena + 0x30));
    assert(pw_prx_lookup(image.descriptor, "__wine_main") == (const void *)(arena + 0x34));
    assert(image.module_start ==
           (int (*)(size_t, const void *))(uintptr_t)(arena + 0x30));
    assert(resolve_calls == 3);
    assert(!strcmp(seen_name[0], "NID12345678") && seen_has_library[0] &&
           !strcmp(seen_library[0], "libtest"));
    assert(!strcmp(seen_name[1], "plain_symbol") && !seen_has_library[1]);
    assert(!strcmp(seen_name[2], "NID12345678") && seen_has_library[2] &&
           !strcmp(seen_library[2], "libtest"));

    /* The zero-filled tail is the map's (fresh anonymous memory reads as zero): the loader never writes it, since each
     * page it wrote became resident, ntdll's 11.4 MiB in every helper (fmem-steam-kinds-2026-10-25-01). */
    reset();
    build_image(PF_R | PF_W | PF_X, 1);
    map_fill = 0x5a;
    assert(pw_prx_elf_load(file, sizeof file, 0, test_map, test_protect, test_resolve, NULL, &image) == PW_PRX_ELF_OK);
    map_fill = 0;
    for (size_t i = LOAD_FILESZ; i < LOAD_MEMSZ; i++) assert(arena[i] == 0x5a);

    /* Fixed mapping at a caller-selected base, with the converter's 0xFE18
     * module type; text only, so protect is RX. */
    reset();
    build_image(PF_R | PF_X, 1);
    ((Elf64_Ehdr *)file)->e_type = 0xfe18;
    status = pw_prx_elf_load(file, sizeof file, (uintptr_t)arena, fixed_map, test_protect,
                             test_resolve, NULL, &image);
    assert(status == PW_PRX_ELF_OK && image.base == (uintptr_t)arena);
    assert(map_log[0].address == (uintptr_t)arena &&
           protect_log[0].prot == (PW_PROT_READ | PW_PROT_EXEC));
    assert(*(uintptr_t *)(arena + REL_SLOT) == (uintptr_t)arena + REL_ADDEND);

    /* An unresolved import is a typed failure, with the mapping reported. */
    reset();
    build_image(PF_R | PF_W, 1);
    resolve_fail = 1;
    status = pw_prx_elf_load(file, sizeof file, 0, test_map, test_protect, test_resolve, NULL,
                             &image);
    assert(status == PW_PRX_ELF_ERR_UNRESOLVED);
    assert(image.segment_count == 1 && protect_calls == 0);

    /* A refused (or inconsistent) mapping fails cleanly. */
    reset();
    build_image(PF_R | PF_W, 1);
    map_fail = 1;
    assert(pw_prx_elf_load(file, sizeof file, 0, test_map, test_protect, test_resolve, NULL,
                           &image) == PW_PRX_ELF_ERR_MAP);
    assert(image.segment_count == 0);
    reset();
    build_image(PF_R | PF_W, 1);
    protect_fail = 1;
    assert(pw_prx_elf_load(file, sizeof file, 0, test_map, test_protect, test_resolve, NULL,
                           &image) == PW_PRX_ELF_ERR_PROTECT);

    /* No descriptor, a non-dynamic ELF and a truncated buffer are refused. */
    reset();
    build_image(PF_R | PF_W, 0);
    assert(pw_prx_elf_load(file, sizeof file, 0, test_map, test_protect, test_resolve, NULL,
                           &image) == PW_PRX_ELF_ERR_DESCRIPTOR);
    reset();
    build_image(PF_R | PF_W, 1);
    ((Elf64_Ehdr *)file)->e_type = ET_EXEC;
    assert(pw_prx_elf_load(file, sizeof file, 0, test_map, test_protect, test_resolve, NULL,
                           &image) == PW_PRX_ELF_ERR_FORMAT);
    assert(pw_prx_elf_load(file, 8, 0, test_map, test_protect, test_resolve, NULL, &image) ==
           PW_PRX_ELF_ERR_TRUNCATED);
    assert(pw_prx_elf_load(NULL, sizeof file, 0, test_map, test_protect, test_resolve, NULL,
                           &image) == PW_PRX_ELF_ERR_ARGUMENT);

    /* The helper's loader choice: this loader unless the environment says WINE_PS5_PRX_LOADER=0. */
    {
        const char *reason = NULL;
        static const char none[] = "WINEDEBUG=-all\0WINE_PS5_CHILD=1\0";
        static const char off[] = "WINEDEBUG=-all\0WINE_PS5_PRX_LOADER=0\0";
        static const char on[] = "WINE_PS5_PRX_LOADER=1\0";
        static const char other[] = "WINE_PS5_PRX_LOADER=00\0WINE_PS5_PRX_LOADER=0\0";  /* first entry wins */
        static const char empty_value[] = "WINE_PS5_PRX_LOADER=\0";
        static const char unended[] = {'W','I','N','E','_','P','S','5','_','P','R','X','_','L','O','A','D','E','R','=','0'};
        assert(pw_prx_elf_wanted(none, sizeof none, &reason) == 1 && !strcmp(reason, "default"));
        assert(pw_prx_elf_wanted(off, sizeof off, &reason) == 0 && !strcmp(reason, "WINE_PS5_PRX_LOADER=0"));
        assert(pw_prx_elf_wanted(on, sizeof on, &reason) == 1 && !strcmp(reason, "WINE_PS5_PRX_LOADER=1"));
        assert(pw_prx_elf_wanted(other, sizeof other, &reason) == 1 && !strcmp(reason, "WINE_PS5_PRX_LOADER=00"));
        assert(pw_prx_elf_wanted(empty_value, sizeof empty_value, &reason) == 1 && !strcmp(reason, "default"));
        assert(pw_prx_elf_wanted(unended, sizeof unended, &reason) == 1 && !strcmp(reason, "default"));
        assert(pw_prx_elf_wanted(off, 0, &reason) == 1 && pw_prx_elf_wanted(NULL, 64, NULL) == 1);
    }

    printf("prx elf passed: PT_LOAD map/copy/BSS, RELATIVE/GLOB_DAT/JUMP_SLOT/R_X86_64_64, "
           "RX/RW protection after relocation, descriptor and module_start, typed failures, "
           "loader choice (default and WINE_PS5_PRX_LOADER=0)\n");
    return 0;
}
