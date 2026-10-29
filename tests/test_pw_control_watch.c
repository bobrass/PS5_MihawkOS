/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "../src/pw_control_watch.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static long long milliseconds(void)
{
    struct timespec now;assert(!clock_gettime(CLOCK_MONOTONIC,&now));
    return now.tv_sec*1000ll+now.tv_nsec/1000000;
}
/* The backstop's rule: per-reason grace, one "seen" each, the earliest deadline wins and a later reason never moves it. */
static void check_doom(void)
{
    PwControlDoom watch = {0};
    int64_t stamps[PW_CHILD_DOOMS] = {0};
    unsigned reason = 0;
    assert(!pw_control_doom_step(&watch, stamps, 1000000, &reason));
    /* GPU at t=1000: seen once, due at +5000 and from then on. */
    stamps[PW_CHILD_DOOM_GPU - 1] = 1000;
    assert(pw_control_doom_step(&watch, stamps, 1000, &reason) == PW_CONTROL_DOOM_SEEN && reason == PW_CHILD_DOOM_GPU);
    assert(!pw_control_doom_step(&watch, stamps, 1000, &reason));
    assert(!pw_control_doom_step(&watch, stamps, 5999, &reason));
    /* EXIT at t=3000 (due 23000) is seen, and the GPU's deadline stands. */
    stamps[PW_CHILD_DOOM_EXIT - 1] = 3000;
    assert(pw_control_doom_step(&watch, stamps, 3000, &reason) == PW_CONTROL_DOOM_SEEN && reason == PW_CHILD_DOOM_EXIT);
    assert(watch.due_ms == 6000 && watch.due_reason == PW_CHILD_DOOM_GPU);
    assert(pw_control_doom_step(&watch, stamps, 6000, &reason) == PW_CONTROL_DOOM_DUE && reason == PW_CHILD_DOOM_GPU);
    assert(pw_control_doom_step(&watch, stamps, 9000, &reason) == PW_CONTROL_DOOM_DUE && reason == PW_CHILD_DOOM_GPU);

    /* EXIT alone: 20 s; a TEST stamped later with an earlier deadline wins; two new reasons come one per call. */
    PwControlDoom exit_watch = {0};
    int64_t later[PW_CHILD_DOOMS] = {0};
    later[PW_CHILD_DOOM_EXIT - 1] = 100;
    assert(pw_control_doom_step(&exit_watch, later, 100, &reason) == PW_CONTROL_DOOM_SEEN && reason == PW_CHILD_DOOM_EXIT);
    assert(!pw_control_doom_step(&exit_watch, later, 20099, &reason));
    later[PW_CHILD_DOOM_ENGINE - 1] = later[PW_CHILD_DOOM_TEST - 1] = 10000;
    assert(pw_control_doom_step(&exit_watch, later, 10000, &reason) == PW_CONTROL_DOOM_SEEN && reason == PW_CHILD_DOOM_ENGINE);
    assert(pw_control_doom_step(&exit_watch, later, 10000, &reason) == PW_CONTROL_DOOM_SEEN && reason == PW_CHILD_DOOM_TEST);
    assert(!pw_control_doom_step(&exit_watch, later, 14999, &reason));
    assert(pw_control_doom_step(&exit_watch, later, 15000, &reason) == PW_CONTROL_DOOM_DUE &&
           reason == PW_CHILD_DOOM_ENGINE);
    /* A stamp that is already overdue at first sight: seen, then due, in the same tick. */
    PwControlDoom late_watch = {0};
    int64_t overdue[PW_CHILD_DOOMS] = {0};
    overdue[PW_CHILD_DOOM_TEST - 1] = 1;
    assert(pw_control_doom_step(&late_watch, overdue, 60000, &reason) == PW_CONTROL_DOOM_SEEN);
    assert(pw_control_doom_step(&late_watch, overdue, 60000, &reason) == PW_CONTROL_DOOM_DUE && reason == PW_CHILD_DOOM_TEST);
    /* STALL after EXIT: its 5 s come first and end the helper; seen after it, and once. */
    PwControlDoom stall_watch = {0};
    int64_t stalled[PW_CHILD_DOOMS] = {0};
    stalled[PW_CHILD_DOOM_EXIT - 1] = 1000;
    assert(pw_control_doom_step(&stall_watch, stalled, 1000, &reason) == PW_CONTROL_DOOM_SEEN && reason == PW_CHILD_DOOM_EXIT);
    stalled[PW_CHILD_DOOM_STALL - 1] = 4000;
    assert(pw_control_doom_step(&stall_watch, stalled, 4000, &reason) == PW_CONTROL_DOOM_SEEN && reason == PW_CHILD_DOOM_STALL);
    assert(!pw_control_doom_step(&stall_watch, stalled, 8999, &reason));
    assert(pw_control_doom_step(&stall_watch, stalled, 9000, &reason) == PW_CONTROL_DOOM_DUE &&
           reason == PW_CHILD_DOOM_STALL);
    /* A STALL stamped after an earlier GPU deadline leaves it standing. */
    PwControlDoom gpu_first = {0};
    int64_t both[PW_CHILD_DOOMS] = {0};
    both[PW_CHILD_DOOM_GPU - 1] = 1000;
    both[PW_CHILD_DOOM_STALL - 1] = 3000;
    assert(pw_control_doom_step(&gpu_first, both, 3000, &reason) == PW_CONTROL_DOOM_SEEN && reason == PW_CHILD_DOOM_GPU);
    assert(pw_control_doom_step(&gpu_first, both, 3000, &reason) == PW_CONTROL_DOOM_SEEN && reason == PW_CHILD_DOOM_STALL);
    assert(gpu_first.due_ms == 6000 && gpu_first.due_reason == PW_CHILD_DOOM_GPU);
}
/* The stall watch's rule: either lock held 3 s in one hold, by an owner; the owner whose hold reached it. */
static void check_stalled(void)
{
    assert(pw_control_stalled(101428, 3000000, NULL, 0, 0, 0) == 101428);
    assert(pw_control_stalled(0, 0, NULL, 0, 101623, 3000000) == 101623);
    assert(pw_control_stalled(101428, 3500000, "fault", 0, 101428, 3400000) == 101428);
    assert(!pw_control_stalled(101428, 2999999, NULL, 0, 101428, 2999999));
    assert(!pw_control_stalled(0, 60000000, NULL, 0, 0, 60000000));   /* no owner, no hold */
    assert(!pw_control_stalled(102422, 157000, NULL, 0, 0, 0));       /* a 2026-10-16 run's longest clean hold */
    assert(pw_control_stalled(102422, 157000, NULL, 0, 101428, 56216000) == 101428);
    /* memory-stall: the title answers the pager's ask after the memory helper's 5 s; a bounded wait, no STALL. */
    assert(!pw_control_stalled(101500, 5004000, PW_CHILD_PAGER_ASK, 0, 0, 0));
    assert(pw_control_stalled(101500, 5004000, "pager-map", 0, 0, 0) == 101500);
    /* After EXIT a hold the exit cut short stays recorded: only the engine lock still counts. */
    assert(!pw_control_stalled(101500, 60000000, NULL, 1000, 0, 0));
    assert(pw_control_stalled(101500, 60000000, NULL, 1000, 101623, 3000000) == 101623);
}
/* ntdll's record of them (pw_ps5_child_doomed, pw_ps5_child_doom): reasons outside 1..5 ignored, the first stamp kept,
 * a stamp at 0 kept as 1, each read back in its place, and no more than asked or than there are. */
