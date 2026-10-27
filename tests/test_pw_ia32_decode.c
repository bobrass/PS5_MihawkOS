/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The IA-32 table, decoder and printer (docs/IA32_DESIGN.md, M1), in 32-bit mode and in long mode
 * (docs/MMU4K.md, section 4.1). The LLVM oracle (tools/ia32-decode-oracle.sh, --64) checks millions
 * of encodings against another disassembler; this test checks what an oracle cannot: the table's own
 * rules, exact fields (REX, VEX, RIP-relative, the offsets the engine rewrites at), pw_ia32_access,
 * every truncation, the 15-byte limit, and that random bytes never make the decoder or printer touch
 * memory they were not given (it runs under AddressSanitizer). */
#include "../src/pw_ia32_decode.h"
#include "../src/pw_ia32_print.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static size_t hex_bytes(const char *hex, uint8_t *out, size_t capacity)
{
    size_t n = 0;
    for (const char *p = hex; p[0] && p[1] && n < capacity; p += 2) {
        if (*p == ' ') { p--; continue; }
        unsigned value;
        sscanf(p, "%2x", &value);
        out[n++] = (uint8_t)value;
    }
    return n;
}

static int modrm_shape(const PwIa32Form *form)
{
    return strchr("mBZfF3QVvx", pw_ia32_shape(form->map, form->opcode)) != NULL;
}

/* The table keeps its own rules. */
static void test_table_rules(void)
{
    const size_t count = pw_ia32_form_count();
    assert(count > 300);
    for (size_t i = 0; i < count; i++) {
        const PwIa32Form *form = pw_ia32_form_at(i);
        assert(form->name[0] && form->mnemonic[0]);
        for (size_t j = 0; j < i; j++) assert(strcmp(form->name, pw_ia32_form_at(j)->name));
        const char shape = pw_ia32_shape(form->map, form->opcode);
        assert(shape != '-' && shape != 'P' && shape != '0' && shape != '8' && shape != 'A');
        if (form->attributes & PW_IA32_ATTR_OPREG) assert((form->opcode & 7) == 0);
        if (form->rm != PW_IA32_SEL_X) assert(form->mod == PW_IA32_MOD_REG);
        /* Long-mode and VEX attributes. */
        const uint32_t a = form->attributes;
        assert(!((a & PW_IA32_ATTR_NO64) && (a & PW_IA32_ATTR_X64)));
        assert(!((a & PW_IA32_ATTR_VEX) && (a & PW_IA32_ATTR_VEXONLY)));
        assert(!((a & PW_IA32_ATTR_L0) && (a & PW_IA32_ATTR_L1)) && !((a & PW_IA32_ATTR_W0) && (a & PW_IA32_ATTR_W1)));
        const uint32_t vex_only_attrs = PW_IA32_ATTR_L0 | PW_IA32_ATTR_L1 | PW_IA32_ATTR_W0 | PW_IA32_ATTR_W1 |
                                        PW_IA32_ATTR_VLH | PW_IA32_ATTR_NOV;
        if (a & vex_only_attrs) assert(a & (PW_IA32_ATTR_VEX | PW_IA32_ATTR_VEXONLY));
        if (a & (PW_IA32_ATTR_VEX | PW_IA32_ATTR_VEXONLY)) {   /* VEX.pp is the prefix; no MMX register */
            assert(form->map != PW_IA32_MAP_PRI && form->prefix != PW_IA32_PFX_ANY && form->prefix != PW_IA32_PFX_NOREP);
            for (unsigned k = 0; k < 3; k++)
                assert(form->operands[k] != PW_IA32_OP_Pq && form->operands[k] != PW_IA32_OP_Qq &&
                       form->operands[k] != PW_IA32_OP_Qd && form->operands[k] != PW_IA32_OP_Nq);
        }
        if (form->size == PW_IA32_SIZE_O64 || form->size == PW_IA32_SIZE_A64) assert(!(a & PW_IA32_ATTR_NO64));
        for (unsigned k = 0; k < 3; k++)   /* By (a VEX.vvvv register) only in VEX-only rows */
            if (form->operands[k] == PW_IA32_OP_By) assert(a & PW_IA32_ATTR_VEXONLY);
        if (a & (PW_IA32_ATTR_MRO | PW_IA32_ATTR_ADDR)) assert(modrm_shape(form));
        const int modrm = strchr("mBZfF3QVvx", shape) != NULL;
        if (form->reg != PW_IA32_SEL_X || form->mod != PW_IA32_MOD_ANY) assert(modrm);
        int ended = 0;
        for (unsigned k = 0; k < 3; k++) {
            const unsigned op = form->operands[k];
            if (op == PW_IA32_OP_NONE) ended = 1;
            else assert(!ended && op < PW_IA32_OPERANDS);
            if (op == PW_IA32_OP_Eb || op == PW_IA32_OP_Ev || op == PW_IA32_OP_Gv || op == PW_IA32_OP_M) assert(modrm);
        }
    }
    assert(pw_ia32_form_at(count) == NULL);
    assert(pw_ia32_shape(PW_IA32_MAPS, 0) == '-' && pw_ia32_shape(0, 256) == '-');
    /* Every slot of the index names rows of that map and opcode. */
    for (unsigned map = 0; map < PW_IA32_MAPS; map++)
        for (unsigned op = 0; op < 256; op++) {
            size_t n = 0;
            const uint16_t *rows = pw_ia32_forms_for(map, op, &n);
            for (size_t k = 0; k < n; k++) {
                const PwIa32Form *form = pw_ia32_form_at(rows[k]);
                const unsigned span = (form->attributes & PW_IA32_ATTR_OPREG) ? 8 : 1;
                assert(form->map == map && op >= form->opcode && op < form->opcode + span);
                if (k) assert(rows[k] > rows[k - 1]);
            }
        }
}

