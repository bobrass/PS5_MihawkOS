#!/usr/bin/env bash
# Build Wine's own conformance tests (dlls/*/tests) as x86_64 and i386 PE programs.
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The tests come from the same rebased Proton Wine source as the runtime
# (tools/build-wine-runtime.sh generates it into .deps/wine-runtime/source),
# built out of tree with the same clang/lld cross toolchain and the same
# configure options except that tests are enabled, so the runtime's own build
# stays as it is. Each test is one program per DLL (ntdll_test.exe) that runs
# every test of that DLL; a program takes a test name to run just that file
# (ntdll_test.exe virtual) and prints Wine's "file:line: Test failed" lines.
#
#   tools/build-wine-tests.sh [dll...]        default: ntdll kernel32 kernelbase
#
# The executables are collected in .deps/wine-tests/exe/<dll>_test.exe.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
source=$root/.deps/wine-runtime/source
work=$root/.deps/wine-tests
revision=$(cat "$root/.deps/wine-runtime/revision")
configure_args=(--enable-win64 --enable-archs=i386,x86_64 --with-mingw=clang --enable-tests
    --without-x --without-wayland --without-gstreamer --without-ffmpeg --without-alsa --without-pulse)
stamp=$(printf '%s\n' "$revision" "${configure_args[@]}" | sha256sum | cut -c1-64)
[[ -d $source ]] || { echo "build the runtime first: tools/build-wine-runtime.sh" >&2; exit 1; }
mkdir -p "$work/build" "$work/exe"
if [[ $(cat "$work/stamp" 2>/dev/null) != "$stamp" ]]; then
    (cd "$work/build" && "$source/configure" "${configure_args[@]}" > "$work/configure.log" 2>&1)
    echo "$stamp" > "$work/stamp"
