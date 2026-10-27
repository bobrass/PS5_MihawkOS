/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PW_PRX_ELF_PS5_H
#define PW_PRX_ELF_PS5_H
#include "../src/pw_prx_elf.h"
#include "../wine/ps5/pw_wine_dl.h"

/* The payload helper's mapping of one staged "<name>.prx.elf" module: reserve an image-sized range, map it
 * read-write, let the portable core copy/relocate, then turn text into JIT memory and data read-only. Returns
 * PW_PRX_ELF_OK or a negative status; error gets one line. The caller calls image->module_start after this
 * returns and registers the descriptor with ntdll. Modules loaded earlier resolve their Wine imports. */
int pw_prx_elf_ps5_load(const char *path, PwPrxElfImage *image, char *error, size_t error_size);
/* PwWineDlOps: every sibling module the Windows side opens loads through the user-space loader too. load_start
 * returns a positive handle for module_info, or -1; module_info fills ntdll's SceKernelModuleInfo record. */
int32_t pw_prx_elf_ps5_load_start(const char *path, size_t argc, const void *argv, uint32_t flags,
                                  const void *option, int *result);
int pw_prx_elf_ps5_module_info(int32_t handle, void *info);
int pw_prx_elf_ps5_last_handle(void);
#endif
