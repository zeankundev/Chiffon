# Specials

This directory contains ports and standalone builds for specialized microcontrollers (MCUs) and non-standard architectures that require dedicated cross-toolchains or custom memory models (e.g., MMU-less systems, Xtensa cores, etc.) rather than the primary hybrid kernel targets (`x86_64` and `aarch64`).

## Available Targets

- [`esp32/`](esp32/): Espressif ESP32 (Xtensa LX6 core, ESP32-D0WD-V3, 520 KB SRAM). Standalone bootloader and kernel image.
