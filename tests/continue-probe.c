/* SPDX-License-Identifier: GPL-3.0-or-later
 * NtContinue back into the code that called it, as ntdll's exception test_continue does (64-bit).
 * On the console (2026-10-26, B13) the test's first call ended in an unhandled access violation at the code's
 * "pop %r14" (code + 0xf3), a write at NtContinue's address with the direction flag set, where the pinned Linux Wine
 * passes; the 4 KiB MMU off changed nothing. This runs the test's own bytes in executable memory and, at any
 * exception, prints the instruction bytes at the address, the registers and the context the code captured before
 * NtContinue, so the record says what ran. Round A runs it as is, round B after setting the debug registers as
 * test_debug_registers leaves them (Dr0-Dr3 and Dr6 0xffffffff, Dr7 0x400), round C after other code ran from the
 * same memory and the test's bytes were copied over it, as in the test, round D after a SIMD invalid-operation
 * exception was continued past its instruction (test_simd_exceptions, the test before test_continue), round E after
 * test_debug_service's int $0x2d breakpoints were continued. PASS when both return with the registers
 * kept. */
#include <windows.h>
_ACRTIMP int __cdecl sprintf(char *, const char *, ...);   /* msvcrt; its stdio.h fails -Wextra */

static const BYTE call_func[] =   /* dlls/ntdll/tests/exception.c test_continue, unchanged */
{
    0x53, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57,
    0x48, 0x83, 0xec, 0x28, 0x48, 0x89, 0x64, 0x24, 0x20,
    0x48, 0x89, 0x4c, 0x24, 0x70, 0x48, 0x89, 0x54, 0x24, 0x78,
    0x4c, 0x89, 0x84, 0x24, 0x80, 0x00, 0x00, 0x00, 0x4c, 0x89, 0x8c, 0x24, 0x88, 0x00, 0x00, 0x00,
    0x41, 0xff, 0xd1,
    0x48, 0xb8, 0xef, 0xbe, 0xad, 0xde, 0xef, 0xbe, 0xad, 0xde,
    0x48, 0x89, 0xc1, 0x48, 0x89, 0xc2, 0x48, 0x89, 0xc3, 0x48, 0x89, 0xc5, 0x48, 0x89, 0xc6, 0x48, 0x89, 0xc7,
    0x49, 0x89, 0xc0, 0x49, 0x89, 0xc1, 0x49, 0x89, 0xc2, 0x49, 0x89, 0xc3, 0x49, 0x89, 0xc4, 0x49, 0x89, 0xc5,
    0x49, 0x89, 0xc6, 0x49, 0x89, 0xc7,
    0x66, 0x48, 0x0f, 0x6e, 0xc0, 0x66, 0x0f, 0x6c, 0xc0,
    0x0f, 0x28, 0xc8, 0x0f, 0x28, 0xd0, 0x0f, 0x28, 0xd8, 0x0f, 0x28, 0xe0, 0x0f, 0x28, 0xe8, 0x0f, 0x28, 0xf0,
    0x0f, 0x28, 0xf8, 0x44, 0x0f, 0x28, 0xc0, 0x44, 0x0f, 0x28, 0xc8, 0x44, 0x0f, 0x28, 0xd0, 0x44, 0x0f, 0x28, 0xd8,
    0x44, 0x0f, 0x28, 0xe0, 0x44, 0x0f, 0x28, 0xe8, 0x44, 0x0f, 0x28, 0xf0, 0x44, 0x0f, 0x28, 0xf8,
    0x48, 0x8b, 0x4c, 0x24, 0x70, 0x48, 0x8b, 0x54, 0x24, 0x78, 0x48, 0x83, 0xec, 0x70,
    0x48, 0x8d, 0x05, 0x18, 0x00, 0x00, 0x00, 0x48, 0x89, 0x81, 0xf8, 0x00, 0x00, 0x00,
    0x9c, 0x48, 0x81, 0x34, 0x24, 0xd5, 0x08, 0x00, 0x00, 0x9d,
    0xff, 0x94, 0x24, 0xf0, 0x00, 0x00, 0x00,
    0x48, 0x3b, 0x64, 0x24, 0x20, 0x74, 0x02, 0x0f, 0x0b,
    0x48, 0x8b, 0x4c, 0x24, 0x70, 0x48, 0x81, 0xc1, 0xd0, 0x04, 0x00, 0x00, 0xff, 0x94, 0x24, 0x88, 0x00, 0x00, 0x00,
    0x48, 0x83, 0xc4, 0x28,
    0x41, 0x5f, 0x41, 0x5e, 0x41, 0x5d, 0x41, 0x5c, 0x5f, 0x5e, 0x5d, 0x5b, 0xc3,
};

