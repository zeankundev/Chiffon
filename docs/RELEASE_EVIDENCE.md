# Release evidence and recovery

Use this runbook with the [production readiness roadmap](../ROADMAP.md) and [threat model](THREAT_MODEL.md). The release smoke scope is the Demo environment with a trusted ext4 disk image and one vCPU. The two configurations are QEMU PC with ATA on x86_64 and QEMU virt with Cortex-A72, GICv2 and virtio-blk on AArch64. They use 256 MiB RAM and serial I/O. Production support requires the wider evidence listed in the roadmap.

## Generate and report evidence

1. A maintainer can manually dispatch **Release readiness smoke checks** for the candidate ref. The [workflow](../.github/workflows/readiness.yml) checks the v1 ABI fixture with both target compilers, runs the boot memory fixture on the Linux host, builds clean Demo images, and boots each image. It has no publishing permissions. The local contributor build remains the command in [.codex/AGENTS.md](../.codex/AGENTS.md).
2. Download the readiness artifact for each architecture, even on failure. A passing run has `PASS abi`, `PASS bootmem-compile`, `PASS bootmem`, and `PASS build` in `result.txt`, `PASS boot-smoke` in `boot/result.txt`, and `PASS vm-probe` in `boot/qemu.log`. Read `abi.log`, `bootmem-compile.log`, `bootmem.log`, `build.log`, `dependencies.log`, or `boot/qemu.log` for the first failing stage. A skipped step or missing result is incomplete evidence. ABI and build checks each have a five-minute deadline. The smoke check gives QEMU 60 seconds for the expected boot banner, init prompt, `uname`, reading `/etc/chiffon.ini`, running the VM probe, and power-off with a clean QEMU exit.
3. Record the candidate commit, release tag, ABI baseline, architecture, board, environment, QEMU version, full job URL, and result for each target. Preserve the artifact and record the image hashes from `boot/images.sha256`. Include the first failing stage and any recovery steps when filing a report. Mark runtime behavior verified only after examining both sets of results.

To repeat the smoke check with Demo assets that include `/bin/vm-probe`, a human operator can run the following on a Linux host with Python 3.11+ and the matching QEMU system emulators. Use a new evidence directory for each attempt.

```sh
python3 tools/release/boot_smoke.py x86_64 chiffon_x86_64.bin \
  chiffon-demo-x86_64.img build/evidence/amd64-repeat
python3 tools/release/boot_smoke.py aarch64 chiffon_aarch64.bin \
  chiffon-demo-aarch64.img build/evidence/arm64-repeat
```

The runner uses a temporary QEMU snapshot and writes the serial transcript and image SHA-256 digests to the evidence directory. If a timeout occurs, QEMU is stopped and the partial transcript is retained.

The boot memory fixture has 15 seconds to check range selection, clipping diagnostics, overlapping reservations, page alignment, and range capacity with AddressSanitizer and UndefinedBehaviorSanitizer enabled. It exercises the shared range code with synthetic maps. Firmware parsing and guest allocation require separate evidence. [Boot memory fixture](../tools/release/bootmem_test.c)

To run the VM probe directly, boot a newly built Demo or local milkos candidate in QEMU and enter `/bin/vm-probe` at the guest shell. Expect one `PASS vm-probe` line and a return to the prompt. A `FAIL vm-probe` line identifies the failing stage. The probe checks anonymous mapping, protection changes, write-and-execute denial from both syscalls, copying and modifying private data after fork, the child exit status, and unmapping. Its coverage is limited to data reads and writes and syscall results. Capture the serial output with the candidate commit and architecture.

## Human milkOS guest check, 2026-11-27

The maintainer confirmed that `make ENV=milkos BUSYBOX=$HOME/busybox/busybox` passed in their WSL instance using the working tree based on `2fadc20`, including its uncommitted integration changes. They booted the resulting x86_64 milkOS image with `run-gfx` and supplied a guest-console screenshot showing the following results with a 1024×768, 32-bit framebuffer.

| Guest action | Observed result |
| --- | --- |
| `vm-probe` | Printed `PASS vm-probe` and returned to the shell. |
| `drvloader status` | Loader ready, framebuffer on port 2, console on port 1. |
| `drvloader start fb` | Framebuffer server ready with PID 11 on port 34. |
| `drvloader test-crash` | The framebuffer replied before termination. The loader reported `-EPIPE`, the test printed its pass message, and the shell accepted another command. |
| `drvloader start fb` | Replacement server ready with PID 14 on port 66. |

## Version release inputs

- Treat [abi_v1.c](../tools/release/abi_v1.c) as the baseline for the checked syscall numbers, constants, structure sizes and offsets in the published [UAPI header](../include/uapi/chiffon_abi.h). Attach any deliberate incompatible change to a new baseline version and document the affected applications and migration. Keep older baselines available for review. The baseline checks declaration compatibility and the smoke test separately exercises a small part of the syscall dispatch.
- For every candidate, retain the source commit and tag, versioned environment configuration, compiler and binutils versions, emulator version, and tool package versions. Retain the exact BusyBox source revision, configuration and binary digest for any BusyBox-based asset. The smoke workflow records resolved packages. Reproducing official releases still requires pinning the release dependency versions, including BusyBox, and validating the resulting images.
- Prepare release notes with changes, known issues, tested configurations and supported limits. Verify downloaded images against the published `SHA256SUMS` obtained through a trusted channel. Retain a trusted previous release with its checksums. Release signing and rollback protection are open gates.

## Update and recover

1. Shut down the existing guest cleanly. Save its kernel, disk image and configuration together and record the checksums. Preserve the entire disk image because Chiffon stores the persistence log at its end.
2. Use a separate copy for the candidate. Rebuilding or replacing the image can discard the persistence log. Keep the old image and the candidate under distinct paths and boot the candidate with the same known-good board configuration. Run the smoke check on the matching Demo assets and independently verify any data migration on a copy.
3. If boot or data verification fails, stop the candidate, preserve its disk and serial log for diagnosis, and start the previous kernel with its matching previous disk and configuration. Check expected files and contents before using the restored system. Preserve the failed disk separately if any changes need manual recovery. Changes written only to the candidate are absent from the restored snapshot.

## Evidence still required

The workflow covers a versioned subset of ABI declarations and normal Demo boot behavior. Track kernel-wide warnings-as-errors, static analysis, fuzzing, in-kernel self-tests, hardware testing, SMP stress, storage durability, independent review, deployment diagnostics, authenticated upgrades and recovery in the [roadmap](../ROADMAP.md). The publishing workflow remains independent of this smoke workflow. The release decision requires review of candidate evidence before publication.
