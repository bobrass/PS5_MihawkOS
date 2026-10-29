# Lapy filesystem-access service

Reviewed 2026-10-07. This directory carries patches against the MIT-licensed
PS5-Lapy-JB-Daemon pin `5b8397b9f2b5f12a7bc2f9c8745a00d1c2dd01ad`.
Upstream source and SDK exports stay in ignored caches; siblings are read-only.
The platform client asks this service for filesystem access and privileged
ownership needed by the Wine prefixes. No legacy elevator fallback is restored.

## Normal startup

Production ships a pinned resident daemon ELF for title PPSA99170. The title
starts it through local websrv/elfldr only when its request has not been read.
It restarts/retries through its current launch path and verifies elevated access.
Visible `/data` alone is not proof of the required user identity.

The service has no boot hook and does not survive console restart. Let ordinary
title startup manage it. Manual `tools/start-lapy-service.py` needs idle state,
the exact ELF and its no-duplicate/no-launch-this-boot checks.

## Patch roles

| Patch | Purpose |
| --- | --- |
| `stdout-mirror.patch` | Numbered diagnostics mirrored to the loader stream |
| `donor-wait.patch` | Retained intermediate diagnostic; its original signal-fault hypothesis was disproved |
| `donor-release.patch` | Expected teardown of disposable donors without changing root-transfer code |
| `service-lifecycle.patch` | Finite validation waits for target exit and root-counter baseline |
| `deterministic-build.patch` | Repeatable build ID and byte-identical payload builds: a SHA-1 linker build ID, and the build identity keyed by each input's path relative to the source, or its file name outside it (the logger header), never by absolute path. Since 2026-10-16 the resident ELF is the same from any folder with the same clang/LLD; the ELF records their versions, so another version gives another file |
| `resident-log.patch` | Resident diagnostics under `/data/ps5proton/logs/lapy-service.log` |
| `sdk042-ucred-attrs.patch` | Adapt the ucred attrs calls to the v0.42+ SDK API (a 32-byte blob, not a uint64); v0.42 is the oldest release whose crt accepts firmware 13.40, 13.42 and 13.60 |
| `optional-log-channel.patch` | The resident daemon runs without dev.conf, a log server or a logs folder (it used to exit 2 before any work); it makes `/data/ps5proton/logs` and keeps its file log when it can. `tests/test_lapy_patches.py` checks it |

## Build and evidence

```sh
python3 tools/build-lapy-validation.py --sdk-zip /path/to/ps5-payload-sdk-v0.42.zip
python3 tools/build-lapy-validation.py --sdk-zip /path/to/ps5-payload-sdk-v0.42.zip --resident
```

Run from the repository root. Inputs are hash-pinned; production validates the
resident ELF hash/mode/title before packaging. These commands build, not deploy.
The resident pin re-made on 2026-10-29 (`76481d3d...`) adds optional-log-channel.patch; the 2026-10-20 pin moved to payload SDK v0.42,
whose crt accepts firmware 13.40, 13.42 and 13.60 (v0.40 faulted with SIGILL in
`_start` on 13.40); the measurements below are of an earlier v0.40 pin.

Kstuff-only finite validation completed 11 elevations across six runs, reaped
28 donors and restored counters. Automatic startup and subsequent reuse were
measured on 10.01. See [current validation](../../docs/ELEVATION_VALIDATION.md),
[NOTICE.md](../../NOTICE.md) and
[the original payload notes](../../docs/history/2026-10-07/platform/lapy/README.md).
