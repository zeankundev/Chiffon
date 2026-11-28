"""Check driver restart on both targets and USB-origin input on x86_64."""
import argparse
import json
import os
from pathlib import Path
import re
import select
import socket
import subprocess
import tempfile
import time

from boot_smoke import qemu_command


def verify(arch, kernel, disk, log, primary=False):
    with tempfile.TemporaryDirectory() as temp:
        address = str(Path(temp) / "qmp")
        command = qemu_command(arch, kernel, disk) + ["-qmp", f"unix:{address},server=on,wait=off"]
        if primary and arch == "aarch64":
            command += ["-device", "virtio-keyboard-device", "-device", "ramfb"]
        if arch == "x86_64" and not primary:
            command += ["-device", "qemu-xhci,id=xhci", "-device", "usb-kbd,bus=xhci.0,id=kb",
                        "-device", "usb-mouse,bus=xhci.0,id=mouse"]
        with subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                              stderr=subprocess.STDOUT, bufsize=0) as guest:
            pending = b""

            def expect(pattern):
                nonlocal pending
                deadline = time.monotonic() + 30
                while not (match := re.search(pattern, pending)):
                    remaining = deadline - time.monotonic()
                    if remaining <= 0 or not select.select([guest.stdout], [], [], remaining)[0]:
                        raise RuntimeError(f"timeout waiting for {pattern!r}")
                    chunk = os.read(guest.stdout.fileno(), 4096)
                    if not chunk:
                        raise RuntimeError(f"guest exited waiting for {pattern!r}")
                    log.write(chunk)
                    log.flush()
                    pending += chunk
                pending = pending[match.end():]
                return match

            def shell(line):
                guest.stdin.write((line + "\n").encode())

            try:
                expect(rb"hello-init# ")
                with socket.socket(socket.AF_UNIX) as sock:
                    sock.settimeout(5)
                    sock.connect(address)
                    with sock.makefile("rwb", buffering=0) as wire:
                        json.loads(wire.readline())

                        def qmp(op, **args):
                            wire.write(json.dumps({"execute": op, "arguments": args}).encode() + b"\n")
                            while True:
                                msg = json.loads(wire.readline())
                                if "return" in msg:
                                    return msg["return"]
                                if "error" in msg:
                                    raise RuntimeError(msg["error"])

                        qmp("qmp_capabilities")
                        if primary:
                            shell("direct-key-probe")
                            expect(rb"READY direct Ctrl-C")
                            for key, down in [("ctrl", True), ("c", True), ("c", False), ("ctrl", False)]:
                                qmp("input-send-event", events=[{"type": "key", "data": {
                                    "down": down, "key": {"type": "qcode", "data": key}}}])
                                time.sleep(0.12)
                            expect(rb"PASS direct Ctrl-C")
                            expect(rb"hello-init# ")
                            shell("direct-key-probe restored")
                            expect(rb"PASS restored TTY")
                            expect(rb"hello-init# ")
                        previous = 0
                        for iteration in range(0 if primary else 2):
                            if iteration:
                                if arch == "x86_64":
                                    qmp("device_del", id="kb")
                                    expect(rb"\[usb\] port \d+: disconnected")
                                    qmp("device_add", driver="usb-kbd", bus="xhci.0", id="kb2")
                                    expect(rb"\[usb\] port \d+: keyboard interface")
                                shell("drvloader test-hid")
                                expect(rb"PASS HID crash and restart")
                                expect(rb"hello-init# ")
                            if arch == "x86_64":
                                shell("/bin/usb-input-probe")
                                expect(rb"READY USB input")
                                for down in (True, False):
                                    qmp("input-send-event", events=[{"type": "key", "data": {
                                        "down": down, "key": {"type": "qcode", "data": "a"}}}])
                                    time.sleep(0.12)
                                qmp("input-send-event", events=[{"type": "rel", "data": {
                                    "axis": "x", "value": 7}}])
                                expect(rb"PASS USB input")
                                expect(rb"hello-init# ")
                                shell("cat /proc/keyboards")
                                stats = expect(rb"(?s)source presses(.*?)hello-init# ").group(1)
                                presses = sum(map(int, re.findall(rb"usb-port-\d+ (\d+)\r?\n", stats)))
                                if presses <= previous:
                                    raise RuntimeError("USB-source press count failed to advance")
                                previous = presses
                shell("poweroff")
                if guest.wait(timeout=15):
                    raise RuntimeError("guest failed to shut down")
            finally:
                if guest.poll() is None:
                    guest.kill()
                    guest.wait()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("arch", choices=["aarch64", "x86_64"])
    parser.add_argument("kernel", type=Path)
    parser.add_argument("disk", type=Path)
    parser.add_argument("log", type=Path)
    parser.add_argument("--primary", action="store_true", help="test Ctrl-C on the primary board keyboard")
    args = parser.parse_args()
    with args.log.open("wb") as output:
        verify(args.arch, args.kernel, args.disk, output, args.primary)
    print("PASS primary keyboard" if args.primary else "PASS HID service", args.arch)
