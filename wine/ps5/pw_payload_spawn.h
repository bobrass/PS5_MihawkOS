/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PW_PAYLOAD_SPAWN_H
#define PW_PAYLOAD_SPAWN_H
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/un.h>
#include <sys/time.h>
#include <time.h>
#include <signal.h>
#include <limits.h>
#include <fcntl.h>
#include "pw_payload_rendezvous.h"
#include "../../src/pw_elfldr.h"

/* Opt-in bounded phase records bypass stderr/klog and their locks. Each call
 * owns its descriptor and preserves errno. The runner removes this diagnostic
 * file before a traced launch; no guest paths or data are recorded. */
static void payload_trace(const char *phase,int pid,int control,int status)
{
    const int saved_errno=errno;
    const char *enabled=getenv("WINETEST_LIFECYCLE_TRACE");
    if(enabled && !strcmp(enabled,"1")) {
        struct timespec now={0};char line[192];
        clock_gettime(CLOCK_MONOTONIC,&now);
        int count=snprintf(line,sizeof(line),"payload-lifecycle: ms=%lld writer=%d child=%d fd=%d phase=%s status=%d\n",
            now.tv_sec*1000ll+now.tv_nsec/1000000,(int)getpid(),pid,control,phase,status);
        int fd=open("/data/homebrew/PPSA99170/payload-lifecycle.log",O_WRONLY|O_CREAT|O_APPEND,0600);
        if(fd>=0) {
            if(count>0 && (size_t)count<sizeof(line)) {
                ssize_t written;
                do written=write(fd,line,(size_t)count);while(written<0 && errno==EINTR);
            }
            close(fd);
        }
    }
    errno=saved_errno;
}

#include <pthread.h>
/* One upload to websrv's payload launcher at a time: starts overlap now (the title makes the next helper while one
 * starts, src/pw_warm_helper.h), and elfldr has only ever been sent one request at a time from here. */
static pthread_mutex_t payload_upload_lock = PTHREAD_MUTEX_INITIALIZER;
/* When a start's steps ended, as monotonic ms (the clock the helper's "wine-child: start" lines and Wine's +timestamp use);
 * spawn_native prints them as one "wine-ps5: spawn" line for tools/start_stages.py. 0: the step was not reached. */
struct payload_spawn_times { long long read, answered, connected, held; unsigned attempts; };
static long long payload_ms(void)
{
    struct timespec now = {0};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return now.tv_sec * 1000ll + now.tv_nsec / 1000000;
}

/* The resident loader performs the privileged spawn. This title uses sockets
 * only, then passes its already-tested packet control endpoint via SCM_RIGHTS
 * (pw_payload_rendezvous.h: the loader acknowledges it before the caller lets go). */
