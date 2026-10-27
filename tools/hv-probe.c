/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * What the kernel says about memory, what core 0's VMCB says about the
 * hypervisor, and what the nested page table maps (docs/HYPERVISOR_PLAN.md,
 * phase 2). A read-only payload sent through websrv's launcher
 * (POST /elfldr, pipe=1), whose output comes back to the sender.
 *
 * It needs the kernel exploit backend (kstuff) and Byepervisor Reborn to have
 * run: NESTED_CTRL's bit 0 is cleared when the exploit disables nested paging.
 * Nothing here writes; no fault changes state.
 *
 * Offsets are the 13.60 map of PS5Dev/Byepervisor-Reborn (GPLv3):
 * include/offsets/13_60.h, and the VMCB walk is break_hv_payload/break_hv.c
 * (pcpu -> m_vmcb at +0xa8 -> subtract the HV heap base). The S3 writeup
 * (soniciso1/ps5-hv-writeups) documents the fields: NESTED_CTRL +0x90,
 * NESTED_CR3 +0xb0, Control2 +0xb8. The nested page table is x86-64 4-level:
 * PML4 index bits 39-47, PDPT 30-38 (1 GiB leaf, PS bit 7), PD 21-29
 * (2 MiB leaf), PT 12-20 (4 KiB leaf).
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/sysctl.h>

#include <ps5/kernel.h>

/* sys/vmmeter.h pulls kernel-only types; the layout is stable (vm/vmtotal). */
struct pw_vmtotal {
    int16_t t_rq, t_dw, t_pw, t_sl, t_sw;
    int32_t t_vm, t_avm, t_rm, t_arm, t_vmshr, t_avmshr, t_rmshr, t_armshr, t_free;
};
#define PW_VM_TOTAL 1

#define O_DMPML4I         0x3B49294
#define O_DMPDPI          0x3B49298
#define O_HV_HEAP         0x83028
#define O_HV_PCPUS        0x82A60

#define HV_BASE_REGULAR   0x062800000ULL

#define VMCB_NESTED_CTRL  0x90
#define VMCB_NESTED_CR3   0xB0
#define VMCB_CONTROL2     0xB8
#define PCPU_M_VMCB       0xA8

#define PML4_MASK         0x000FFFFFFFFFF000ULL
#define PDPT_1G_MASK      0x000FFFFFC0000000ULL
#define PD_2M_MASK        0x000FFFFFFFE00000ULL
#define PT_4K_MASK        0x000FFFFFFFFFF000ULL

#define MAX_WALK_ENTRIES  2000000ULL
#define SAMPLE_ENTRIES    12

static uint64_t g_text_base;
static uint64_t g_dmap;
static uint64_t g_walk_entries;
static uint64_t g_4k, g_2m, g_1g;
static uint64_t g_mapped_bytes;
static uint64_t g_guest_min = UINT64_MAX, g_guest_max;
static uint64_t g_host_min = UINT64_MAX, g_host_max;
static uint64_t g_sampled[SAMPLE_ENTRIES][3];
static unsigned g_sampled_count;

static uint64_t ktext(uint64_t off) { return g_text_base + off; }
static uint64_t kread32(uint64_t addr) { return kernel_getint((intptr_t)addr); }
static uint64_t kread64(uint64_t addr) { return kernel_getlong((intptr_t)addr); }

static void show_num(const char *name, int mib)
{
    uint64_t value = 0;
    size_t size = sizeof value;

    if (sysctlbyname(name, &value, &size, NULL, 0) != 0) {
        uint32_t small = 0;
        int error = errno;
        size = sizeof small;
        if (sysctlbyname(name, &small, &size, NULL, 0) == 0) value = small;
        else {
            printf("%-32s error %d\n", name, error);
            return;
        }
    }
    if (mib) printf("%-32s %llu (%llu MiB)\n", name, (unsigned long long)value, (unsigned long long)(value >> 20));
    else printf("%-32s %llu\n", name, (unsigned long long)value);
}

static void memory_accounting(void)
{
    printf("== kernel memory accounting ==\n");
    show_num("hw.physmem", 1);
    show_num("hw.usermem", 1);
    show_num("hw.realmem", 1);
    show_num("vm.kmem_size", 1);
    show_num("vm.swap_total", 1);
    show_num("vm.swap_reserved", 1);
    show_num("vm.overcommit", 1);
    show_num("vm.stats.vm.v_page_count", 0);
    show_num("vm.stats.vm.v_free_count", 0);
    show_num("vm.stats.vm.v_inactive_count", 0);
    show_num("vm.stats.vm.v_wire_count", 0);
    show_num("vm.stats.vm.v_laundry_count", 0);
    show_num("vm.stats.vm.v_cache_count", 0);
}

