# P2a — USB HID isolation

## Placement and dispatch

The embedded loader starts an embedded `/bin/hidserver` child at boot. Only the loader's bootstrap child can exec that privileged image. Exec installs one device-scoped `CHI_CAP_DRIVER` for `DEV_TYPE_HID`. Ordinary exec, fork and exit use the existing capability cleanup rules. The service has no MMIO, IRQ, DMA or framebuffer grant. On x86, the build embeds the loader and HID server even for root filesystems without driver tools. Demo and milkOS provide the management CLI.

The x86 xHCI driver retains exclusive control of PCI bus mastering, MMIO, slots, endpoint configuration, rings and transfer buffers. The kernel parses transport configuration to choose supported endpoints and copies completed input reports. The user process parses keyboard report descriptors, decodes boot and report-protocol keyboard packets, and interprets boot mouse motion and button state. The kernel validates the result against the live endpoint, merges held keys and buttons across sources, and owns key repeat, the TTY and the input event queue. The USB bulk-only storage path, filesystem mount/unmount and primary PS/2 or virtio console keyboards continue in the kernel. AArch64 has no xHCI transport implementation.

## Driver ABI version 3

The existing syscalls, opcodes and binary layouts retain their values. New service and loader opcodes are declared in `include/uapi/chiffon_abi.h`. The reserved `usb.hid` user port accepts health requests. The kernel sends an origin-stamped wake when raw input is available. The reserved x86 kernel port `usb.hid.transport` accepts requests only from the HID capability holder. Its head operation returns a transaction serial, report kind, report length, descriptor length and descriptor identity. Report and descriptor bytes are transferred in slices of up to 48 bytes. The final message consumes the transaction and carries a keyboard bitmap for a bounded report slot or three mouse buttons and signed relative motion. The service has no interface for programming endpoints, accessing storage, submitting DMA addresses or writing board-keyboard events.

The queue holds 32 reports of up to 1024 bytes. Descriptors have a 4096-byte limit. While the timer handles completions it never waits for a server. Detach invalidates queued work by attachment generation and releases the device's keys and buttons. Restart discards in-flight work and releases held input. On queue overflow the transport releases held state and discards the backlog before accepting new reports. Input during disruption can be lost. Descriptor layouts are cached by attachment-specific identity. The loader's `drvloader start hid` and `drvloader test-hid` commands use its established IPC service. The latter checks the blocked and stale port responses and a request to the replacement instance.

## Verification

Build Demo using the P0 commands for `x86_64` and `aarch64`. Both clean GCC `-Werror` builds and the release serial smoke suite passed. In the same export run the following after each build:

```sh
images="build/demo/$arch-qemu"
python3 tools/release/hid_service.py "$arch" "$images/chiffon.bin" "$images/disk.img" "hid-$arch.serial"
```

Both targets passed the HID service exit, stale-handle and restart checks. On x86 the runner connects QEMU USB keyboard and mouse devices to xHCI, checks press, release and relative motion through `/dev/input/event0`, and verifies increasing USB-source counts in `/proc/keyboards`. It detaches and reattaches the keyboard and repeats the input checks after restarting the service. It also checks denial of unauthorized HID registration and transport access. This exposed an initialization dependency: QEMU fetches the event-ring segment table when its address is programmed. Bus mastering must be enabled after preparing the DMA frames and before writing that register. The smoke suite exercises report-protocol descriptor parsing with fixture data. Physical USB devices, report-protocol keyboards on hardware, interrupt-driven xHCI and IOMMU confinement have no test result in this phase.
