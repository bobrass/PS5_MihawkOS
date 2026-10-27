/* SPDX-License-Identifier: GPL-3.0-or-later
 * A deep stack keeps its exception handling. kernel32 heap's test_GlobalAlloc has 512 KiB of locals (x86_64) and
 * calls GlobalSize on 0xdeadbee0; on the console (2026-10-26, B13) the read faulted as on the PC, but the dispatcher
 * found the handler's frame (0x100018f840) below the thread's StackLimit (0x10001f8000, StackBase 0x1000210000), refused
 * to dispatch ("Exception frame is not in stack limits") and the process ended, where the pinned Linux Wine catches it.
 *
 * The main thread first recurses ~600 KiB down its stack in frames under a page each, touching each frame's bytes
 * from the top down as a compiler's stack probe does ("walked"), then, in a new thread, enters one 512 KiB frame with
 * no probe at all, as code calling ntdll's __chkstk (which does not probe) does ("skipped"; this file is built with
 * -mno-stack-arg-probe). At each bottom it asks IsBadReadPtr about 0xffffffffdeadbee0, which kernelbase answers
 * through a handler on the stack. PASS when both answer TRUE with StackLimit at or below the frame; a refused
 * dispatch ends the process with no verdict line. */
#include <windows.h>
#include <winternl.h>

_ACRTIMP int __cdecl sprintf(char *, const char *, ...);   /* msvcrt; its stdio.h fails -Wextra */

enum { FRAME = 3000, DEPTH = 200 };   /* 600 KB in frames under a page */

static void say(const char *text)
{
    DWORD written;
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), text, lstrlenA(text), &written, NULL);
}

static void *limit(void) { return NtCurrentTeb()->Tib.StackLimit; }

static int bottom(const char *how, volatile char *frame)
{
    char text[300];
    int bad;

    sprintf(text, "PS5_PROTON_STACK_LIMIT %s: %#lx bytes down, rsp near %p, StackLimit %p, StackBase %p\n", how,
            (unsigned long)((char *)NtCurrentTeb()->Tib.StackBase - (char *)frame), (void *)frame, limit(), NtCurrentTeb()->Tib.StackBase);
    say(text);
    bad = IsBadReadPtr((void *)(LONG_PTR)(LONG)0xdeadbee0, 4);
    sprintf(text, "PS5_PROTON_STACK_LIMIT %s: IsBadReadPtr(0xdeadbee0) = %d with StackLimit %p at %p\n", how, bad,
            limit(), (void *)frame);
    say(text);
    return bad && limit() <= (void *)frame ? 0 : 1;
}

static int deep(int depth)
{
    volatile char frame[FRAME];
    int i, failed;

    for (i = FRAME - 1; i >= 0; i -= 512) frame[i] = (char)depth;
    failed = depth ? deep(depth - 1) : bottom("walked", frame);
    frame[0] = 0;   /* the frame stays live across the call: no tail call */
    return failed;
}

static DWORD WINAPI skipped(void *arg)
{
    volatile char frame[512 * 1024];   /* test_GlobalAlloc's HGLOBAL globals[0x10000], entered with no probe */

    (void)arg;
    frame[0] = 1;
    return bottom("skipped", frame);
}

void mainCRTStartup(void)
{
    char text[200];
    int failed;

    sprintf(text, "PS5_PROTON_STACK_LIMIT at the start: StackLimit %p, StackBase %p, DeallocationStack %p\n", limit(),
            NtCurrentTeb()->Tib.StackBase, ((void **)NtCurrentTeb())[0x1478 / sizeof(void *)]);
    say(text);
    failed = deep(DEPTH);
    {
        HANDLE thread = CreateThread(NULL, 0, skipped, NULL, 0, NULL);
        DWORD code = 1;
        WaitForSingleObject(thread, INFINITE);
        GetExitCodeThread(thread, &code);
        failed |= code;
    }
    say(failed ? "PS5_PROTON_STACK_LIMIT FAIL: a deep stack lost its exception handling\n"
               : "PS5_PROTON_STACK_LIMIT PASS: exceptions are dispatched at the bottom of a deep stack, walked or skipped\n");
    ExitProcess(failed);
}