typedef struct Case {
    const char *hex;
    int result;
    unsigned length;
    const char *form;   /* the row name, or NULL */
    const char *text;   /* the printed text with tabs, or NULL */
} Case;

static const Case cases[] = {
    { "90", PW_OK, 1, "nop", "nop" },
    { "f390", PW_OK, 2, "pause", "pause" },
    { "64a118000000", PW_OK, 6, "mov_eAX_Ov", "mov\teax, dword ptr fs:[24]" },
    { "8b448810", PW_OK, 4, "mov_Gv_Ev", "mov\teax, dword ptr [eax + 4*ecx + 16]" },
    { "8b4488f0", PW_OK, 4, "mov_Gv_Ev", "mov\teax, dword ptr [eax + 4*ecx - 16]" },
    { "8b0424", PW_OK, 3, "mov_Gv_Ev", "mov\teax, dword ptr [esp]" },
    { "8b04a0", PW_OK, 3, "mov_Gv_Ev", "mov\teax, dword ptr [eax + 4*eiz]" },
    { "8b042544332211", PW_OK, 7, "mov_Gv_Ev", "mov\teax, dword ptr [1*eiz + 287454020]" },
    { "8b05f0ffffff", PW_OK, 6, "mov_Gv_Ev", "mov\teax, dword ptr [-16]" },
    { "678b4710", PW_OK, 4, "mov_Gv_Ev", "mov\teax, dword ptr [bx + 16]" },
    { "678b061122", PW_OK, 5, "mov_Gv_Ev", "addr16\tmov\teax, dword ptr [8721]" },
    { "6689c8", PW_OK, 3, "mov_Ev_Gv", "mov\tax, cx" },
    { "83c0ff", PW_OK, 3, "add_Ev_Ibs", "add\teax, -1" },
    { "81c0ffffffff", PW_OK, 6, "add_Ev_Iz", "add\teax, 4294967295" },
    { "6605ffff", PW_OK, 4, "add_eAX_Iz", "add\tax, 65535" },
    { "d1e0", PW_OK, 2, "shl_Ev_1", "shl\teax" },
    { "c1e002", PW_OK, 3, "shl_Ev_Ib", "shl\teax, 2" },
    { "f3a5", PW_OK, 2, "movsd", "rep\tmovsd\tdword ptr es:[edi], dword ptr [esi]" },
    { "64a5", PW_OK, 2, "movsd", "movsd\tdword ptr es:[edi], dword ptr fs:[esi]" },
    { "f00fb108", PW_OK, 4, "cmpxchg_Ev_Gv", "lock\tcmpxchg\tdword ptr [eax], ecx" },
    { "f38700", PW_OK, 3, "xchg_Mv_Gv", "xrelease\txchg\tdword ptr [eax], eax" },
    { "87c8", PW_OK, 2, "xchg_Gv_Rv", "xchg\tecx, eax" },
    { "eb05", PW_OK, 2, "jmp_Jb", "jmp\t5" },
    { "e9fbffffff", PW_OK, 5, "jmp_Jz", "jmp\t-5" },
    { "660f840000", PW_OK, 5, "je_Jz", "je\t0" },
    { "9a112233445566", PW_OK, 7, "lcall_Ap", "lcall\t26197, 1144201745" },
    { "c8100001", PW_OK, 4, "enter", "enter\t16, 1" },
    { "c21000", PW_OK, 3, "ret_Iw", "ret\t16" },
    { "ff18", PW_OK, 2, "lcall_Mp", "call\t[eax]" },
    { "8d4004", PW_OK, 3, "lea_Gv_M", "lea\teax, [eax + 4]" },
    { "0fb7c0", PW_OK, 3, "movzx_Gv_Ew", "movzx\teax, ax" },
    { "f30fb8c0", PW_OK, 4, "popcnt_Gv_Ev", "popcnt\teax, eax" },
    { "0f01d0", PW_OK, 3, "xgetbv", "xgetbv" },
    { "e3fe", PW_OK, 2, "jecxz_Jb", "jecxz\t-2" },
    { "67e3fe", PW_OK, 3, "jcxz_Jb", "jcxz\t-2" },
    { "f1", PW_OK, 1, "int1", "int1" },               /* the CPU's INT1; LLVM rejects it */
    { "8c30", PW_ERR_MALFORMED, 2, NULL, NULL },     /* no segment register 6 */
    { "0f04", PW_ERR_MALFORMED, 2, NULL, NULL },     /* undefined in the 0F map */
    { "0f28c1", PW_OK, 3, "movaps_np", "movaps\txmm0, xmm1" },
    { "f30f1000", PW_OK, 4, "movss_pf3", "movss\txmm0, dword ptr [eax]" },
    { "0fc20001", PW_OK, 4, "cmpps_x", "cmpltps\txmm0, xmmword ptr [eax]" },
    { "0fc20008", PW_OK, 4, "cmpps_x", "cmpps\txmm0, xmmword ptr [eax], 8" },
    { "660f381000", PW_OK, 5, "pblendvb_p66", "pblendvb\txmm0, xmmword ptr [eax], xmm0" },
    { "0f60c1", PW_OK, 3, "punpcklbw_np", "punpcklbw\tmm0, mm1" },
    { "dbe8", PW_OK, 2, "fucomi", "fucomi\tst, st(0)" },
    { "dd38", PW_OK, 2, "fnstsw_dd_7", "fnstsw\tword ptr [eax]" },
    { "db28", PW_OK, 2, "fld_db_5", "fld\ttbyte ptr [eax]" },
    { "0f0110", PW_ERR_UNSUPPORTED, 3, NULL, NULL }, /* LGDT: length known, no form */
    { "0f0100", PW_OK, 3, "sgdtd_M", "sgdtd\t[eax]" },
    { "0f010c24", PW_OK, 4, "sidtd_M", "sidtd\t[esp]" },
    { "0f01e0", PW_OK, 3, "smsw_Evw", "smsw\teax" },
    { "0f0000", PW_OK, 3, "sldt_Evw", "sldt\tword ptr [eax]" },
    { "0f00c8", PW_OK, 3, "str_Evw", "str\teax" },
    { "c5f877", PW_ERR_UNSUPPORTED, 3, NULL, NULL }, /* VEX: length only */
    { "c4e27918c1", PW_ERR_UNSUPPORTED, 5, NULL, NULL },
    { "62f17c0828c1", PW_ERR_UNSUPPORTED, 6, NULL, NULL }, /* EVEX */
    { "66c5f877", PW_ERR_MALFORMED, 4, NULL, NULL },   /* 66 before VEX is #UD */
};

