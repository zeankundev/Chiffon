# Raspberry Pi Zero 2W (BCM2837B0, quad Cortex-A53, 512MB RAM)
# Loaded at 0x80000 by VideoCore bootloader / rpiboot.
LOAD_ADDR = 0x80000

# Bit n of BOOT_DEVICE_GB_MASK maps physical GiB n as device memory during early boot.
# Since Pi RAM is in GiB 0 (0x00000000 - 0x1FFFFFFF), GiB 0 MUST be normal RAM so code can execute!
# BCM2837 peripherals (0x3F000000) and ARM local peripherals (0x40000000) will be mapped as device
# memory via arch_map_device() in board_early_init().
BOOT_DEVICE_GB_MASK = 0x0

# Emulation with QEMU raspi3b (shares BCM2837 SoC with Pi Zero 2W):
# QEMU exposes Mini UART (aux) on the second serial port (-serial null -serial stdio)
QEMU_CMD = qemu-system-aarch64 -M raspi3b -kernel $(KERNEL) -serial null -serial stdio -no-reboot