static void show_mib(int a, int b, const char *label)
{
    uint64_t value = 0;
    size_t size = sizeof value;
    int mib[2] = { a, b };

    if (sysctl(mib, 2, &value, &size, NULL, 0) == 0) {
        printf("%-32s %llu (%llu MiB)\n", label, (unsigned long long)value, (unsigned long long)(value >> 20));
        return;
    }
    {
        uint32_t small = 0;
        int error = errno;
        size = sizeof small;
        if (sysctl(mib, 2, &small, &size, NULL, 0) == 0)
            printf("%-32s %u (%u MiB)\n", label, small, small >> 20);
        else
            printf("%-32s error %d\n", label, error);
    }
}

static void vm_meter(void)
{
    struct pw_vmtotal total;
    size_t size = sizeof total;
    int error = sysctlbyname("vm.vmtotal", &total, &size, NULL, 0);

    printf("== vm.vmtotal ==\n");
    if (error) {
        int mib[2] = { CTL_VM, PW_VM_TOTAL };
        size = sizeof total;
        error = sysctl(mib, 2, &total, &size, NULL, 0);
    }
    if (error) {
        printf("vm.vmtotal: error %d (size %zu)\n", errno, size);
        return;
    }
    printf("t_vm %d t_avm %d t_rm %d t_arm %d t_free %d (pages)\n",
           total.t_vm, total.t_avm, total.t_rm, total.t_arm, total.t_free);
    printf("resident %llu MiB, free %llu MiB (16 KiB pages)\n",
           (unsigned long long)total.t_rm * 16384 >> 20,
           (unsigned long long)total.t_free * 16384 >> 20);
}

static void note(uint64_t guest, uint64_t host, uint64_t size)
{
    g_mapped_bytes += size;
    if (guest < g_guest_min) g_guest_min = guest;
    if (guest + size > g_guest_max) g_guest_max = guest + size;
    if (host < g_host_min) g_host_min = host;
    if (host + size > g_host_max) g_host_max = host + size;
    if (g_sampled_count < SAMPLE_ENTRIES) {
        g_sampled[g_sampled_count][0] = guest;
        g_sampled[g_sampled_count][1] = host;
        g_sampled[g_sampled_count][2] = size;
        g_sampled_count++;
    }
}

static void walk_npt(uint64_t table, uint64_t base, int level)
{
    for (unsigned i = 0; i < 512; i++) {
        if (g_walk_entries++ >= MAX_WALK_ENTRIES) return;
        const uint64_t entry = kread64(g_dmap + table + (uint64_t)i * 8);
        if (!(entry & 1)) continue;

        const uint64_t guest = base + ((uint64_t)i << (12 + 9 * (level - 1)));
        if (level == 3 && (entry & 0x80)) {
            note(guest, entry & PDPT_1G_MASK, 1ULL << 30);
            g_1g++;
        } else if (level == 2 && (entry & 0x80)) {
            note(guest, entry & PD_2M_MASK, 1ULL << 21);
            g_2m++;
        } else if (level == 1) {
            note(guest, entry & PT_4K_MASK, 1ULL << 12);
            g_4k++;
        } else if (level > 1) {
            walk_npt(entry & PML4_MASK, guest, level - 1);
        }
    }
}

