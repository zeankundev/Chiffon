"""Verify Chiffon's disk journal across orderly and interrupted QEMU boots."""

import argparse
import os
from pathlib import Path
import re
import select
import shutil
import subprocess
import tempfile
import time

from boot_smoke import qemu_command


def prompt(guest, log, timeout):
    deadline = time.monotonic() + timeout
    output = bytearray()
    while b"hello-init# " not in output:
        remaining = deadline - time.monotonic()
        if remaining <= 0 or not select.select([guest.stdout], [], [], remaining)[0]:
            raise RuntimeError("guest prompt timed out")
        chunk = os.read(guest.stdout.fileno(), 8192)
        if not chunk:
            raise RuntimeError("guest exited before the prompt")
        log.write(chunk)
        log.flush()
        output.extend(chunk)
    return output


def boot(command, steps, log, *, interrupted=False, compacted=False):
    with subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, bufsize=0) as guest:
        try:
            banner = prompt(guest, log, 60)
            if compacted:
                gen = re.search(rb"\[persist\] log gen (\d+): (\d+) records replayed", banner)
                if not gen or int(gen[1]) <= 1 or not int(gen[2]):
                    raise RuntimeError("boot did not replay a compacted journal")
            for cmd, marker in steps:
                guest.stdin.write((cmd + "\n").encode())
                result = prompt(guest, log, 240 if cmd == "fsutil fill 3000" else 60)
                if (marker not in result or b"fsutil:" in result.replace(b"fsutil: fill done", b"")
                        or b"command not found" in result or b"compaction failed" in result):
                    raise RuntimeError(f"{cmd} failed: {result[-512:]!r}")
            if interrupted:
                guest.kill()
            else:
                guest.stdin.write(b"poweroff\n")
            guest.wait(timeout=30)
            if not interrupted and guest.returncode:
                raise RuntimeError(f"QEMU exited with status {guest.returncode}")
        finally:
            if guest.poll() is None:
                guest.kill()


def run(arch, images, evidence):
    evidence.mkdir(parents=True, exist_ok=False)
    with tempfile.TemporaryDirectory() as scratch:
        disk = Path(scratch) / "working.img"
        shutil.copyfile(images / "disk.img", disk)
        command = qemu_command(arch, images / "chiffon.bin", disk)
        command.remove("-snapshot")
        if arch == "aarch64":
            command.extend(["-device", "ramfb"])
        else:
            command.extend(["-vga", "std"])
        with (evidence / "qemu.log").open("wb") as log:
            boot(command, [
                ("fsutil write /tmp/a hello", b""),
                ("drvloader test-crash", b"Framebuffer crash test passed."),
                ("drvloader start fb", b"fbserver ready (PID "),
                ("fsutil fill 3000", b"fsutil: fill done"),
                ("fsutil sync", b""),
            ], log)
            fill = bytes([97 + 2999 % 26]) + bytes(97 + i % 26 for i in range(1, 1024))
            boot(command, [
                ("cat /tmp/a", b"\nhello\r\n"),
                ("cat /tmp/fill", fill),
                ("fsutil mv /tmp/a /tmp/saved", b""),
                ("fsutil rm /tmp/fill", b""),
                ("fsutil sync", b""),
            ], log, compacted=True, interrupted=True)
            boot(command, [
                ("cat /tmp/saved", b"\nhello\r\n"),
                ("cat /tmp/a", b"cat: cannot open /tmp/a"),
                ("cat /tmp/fill", b"cat: cannot open /tmp/fill"),
            ], log, compacted=True)
    result = f"PASS storage-smoke {arch}"
    (evidence / "result.txt").write_text(result + "\n", encoding="utf-8")
    print(result)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("arch", choices=("aarch64", "x86_64"))
    parser.add_argument("images", type=Path)
    parser.add_argument("evidence", type=Path)
    args = parser.parse_args()
    run(args.arch, args.images, args.evidence)
