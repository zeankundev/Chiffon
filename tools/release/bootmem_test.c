/* Host regression fixture for the range logic used by both QEMU boards. */
#include "bootmem.h"
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>

static jmp_buf panic_target;
static const char *expected_panic;
static unsigned int log_count;

void kprintf(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    log_count++;
}

void panic(const char *fmt, ...) {
    if (expected_panic && !strcmp(fmt, expected_panic)) longjmp(panic_target, 1);
    fprintf(stderr, "Unexpected bootmem panic: %s\n", fmt);
    exit(EXIT_FAILURE);
}

static void expect_range(const struct bootmem *m, int index,
                         uintptr_t start, uintptr_t end) {
    uintptr_t actual_start, actual_end;
    assert(bootmem_get(m, index, &actual_start, &actual_end) == 0);
    assert(actual_start == start);
    assert(actual_end == end);
}

static void check_reservations(void) {
    struct bootmem m = {0};
    bootmem_add(m.ram, &m.nram, PAGE_SIZE, 11 * PAGE_SIZE);
    /* Adjacent and overlapping reservations form one hole. Partial pages are excluded. */
    bootmem_add(m.reserved, &m.nreserved, 6 * PAGE_SIZE, PAGE_SIZE + 1);
    bootmem_add(m.reserved, &m.nreserved, 2 * PAGE_SIZE + 1, 3 * PAGE_SIZE - 1);
    bootmem_add(m.reserved, &m.nreserved, 4 * PAGE_SIZE, 2 * PAGE_SIZE);
    bootmem_prepare(&m, BOOTMEM_LIMIT);
    assert(m.nusable == 2);
    expect_range(&m, 0, 8 * PAGE_SIZE, 12 * PAGE_SIZE);
    expect_range(&m, 1, PAGE_SIZE, 2 * PAGE_SIZE);
}

static void check_limit(void) {
    struct bootmem m = {0};
    bootmem_add(m.ram, &m.nram, 2 * PAGE_SIZE, 4 * PAGE_SIZE);
    bootmem_add(m.ram, &m.nram, 8 * PAGE_SIZE, PAGE_SIZE);
    unsigned int initial_logs = log_count;
    bootmem_prepare(&m, 5 * PAGE_SIZE);
    assert(log_count == initial_logs + 2);
    assert(m.nusable == 1);
    expect_range(&m, 0, 2 * PAGE_SIZE, 5 * PAGE_SIZE);
}

static void check_capacity(void) {
    struct bootmem m = {0};
    for (int i = 0; i < BOOTMEM_RANGES; i++)
        bootmem_add(m.ram, &m.nram, (2 * i + 1) * PAGE_SIZE, PAGE_SIZE);
    bootmem_prepare(&m, BOOTMEM_LIMIT);
    assert(m.nusable == BOOTMEM_RANGES);
    for (int i = 0; i < BOOTMEM_RANGES; i++)
        expect_range(&m, i, (2 * i + 1) * PAGE_SIZE, (2 * i + 2) * PAGE_SIZE);
    uintptr_t start = 1, end = 2;
    assert(bootmem_get(&m, -1, &start, &end) == -1);
    assert(bootmem_get(&m, BOOTMEM_RANGES, &start, &end) == -1);
    assert(start == 1 && end == 2);
}

static void check_fragment_capacity(void) {
    struct bootmem m = {0};
    bootmem_add(m.ram, &m.nram, PAGE_SIZE, (2 * BOOTMEM_RANGES + 1) * PAGE_SIZE);
    for (int i = 0; i < BOOTMEM_RANGES; i++)
        bootmem_add(m.reserved, &m.nreserved, (2 * i + 2) * PAGE_SIZE, PAGE_SIZE);
    bootmem_prepare(&m, BOOTMEM_LIMIT);
}

int main(void) {
    check_reservations();
    check_limit();
    check_capacity();
    expected_panic = "mem: too many boot ranges";
    if (setjmp(panic_target) == 0) {
        check_fragment_capacity();
        fputs("FAIL bootmem: expected a range capacity error\n", stderr);
        return EXIT_FAILURE;
    }
    puts("PASS bootmem fixture");
    return EXIT_SUCCESS;
}