static void check_doom_record(void)
{
    int64_t record[PW_CHILD_DOOMS + 1] = {0}, read[PW_CHILD_DOOMS + 2];
    assert(!pw_child_doom_stamp(record, 0, 100) && !pw_child_doom_stamp(record, PW_CHILD_DOOMS + 1, 100));
    assert(!record[0] && !record[PW_CHILD_DOOMS]);
    assert(pw_child_doom_stamp(record, PW_CHILD_DOOM_GPU, 1000) && !pw_child_doom_stamp(record, PW_CHILD_DOOM_GPU, 2000));
    assert(pw_child_doom_stamp(record, PW_CHILD_DOOM_EXIT, 0) && record[PW_CHILD_DOOM_EXIT - 1] == 1);
    assert(pw_child_doom_stamp(record, PW_CHILD_DOOM_ENGINE, 3000) && pw_child_doom_stamp(record, PW_CHILD_DOOM_TEST, 4000));
    assert(pw_child_doom_stamp(record, PW_CHILD_DOOM_STALL, 5000) && !pw_child_doom_stamp(record, PW_CHILD_DOOM_STALL, 6000));
    for (unsigned i = 0; i < PW_CHILD_DOOMS + 2; ++i) read[i] = -1;
    assert(pw_child_doom_read(record, read, 2) == PW_CHILD_DOOMS && read[0] == 1000 && read[1] == 1 && read[2] == -1);
    assert(pw_child_doom_read(record, read, PW_CHILD_DOOMS + 2) == PW_CHILD_DOOMS && read[2] == 3000 && read[3] == 4000 &&
           read[4] == 5000 && read[PW_CHILD_DOOMS] == -1 && read[PW_CHILD_DOOMS + 1] == -1);
}