static void test_cases(void)
{
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        const Case *c = &cases[i];
        uint8_t bytes[32];
        const size_t n = hex_bytes(c->hex, bytes, sizeof(bytes));
        PwIa32Inst inst;
        const int result = pw_ia32_decode(bytes, n, &inst);
        if (result != c->result || inst.length != c->length) {
            fprintf(stderr, "%s: result %d length %u, want %d %u\n", c->hex, result, inst.length, c->result, c->length);
            assert(0);
        }
        if (c->form) assert(inst.form && !strcmp(inst.form->name, c->form));
        else assert(!inst.form);
        if (c->text) {
            char text[128];
            assert(pw_ia32_print(&inst, text, sizeof(text)) == PW_OK);
            if (strcmp(text, c->text)) {
                fprintf(stderr, "%s: printed \"%s\", want \"%s\"\n", c->hex, text, c->text);
                assert(0);
            }
        }
        /* Every shorter view of a complete legacy instruction is TRUNCATED. */
        if (c->length && result != PW_ERR_MALFORMED)
            for (size_t k = 0; k < c->length; k++) {
                PwIa32Inst part;
                assert(pw_ia32_decode(bytes, k, &part) == PW_ERR_TRUNCATED);
            }
    }
}

static void test_fields(void)
{
    uint8_t bytes[16];
    PwIa32Inst inst;
    size_t n = hex_bytes("26f20f2ac4", bytes, sizeof(bytes));      /* prefixes are recorded */
    pw_ia32_decode(bytes, n, &inst);
    assert(inst.segment == PW_IA32_SEG_ES && inst.rep == 0xf2 && inst.prefixes == 2 && inst.map == PW_IA32_MAP_0F);
    n = hex_bytes("8b4c8d08", bytes, sizeof(bytes));               /* mov ecx, [ebp + 4*ecx + 8] */
    assert(pw_ia32_decode(bytes, n, &inst) == PW_OK);
    assert(inst.base == PW_IA32_EBP && inst.index == PW_IA32_ECX && inst.scale == 4 && inst.disp == 8);
    assert(inst.default_segment == PW_IA32_SEG_SS && inst.disp_size == 1);
    n = hex_bytes("678b02", bytes, sizeof(bytes));                 /* mov eax, [bp + si] */
    assert(pw_ia32_decode(bytes, n, &inst) == PW_OK);
    assert(inst.address_size == 16 && inst.base == PW_IA32_EBP && inst.index == PW_IA32_ESI);
    n = hex_bytes("66b81234", bytes, sizeof(bytes));               /* mov ax, 0x3412 */
    assert(pw_ia32_decode(bytes, n, &inst) == PW_OK && inst.operand_size == 16 && inst.imm == 0x3412);
}

/* Fifteen bytes is the architectural limit; prefixes count toward it. */
static void test_length_limit(void)
{
    uint8_t bytes[20];
    PwIa32Inst inst;
    memset(bytes, 0x66, sizeof(bytes));
    bytes[14] = 0x90;
    assert(pw_ia32_decode(bytes, sizeof(bytes), &inst) == PW_OK && inst.length == 15);
    bytes[14] = 0x66;
    bytes[15] = 0x90;
    assert(pw_ia32_decode(bytes, sizeof(bytes), &inst) == PW_ERR_MALFORMED);
    assert(pw_ia32_decode(NULL, 1, &inst) == PW_ERR_PRECONDITION);
    assert(pw_ia32_decode(bytes, 1, NULL) == PW_ERR_PRECONDITION);
}

