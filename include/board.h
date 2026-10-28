#pragma once
#include <stdint.h>
#include <stddef.h>
#include "arch.h"

void board_early_init(uintptr_t bootinfo);

void board_mem_range(uintptr_t *start, uintptr_t *end);

const char *board_cmdline(void);

void board_console_putc(int c);
int board_console_getc(void);

void board_init(void);

void board_irq(struct trapframe *tf);

void board_poweroff(void) __attribute__((noreturn));

int board_blk_init(void);
uint64_t board_blk_sectors(void);
int board_blk_read(uint64_t lba, void *buf, uint32_t nsect);