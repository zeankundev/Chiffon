# Chiffon
A minimal, Unix-like **hybrid kernel** + operating system for x86_64 and aarch64.

## Building
```
make run        # QEMU on the serial console (this terminal)
make run-gfx    # QEMU window: framebuffer console + PS/2 keyboard (x86_64), ramfb (aarch64)
```

## Architecture

Chiffon is hybrid in the classic sense: the performance-critical core (scheduler, memory, VFS,
and all device drivers) runs in kernel space, while everything the kernel offers can *also* be reached
through a message-passing layer, and user-space processes can be servers on that same layer.

```
 user programs ── Linux-style syscalls ──► VFS (/dev/console, /dev/fb0, /proc)  ─┐
      │                                                                          ├─► kernel-space drivers
      └──── IPC syscalls (ports, 64-byte messages) ──► kernel services ("fb",   ─┘   (drivers/<arch>/)
                          ▲                             "console") and user servers
                          └── user-space servers (ipc_create / recv / reply)
```

### Driver model (`include/driver.h`, `kernel/driver.c`)
A driver is a `struct kdriver` registered with `KDRIVER()`; the linker script collects them, so adding
a driver means dropping a `.c` into `drivers/<arch>/` (the Makefile picks it up). `drivers_init()` runs
once from `kmain()`, ordered by class and priority (bus → input → display), calling `probe()` then
`init()`. Drivers hook into the rest of the kernel through `irq_register()`, `fb_register()`,
`console_register()` and `ipc_register_service()`. `/proc/drivers` lists them and their state.

### IPC (`include/ipc.h`, `include/uapi/chiffon_abi.h`, `kernel/ipc.c`)
Named ports, 64-byte messages (48 bytes payload). `ipc_create` / `ipc_lookup` / `ipc_send` / `ipc_recv` /
`ipc_call` / `ipc_reply` / `ipc_close` are syscalls 1000-1006. A port is either
* a **user port**: a server process `recv`s requests and `reply`s to callers (blocking call/reply);
* a **kernel service**: a handler function run synchronously in the caller's context.

If a server exits, its ports are removed and callers blocked on it fail with `-EPIPE`.
Built-in services: `fb` (info / fill / pan) and `console` (write / size / clear / graphics mode).
`/proc/ipc` lists ports. Try `ipcdemo` and `ipcdemo -crash` in the demo environment.

### Video
* `kernel/fb.c` – framebuffer core. One primary framebuffer is bound to the text console and exported as
  `/dev/fb0` (read / write / lseek / **mmap**, Linux `FBIOGET_VSCREENINFO`, `FBIOGET_FSCREENINFO`,
  `FBIOPAN_DISPLAY`) and as the `fb` IPC service.
* `kernel/vt.c`, `kernel/fbcon.c` – VT100/ANSI terminal emulator (cursor movement, erase, scroll regions,
  16 colours, UTF-8→Latin-1) painting an 8x16 font; only changed cells are redrawn, once per timer tick.
  The boot log is kept and replayed onto the screen when a display attaches. `TIOCGWINSZ` reports the
  real size, and `KDSETMODE` lets a program take over the screen (see `fbdemo`).
* Display drivers:
  | driver | where | notes |
  |---|---|---|
  | `bochs-vbe` | `drivers/x86_64/` | QEMU `-vga std` / Bochs / VirtualBox; sets the mode (`video=WxH`, default 1024x768), double-height virtual screen for page flipping |
  | `multiboot-fb` | `drivers/x86_64/` | adopts a linear framebuffer provided by GRUB (`gfxpayload`) |
  | `vga-text` | `drivers/x86_64/` | legacy 80x25 text buffer, used when no graphics framebuffer is available |
  | `ramfb` | `drivers/aarch64/` | QEMU `virt` via fw_cfg (`-device ramfb`); pair with `-device virtio-keyboard-device` for typing |
* Input: `ps2-kbd` on x86_64 (IRQ1) and `virtio-kbd` on aarch64 (virtio-input over virtio-mmio, polled; QEMU forwards
  window/VNC keystrokes to it). Both share the key translation in `kernel/kbd.c` (US layout, arrows / Home / End / F-keys as VT100 sequences)
  feeds the same tty line discipline as the serial port, so the on-screen console is fully interactive.
  Serial remains available as a second console.

Kernel command line: `video=1280x720`, `fbcon=off`.

### Not (yet) done
Drivers still run in ring 0 – there is no user-space driver loader. Real HDMI output needs a SoC-specific
display driver (e.g. a mailbox framebuffer on Raspberry Pi or a vendor DRM block): add it as
`drivers/aarch64/<soc>.c` that calls `fb_register()` and the console, `/dev/fb0` and IPC services work unchanged.
