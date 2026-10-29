/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The helper loader: a payload process the title starts through websrv's payload launcher. It receives the
 * SOCK_SEQPACKET control endpoint, then the Wine bootstrap (proton_wine_child.c). fd 3 is control;
 * WINESERVERSOCKET is separate. */
#include <sys/un.h>
#include "../wine/ps5/pw_payload_rendezvous.h"
#include <signal.h>
#include <sys/mman.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <ps5/payload.h>
#include <ps5platform/kernel.h>
#include "../wine/ps5/pw_wine_prx.h"
extern int sceKernelAllocateDirectMemoryForApp(int64_t, int64_t, size_t, size_t, int, int64_t *);

/* Each helper the title ends leaked one of the kernel's 128 TTY(STDIN) slots while its loader child kept fd 0: 95
 * "!!LEAKED OPEN!!" entries after a day of unloaded Explorers, then "child bootstrap failed errno=5" (docs/PLAN.md,
 * 2026-10-16, record removed from the tree). Take /dev/null for fd 0 first thing, before the rendezvous or any log, and for fd 1
 * and 2 when they are the same terminal: nothing in a helper reads the console. What fd 0 was (fstat's type, device and
 * mode) and what this did are logged once the loader's own log is open (native/proton_wine_child.c "console tty" line). */
static int tty_fstat_status, tty_fstat_error, tty_stdin_null, tty_stdout_null, tty_stderr_null, tty_error;
static unsigned long long tty_dev, tty_rdev;
static unsigned tty_mode;
static void tty_release_console(void)
{
    struct stat console;
    tty_fstat_status = fstat(STDIN_FILENO, &console);
    tty_fstat_error = tty_fstat_status ? errno : 0;
    if (!tty_fstat_status) { tty_dev = (unsigned long long)console.st_dev; tty_rdev = (unsigned long long)console.st_rdev;
                             tty_mode = (unsigned)console.st_mode; }
    const int console_tty = !tty_fstat_status && S_ISCHR(console.st_mode);
    const int null_fd = open("/dev/null", O_RDWR);
    if (null_fd < 0) { tty_error = errno; return; }
    if (dup2(null_fd, STDIN_FILENO) < 0) { tty_error = errno; close(null_fd); return; }
    tty_stdin_null = 1;
    for (int fd = STDOUT_FILENO; fd <= STDERR_FILENO; ++fd) {
        struct stat same;
        if (!console_tty || fstat(fd, &same) || same.st_dev != console.st_dev || same.st_rdev != console.st_rdev) continue;
        if (dup2(null_fd, fd) < 0) { tty_error = errno; continue; }
        if (fd == STDOUT_FILENO) tty_stdout_null = 1; else tty_stderr_null = 1;
    }
    if (null_fd != STDIN_FILENO) close(null_fd);
}

/* websrv's elfldr starts this loader inside Sony's carrier executable (NPXS40112's eboot.bin, 0x400000-0x584000), whose
 * code never runs, and maps the loader itself at 0x200000000. A 32-bit program linked /fixed at 0x400000 (an old
 * installer) can load only there, so its helper, and only its (PW_CHILD_FIXED_BASE, set by the title), gives the
 * carrier's segments back once the bootstrap says so, before Wine reserves anything (pw_prx_release_low_image;
 * carrier-image-2026-10-19-03). Every other helper keeps them: a relocatable 32-bit program placed at 0x400000 lost
 * its audio and controller drivers (free-0x400000-control-2026-10-19-01). libkernel still lists eboot.bin with its
 * old segments afterwards; nothing in a helper reads the module list. Logged in proton_wine_child.c. */
extern int sceKernelGetModuleList(int32_t *, size_t, size_t *);
extern int sceKernelGetModuleInfo(int, void *);
static int carrier_refused = -1, carrier_errno;   /* -1: no module held 0x400000 */
static uintptr_t carrier_end;
static int carrier_unmap(void *address, size_t bytes)
{
    const int status = munmap(address, bytes);
    if (status) carrier_errno = errno;
    return status;
}
static void carrier_release(void)
{
    int32_t handles[16];
    size_t count = 0;
    if (sceKernelGetModuleList(handles, 16, &count)) return;
    for (size_t i = 0; i < count && i < 16 && carrier_refused < 0; i++) {
        union { uint64_t size; unsigned char bytes[PW_PRX_MODULE_INFO_BYTES]; } info = { .size = PW_PRX_MODULE_INFO_BYTES };
        if (sceKernelGetModuleInfo(handles[i], info.bytes) >= 0)
            carrier_refused = pw_prx_release_low_image(info.bytes, 0x400000, carrier_unmap, &carrier_end);
    }
}

