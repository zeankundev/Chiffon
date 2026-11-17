/* SPI flash, read through the chip's flash cache.  The window at 0x3F400000 is 64 KiB pages that the
 * flash MMU table at 0x3FF10000 maps to flash offsets.  We remap the first 16 pages (1 MiB) to the
 * filesystem partition, with the ROM's own cache routines (the same calls ESP-IDF's spi_flash makes).
 * Real hardware: check the boot log for a fault when the window is first read. */
#include "kernel.h"

#define FLASH_WINDOW   0x3F400000u
#define FLASH_MMU      ((volatile uint32_t *)0x3FF10000)   /* DPORT_PRO_FLASH_MMU_TABLE, 64 KiB pages */
#define FLASH_MMU_INVALID 0x100u
#define FLASH_PAGE     0x10000u

/* ROM routines (esp32.rom.ld in ESP-IDF).  They are windowed-ABI functions, called via rom_call1. */
#define ROM_CACHE_READ_DISABLE 0x40009ab8u
#define ROM_CACHE_FLUSH        0x40009a14u
#define ROM_CACHE_READ_ENABLE  0x40009a84u

void rom_call1(uint32_t fn, uint32_t arg);

static uint32_t mapped_base;     /* flash offset of window byte 0 */
static uint32_t mapped_len;

/* Map `pages` 64 KiB pages starting at flash offset `off` (a multiple of 64 KiB) to the window. */
int flash_map(uint32_t off, uint32_t pages) {
    if (off & (FLASH_PAGE - 1) || pages == 0 || pages > 16) return -EINVAL;
    rom_call1(ROM_CACHE_READ_DISABLE, 0);
    for (uint32_t i = 0; i < 64; i++)
        FLASH_MMU[i] = i < pages ? ((off / FLASH_PAGE + i) & 0xFF) : FLASH_MMU_INVALID;
    rom_call1(ROM_CACHE_FLUSH, 0);
    rom_call1(ROM_CACHE_READ_ENABLE, 0);
    mapped_base = off;
    mapped_len = pages * FLASH_PAGE;
    return 0;
}

/* Read n bytes at flash offset off (must lie inside the mapped window). */
int flash_read(uint32_t off, void *buf, size_t n) {
    if (off < mapped_base || off - mapped_base + n > mapped_len) return -EINVAL;
    const volatile uint8_t *src = (const volatile uint8_t *)(FLASH_WINDOW + (off - mapped_base));
    uint8_t *d = buf;
    for (size_t i = 0; i < n; i++) d[i] = src[i];
    return 0;
}
