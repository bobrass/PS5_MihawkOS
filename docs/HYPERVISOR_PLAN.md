# Hypervisor program plan (2026-10-21)

Parent evidence: `docs/HYPERVISOR.md` (console measurements, exploit behavior,
ranked ROI). This plan covers the user-selected items: #1 (kernel/HV R/W API),
#2 (memory + VMCB probe), #3 (memory ceiling), #4 (boot persistence), #5 (QA
flags). Linux is out of scope.

Aegis visibility: the work adds a new payload surface (kernel/HV R/W in our
own payloads) and touches the console's memory boundary. The plan exists so the
probe's evidence, not a guess, decides the memory fix.

## Goal and acceptance

- Goal: a read-only probe (this phase) that prints the kernel's memory
  accounting and core 0's VMCB nested-paging fields, run on the console through
  websrv, with its output recorded.
- Acceptance: the probe runs on the console and prints (a) `hw.physmem`,
  `vm.swap_*` and the `vm.stats.vm.*` page counts, (b) the VMCB `NESTED_CTRL`,
  `NESTED_CR3`, `Control2` for core 0, and (c) the compromised/armed state.
  The numbers are the input to the #3 decision.

## Baseline and authority

- Console: standard PS5; Byepervisor Reborn reports "Hypervisor is already
  compromised", detects 13.60, non-Pro HV loader PA `0x62800000`. The title
  last reported installed 13.40 / reported 13.59; the probe prints the firmware
  it sees so the offset map is explicit.
- Reference: `PS5Dev/Byepervisor-Reborn` (GPLv3, cloned read-only at
  `cleanroom-5c5728e/refs/Byepervisor-Reborn`). Offsets used: 13.60 map
  (`include/offsets/13_60.h`), `HV_SYM_HEAP_BASE`, `HV_SYM_PCPUS`,
  `KERNEL_SYM_DMPML4I/DMPDPI`, DATA_CAVE. The VMCB formula is from the S3
  writeup and `break_hv_payload/break_hv.c`.
- SDK: payloads build with `PS5_PayloadSDK` (GPLv3) / the pinned
  `.deps/native/ps5-payload-sdk`; `<ps5/kernel.h>` provides
  `kernel_getlong`, `kernel_getint`, `kernel_get_fw_version`,
  `KERNEL_ADDRESS_TEXT_BASE`. The kernel exploit backend is loaded on the
  console (kstuff), as the exploit's own run proves.
- Our tool chain for payloads is `tools/console-memory.py` (websrv POST
  `/elfldr`, `pipe=1`).

## Change necessity

The kernel's memory accounting and the VMCB fields are not observable from the
title, klog, or sysctl-only payloads: the title API does not expose the
hypervisor, and a kernel memory ceiling is only visible once read. A new
read-only payload is the minimum way to get those numbers.

## TDD route

TDD Route: off (default). Decision: skipped. Authority: no explicit TDD
request, project default. Verification: the probe's console output is compared
against known facts (14.6-16 GiB physical; VMCB `Control2 == 1`; `NESTED_CTRL`
bit 0 cleared by the exploit = compromised). A host self-check is not possible
for kernel memory; the console output is the check.

## Phases and tasks

1. **Probe (#2)** — files `tools/hv-probe.c`, `tools/hv-probe.py`, plan doc.
   Read-only; prints firmware, kernel base, HV base, dmap, core-0 VMCB
   (`NESTED_CTRL` +0x90, `NESTED_CR3` +0xb0, `Control2` +0xb8), and the sysctl
   memory accounting. Verify: sent via websrv, output captured; VMCB
   `Control2` must read 1 and `NESTED_CTRL` bit 0 must be 0 (exploit active).
   Risk: reads only; no writes. If the payload faults, no state changes.
2. **Kernel/HV R/W API (#1)** — `tools/hvctl.c` + `tools/hvctl.py` (or a
   library) exposing read/write/readback; built on the same primitives. Verify:
   write/readback/restore of an inert HV or kernel word, as the S3 writeup
   describes (`VMCB+0x100`), with the restore verified. Risk: medium; keep the
   restore pattern.
3. **Memory ceiling (#3)** — decided by phase 1: if the limit is a kernel
   policy, patch it; if the NPT mapping, patch `NESTED_CR3`/NPT. Acceptance:
   Steam's login + library under the guard, then guard retired. Risk: high;
   only after 1 and 2.
4. **Boot persistence (#4)** — apply the phase-3 patches on every S3 resume
   through elfldr/pldmgr autoload; no cold-boot persistence. Verify: a fresh
   exploit + resume, then the patch is present before the title starts.
5. **QA flags (#5)** — set `KERNEL_ADDRESS_QA_FLAGS` (and uToken/security if
   needed) through the API; verify the debug menu/settings appear. Low risk.

## Verification and evidence

Each phase writes its console output into `klog/` (ignored) and a small record
in `evidence/`. The probe's numbers go into `docs/HYPERVISOR.md` as the measured
baseline.

## Risks and retirement

- A wrong kernel/HV write can panic the console; phases 2-4 use write/readback/
  restore and never the guard-off path that already panicked once.
- The exploit is per-boot and firmware-specific; the probe prints the firmware
  so a mismatch is visible, not guessed.
- No old path is retired; the probe and API are additive. The title's memory
  guard stays until phase 3 proves the ceiling.