/* Random bytes, each run in a buffer of exactly its size, so a read past it is an ASan error. */
static void test_random_bytes(void)
{
    uint64_t state = 0x9e3779b97f4a7c15ull;
    unsigned ok = 0;
    for (unsigned trial = 0; trial < 200000; trial++) {
        state ^= state << 13; state ^= state >> 7; state ^= state << 17;
        const size_t size = (size_t)(state % 18);
        uint8_t *bytes = malloc(size ? size : 1);
        for (size_t i = 0; i < size; i++) {
            state ^= state << 13; state ^= state >> 7; state ^= state << 17;
            /* Bias toward prefixes and escapes, which reach the deeper paths. */
            static const uint8_t interesting[] = { 0x66, 0x67, 0xf2, 0xf3, 0xf0, 0x0f, 0x38, 0x3a, 0x64, 0xc4, 0x62, 0x8f };
            bytes[i] = (state >> 40) % 4 == 0 ? interesting[(state >> 20) % sizeof(interesting)] : (uint8_t)(state >> 24);
        }
        PwIa32Inst inst;
        const int result = pw_ia32_decode(bytes, size, &inst);
        assert(inst.length <= size && inst.length <= PW_IA32_MAX_LENGTH);
        if (result == PW_OK) {
            char text[160], small[8];
            assert(pw_ia32_print(&inst, text, sizeof(text)) == PW_OK);
            assert(strlen(text) < sizeof(text));
            const int r = pw_ia32_print(&inst, small, sizeof(small));
            assert((r == PW_OK || r == PW_ERR_LIMIT) && strlen(small) < sizeof(small));
            ok++;
        }
        free(bytes);
    }
    assert(ok > 10000);
}

/* Long mode: results, lengths, rows and text. VEX forms print nothing (PW_ERR_UNSUPPORTED), so their
 * text is NULL here and pw_ia32_access says what matters (test_access_64). */
static const Case cases_64[] = {
    { "4889c8", PW_OK, 3, "mov_Ev_Gv", "mov\trax, rcx" },
    { "4c89c0", PW_OK, 3, "mov_Ev_Gv", "mov\trax, r8" },                 /* REX.R */
    { "41ffd3", PW_OK, 3, "call_Ev", "call\tr11" },                      /* REX.B, D64 */
    { "4088e0", PW_OK, 3, "mov_Eb_Gb", "mov\tal, spl" },                 /* any REX: SPL, not AH */
    { "88e0", PW_OK, 2, "mov_Eb_Gb", "mov\tal, ah" },
    { "8b0501000000", PW_OK, 6, "mov_Gv_Ev", "mov\teax, dword ptr [rip + 1]" },
    { "678b0501000000", PW_OK, 7, "mov_Gv_Ev", "mov\teax, dword ptr [eip + 1]" },
    { "8b042501000000", PW_OK, 7, "mov_Gv_Ev", "mov\teax, dword ptr [1]" },   /* SIB without base: not RIP */
    { "42 8b 04 20", PW_OK, 4, "mov_Gv_Ev", "mov\teax, dword ptr [rax + r12]" },   /* REX.X: index 4 is r12 */
    { "41 8b 45 00", PW_OK, 4, "mov_Gv_Ev", "mov\teax, dword ptr [r13]" },
    { "48b80102030405060708", PW_OK, 10, "movabs_Zv_Iv", "movabs\trax, 578437695752307201" },
    { "b801020304", PW_OK, 5, "mov_Zv_Iz", "mov\teax, 67305985" },
    { "48a10102030405060708", PW_OK, 10, "movabs_eAX_Ov", "movabs\trax, qword ptr [578437695752307201]" },
    { "4805ffffffff", PW_OK, 6, "add_eAX_Iz", "add\trax, -1" },          /* imm32, sign-extended */
    { "44f60011", PW_OK, 4, "test_Eb_Ib", "test\tbyte ptr [rax], 17" },   /* REX.R leaves TEST's /0 and its imm8 */
    { "6a01", PW_OK, 2, "push_Ibs", "push\t1" },
    { "50", PW_OK, 1, "push_Zv", "push\trax" },
    { "4150", PW_OK, 2, "push_Zv", "push\tr8" },
    { "6650", PW_OK, 2, "push_Zv", "push\tax" },
    { "4863c1", PW_OK, 3, "movsxd_Gv_Ed", "movsxd\trax, ecx" },
    { "4898", PW_OK, 2, "cdqe", "cdqe" },
    { "4899", PW_OK, 2, "cqo", "cqo" },
    { "48a5", PW_OK, 2, "movsq", "movsq\tqword ptr es:[rdi], qword ptr [rsi]" },
    { "48cf", PW_OK, 2, "iretq", "iretq" },
    { "9c", PW_OK, 1, "pushfq", "pushfq" },
    { "e301", PW_OK, 2, "jrcxz_Jb", "jrcxz\t1" },
    { "4090", PW_OK, 2, "nop", "nop" },
    { "4190", PW_OK, 2, "xchg_eAX_Zv", "xchg\teax, r8d" },                /* 90 with REX.B is not NOP */
    { "f30f1efa", PW_OK, 4, "endbr64", "endbr64" },
    { "480fc708", PW_OK, 4, "cmpxchg16b_Mx", "cmpxchg16b\txmmword ptr [rax]" },
    { "ff2500000000", PW_OK, 6, "jmp_Ev", "jmp\tqword ptr [rip]" },
    { "06", PW_ERR_MALFORMED, 1, NULL, NULL },                           /* NO64: PUSH ES */
    { "d40a", PW_ERR_MALFORMED, 2, NULL, NULL },                         /* NO64: AAM */
    { "66e81122", PW_ERR_UNSUPPORTED, 4, NULL, NULL },                   /* 66 near branch: AMD's rel16 */
    { "66ffd0", PW_ERR_UNSUPPORTED, 3, NULL, NULL },
    { "0f05", PW_ERR_UNSUPPORTED, 2, NULL, NULL },                       /* SYSCALL: no form, nothing runs it */
    { "c4e278f2c1", PW_OK, 5, "andn", NULL },
    { "c5fc1000", PW_OK, 4, "movups_np", NULL },
    { "c4e37d190001", PW_OK, 6, "vextractf128", NULL },
    { "c4e27d2100", PW_OK, 5, "pmovsxbd_p66", NULL },
    { "c5f877", PW_OK, 3, "vzeroupper", NULL },
    { "c5fc77", PW_OK, 3, "vzeroall", NULL },
    { "c4e17010c1", PW_ERR_UNSUPPORTED, 5, NULL, NULL },                 /* VMOVUPS with a vvvv: #UD (NOV) */
    { "c5fd6e00", PW_ERR_UNSUPPORTED, 4, NULL, NULL },                   /* VMOVD with L = 1: #UD (L0) */
    { "62f17c481000", PW_ERR_UNSUPPORTED, 6, NULL, NULL },               /* EVEX: no form */
    { "66c5f877", PW_ERR_MALFORMED, 4, NULL, NULL },                     /* 66 before VEX */
    { "48c5f877", PW_ERR_MALFORMED, 4, NULL, NULL },                     /* REX before VEX */
};

