"""Check the hybrid network path before and after a framebuffer server crash."""
import argparse
import os
from pathlib import Path
import select
import subprocess
import time

from boot_smoke import qemu_command


def run(arch, images):
    command = qemu_command(arch, images / "chiffon.bin", images / "disk.img")
    nic = "virtio-net-device" if arch == "aarch64" else "virtio-net-pci,disable-legacy=on"
    i = command.index("-nic")
    command[i:i + 2] = ["-netdev", "user,id=n0", "-device", nic + ",netdev=n0"]
    guest = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT, bufsize=0)
    try:
        def prompt():
            deadline = time.monotonic() + 60
            output = bytearray()
            while b"hello-init# " not in output:
                if not select.select([guest.stdout], [], [], max(0, deadline - time.monotonic()))[0]:
                    raise RuntimeError(f"prompt timeout: {output[-512:]!r}")
                chunk = os.read(guest.stdout.fileno(), 4096)
                if not chunk:
                    raise RuntimeError(f"early guest exit: {output[-512:]!r}")
                output.extend(chunk)
            return output

        prompt()
        for cmd, marker in [
            ("netinfo", b"gateway  10.0.2.2"),
            ("ping 10.0.2.2", b"4 of 4 replies"),
            ("drvloader test-crash", b"Framebuffer crash test passed."),
            ("drvloader start fb", b"fbserver ready (PID "),
            ("ping 10.0.2.2", b"4 of 4 replies"),
        ]:
            guest.stdin.write((cmd + "\n").encode())
            output = prompt()
            if marker not in output:
                raise RuntimeError(f"{cmd}: {output[-512:]!r}")
        guest.stdin.write(b"poweroff\n")
        if guest.wait(timeout=30):
            raise RuntimeError("QEMU failed during poweroff")
    finally:
        if guest.poll() is None:
            guest.kill()
            guest.wait()
    print(f"PASS network-smoke {arch}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("arch", choices=("aarch64", "x86_64"))
    parser.add_argument("images", type=Path)
    args = parser.parse_args()
    run(args.arch, args.images)
