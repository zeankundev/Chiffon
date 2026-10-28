/* x86_64 "qemu" board: QEMU's PC machine.  Identical hardware to "generic"; additionally
 * powers the VM off through QEMU's ACPI port when the system shuts down. */
#define BOARD_QEMU 1
#define BOARD_NAME "qemu"
#include "../generic/board.c"