static int payload_write(int fd,const void *data,size_t bytes)
{
    const char *p=data;
    while(bytes) { ssize_t n=send(fd,p,bytes,MSG_NOSIGNAL);if(n<0 && errno==EINTR)continue;if(n<=0)return -1;p+=n;bytes-=n; }
    return 0;
}
static int payload_spawn(int control,int *native_pid,struct payload_spawn_times *times)
{
    struct payload_spawn_times unused;
    if(!times)times=&unused;
    memset(times,0,sizeof(*times));
    static unsigned long long serial;
    char token[35],path[108];
    snprintf(token,sizeof(token),"pp%016llx%016llx",(unsigned long long)getpid(),__atomic_add_fetch(&serial,1,__ATOMIC_RELAXED));
    snprintf(path,sizeof(path),"/system_tmp/proton-%s.sock",token);
    const char *image=getenv("WINE_PS5_PAYLOAD_IMAGE");
    if(!image)image="/data/ps5proton/run/proton-payload-child.elf";
    FILE *file=fopen(image,"rb");if(!file)return -1;
    if(fseek(file,0,SEEK_END)) { fclose(file);return -1; }
    const long size=ftell(file);rewind(file);
    if(size<=0 || size>(4<<20)) { fclose(file);errno=EFBIG;return -1; }
    char *elf=malloc((size_t)size);if(!elf) { fclose(file);return -1; }
    const int read_ok=fread(elf,1,(size_t)size,file)==(size_t)size;fclose(file);
    if(!read_ok) { free(elf);errno=EIO;return -1; }
    times->read=payload_ms();
    PwElfUpload request;
    if(pw_elfldr_request(&request,"child.elf",token,elf,(size_t)size)) { free(elf);errno=EINVAL;return -1; }
    int web=socket(AF_INET,SOCK_STREAM,0),result=-1;
    if(web<0) { free(elf);return -1; }
    struct timeval timeout={15,0};
    setsockopt(web,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout));setsockopt(web,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
    struct sockaddr_in address={0};address.sin_family=AF_INET;address.sin_port=htons(8080);address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    struct timespec asked={0},answered={0};
    /* What failed, when something did: the stage and the errno of the call that failed there (0 when the loader
     * answered, or closed, without a call failing). */
    char reply[128]={0};const char *stage="connect";int web_errno=0;
    pthread_mutex_lock(&payload_upload_lock);
    clock_gettime(CLOCK_MONOTONIC,&asked);
    if(connect(web,(struct sockaddr *)&address,sizeof(address))) {
        web_errno=errno;
    } else {
        stage="upload";
        if(payload_write(web,request.head,request.head_size) ||
           payload_write(web,elf,(size_t)size) || payload_write(web,request.tail,request.tail_size)) {
            web_errno=errno;
        } else {
            size_t used=0;
            stage="answer";
            while(used<sizeof(reply)-1 && !strstr(reply,"\r\n")) {
                ssize_t n=recv(web,reply+used,sizeof(reply)-1-used,0);
                if(n<0 && errno==EINTR)continue;
                if(n<0)web_errno=errno;
                if(n<=0)break;
                used+=(size_t)n;
            }
            if(strstr(reply," 200 ")) { result=0;times->answered=payload_ms(); }
        }
    }
    close(web);free(elf);
    pthread_mutex_unlock(&payload_upload_lock);
    if(result) {
        /* The loader may have made the process before it failed: in step3-gate-2026-10-10-01-gpu-window64 two such
         * processes stayed stopped and the console needed a restart. Say what it answered, and the token it was given,
         * which is the process's argument. */
        clock_gettime(CLOCK_MONOTONIC,&answered);
        char *eol=strpbrk(reply,"\r\n");if(eol)*eol=0;
        fprintf(stderr,"wine-ps5: payload loader failed at %s token=%s after %lld ms: \"%s\" errno=%d\n",stage,
                token,(answered.tv_sec-asked.tv_sec)*1000ll+(answered.tv_nsec-asked.tv_nsec)/1000000,reply,web_errno);
        errno=EIO;return -1;
    }
    struct sockaddr_un channel={0};channel.sun_family=AF_UNIX;strcpy(channel.sun_path,path);
    int connection=-1;
    /* The loader listens a moment after elfldr answers. A refused connect costs microseconds, so the pause starts at
     * 1 ms and stops growing at 8: a fixed 100 ms one kept every start waiting up to 100 ms for nothing (installers
     * start a helper per custom action, docs/PLAN.md). The same 20 s limit as before. */
    const long long deadline=payload_ms()+20000;
    for(unsigned n=0,pause_us=1000;;++n) {
        connection=socket(AF_UNIX,SOCK_STREAM,0);if(connection<0)return -1;
        times->attempts=n+1;
        if(!connect(connection,(struct sockaddr *)&channel,sizeof(channel))) { times->connected=payload_ms();break; }
        close(connection);connection=-1;
        if(payload_ms()>=deadline)break;
        usleep(pause_us);
        if(pause_us<8000)pause_us*=2;
    }
    if(connection<0) { errno=ETIMEDOUT;return -1; }
    /* Returns once the loader holds control: only then may the caller close its copy (pw_payload_rendezvous.h). */
    int native=0;
    result=pw_payload_give(connection,control,&native);
    const int saved=errno;
    if(!result)times->held=payload_ms();
    if(native>0) {
        *native_pid=native;
        fprintf(stderr,"wine-ps5: payload rendezvous native=%d held=%d errno=%d\n",native,!result,result ? saved : 0);
    }
    close(connection);errno=saved;return result;
}
/* ESRCH is positive proof of absence; EPERM or any other error is not. */
static int payload_absent(int pid)
{
    errno=0;return kill(pid,0)<0 && errno==ESRCH;
}
/* Consumes control on every outcome. Close before waiting: shutdown alone can
 * leave queued control packets readable without peer HUP on the console.
 * In terminal-focus08 both shell helpers observed HUP only when the owner's
 * ten-second absence wait expired and its caller finally closed the socket.
 * Releasing this endpoint is the shutdown request; ESRCH remains the proof. */
static int payload_close(int pid,int control)
{
    struct timespec begin={0},end={0};
    int last_error=0;
    clock_gettime(CLOCK_MONOTONIC,&begin);
    payload_trace("shutdown-begin",pid,control,0);
    int result=shutdown(control,SHUT_RDWR);
    payload_trace("shutdown-end",pid,control,result ? errno : 0);
    result=close(control);
    payload_trace("endpoint-released",pid,control,result ? errno : 0);
    for(unsigned n=0;n<100;++n) {
        if(payload_absent(pid)) {
            payload_trace("native-absent",pid,control,0);
            clock_gettime(CLOCK_MONOTONIC,&end);
            fprintf(stderr,"wine-ps5: payload close native=%d absent=1 polls=%u begin_ms=%lld end_ms=%lld\n",
                    pid,n+1,begin.tv_sec*1000ll+begin.tv_nsec/1000000,end.tv_sec*1000ll+end.tv_nsec/1000000);
            return 0;
        }
        last_error=errno;
        usleep(100000);
    }
    clock_gettime(CLOCK_MONOTONIC,&end);
    payload_trace("absence-timeout",pid,control,last_error);
    fprintf(stderr,"wine-ps5: payload close native=%d absent=0 polls=100 errno=%d begin_ms=%lld end_ms=%lld\n",
            pid,last_error,begin.tv_sec*1000ll+begin.tv_nsec/1000000,end.tv_sec*1000ll+end.tv_nsec/1000000);
    errno=ETIMEDOUT;return -1;
}
#endif
