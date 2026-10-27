/* SPDX-License-Identifier: GPL-3.0-or-later
 * The descriptor-table instructions a CPU with CR4.UMIP refuses in user mode, run by a 64-bit and a 32-bit (WoW64)
 * build of the same file. The console's kernel sets UMIP and leaves the #GP(0) to the program; a store game's
 * anti-tamper code (`sgdt [rsp]`) then died on an access violation reading 0xFFFFFFFFFFFFFFFF (2026-10-27). Patch
 * 2102 emulates them as src/pw_umip.h says; a PC without UMIP runs them for real and prints its kernel's tables, so
 * the console is the only place this verdict means anything.
 *
 *   sgdt / sidt         into memory: limit 0, base 0xFFFFFFFFFFFE0000 / 0xFFFFFFFFFFFF0000 (low 4 bytes in 32-bit code)
 *   smsw-reg, smsw-mem  CR0 0x80050033 in a register of the native size, 0x0033 in a word
 *   sldt-*, str-*       0 and 0x40, in a register and in a word
 *   read-only           SGDT into a read-only page raises a write access violation at that page, and the program
 *                       goes on (a vectored handler steps over the 3-byte `sgdt (%ebx)`/`(%rbx)`)
 * Lines: PS5_PROTON_UMIP <step> PASS|FAIL: <detail>; then PS5_PROTON_UMIP PASS|FAIL. Ends by itself. */
#include <windows.h>

int __cdecl sprintf(char *out, const char *format, ...);   /* msvcrt's */

#ifdef _WIN64
static const ULONG_PTR GDT_BASE = 0xfffffffffffe0000ull, IDT_BASE = 0xffffffffffff0000ull;
#define BASE_BYTES 8
#else
static const ULONG_PTR GDT_BASE = 0xfffe0000u, IDT_BASE = 0xffff0000u;
#define BASE_BYTES 4
#endif

static int failures;
static volatile LONG faults;
static volatile ULONG_PTR fault_kind, fault_address;

static void line(const char *step, int ok, const char *detail)
{
    char text[300];
    DWORD written;
    if (!ok) failures++;
    sprintf(text, "PS5_PROTON_UMIP %s %s: %s\n", step, ok ? "PASS" : "FAIL", detail);
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), text, lstrlenA(text), &written, NULL);
    OutputDebugStringA(text);
}

static void table(const char *step, const BYTE *stored, ULONG_PTR want_base)
{
    char detail[200];
    WORD limit;
    ULONG_PTR base = 0;
    memcpy(&limit, stored, 2);
    memcpy(&base, stored + 2, BASE_BYTES);
    sprintf(detail, "limit %#x base %#llx (want 0 and %#llx)", limit, (unsigned long long)base, (unsigned long long)want_base);
    line(step, limit == 0 && base == want_base, detail);
}

static void value(const char *step, ULONG_PTR got, ULONG_PTR want)
{
    char detail[120];
    sprintf(detail, "%#llx (want %#llx)", (unsigned long long)got, (unsigned long long)want);
    line(step, got == want, detail);
}

static LONG CALLBACK on_fault(EXCEPTION_POINTERS *e)
{
    if (e->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
    faults++;
    fault_kind = e->ExceptionRecord->ExceptionInformation[0];
    fault_address = e->ExceptionRecord->ExceptionInformation[1];
#ifdef _WIN64
    e->ContextRecord->Rip += 3;   /* sgdt (%rbx) */
#else
    e->ContextRecord->Eip += 3;   /* sgdt (%ebx) */
#endif
    return EXCEPTION_CONTINUE_EXECUTION;
}

void __cdecl mainCRTStartup(void)
{
    BYTE gdt[10] = {0}, idt[10] = {0};
    ULONG_PTR msw = ~(ULONG_PTR)0, ldt = ~(ULONG_PTR)0, tr = ~(ULONG_PTR)0;
    WORD msw16 = 0xaaaa, ldt16 = 0xaaaa, tr16 = 0xaaaa;
    char detail[160];

    line("start", 1, "each step prints after its instruction ran; a missing step is the one that ended the program");
    __asm__ volatile("sgdt %0" : "=m"(gdt));
    table("sgdt", gdt, GDT_BASE);
    __asm__ volatile("sidt %0" : "=m"(idt));
    table("sidt", idt, IDT_BASE);
    __asm__ volatile("smsw %0" : "=r"(msw));
    value("smsw-reg", msw, 0x80050033);
    __asm__ volatile("smsw %0" : "=m"(msw16));
    value("smsw-mem", msw16, 0x0033);
    __asm__ volatile("sldt %0" : "=r"(ldt));
    value("sldt-reg", ldt, 0);
    __asm__ volatile("sldt %0" : "=m"(ldt16));
    value("sldt-mem", ldt16, 0);
    __asm__ volatile("str %0" : "=r"(tr));
    value("str-reg", tr, 0x40);
    __asm__ volatile("str %0" : "=m"(tr16));
    value("str-mem", tr16, 0x40);

    {
        BYTE *page = VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READONLY);
        PVOID handler = AddVectoredExceptionHandler(1, on_fault);
        __asm__ volatile("sgdt (%0)" : : "b"(page) : "memory");
        RemoveVectoredExceptionHandler(handler);
        sprintf(detail, "%ld fault(s), kind %llu at %p (want 1, write 1 at %p)", faults, (unsigned long long)fault_kind,
                (void *)fault_address, page);
        line("read-only", page && faults == 1 && fault_kind == 1 && fault_address == (ULONG_PTR)page, detail);
        VirtualFree(page, 0, MEM_RELEASE);
    }

    {
        DWORD written;
        const char *verdict = failures ? "PS5_PROTON_UMIP FAIL: a check failed\n" : "PS5_PROTON_UMIP PASS: every check held\n";
        WriteFile(GetStdHandle(STD_ERROR_HANDLE), verdict, lstrlenA(verdict), &written, NULL);
        OutputDebugStringA(verdict);
    }
    ExitProcess(failures ? 1 : 0);
}