/* The title's pid in the bootstrap's block: found among others, 0 when absent, empty, signed or not a number. */
static void check_title_pid(void)
{
    static const char block[] = "WINE_PS5_DESKTOP=1\0WINE_PS5_TITLE_PID=1055\0DXVK_HUD=fps\0";
    assert(pw_control_title_pid(block, sizeof(block)) == 1055);
    static const char none[] = "WINE_PS5_DESKTOP=1\0\0WINE_PS5_TITLE_PID=7\0";   /* after the list's end */
    assert(pw_control_title_pid(none, sizeof(none)) == 0);
    static const char bad[] = "WINE_PS5_TITLE_PID=-5\0";
    assert(pw_control_title_pid(bad, sizeof(bad)) == 0);
    static const char empty[] = "WINE_PS5_TITLE_PID=\0";
    assert(pw_control_title_pid(empty, sizeof(empty)) == 0);
    static const char other[] = "WINE_PS5_TITLE_PIDX=5\0WINE_PS5_TITLE_PID=12a\0";
    assert(pw_control_title_pid(other, sizeof(other)) == 0);
    char unended[24];
    memcpy(unended, "WINE_PS5_TITLE_PID=42", 21); memset(unended + 21, 'x', 3);   /* no zero within size */
    assert(pw_control_title_pid(unended, sizeof(unended)) == 0);
    assert(pw_control_title_pid(unended, 21) == 42);
}
int main(void)
{
    check_doom();
    check_title_pid();
    check_doom_record();
    check_stalled();
    int pair[2];short events;char packet=0;
    assert(!socketpair(AF_UNIX,SOCK_SEQPACKET,0,pair));
    assert(send(pair[0],"s",1,0)==1);
    /* Reproduce the old observer's immediate wake without consuming anything. */
    struct pollfd old={pair[1],POLLIN,0};
    for(unsigned i=0;i<100;++i)assert(poll(&old,1,20)==1 && (old.revents&POLLIN));
    long long start=milliseconds();
    for(unsigned i=0;i<5;++i)assert(pw_control_watch(pair[1],20,&events)==0 && !events);
    assert(milliseconds()-start>=80);
    assert(recv(pair[1],&packet,1,MSG_DONTWAIT)==1 && packet=='s');
    assert(send(pair[0],"q",1,0)==1);
    close(pair[0]);
    assert(pw_control_watch(pair[1],100,&events)==1 && (events&POLLHUP));
    assert(recv(pair[1],&packet,1,MSG_DONTWAIT)==1 && packet=='q');
    close(pair[1]);
    assert(pw_control_watch(pair[1],100,&events)==1 && (events&POLLNVAL));
    puts("control watch: PASS (queued packets wait, consumer retains data, buffered hangup, invalid fd, doom grace, doom record, stall rule, title pid)");
    return 0;
}