/* MainDirectMemory selects the system partition in a budget-0 payload.
 * ForApp explicitly takes pages from the running application's direct pool,
 * while the allocating payload retains ownership of its allocation. */
static int payload_allocate_direct(size_t bytes, size_t alignment, int type, int64_t *offset)
{
    return sceKernelAllocateDirectMemoryForApp(0, sceKernelGetDirectMemorySize(), bytes, alignment, type, offset);
}
#define PROTON_CHILD_GPU_BROKER
#include "proton_wine_child.c"

/* Wire every segment of this payload's verified PRX module it cannot write, text and read-only data (pw_prx_wired),
 * while the load has them in memory: a payload cannot page a fake-signed module back in once the kernel has evicted it
 * (klog "pager error"; in a 2026-10-06 run, record removed from the tree, the game's helper died on win32u text, and again on the
 * text of ntdll's signal handler; .rodata and .eh_frame, which Wine's unwinder reads, come from the same SELF). Writable
 * data and bss are ordinary memory. Its text keeps Sony's execute-only protection (0x4): no protection call, so no SDK
 * write to the kernel's map entry, and the loader never reads it, since reading execute-only text ends the payload
 * (SYSTEM_XO_VIOLATION, payload-graphics-08). mlock leaves it 0x4 and resident (K9, exec-kernel-2026-10-14-02). ntdll
 * unwires the same segments before it unloads a module (pw_wine_dl_libc.c, pw_prx_unwire): the teardown of wired
 * memory is what hangs the console ("unwire timeout"). A segment that cannot be wired is counted and logged, not
 * refused: it stays evictable as before. */
static unsigned payload_text_unwired;
void pw_malloc_trace_module(const char *name,const PwPrxSegment *segments,uint32_t count);
static int payload_prepare_module(int32_t handle)
{
    unsigned char info[PW_PRX_MODULE_INFO_BYTES]={0};
    uint64_t size=sizeof(info);memcpy(info,&size,sizeof(size));
    PwPrxSegment segments[PW_PRX_MAX_SEGMENTS];uint32_t count=0;
    char name[PW_PRX_MAX_NAME],line[256];
    if(sceKernelGetModuleInfo(handle,info)<0 ||
       pw_prx_parse_module_info(info,name,segments,&count))return -1;
    unsigned locked=0,text_unwired=0;size_t wired=0;int why=0;
    for(unsigned i=0;i<count;i++) {
        if(!pw_prx_wired(segments[i].protection))continue;
        const int wired_now=!mlock(segments[i].address,segments[i].size);
        if(wired_now) { locked++;wired+=segments[i].size; } else why=errno;
        text_unwired+=!wired_now;
        if(!(segments[i].protection&PROT_EXEC))continue;
        /* Every text base, wired or not: tools/profile-report.py names native RIPs from this line. */
        snprintf(line,sizeof(line),"wine-child: payload module=%s text=%p bytes=%u wired=%d errno=%d unwired_text=%u\n",
                 name,segments[i].address,segments[i].size,wired_now,wired_now?0:why,
                 wired_now?__atomic_load_n(&payload_text_unwired,__ATOMIC_RELAXED)
                          :__atomic_add_fetch(&payload_text_unwired,1,__ATOMIC_RELAXED));
        stage(line);
    }
    pw_malloc_trace_module(name,segments,count);
    if(!text_unwired)__atomic_add_fetch(&payload_modules,1,__ATOMIC_RELAXED);
    struct rlimit limit={0};
    const long long most=getrlimit(RLIMIT_MEMLOCK,&limit) ? -2 : limit.rlim_cur==RLIM_INFINITY ? -1 : (long long)limit.rlim_cur;
    snprintf(line,sizeof(line),"wine-child: payload module=%s locked=%u/%u bytes=%zu errno=%d limit=%lld\n",name,locked,count,wired,why,most);
    stage(line);
    return 0;
}