static void test_cases_64(void)
{
    for (size_t i = 0; i < sizeof(cases_64) / sizeof(cases_64[0]); i++) {
        const Case *c = &cases_64[i];
        uint8_t bytes[32];
        const size_t n = hex_bytes(c->hex, bytes, sizeof(bytes));
        PwIa32Inst inst;
        const int result = pw_ia32_decode_mode(bytes, n, PW_IA32_MODE_64, &inst);
        if (result != c->result || inst.length != c->length) {
            fprintf(stderr, "64 %s: result %d length %u, want %d %u\n", c->hex, result, inst.length, c->result, c->length);
            assert(0);
        }
        if (c->form) assert(inst.form && !strcmp(inst.form->name, c->form));
        else assert(!inst.form);
        char text[128];
        if (c->text) {
            assert(pw_ia32_print(&inst, text, sizeof(text)) == PW_OK);
            if (strcmp(text, c->text)) {
                fprintf(stderr, "64 %s: printed \"%s\", want \"%s\"\n", c->hex, text, c->text);
                assert(0);
            }
        } else if (c->form) {
            assert(pw_ia32_print(&inst, text, sizeof(text)) == PW_ERR_UNSUPPORTED && !text[0]);
        }
        if (c->length && result != PW_ERR_MALFORMED)
            for (size_t k = 0; k < c->length; k++) {
                PwIa32Inst part;
                assert(pw_ia32_decode_mode(bytes, k, PW_IA32_MODE_64, &part) == PW_ERR_TRUNCATED);
            }
    }
}

/* The fields the engine rewrites from: prefixes, REX, the opcode and ModRM offsets, the extended
 * registers, RIP-relative, and VEX's L, W, vvvv and pp. */