fi
# The console cannot afford a title launch per test file (every launch opens and closes the video output, and
# back-to-back launches have cost power-offs), so each program also takes a list of test files ("virtual,path,env",
# or --all) and runs them one after another in the same process, with the counters reset between them and
# "winetest-batch: begin NAME" and "winetest-batch: end NAME STATUS" lines around each. The list is handled by
# a copy of Wine's test.h that comes first in the include path (the build directory of each test comes first).
override=$work/test.h
python3 - "$source/include/wine/test.h" "$override" <<'PY'
import sys
text = open(sys.argv[1]).read()
anchor = "/* Display usage and exit */"
guard = r"""/* A test function of the Direct3D tests that crashes (the null interface a failed call leaves behind, then used) ends the
 * whole file. They run through BATCH_GUARD() and batch_guarded() instead (tools/build-wine-tests.sh rewrites their call
 * sites), which report the crash as a failure named by the call's line (or the queued function's offset in the program) and
 * go on with the next one. It uses the unhandled exception filter, so a test that expects and handles a crash itself still
 * does. */
#include <setjmp.h>
#include <stdlib.h>
struct batch_guard { jmp_buf jump; DWORD code; ULONG_PTR address; };
static DWORD batch_guard_slot = TLS_OUT_OF_INDEXES;
static LPTOP_LEVEL_EXCEPTION_FILTER batch_guard_previous;

static inline LONG WINAPI batch_guard_filter( EXCEPTION_POINTERS *info )
{
    struct batch_guard *guard = batch_guard_slot == TLS_OUT_OF_INDEXES ? NULL : TlsGetValue( batch_guard_slot );

    if (guard)
    {
        guard->code = info->ExceptionRecord->ExceptionCode;
        guard->address = (ULONG_PTR)info->ExceptionRecord->ExceptionAddress - (ULONG_PTR)GetModuleHandleA( NULL );
        longjmp( guard->jump, 1 );
    }
    return batch_guard_previous ? batch_guard_previous( info ) : EXCEPTION_CONTINUE_SEARCH;
}

static inline void batch_guard_arm( struct batch_guard *guard )
{
    static LONG installed;

    if (!InterlockedCompareExchange( &installed, 1, 0 ))
    {
        batch_guard_slot = TlsAlloc();
        batch_guard_previous = SetUnhandledExceptionFilter( batch_guard_filter );
    }
    TlsSetValue( batch_guard_slot, guard );
}

static inline void batch_guard_disarm( void ) { TlsSetValue( batch_guard_slot, NULL ); }

static inline void batch_guard_report( const struct batch_guard *guard, int id, const char *what )
{
    batch_guard_disarm();
    winetest_set_location( "crash.c", id );
    winetest_ok( 0, "%s crashed with %#lx at offset %#Ix\n", what, guard->code, (size_t)guard->address );
}

/* A Direct3D test file can be run in slices of its guarded calls ("d3d11@40-79" in the list: calls 40 to 79, counted
 * in the order the file makes them), so that what the calls leave behind (devices with their threads) stays within
 * what one launch of the console can hold. The first number counted is 0. The counter and the slice are
 * environment variables, which the test sources and the batch runner both see. */
static inline unsigned int batch_env_number( const char *name, unsigned int fallback )
{
    char buffer[16];

    return GetEnvironmentVariableA( name, buffer, sizeof(buffer) ) ? (unsigned int)atoi( buffer ) : fallback;
}

static inline int batch_slice_wants( void )
{
    unsigned int n = batch_env_number( "WINETEST_GUARDED", 0 ), from = batch_env_number( "WINETEST_SLICE_FROM", 0 );
    unsigned int to = batch_env_number( "WINETEST_SLICE_TO", 0xffffffff ), value = n + 1;
    char digits[16], reversed[16];
    int count = 0, i;

    do reversed[count++] = '0' + value % 10; while ((value /= 10));
    for (i = 0; i < count; i++) digits[i] = reversed[count - 1 - i];
    digits[count] = 0;
    SetEnvironmentVariableA( "WINETEST_GUARDED", digits );
    return n >= from && n < to;
}

#define BATCH_GUARD(call) do { \
        struct batch_guard guard_; \
        if (batch_slice_wants()) { \
            if (!setjmp( guard_.jump )) { batch_guard_arm( &guard_ ); call; } \
            else batch_guard_report( &guard_, __LINE__, #call ); \
            batch_guard_disarm(); \
        } \
    } while (0)

static inline void batch_guarded_call( void (*plain)(void), void (*with_level)(unsigned int), unsigned int level )
{
    struct batch_guard guard;
    int offset = (int)((ULONG_PTR)(plain ? (void *)plain : (void *)with_level) - (ULONG_PTR)GetModuleHandleA( NULL ));

    if (!batch_slice_wants()) return;
    if (!setjmp( guard.jump ))
    {
        batch_guard_arm( &guard );
        if (plain) plain(); else with_level( level );
    }
    else batch_guard_report( &guard, offset, "a queued test function" );
    batch_guard_disarm();
}
static inline void batch_guarded( void (*test)(void) ) { batch_guarded_call( test, NULL, 0 ); }
static inline void batch_guarded_fl( void (*test)(unsigned int), unsigned int level ) { batch_guarded_call( NULL, test, level ); }


"""
batch = r"""/* PS5_Proton: several test files in turn, in this process (tools/build-wine-tests.sh) */
static HANDLE batch_watchdog_done;
static const char *batch_watchdog_name;
static DWORD batch_watchdog_seconds = 120;

/* A test that waits for a child process the console cannot start would wait for ever, and a title that is killed
 * costs the console its services: so a test file that is still running after batch_watchdog_seconds ends the program
 * itself, which closes the title the normal way. The line is written without the C library, whose lock the stuck
 * test may hold. Seconds can be set with WINETEST_FILE_TIMEOUT. */
static DWORD WINAPI batch_watchdog( void *arg )
{
    char text[160], digits[12];
    DWORD written, seconds = batch_watchdog_seconds;
    int length, count = 0;

    if (WaitForSingleObject( batch_watchdog_done, seconds * 1000 ) != WAIT_TIMEOUT) return 0;
    lstrcpyA( text, "\nwinetest-batch: timeout " );
    lstrcatA( text, batch_watchdog_name );
    lstrcatA( text, " " );
    do digits[count++] = '0' + seconds % 10; while ((seconds /= 10));
    length = lstrlenA( text );
    while (count) text[length++] = digits[--count];
    text[length++] = '\n';
    WriteFile( GetStdHandle( STD_OUTPUT_HANDLE ), text, length, &written, NULL );
    ExitProcess( 124 );
}

static int run_tests_in_turn( const char *list )
{
    int status = 0, count = 0;
    const char *at = list;
    HANDLE watchdog;
    char *seconds = getenv( "WINETEST_FILE_TIMEOUT" );

    if (seconds && atoi( seconds ) > 0) batch_watchdog_seconds = atoi( seconds );

    for (;;)
    {
        char name[64], full_name[64], guarded[16];
        char *slice;
        size_t length = 0;
        int result;

        if (!strcmp( list, "--all" ))
        {
            if (!winetest_testlist[count].name) break;
            lstrcpynA( name, winetest_testlist[count++].name, sizeof(name) );
        }
        else
        {
            while (at[length] && at[length] != ',' && length < sizeof(name) - 1) length++;
            if (!length) break;
            memcpy( name, at, length );
            name[length] = 0;
            at += length;
            if (*at == ',') at++;
        }
        lstrcpynA( full_name, name, sizeof(full_name) );
        /* "name@from-to": only the guarded calls from to to (not included) run */
        SetEnvironmentVariableA( "WINETEST_SLICE_FROM", NULL );
        SetEnvironmentVariableA( "WINETEST_SLICE_TO", NULL );
        SetEnvironmentVariableA( "WINETEST_GUARDED", "0" );
        if ((slice = strchr( name, '@' )))
        {
            char *dash = strchr( slice + 1, '-' );

            *slice++ = 0;
            if (dash)
            {
                *dash++ = 0;
                SetEnvironmentVariableA( "WINETEST_SLICE_FROM", slice );
                SetEnvironmentVariableA( "WINETEST_SLICE_TO", dash );
            }
        }
        winetest_successes = winetest_failures = winetest_flaky_failures = winetest_skipped = 0;
        winetest_todo_successes = winetest_todo_failures = 0;
        winetest_muted_traces = winetest_muted_skipped = winetest_muted_todo_successes = 0;
        printf( "winetest-batch: begin %s\n", full_name );
        fflush( stdout );
        batch_watchdog_name = full_name;
        batch_watchdog_done = CreateEventA( NULL, TRUE, FALSE, NULL );
        watchdog = CreateThread( NULL, 0, batch_watchdog, NULL, 0, NULL );
        /* A test starts its child processes as "argv[0] argv[1] child-args": argv[1] has to be this file's name, or every
         * child (a debuggee, a dummy) is a whole new batch of the list instead (ntdll info's children, 2026-10-25). */
        if (winetest_argc > 1) winetest_argv[1] = name;
        result = run_test( name );
        SetEvent( batch_watchdog_done );
        if (watchdog) { WaitForSingleObject( watchdog, INFINITE ); CloseHandle( watchdog ); }
        CloseHandle( batch_watchdog_done );
        if (GetEnvironmentVariableA( "WINETEST_GUARDED", guarded, sizeof(guarded) ) && atoi( guarded ))
            printf( "winetest-batch: guarded %s\n", guarded );
        printf( "winetest-batch: end %s %d\n", full_name, result );
        status |= result;
        if (strcmp( list, "--all" ) && !*at) break;
    }
    printf( "winetest-batch: done\n" );
    return status;
}


"""
assert text.count(anchor) == 1
text = text.replace(anchor, batch + anchor)
anchor_guard = "/************************************************************************/\n/* Below is the implementation"
assert text.count(anchor_guard) == 1
text = text.replace(anchor_guard, guard + anchor_guard)
anchor2 = '    if (!strcmp( argv[1], "--list" ))'
assert text.count(anchor2) == 1
text = text.replace(anchor2, '    if (strchr( argv[1], \',\' ) || !strcmp( argv[1], "--all" ))\n        return run_tests_in_turn( argv[1] );\n' + anchor2)
open(sys.argv[2], "w").write(text)
PY
cmp -s "$override" "$work/test.h.built" 2>/dev/null || rm -f "$work"/build/dlls/*/tests/{x86_64,i386}-windows/*.o "$work"/build/dlls/*/tests/{x86_64,i386}-windows/*_test.exe
# The Direct3D tests that queue their test functions get every call guarded (see batch_guarded above): the call sites are
# rewritten in a copy of the source, and a compiler wrapper (make x86_64_CC=...) swaps that copy in for the original.
mkdir -p "$work/guarded"
python3 - "$source" "$work/guarded" <<'PY'
import sys
source, out = sys.argv[1:3]
rewrites = {
    ('d3d11', 'd3d11'): [('t->u.test_fl(t->fl);', 'batch_guarded_fl( (void (*)(unsigned int))t->u.test_fl, t->fl );'),
                         ('t->u.test();', 'batch_guarded( t->u.test );')],
    ('d3d10core', 'd3d10core'): [('mt_tests[j].test();', 'batch_guarded( mt_tests[j].test );'),
                                 ('mt_tests[i].test();', 'batch_guarded( mt_tests[i].test );')],
    ('dxgi', 'dxgi'): [('mt_tests[j].test();', 'batch_guarded( mt_tests[j].test );'),
                       ('mt_tests[i].test();', 'batch_guarded( mt_tests[i].test );')],
    # d3d9: one test program of four files (d3d9ex, device, stateblock, visual), whose START_TEST calls are all direct
    ('d3d12', 'd3d12'): [],   # Wine's small d3d12 test: direct calls
    ('d3d9', 'd3d9ex'): [], ('d3d9', 'device'): [], ('d3d9', 'stateblock'): [], ('d3d9', 'visual'): [],
}
import re
for (dll, name), pairs in rewrites.items():
    text = open(f'{source}/dlls/{dll}/tests/{name}.c').read()
    for old, new in pairs:
        assert text.count(old) >= 1, (dll, old)
        text = text.replace(old, new)
    # the calls START_TEST makes itself: "    test_something();" and "    run_on_d3d10(test_something);" on one line
    head, marker, body = text.partition(f'\nSTART_TEST({name})')
    assert marker, (dll, name)
    body = re.sub(r'^(    )((?:test_\w+|run_on_\w+)\(.*\));$', r'\1BATCH_GUARD( \2 );', body, flags=re.M)
    open(f'{out}/{dll}-{name}.c', 'w').write(head + marker + body)
PY
cat > "$work/guard-cc" <<WRAPPER
#!/bin/sh
n=\$#; i=0
while [ \$i -lt \$n ]; do
    a=\$1; shift
    case "\$a" in
        */dlls/d3d11/tests/d3d11.c) a=$work/guarded/d3d11-d3d11.c;;
        */dlls/d3d10core/tests/d3d10core.c) a=$work/guarded/d3d10core-d3d10core.c;;
        */dlls/dxgi/tests/dxgi.c) a=$work/guarded/dxgi-dxgi.c;;
        */dlls/d3d12/tests/d3d12.c) a=$work/guarded/d3d12-d3d12.c;;
        */dlls/d3d9/tests/d3d9ex.c) a=$work/guarded/d3d9-d3d9ex.c;;
        */dlls/d3d9/tests/device.c) a=$work/guarded/d3d9-device.c;;
        */dlls/d3d9/tests/stateblock.c) a=$work/guarded/d3d9-stateblock.c;;
        */dlls/d3d9/tests/visual.c) a=$work/guarded/d3d9-visual.c;;
    esac
    set -- "\$@" "\$a"; i=\$((i+1))
