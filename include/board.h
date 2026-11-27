#pragma once
#include <stdint.h>
#include <stddef.h>
#include "arch.h"

void board_early_init(uintptr_t bootinfo);

/* Contiguous usable RAM for the heap and initial PMM range, after boot reservations. */
void board_mem_range(uintptr_t *start, uintptr_t *end);
/* Further RAM the board knows about (external RAM, a second memory node).  Returns 0 and the range
 * for index i, or -1 when there is no range i.  A board without extra RAM need not define it. */
int board_mem_extra(int i, uintptr_t *start, uintptr_t *end);

const char *board_cmdline(void);

void board_console_putc(int c);
int board_console_getc(void);

void board_init(void);

void board_irq(struct trapframe *tf);

void board_poweroff(void) __attribute__((noreturn));

int board_blk_init(void);
uint64_t board_blk_sectors(void);
int board_blk_read(uint64_t lba, void *buf, uint32_t nsect);
/* On success all requested sectors are durable and ordered before later writes.
 * A zero-sector write flushes outstanding writes and checks flush support. */
int board_blk_write(uint64_t lba, const void *buf, uint32_t nsect);