static void test_fields_64(void)
{
    uint8_t bytes[16];
    PwIa32Inst inst;
    size_t n = hex_bytes("644a8b84a511223344", bytes, sizeof(bytes));   /* mov rax, fs:[rbp + 4*r12 + disp32] */
    assert(pw_ia32_decode_mode(bytes, n, PW_IA32_MODE_64, &inst) == PW_OK);
    assert(inst.mode == PW_IA32_MODE_64 && inst.prefixes == 2 && inst.rex == 0x4a && inst.opcode_at == 2);
    assert(inst.modrm_at == 3 && inst.has_sib && inst.base == PW_IA32_EBP && inst.index == 12 && inst.scale == 4);
    assert(inst.disp == 0x44332211 && inst.disp_size == 4 && inst.operand_size == 64 && inst.address_size == 64);
    assert(inst.segment == PW_IA32_SEG_FS && !inst.rip_relative);
    /* ES/CS/SS/DS are null prefixes in long mode: an FS or GS before them stays; in 32-bit mode the last one counts. */
    n = hex_bytes("643e488b042500000000", bytes, sizeof(bytes));        /* mov rax, fs:[0] with a DS after FS */
    assert(pw_ia32_decode_mode(bytes, n, PW_IA32_MODE_64, &inst) == PW_OK && inst.segment == PW_IA32_SEG_FS);
    n = hex_bytes("6526488b042500000000", bytes, sizeof(bytes));        /* mov rax, gs:[0] with an ES after GS */
    assert(pw_ia32_decode_mode(bytes, n, PW_IA32_MODE_64, &inst) == PW_OK && inst.segment == PW_IA32_SEG_GS);
    n = hex_bytes("3e64488b042500000000", bytes, sizeof(bytes));        /* DS then FS: FS */
    assert(pw_ia32_decode_mode(bytes, n, PW_IA32_MODE_64, &inst) == PW_OK && inst.segment == PW_IA32_SEG_FS);
    n = hex_bytes("643e8b042500000000", bytes, sizeof(bytes));          /* 32-bit: mov eax, ds:[0] */
    assert(pw_ia32_decode_mode(bytes, n, PW_IA32_MODE_32, &inst) == PW_OK && inst.segment == PW_IA32_SEG_DS);
    n = hex_bytes("486689c0", bytes, sizeof(bytes));                    /* REX then 66: the REX is ignored */
    assert(pw_ia32_decode_mode(bytes, n, PW_IA32_MODE_64, &inst) == PW_OK);
    assert(inst.rex == 0 && inst.operand_size == 16 && inst.prefixes == 2 && inst.opcode_at == 2);
    n = hex_bytes("66480f1f440000", bytes, sizeof(bytes));              /* 66 then REX.W: 64 bits */
    assert(pw_ia32_decode_mode(bytes, n, PW_IA32_MODE_64, &inst) == PW_OK);
    assert(inst.rex == 0x48 && inst.operand_size == 64 && inst.opcode_at == 3 && inst.modrm_at == 4);
    n = hex_bytes("f2410f5905f0ffffff", bytes, sizeof(bytes));          /* mulsd xmm0, [rip - 16]: REX.B unused */
    assert(pw_ia32_decode_mode(bytes, n, PW_IA32_MODE_64, &inst) == PW_OK);
    assert(inst.rip_relative && inst.base == PW_IA32_NO_REG && inst.index == PW_IA32_NO_REG && inst.disp == -16);
    assert(inst.opcode_at == 3 && inst.modrm_at == 4 && inst.rm == 13);
    n = hex_bytes("c4c17c100424", bytes, sizeof(bytes));                /* vmovups ymm0, [r12] */
    assert(pw_ia32_decode_mode(bytes, n, PW_IA32_MODE_64, &inst) == PW_OK);
    assert(inst.encoding == PW_IA32_ENC_VEX && inst.prefixes == 0 && inst.opcode_at == 3 && inst.modrm_at == 4);
    assert(inst.vex_l == 1 && inst.vex_w == 0 && inst.vex_vvvv == 0 && inst.vex_pp == PW_IA32_PP_NONE);
    assert(inst.base == 12 && inst.index == PW_IA32_NO_REG && inst.rex == 0);
    n = hex_bytes("c4e2b0f2c1", bytes, sizeof(bytes));                  /* andn rax, r9, rcx: W, vvvv 9 */
    assert(pw_ia32_decode_mode(bytes, n, PW_IA32_MODE_64, &inst) == PW_OK);
    assert(inst.vex_w == 1 && inst.vex_vvvv == 9 && inst.operand_size == 64 && inst.reg == 0 && inst.rm == 1);
    n = hex_bytes("c57a1000", bytes, sizeof(bytes));                    /* vmovss xmm8, [rax]: VEX2 R, pp F3 */
    assert(pw_ia32_decode_mode(bytes, n, PW_IA32_MODE_64, &inst) == PW_OK && !strcmp(inst.form->name, "vmovss_load"));
    assert(inst.reg == 8 && inst.vex_pp == PW_IA32_PP_F3 && inst.vex_l == 0 && inst.opcode_at == 2);
    n = hex_bytes("c5ba10c1", bytes, sizeof(bytes));                    /* vmovss xmm0, xmm8, xmm1: vvvv used */
    assert(pw_ia32_decode_mode(bytes, n, PW_IA32_MODE_64, &inst) == PW_OK && !strcmp(inst.form->name, "vmovss_rr"));
    assert(inst.vex_vvvv == 8);
    /* 32-bit mode does not see REX: 48 is DEC EAX. */
    n = hex_bytes("4889c8", bytes, sizeof(bytes));
    assert(pw_ia32_decode(bytes, n, &inst) == PW_OK && inst.length == 1 && !strcmp(inst.form->name, "dec_Zv"));
    assert(inst.mode == PW_IA32_MODE_32 && inst.rex == 0);
    assert(pw_ia32_decode_mode(bytes, n, 16, &inst) == PW_ERR_PRECONDITION);
}

typedef struct AccessCase {
    const char *hex;
    unsigned mode;
    int result;
    uint8_t kind, exact;
    uint16_t bytes;
} AccessCase;

