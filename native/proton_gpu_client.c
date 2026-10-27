/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Wine environment binding; the Vulkan client implementation is PS5_Vulkan's. */
#include "../.deps/graphics-foundation/tooling/gpu-broker/client.h"
#include <stdlib.h>
#include <string.h>
int proton_gpu_client_bind(int fd)
{
    const char *reserved=getenv("WINE_PS5_GPU_RESERVED"),*placed=getenv("WINE_PS5_GPU_PLACED");
    /* A 32-bit program maps its GPU memory again below 4 GiB (VK_EXT_map_memory_placed): ntdll sets this in a WoW64
     * process (docs/IA32_DESIGN.md, M4 step 3), and the client then keeps what a placed mapping needs. */
    if(placed && !strcmp(placed,"1"))ps5_gpu_client_keep_sources();
    /* A Windows program run inside the title, whose broker maps the buffers in this same process (docs/TITLE_CLIENT.md,
     * graphics patch 0031). */
    const char *in_owner=getenv("PROTON_GPU_IN_OWNER");
    if(in_owner && !strcmp(in_owner,"1"))ps5_gpu_client_in_owner();
    return ps5_gpu_client_bind(fd,reserved && !strcmp(reserved,"1"));
}
