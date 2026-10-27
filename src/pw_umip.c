/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pw_umip.h"

#include <string.h>

#include "pw_ia32_decode.h"

enum {
    UMIP_GDT_BASE_HIGH = 0xfffe0000u,   /* Linux's UMIP_DUMMY_GDT_BASE 0xfffffffffffe0000, low 32 bits */
    UMIP_IDT_BASE_HIGH = 0xffff0000u,   /* UMIP_DUMMY_IDT_BASE 0xffffffffffff0000 */
    UMIP_CR0 = 0x80050033u,             /* CR0_STATE */
    UMIP_LDT = 0,
    UMIP_TR = 0x40,                     /* KGDT64_SYS_TSS */
};

static uint64_t effective_address(const PwIa32Inst *inst, const PwUmipCpu *cpu)
{
    uint64_t a = (uint64_t)(int64_t)inst->disp;
    if (inst->rip_relative) a += cpu->rip + inst->length;
    if (inst->base != PW_IA32_NO_REG) a += cpu->gpr[inst->base];
    if (inst->index != PW_IA32_NO_REG) a += cpu->gpr[inst->index] * inst->scale;
    return inst->address_size == 16 ? (uint16_t)a : inst->address_size == 32 ? (uint32_t)a : a;
}

/* The register as the CPU leaves it after writing `value` at the operand size. */
static uint64_t merged(uint64_t old, uint64_t value, unsigned operand_size)
{
    if (operand_size == 16) return (old & ~0xffffull) | (value & 0xffff);
    if (operand_size == 32) return (uint32_t)value;
    return value;
}

int pw_umip_emulate(const uint8_t *code, size_t available, const PwUmipCpu *cpu, PwUmipEffect *effect)
{
    if (!code || !cpu || !effect || (cpu->mode != PW_IA32_MODE_32 && cpu->mode != PW_IA32_MODE_64))
        return PW_ERR_PRECONDITION;
    memset(effect, 0, sizeof(*effect));
    PwIa32Inst inst;
    if (pw_ia32_decode_mode(code, available, cpu->mode, &inst) != PW_OK || !inst.form) return PW_ERR_UNSUPPORTED;
    const char *name = inst.form->name;
    const int gdt = !strncmp(name, "sgdt", 4), table = gdt || !strncmp(name, "sidt", 4);   /* by size: sgdt, sgdtw, sgdtd */
    uint64_t value;
    if (table) value = 0;
    else if (!strcmp(name, "smsw_Evw")) value = UMIP_CR0;
    else if (!strcmp(name, "sldt_Evw")) value = UMIP_LDT;
    else if (!strcmp(name, "str_Evw")) value = UMIP_TR;
    else return PW_ERR_UNSUPPORTED;
    if (inst.lock || inst.segment == PW_IA32_SEG_FS || inst.segment == PW_IA32_SEG_GS) return PW_ERR_UNSUPPORTED;

    effect->mnemonic = inst.form->mnemonic;
    effect->length = inst.length;
    effect->rip = cpu->rip + inst.length;
    if (cpu->mode == PW_IA32_MODE_32) effect->rip = (uint32_t)effect->rip;
    if (inst.mod == 3) {   /* SMSW, SLDT, STR to a register; SGDT/SIDT have no register form in the table */
        effect->to_register = 1;
        effect->reg = inst.rm;
        effect->value = merged(cpu->gpr[inst.rm], value, inst.operand_size);
        return PW_OK;
    }
    effect->address = effective_address(&inst, cpu);
    if (table) {
        const uint64_t base = 0xffffffff00000000ull |
                              (gdt ? UMIP_GDT_BASE_HIGH : UMIP_IDT_BASE_HIGH);
        effect->size = cpu->mode == PW_IA32_MODE_64 ? 10 : 6;
        memcpy(effect->data + 2, &base, effect->size - 2u);   /* limit 0 in data[0..1]; little endian */
    } else {
        effect->size = 2;
        memcpy(effect->data, &value, 2);
    }
    return PW_OK;
}