done
exec clang "\$@"
WRAPPER
chmod +x "$work/guard-cc"
dlls=("$@")
[[ ${#dlls[@]} -gt 0 ]] || dlls=(ntdll kernel32 kernelbase)
for dll in "${dlls[@]}"; do
    # a DLL's tests are dlls/NAME/tests (NAME may be ndis.sys, ntoskrnl.exe); a program's are programs/NAME/tests,
    # named NAME.exe (cmd.exe_test.exe), as Wine names them
    [[ $dll =~ ^[A-Za-z0-9_.-]+$ ]] || { echo "test module name: letters, digits, _, . and -" >&2; exit 2; }
    if [[ -d $source/dlls/$dll/tests ]]; then dir=dlls/$dll
    elif [[ $dll == *.exe && -d $source/programs/${dll%.exe}/tests ]]; then dir=programs/${dll%.exe}
    else echo "no tests for $dll" >&2; exit 2; fi
    mkdir -p "$work/build/$dir/tests/wine"
    cp "$override" "$work/build/$dir/tests/wine/test.h"
    for arch in x86_64 i386; do
        suffix=; [[ $arch != i386 ]] || suffix=32
        make -C "$work/build" -j"${JOBS:-14}" x86_64_CC="$work/guard-cc" i386_CC="$work/guard-cc" \
            "$dir/tests/$arch-windows/${dll}_test.exe" > "$work/make-$dll-$arch.log" 2>&1 ||
            { echo "cannot build the $dll $arch tests; see $work/make-$dll-$arch.log" >&2; exit 1; }
        cp "$work/build/$dir/tests/$arch-windows/${dll}_test.exe" "$work/exe/${dll}_test$suffix.exe"
        llvm-strip --strip-debug "$work/exe/${dll}_test$suffix.exe"
    done
done
cp "$override" "$work/test.h.built"
# Pin what the comparison ran, including the generated test header and the test executables.
python3 - "$work" "$revision" <<'PYMETA'
import hashlib, json, pathlib, sys
work, revision = pathlib.Path(sys.argv[1]), sys.argv[2]
digest = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
manifest = {'wine_revision': revision, 'batch_header_sha256': digest(work / 'test.h'),
            'executables': {p.name: digest(p) for p in sorted((work / 'exe').glob('*.exe'))}}
(work / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
PYMETA
ls -la "$work/exe"
