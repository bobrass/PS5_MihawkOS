/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PW_UMIP_H
#define PW_UMIP_H
#include <stddef.h>
#include <stdint.h>

/* The five descriptor-table instructions a CPU with CR4.UMIP refuses in user mode: SGDT, SIDT, SLDT, SMSW and
 * STR raise #GP(0). The console's kernel sets UMIP and hands the fault to the program, which Wine then reports
 * as an access violation reading 0xFFFFFFFFFFFFFFFF; on Windows the five run. A store game died that way about
 * 40 s in (2026-10-27, the base PS5, Alpha 4): its anti-tamper code runs `sgdt [rsp]`. Linux spoofs the
 * results instead (arch/x86/kernel/umip.c), which is why Proton runs the same game on a PC; ntdll's fault
 * handler does the same here (wine/patches/2102), with this function deciding what the instruction would have
 * done.
 *
 * pw_umip_emulate decodes the instruction at `code` (`available` bytes, at most 15 read) in cpu->mode
 * (PW_IA32_MODE_32 for WoW64 code, PW_IA32_MODE_64) with the one x86 decoder (src/pw_ia32_decode.h) and
 * fills *effect without touching memory or registers itself:
 *   SGDT / SIDT  memory only: a 16-bit limit 0, then a base of 8 bytes in long mode, 4 otherwise (also with a
 *                16-bit operand size, as Linux); bases Linux's: GDT 0xFFFFFFFFFFFE0000, IDT 0xFFFFFFFFFFFF0000.
 *   SMSW         CR0 0x80050033 (Linux's CR0_STATE: PE MP ET NE WP AM PG): a word in memory, or the operand
 *                size in a register.
 *   SLDT / STR   Linux does not emulate them; Windows' own values: no LDT (0), and the 64-bit TSS selector
 *                0x40 (KGDT64_SYS_TSS), which WoW64 code sees too. A word in memory, or the operand size in a
 *                register.
 * A register destination is merged as the CPU writes it: a 16-bit write keeps the upper bits, a 32-bit write
 * zero-extends, a 64-bit one replaces. The next instruction is rip + length, wrapped to 32 bits in 32-bit mode.
 *
 * Returns PW_OK (0; include/prospero_win.h, which this header leaves out, as ntdll includes it alone) for one of
 * the five; PW_ERR_UNSUPPORTED for anything else, so the fault goes on as before: any other instruction, a
 * truncated or malformed one, a LOCK prefix (#UD on the CPU), or an FS/GS override.
 * shortcut: FS/GS-relative destinations are not emulated (their bases are not in the context passed here);
 * add the bases to PwUmipCpu if a program ever stores the tables through FS or GS.
 * PW_ERR_PRECONDITION: a null argument or another mode. Host tests: tests/test_pw_umip.c. */

typedef struct PwUmipCpu {
    uint64_t gpr[16];     /* in encoding order (rax rcx rdx rbx rsp rbp rsi rdi r8-r15) */
    uint64_t rip;         /* the faulting instruction */
    unsigned mode;        /* PW_IA32_MODE_32 or PW_IA32_MODE_64 */
} PwUmipCpu;

typedef struct PwUmipEffect {
    const char *mnemonic; /* "sgdt", "sidt", "sldt", "smsw" or "str" */
    uint8_t length;       /* bytes of the instruction */
    uint64_t rip;         /* the next instruction */
    uint8_t to_register;  /* 1: gpr[reg] becomes value; 0: data[0..size) is stored at address */
    uint8_t reg;
    uint64_t value;
    uint64_t address;
    uint8_t size;         /* 2, 6 or 10 */
    uint8_t data[10];
} PwUmipEffect;

int pw_umip_emulate(const uint8_t *code, size_t available, const PwUmipCpu *cpu, PwUmipEffect *effect);
#endif