/* pw_ia32_access: what the engine trusts for a store's extent (docs/MMU4K.md, section 5). */
static const AccessCase access_cases[] = {
    { "8900", 64, PW_OK, PW_IA32_ACCESS_WRITE, 1, 4 },
    { "488900", 64, PW_OK, PW_IA32_ACCESS_WRITE, 1, 8 },
    { "66c70011 22", 64, PW_OK, PW_IA32_ACCESS_WRITE, 1, 2 },
    { "0100", 64, PW_OK, PW_IA32_ACCESS_RW, 1, 4 },            /* LOCK-able: read-modify-write */
    { "f0480100", 64, PW_OK, PW_IA32_ACCESS_RW, 1, 8 },
    { "d100", 64, PW_OK, PW_IA32_ACCESS_RW, 1, 4 },            /* O1: a shift */
    { "3900", 64, PW_OK, PW_IA32_ACCESS_READ, 1, 4 },          /* MRO */
    { "8b00", 64, PW_OK, PW_IA32_ACCESS_READ, 1, 4 },
    { "f600ff", 64, PW_OK, PW_IA32_ACCESS_READ, 1, 1 },        /* TEST */
    { "ff20", 64, PW_OK, PW_IA32_ACCESS_READ, 1, 8 },          /* JMP [rax] */
    { "8d00", 64, PW_OK, PW_IA32_ACCESS_ADDRESS, 0, 0 },
    { "0f1800", 64, PW_OK, PW_IA32_ACCESS_ADDRESS, 0, 0 },
    { "0f1f00", 64, PW_OK, PW_IA32_ACCESS_ADDRESS, 0, 0 },
    { "89c0", 64, PW_OK, PW_IA32_ACCESS_NONE, 0, 0 },
    { "ff30", 64, PW_ERR_UNSUPPORTED, 0, 0, 0 },               /* PUSH [rax]: STACK */
    { "ff10", 64, PW_ERR_UNSUPPORTED, 0, 0, 0 },               /* CALL [rax]: STACK */
    { "a4", 64, PW_ERR_UNSUPPORTED, 0, 0, 0 },                 /* STR */
    { "a30102030405060708", 64, PW_ERR_UNSUPPORTED, 0, 0, 0 }, /* moffs */
    { "0fae00", 64, PW_ERR_UNSUPPORTED, 0, 0, 0 },             /* FXSAVE: IMPL */
    { "dd30", 64, PW_ERR_UNSUPPORTED, 0, 0, 0 },               /* FNSAVE: IMPL */
    { "0ff7c1", 64, PW_ERR_UNSUPPORTED, 0, 0, 0 },             /* MASKMOVQ: IMPL (implicit [rdi]) */
    { "d900", 64, PW_OK, PW_IA32_ACCESS_READ, 1, 4 },          /* FLD m32 */
    { "d918", 64, PW_OK, PW_IA32_ACCESS_WRITE, 1, 4 },         /* FSTP m32 */
    { "db38", 64, PW_OK, PW_IA32_ACCESS_WRITE, 1, 10 },        /* FSTP m80 */
    { "0f9000", 64, PW_OK, PW_IA32_ACCESS_WRITE, 1, 1 },       /* SETO */
    { "0fc708", 64, PW_OK, PW_IA32_ACCESS_RW, 1, 8 },          /* CMPXCHG8B */
    { "480fc708", 64, PW_OK, PW_IA32_ACCESS_RW, 1, 16 },       /* CMPXCHG16B */
    { "660f3a160001", 64, PW_OK, PW_IA32_ACCESS_WRITE, 1, 4 }, /* PEXTRD */
    { "66480f3a160001", 64, PW_OK, PW_IA32_ACCESS_WRITE, 1, 8 }, /* PEXTRQ */
    { "f20f1100", 64, PW_OK, PW_IA32_ACCESS_WRITE, 1, 8 },     /* MOVSD m64, xmm */
    { "0f2900", 64, PW_OK, PW_IA32_ACCESS_WRITE, 1, 16 },      /* MOVAPS m128, xmm */
    { "c5fc1100", 64, PW_OK, PW_IA32_ACCESS_WRITE, 1, 32 },    /* VMOVUPS m256, ymm */
    { "c5f81100", 64, PW_OK, PW_IA32_ACCESS_WRITE, 1, 16 },
    { "c5fde700", 64, PW_OK, PW_IA32_ACCESS_WRITE, 1, 32 },    /* VMOVNTDQ m256 */
    { "c5fa1100", 64, PW_OK, PW_IA32_ACCESS_WRITE, 1, 4 },     /* VMOVSS m32 */
    { "c4e37d190001", 64, PW_OK, PW_IA32_ACCESS_WRITE, 1, 16 }, /* VEXTRACTF128: 16 bytes whatever L */
    { "c4e37d1d0001", 64, PW_OK, PW_IA32_ACCESS_WRITE, 1, 16 }, /* VCVTPS2PH ymm: VLH */
    { "c4e3791d0001", 64, PW_OK, PW_IA32_ACCESS_WRITE, 1, 8 },
    { "c4e27d2100", 64, PW_OK, PW_IA32_ACCESS_READ, 1, 8 },    /* VPMOVSXBD ymm, m64: VLH */
    { "c4e2792100", 64, PW_OK, PW_IA32_ACCESS_READ, 1, 4 },
    { "c4e27d1800", 64, PW_OK, PW_IA32_ACCESS_READ, 1, 4 },    /* VBROADCASTSS */
    { "c4e27d5a00", 64, PW_OK, PW_IA32_ACCESS_READ, 1, 16 },   /* VBROADCASTI128 */
    { "c4e17dd100", 64, PW_OK, PW_IA32_ACCESS_READ, 1, 16 },   /* VPSRLW ymm, ymm, m128: the count is 128 bits */
    { "c4e278f200", 64, PW_OK, PW_IA32_ACCESS_READ, 1, 4 },    /* ANDN r32, r32, m32 */
    { "c4e2f8f200", 64, PW_OK, PW_IA32_ACCESS_READ, 1, 8 },
    { "c5f9f7c1", 64, PW_ERR_UNSUPPORTED, 0, 0, 0 },           /* VMASKMOVDQU: IMPL */
    { "8900", 32, PW_OK, PW_IA32_ACCESS_WRITE, 1, 4 },
    { "668900", 32, PW_OK, PW_IA32_ACCESS_WRITE, 1, 2 },
    { "62 00", 32, PW_OK, PW_IA32_ACCESS_READ, 1, 8 },          /* BOUND: two bounds */
    { "ff18", 32, PW_ERR_UNSUPPORTED, 0, 0, 0 },               /* CALL far [eax]: STACK */
};