/* The payload SDK's grant of execute (its libc mmap and mprotect, and its so loader) writes the kernel's vm_map entry
 * without the map's lock (PS5_PayloadSDK crt/kernel.c:1492) while other threads change the map. Linked wrapped
 * (tools/build-payload-loader.sh, which also checks the disassembly calls neither), so it is only ever counted and
 * refused; execute comes from JIT memory instead (wine/ps5/pw_wine_dmem_ps5.c). */
int __wrap_kernel_mprotect(pid_t pid, intptr_t address, size_t bytes, int protection)
{
    (void)pid;(void)address;(void)bytes;(void)protection;
    __atomic_add_fetch(&sdk_exec_refused,1,__ATOMIC_RELAXED);
    errno=EPERM;
    return -1;
}
int __wrap_kernel_set_vmem_protection(pid_t pid, intptr_t address, size_t bytes, int protection)
{
    return __wrap_kernel_mprotect(pid,address,bytes,protection);
}

static int take_control(int argc, char **argv)
{
    const char *token = NULL;
    for(int i=0;i<argc;++i) {
        const char *a=argv[i];
        if(!a || strncmp(a,"pp",2) || strlen(a)!=34)continue;
        if(strspn(a+2,"0123456789abcdef")==32)token=a;
    }
    if(!token)return -1;
    struct sockaddr_un address={0};address.sun_family=AF_UNIX;
    if(snprintf(address.sun_path,sizeof(address.sun_path),"/system_tmp/proton-%s.sock",token)>=(int)sizeof(address.sun_path))return -1;
    const int listener=socket(AF_UNIX,SOCK_STREAM,0);
    if(listener<0)return -1;
    if(bind(listener,(struct sockaddr *)&address,sizeof(address)) || listen(listener,1)) { close(listener);return -1; }
    chmod(address.sun_path,0666);
    struct pollfd pending={listener,POLLIN,0};
    const int connection=poll(&pending,1,20000)==1 ? accept(listener,NULL,NULL) : -1;
    close(listener);unlink(address.sun_path);
    if(connection<0)return -1;
    const payload_args_t *a=payload_get_args();
    if(a->rwpipe[0]==3 || a->rwpipe[1]==3 || a->rwpair[0]==3 || a->rwpair[1]==3) { close(connection);return -1; }
    /* The title closes its copy of the endpoint only after this says it holds it (pw_payload_rendezvous.h). */
    const int control=pw_payload_take(connection);
    close(connection);
    if(control<0)return -1;
    if(dup2(control,3)!=3) { close(control);return -1; }
    if(control!=3)close(control);
    return 0;
}
int main(int argc,char **argv)
{
    /* First, before the rendezvous or any failure path: no helper keeps the console's TTY (see above). */
    tty_release_console();
    signal(SIGPIPE,SIG_IGN);
    /* payload_prepare_module wires each module: take the most the limit allows. */
    struct rlimit wired;
    if(!getrlimit(RLIMIT_MEMLOCK,&wired) && wired.rlim_cur!=wired.rlim_max) { wired.rlim_cur=wired.rlim_max;(void)setrlimit(RLIMIT_MEMLOCK,&wired); }
    start_main_ms=payload_monotonic_ms();
    if(take_control(argc,argv))return 1;
    start_control_ms=payload_monotonic_ms();
    wine_child_run();
    /* The kernel's exit, not libc's: Wine has killed its threads asynchronously, after which only async-signal-safe
     * calls are reliable. In a GPU helper libc's exit came back without running its handlers (v6/v7/v8-exit-2026-10-15:
     * the last handler never ran, exit returned to the SDK's crt), which then trapped at the loader's 0x398 and the
     * kernel dumped the process: nearly every run of the earlier emulator experiment since 2026-10-13. The terminal is flushed; nothing else is
     * left to do. A Sony title still never returns. */
    char line[96];
    snprintf(line,sizeof(line),"wine-child: payload ending monotonic_ms=%lld\n",payload_monotonic_ms());
    stage(line);
    _exit(0);
}