static struct { CONTEXT before, after; } contexts;
static BYTE *code;

static void say(const char *text)
{
    DWORD written;
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), text, lstrlenA(text), &written, NULL);
}

static void show(const char *what, const CONTEXT *c)
{
    char text[400];
    sprintf(text, "PS5_PROTON_CONTINUE %s: rip=%p (code+%#llx) rsp=%p eflags=%#lx cs=%#x ss=%#x ds=%#x es=%#x fs=%#x gs=%#x\n",
            what, (void *)c->Rip, (unsigned long long)(c->Rip - (DWORD64)code), (void *)c->Rsp, c->EFlags, c->SegCs, c->SegSs,
            c->SegDs, c->SegEs, c->SegFs, c->SegGs);
    say(text);
    sprintf(text, "PS5_PROTON_CONTINUE %s: rax=%p rcx=%p rdx=%p r8=%p r9=%p mxcsr=%#lx\n", what, (void *)c->Rax,
            (void *)c->Rcx, (void *)c->Rdx, (void *)c->R8, (void *)c->R9, c->MxCsr);
    say(text);
}

static LONG WINAPI handler(EXCEPTION_POINTERS *e)
{
    char text[300];
    const BYTE *at = e->ExceptionRecord->ExceptionAddress;
    int i, n;

    if (!(e->ExceptionRecord->ExceptionCode & 0x80000000)) return EXCEPTION_CONTINUE_SEARCH;   /* informational */
    if (e->ExceptionRecord->ExceptionCode == EXCEPTION_BREAKPOINT && at >= code + 0x800 && at < code + 0x900)
        return EXCEPTION_CONTINUE_EXECUTION;   /* round E's int $0x2d, continued as test_debug_service's handler does */
    if (e->ExceptionRecord->ExceptionCode == EXCEPTION_FLT_INVALID_OPERATION && at == code + 0x400 + 0x1b)
    {
        e->ContextRecord->Rip += 3;   /* round D's divps, skipped as test_simd_exceptions' handler does */
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    n = sprintf(text, "PS5_PROTON_CONTINUE FAIL: exception %#lx at %p (code+%#llx), info %p %p, bytes", e->ExceptionRecord->ExceptionCode,
                (void *)at, (unsigned long long)(at - code), (void *)e->ExceptionRecord->ExceptionInformation[0],
                (void *)e->ExceptionRecord->ExceptionInformation[1]);
    for (i = -8; i < 8; i++) n += sprintf(text + n, " %02x", at[i]);
    sprintf(text + n, "\n");
    say(text);
    show("at the exception", e->ContextRecord);
    show("captured before NtContinue", &contexts.before);
    ExitProcess(1);
    return EXCEPTION_CONTINUE_SEARCH;
}

static int round_(const char *name)
{
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    LONG (WINAPI *func)(void *, void *, void *, void *) = (void *)code;
    char text[200];
    int differ = 0;

    memset(&contexts, 0, sizeof(contexts));
    func(&contexts, 0, (void *)GetProcAddress(ntdll, "NtContinue"), (void *)GetProcAddress(ntdll, "RtlCaptureContext"));
#define SAME(r) differ += contexts.before.r != contexts.after.r
    SAME(Rax); SAME(Rdx); SAME(Rbx); SAME(Rbp); SAME(Rsi); SAME(Rdi); SAME(R8); SAME(R9); SAME(R10); SAME(R11);
    SAME(R12); SAME(R13); SAME(R14); SAME(R15);
    sprintf(text, "PS5_PROTON_CONTINUE %s: %d registers differ, flags %#lx captured with ContextFlags %#lx\n", name, differ,
            contexts.after.EFlags, contexts.before.ContextFlags);
    say(text);
    return differ;
}

void mainCRTStartup(void)
{
    CONTEXT ctx;
    int differ;

    code = VirtualAlloc(NULL, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    memcpy(code, call_func, sizeof(call_func));
    FlushInstructionCache(GetCurrentProcess(), code, sizeof(call_func));
    AddVectoredExceptionHandler(1, handler);
    differ = round_("round A, no debug registers");
    /* what ntdll exception's test_debug_registers leaves set before test_continue */
    memset(&ctx, 0, sizeof(ctx));
    ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    ctx.Dr0 = ctx.Dr1 = ctx.Dr2 = ctx.Dr3 = ctx.Dr6 = 0xffffffff;
    ctx.Dr7 = 0x400;
    SetThreadContext(GetCurrentThread(), &ctx);
    differ += round_("round B, debug registers as test_debug_registers leaves them");
    /* as the tests before test_continue do: other code run from the same memory, then the test's copied over it */
    {
        static const BYTE other[] = { 0x48, 0x31, 0xc0, 0x90, 0x90, 0x90, 0x90, 0xc3 };   /* xor %rax,%rax; nops; ret */
        void (*run)(void) = (void *)code;
        int i;
        for (i = 0; i < 0x200; i += sizeof(other)) memcpy(code + i, other, sizeof(other));
        FlushInstructionCache(GetCurrentProcess(), code, 0x200);
        run();
        memcpy(code, call_func, sizeof(call_func));
        FlushInstructionCache(GetCurrentProcess(), code, sizeof(call_func));
    }
    differ += round_("round C, the code copied over other code that ran from the same memory");
    /* as test_simd_exceptions does last: a SIMD invalid operation, its exception continued past the instruction */
    {
        static const BYTE simd[] =   /* exception.c simd_exception_test2 */
        {
            0x48, 0x83, 0xec, 0x8, 0x0f, 0xae, 0x1c, 0x24, 0x8b, 0x04, 0x24, 0x66, 0x81, 0x24, 0x24, 0x7f, 0xff,
            0x0f, 0xae, 0x14, 0x24, 0x0f, 0x57, 0xc9, 0x0f, 0x57, 0xc0, 0x0f, 0x5e, 0xc8, 0x89, 0x04, 0x24,
            0x0f, 0xae, 0x14, 0x24, 0x48, 0x83, 0xc4, 0x08, 0xc3,
        };
        void (*run)(void) = (void *)(code + 0x400);
        memcpy(code + 0x400, simd, sizeof(simd));
        FlushInstructionCache(GetCurrentProcess(), code + 0x400, sizeof(simd));
        run();
    }
    differ += round_("round D, after a SIMD exception continued past its instruction");
    /* as test_debug_service does before: int $0x2d with each debug service number, the breakpoints continued */
    {
        static const BYTE service[] =   /* exception.c call_debug_service_code, x86_64 */
        {
            0x53, 0x57, 0x48, 0x89, 0xc8, 0x48, 0xb9, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x48,
            0xba, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x48, 0xbb, 0x33, 0x33, 0x33, 0x33, 0x33,
            0x33, 0x33, 0x33, 0x48, 0xbf, 0x44, 0x44, 0x44, 0x44, 0x44, 0x44, 0x44, 0x44, 0xcd, 0x2d, 0xeb,
            0x0f, 0x1f, 0x00, 0x48, 0x31, 0xc0, 0xeb, 0x0e, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
            0x48, 0x31, 0xc0, 0x48, 0xff, 0xc0, 0x5f, 0x5b, 0xc3,
        };
        DWORD_PTR (*call)(DWORD_PTR) = (void *)(code + 0x800);
        static const DWORD_PTR numbers[] = { 0, 2, 6, 1, 3, 4, 5 };
        unsigned int i;
        memcpy(code + 0x800, service, sizeof(service));
        FlushInstructionCache(GetCurrentProcess(), code + 0x800, sizeof(service));
        for (i = 0; i < sizeof(numbers) / sizeof(numbers[0]); i++) call(numbers[i]);
    }
    differ += round_("round E, after int $0x2d breakpoints continued");
    say(differ ? "PS5_PROTON_CONTINUE FAIL: registers differ after NtContinue\n"
               : "PS5_PROTON_CONTINUE PASS: NtContinue returned into the code, registers kept, with and without debug registers set\n");
    ExitProcess(differ ? 1 : 0);
}
