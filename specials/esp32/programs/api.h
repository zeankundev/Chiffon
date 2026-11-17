/* The interface a program gets from the kernel (esp32/elf.c fills it in). */
#pragma once
#include <stdint.h>
#include <stddef.h>
struct esp_api {
    int (*printf)(const char *fmt, ...);
    void (*putc)(int c);
    void *(*malloc)(size_t n);
    void (*free)(void *p);
    uint32_t (*ccount)(void);            /* CPU cycle counter */
};