static void test_access_64(void)
{
    for (size_t i = 0; i < sizeof(access_cases) / sizeof(access_cases[0]); i++) {
        const AccessCase *c = &access_cases[i];
        uint8_t bytes[32];
        const size_t n = hex_bytes(c->hex, bytes, sizeof(bytes));
        PwIa32Inst inst;
        assert(pw_ia32_decode_mode(bytes, n, c->mode, &inst) == PW_OK);
        PwIa32Access access;
        const int result = pw_ia32_access(&inst, &access);
        if (result != c->result || (result == PW_OK && (access.kind != c->kind || access.exact != c->exact ||
                                                        access.bytes != c->bytes))) {
            fprintf(stderr, "access %s (%u, %s): %d kind %u exact %u bytes %u, want %d %u %u %u\n", c->hex, c->mode,
                    inst.form->name, result, access.kind, access.exact, access.bytes, c->result, c->kind, c->exact,
                    c->bytes);
            assert(0);
        }
    }
    PwIa32Inst inst;
    PwIa32Access access;
    const uint8_t undefined[] = { 0x0f, 0x04 };
    pw_ia32_decode_mode(undefined, sizeof(undefined), PW_IA32_MODE_64, &inst);
    assert(pw_ia32_access(&inst, &access) == PW_ERR_PRECONDITION);
    assert(pw_ia32_access(NULL, &access) == PW_ERR_PRECONDITION && pw_ia32_access(&inst, NULL) == PW_ERR_PRECONDITION);
}

/* Long mode: fifteen bytes still, REX bytes counted; a REX alone is truncated. */
static void test_length_limit_64(void)
{
    uint8_t bytes[20];
    PwIa32Inst inst;
    memset(bytes, 0x48, sizeof(bytes));
    bytes[14] = 0x90;
    assert(pw_ia32_decode_mode(bytes, sizeof(bytes), PW_IA32_MODE_64, &inst) == PW_OK && inst.length == 15);
    assert(inst.prefixes == 14 && inst.rex == 0x48);
    bytes[14] = 0x48;
    bytes[15] = 0x90;
    assert(pw_ia32_decode_mode(bytes, sizeof(bytes), PW_IA32_MODE_64, &inst) == PW_ERR_MALFORMED);
    assert(pw_ia32_decode_mode(bytes, 1, PW_IA32_MODE_64, &inst) == PW_ERR_TRUNCATED);
}

/* Random bytes in long mode, under AddressSanitizer as in 32-bit mode; every form's access is
 * answered, and a sized access names at least one byte. */
static void test_random_bytes_64(void)
{
    uint64_t state = 0x2545f4914f6cdd1dull;
    unsigned ok = 0, vex = 0;
    for (unsigned trial = 0; trial < 200000; trial++) {
        state ^= state << 13; state ^= state >> 7; state ^= state << 17;
        const size_t size = (size_t)(state % 18);
        uint8_t *bytes = malloc(size ? size : 1);
        for (size_t i = 0; i < size; i++) {
            state ^= state << 13; state ^= state >> 7; state ^= state << 17;
            static const uint8_t interesting[] = { 0x66, 0x67, 0xf2, 0xf3, 0xf0, 0x0f, 0x38, 0x3a, 0x64, 0xc4, 0xc5,
                                                   0x62, 0x8f, 0x48, 0x41, 0x4c };
            bytes[i] = (state >> 40) % 4 == 0 ? interesting[(state >> 20) % sizeof(interesting)] : (uint8_t)(state >> 24);
        }
        PwIa32Inst inst;
        const int result = pw_ia32_decode_mode(bytes, size, PW_IA32_MODE_64, &inst);
        assert(inst.length <= size && inst.length <= PW_IA32_MAX_LENGTH);
        if (result == PW_OK) {
            char text[160], small[8];
            const int printed = pw_ia32_print(&inst, text, sizeof(text));
            assert(printed == PW_OK || (printed == PW_ERR_UNSUPPORTED && inst.encoding == PW_IA32_ENC_VEX));
            assert(strlen(text) < sizeof(text));
            const int r = pw_ia32_print(&inst, small, sizeof(small));
            assert((r == PW_OK || r == PW_ERR_LIMIT || r == PW_ERR_UNSUPPORTED) && strlen(small) < sizeof(small));
            if (inst.has_modrm && inst.mod != 3) assert(pw_ia32_print_memory(&inst, 256, small, sizeof(small)) != PW_ERR_PRECONDITION);
            PwIa32Access access;
            const int a = pw_ia32_access(&inst, &access);
            assert(a == PW_OK || a == PW_ERR_UNSUPPORTED);
            if (a == PW_OK && access.kind >= PW_IA32_ACCESS_READ) assert(access.bytes >= 1 && access.bytes <= 32);
            assert(inst.opcode_at < inst.length && (inst.modrm_at == PW_IA32_NO_OFFSET || inst.modrm_at < inst.length));
            ok++;
            vex += inst.encoding == PW_IA32_ENC_VEX;
        }
        free(bytes);
    }
    assert(ok > 10000 && vex > 100);
}

int main(void)
{
    test_table_rules();
    test_cases();
    test_fields();
    test_length_limit();
    test_random_bytes();
    test_cases_64();
    test_fields_64();
    test_access_64();
    test_length_limit_64();
    test_random_bytes_64();
    printf("pw_ia32_decode: %zu forms, all checks passed\n", pw_ia32_form_count());
    return 0;
}
