/* hello: a program loaded from the root filesystem and run by the ESP32 shell.  It uses only the
 * kernel interface in api.h, so it needs no libc. */
#include "api.h"

int prog_main(const struct esp_api *api) {
    uint32_t t0 = api->ccount();
    unsigned long sum = 0;
    for (unsigned long i = 1; i <= 1000; i++) sum += i * i;      /* a little work to time */
    uint32_t cycles = api->ccount() - t0;
    api->printf("hello from a Chiffon program on ESP32\n");
    api->printf("sum of squares 1..1000 = %lu\n", sum);
    api->printf("took %u cycles, allocating 256 bytes: %s\n", (unsigned)cycles,
                api->malloc(256) ? "ok" : "failed");
    return 0;
}
