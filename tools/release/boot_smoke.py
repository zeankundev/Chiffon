"""Human- or CI-invoked serial smoke check for prebuilt Chiffon Demo images."""

import argparse
import hashlib
import os
import shlex
import re
import select
import subprocess
import time
from pathlib import Path


def qemu_command(arch, kernel, disk):
    # Match the QEMU board recipes with one vCPU, no network, and a disk overlay.
    command = [
        f"qemu-system-{arch}", "-m", "256", "-smp", "1", "-kernel", str(kernel),
        "-display", "none", "-serial", "stdio", "-monitor", "none",
        "-nic", "none", "-snapshot",
    ]
    if arch == "aarch64":
        return command + [
            "-M", "virt,gic-version=2", "-cpu", "cortex-a72",
            "-drive", f"file={disk},format=raw,if=none,id=hd0",
            "-device", "virtio-blk-device,drive=hd0",
        ]
    return command + ["-drive", f"file={disk},format=raw,if=ide"]


def smoke(command, arch, log, timeout):
    deadline = time.monotonic() + timeout
    pending = b""
    with subprocess.Popen(
        command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, bufsize=0,
    ) as guest:
        def read_output(stage):
            remaining = deadline - time.monotonic()
            if remaining <= 0 or not select.select([guest.stdout], [], [], remaining)[0]:
                raise RuntimeError(f"timed out during {stage}")
            chunk = os.read(guest.stdout.fileno(), 4096)
            log.write(chunk)
            log.flush()
            return chunk

        def expect(stage, pattern):
            nonlocal pending
            while True:
                match = re.search(pattern, pending)
                if match:
                    result = pending[:match.end()]
                    pending = pending[match.end():]
                    return result
                chunk = read_output(stage)
                if not chunk:
                    raise RuntimeError(f"QEMU exited before {stage}")
                pending = (pending + chunk)[-65536:]

        try:
            banner = re.escape(arch.encode())
            prompt = rb"hello-init# "
            expect("kernel banner", rb"Chiffon [^\r\n]+, " + banner + rb"\)")
            expect("init prompt", prompt)
            guest.stdin.write(b"uname\n")
            output = expect("uname completion", prompt)
            if not re.search(rb"Chiffon demo [^\r\n]+ " + banner + rb"\r?\n", output):
                raise RuntimeError("unexpected uname output")
            guest.stdin.write(b"cat /etc/chiffon.ini\n")
            output = expect("filesystem read completion", prompt)
            if not re.search(rb"hostname = demo\r?\n", output):
                raise RuntimeError("expected Demo filesystem contents")
            guest.stdin.write(b"poweroff\n")
            while read_output("poweroff"):
                pass
            if guest.wait(timeout=max(0, deadline - time.monotonic())) != 0:
                raise RuntimeError(f"QEMU exited with status {guest.returncode}")
        finally:
            if guest.poll() is None:
                guest.kill()
                guest.wait()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("arch", choices=("aarch64", "x86_64"))
    parser.add_argument("kernel", type=Path)
    parser.add_argument("disk", type=Path)
    parser.add_argument("evidence", type=Path)
    parser.add_argument("--timeout", type=int, default=60)
    args = parser.parse_args()
    if args.timeout < 1:
        parser.error("--timeout must be positive")
    args.evidence.mkdir(parents=True, exist_ok=False)
    result = f"PASS boot-smoke {args.arch}"
    exit_code = 0
    with (args.evidence / "qemu.log").open("wb") as log:
        try:
            with (args.evidence / "images.sha256").open("w", encoding="utf-8") as hashes:
                for path in (args.kernel, args.disk):
                    with path.open("rb") as image:
                        digest = hashlib.file_digest(image, "sha256").hexdigest()
                    hashes.write(f"{digest}  {path}\n")
            command = qemu_command(args.arch, args.kernel, args.disk)
            (args.evidence / "command.txt").write_text(shlex.join(command) + "\n", encoding="utf-8")
            smoke(command, args.arch, log, args.timeout)
        except (OSError, RuntimeError, subprocess.TimeoutExpired) as exc:
            result = f"FAIL boot-smoke {args.arch}: {exc}"
            exit_code = 1
    (args.evidence / "result.txt").write_text(result + "\n", encoding="utf-8")
    print(result)
    return exit_code


if __name__ == "__main__":
    raise SystemExit(main())
