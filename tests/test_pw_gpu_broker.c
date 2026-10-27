/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "../src/pw_gpu_broker.h"
#include <assert.h>
#include <stdio.h>
int main(void)
{
    _Static_assert(sizeof(PwGpuMessage)==64,"bounded RPC frame");
    assert(pw_gpu_contains(0x200000000,16384,0x200000000,16384));
    assert(pw_gpu_contains(0x200000000,16384,0x200003ffc,4));
    assert(!pw_gpu_contains(0x200000000,16384,0x200003ffc,8));
    assert(!pw_gpu_contains(0x200000000,16384,0x1ffffffff,4));
    assert(!pw_gpu_contains(0x200000000,16384,0x200004000,1));
    assert(!pw_gpu_contains(0x200000000,16384,0x200000000,UINT64_MAX));
    assert(!pw_gpu_contains(UINT64_MAX-7,16,UINT64_MAX-7,4));
    assert(!pw_gpu_contains(0,0,0,1));
    assert(!pw_gpu_contains(0,16384,0,0));
 assert(pw_gpu_client_pid(0,42,1));
 assert(pw_gpu_client_pid(42,42,1));
 assert(!pw_gpu_client_pid(42,43,1));
 /* The owner's own pid: only in the slot the owner opened for itself (graphics patch 0031). */
 assert(pw_gpu_client_pid(42,42,42) && !pw_gpu_client_pid(0,42,42) && !pw_gpu_client_pid(43,42,42));
 assert(!pw_gpu_client_pid(0,0,1));
 assert(!pw_gpu_client_pid(-1,42,1));
 assert(!pw_gpu_client_pid(0,UINT64_MAX,1));
 PwGpuMessage m={.address=0x240000000ull,.bytes=640*360*4,
 .argument={640ull|(360ull<<32),2560ull|(0xffee1234ull<<32),0x80000000ffffffffull,1}};
 assert(pw_gpu_window_image(&m));
 assert((uint32_t)(m.argument[1]>>32)==0xffee1234u && m.argument[2]==0x80000000ffffffffull);
 m.argument[1]++;assert(!pw_gpu_window_image(&m));m.argument[1]--;
 m.bytes++;assert(!pw_gpu_window_image(&m));m.bytes--;
 m.address++;assert(!pw_gpu_window_image(&m));m.address--;
 m.address=0;assert(!pw_gpu_window_image(&m));m.address=0x240000000ull;
 m.argument[0]=640ull|(0x10000000ull<<32);assert(!pw_gpu_window_image(&m));
 m.argument[0]=640ull|(360ull<<32);m.argument[2]=0;assert(!pw_gpu_window_image(&m));
 m.argument[2]=1;m.argument[1]=2560;assert(!pw_gpu_window_image(&m));
 m.argument[1]=2560ull|(2ull<<32);m.argument[3]=2;assert(!pw_gpu_window_image(&m));

    puts("GPU broker bounds and owner PID/window packet contracts passed");
}