static void report_npt(uint64_t nested_cr3, uint64_t vmcb)
{
    const uint64_t root = nested_cr3 & PML4_MASK;

    printf("== nested page table ==\n");
    printf("NESTED_CR3 root PA           0x%llx\n", (unsigned long long)root);
    if (!root || root == vmcb) {
        printf("NPT: no usable root\n");
        return;
    }

    walk_npt(root, 0, 4);

    printf("NPT entries walked           %llu%s\n", (unsigned long long)g_walk_entries,
           g_walk_entries >= MAX_WALK_ENTRIES ? " (capped)" : "");
    printf("NPT leaves                   1G=%llu 2M=%llu 4K=%llu\n", (unsigned long long)g_1g,
           (unsigned long long)g_2m, (unsigned long long)g_4k);
    printf("NPT mapped                   %llu MiB\n", (unsigned long long)(g_mapped_bytes >> 20));
    if (g_mapped_bytes) {
        printf("NPT guest range              0x%llx..0x%llx\n", (unsigned long long)g_guest_min,
               (unsigned long long)g_guest_max);
        printf("NPT host range               0x%llx..0x%llx\n", (unsigned long long)g_host_min,
               (unsigned long long)g_host_max);
    }
    for (unsigned i = 0; i < g_sampled_count; i++)
        printf("  guest 0x%llx -> host 0x%llx (%llu KiB)\n", (unsigned long long)g_sampled[i][0],
               (unsigned long long)g_sampled[i][1], (unsigned long long)(g_sampled[i][2] >> 10));
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    /* The pinned SDK's table only knows up to 10.x and reports its own target
     * version, not this console's. The kernel data base is the real one (the
     * payload args carry it), and the 13.60 case in the current SDK source is
     * data base - 0x0CC0000 (PS5_PayloadSDK/crt/kernel.c, case 0x13600000). */
    g_text_base = (uint64_t)KERNEL_ADDRESS_DATA_BASE - 0x0CC0000ULL;
    printf("sdk firmware                 %x.%x (payload target)\n",
           (kernel_get_fw_version() >> 24) & 0xFF, (kernel_get_fw_version() >> 16) & 0xFF);
    printf("kernel data base             0x%llx\n", (unsigned long long)KERNEL_ADDRESS_DATA_BASE);
    printf("kernel .text base (13.60)    0x%llx\n", (unsigned long long)g_text_base);
    printf("kernel text first 8 bytes    0x%llx\n", (unsigned long long)kread64(g_text_base));

    memory_accounting();
    vm_meter();
    show_mib(CTL_HW, HW_PHYSMEM, "hw.physmem (raw)");
    show_mib(CTL_HW, HW_USERMEM, "hw.usermem (raw)");
    show_mib(CTL_HW, HW_REALMEM, "hw.realmem (raw)");

    printf("== hypervisor (VMCB core 0) ==\n");
    printf("HV loader PA                  0x%llx (regular)\n", (unsigned long long)HV_BASE_REGULAR);

    g_dmap = 0xFFFF800000000000ULL |
        ((uint64_t)kread32(ktext(O_DMPML4I)) << 39) |
        ((uint64_t)kread32(ktext(O_DMPDPI)) << 30);
    printf("dmap base                     0x%llx\n", (unsigned long long)g_dmap);
    if ((g_dmap & 0xFFFF800000000000ULL) == 0) {
        printf("dmap: unreadable\n");
        return 0;
    }

    const uint64_t heap = kread64(g_dmap + HV_BASE_REGULAR + O_HV_HEAP);
    const uint64_t pcpu0 = kread64(g_dmap + HV_BASE_REGULAR + O_HV_PCPUS);
    printf("HV heap base                  0x%llx\n", (unsigned long long)heap);
    printf("pcpu[0]                       0x%llx\n", (unsigned long long)pcpu0);
    if (!heap || !pcpu0) {
        printf("VMCB: pcpu[0] or heap unreadable\n");
        return 0;
    }

    const uint64_t vmcb_va = kread64(g_dmap + (pcpu0 - heap) + PCPU_M_VMCB);
    printf("m_vmcb                        0x%llx\n", (unsigned long long)vmcb_va);
    if (!vmcb_va) {
        printf("VMCB: m_vmcb unreadable\n");
        return 0;
    }

    const uint64_t vmcb_pa = vmcb_va - heap;
    const uint64_t nested_ctrl = kread64(g_dmap + vmcb_pa + VMCB_NESTED_CTRL);
    const uint64_t nested_cr3 = kread64(g_dmap + vmcb_pa + VMCB_NESTED_CR3);
    const uint64_t control2 = kread64(g_dmap + vmcb_pa + VMCB_CONTROL2);

    printf("VMCB page                     0x%llx\n", (unsigned long long)vmcb_pa);
    printf("NESTED_CTRL                   0x%llx%s\n", (unsigned long long)nested_ctrl,
           (nested_ctrl & 1) ? " (nested paging on; exploit not active)" : " (nested paging off; HV compromised)");
    printf("NESTED_CR3                    0x%llx\n", (unsigned long long)nested_cr3);
    printf("Control2                      0x%llx\n", (unsigned long long)control2);

    report_npt(nested_cr3, vmcb_pa);

    return 0;
}
