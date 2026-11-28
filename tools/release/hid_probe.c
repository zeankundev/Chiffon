/* Guest smoke check of the shared USB HID keyboard report parser. */
#include "libchiffon.h"
#include "drivers/hid_keyboard.h"

static const uint8_t boot_descriptor[] = {
    0x05, 0x01, 0x09, 0x06, 0xa1, 0x01,
    0x05, 0x07, 0x19, 0xe0, 0x29, 0xe7, 0x15, 0x00,
    0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
    0x75, 0x08, 0x95, 0x01, 0x81, 0x01,
    0x19, 0x00, 0x29, 0x65, 0x15, 0x00, 0x25, 0x65,
    0x75, 0x08, 0x95, 0x06, 0x81, 0x00, 0xc0
};

/* Report 2 belongs to a mouse; report 1 contains 224 keyboard bits. */
static const uint8_t nkro_descriptor[] = {
    0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0x85, 0x02,
    0x75, 0x08, 0x95, 0x01, 0x81, 0x02, 0xc0,
    0x05, 0x01, 0x09, 0x06, 0xa1, 0x01, 0x85, 0x01,
    0x05, 0x07, 0x19, 0x00, 0x2a, 0xdf, 0x00,
    0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x96, 0xe0, 0x00,
    0x81, 0x02, 0xc0
};

/* Logical values 1 and 2 select A and Z in a non-contiguous usage list. */
static const uint8_t list_descriptor[] = {
    0x05, 0x01, 0x09, 0x06, 0xa1, 0x01, 0x05, 0x07, 0x09, 0x04,
    0x09, 0x1d, 0x15, 0x01, 0x25, 0x02, 0x75, 0x08, 0x95, 0x01, 0x81, 0x00, 0xc0
};

static int fail(const char *stage) {
    chiffon_puts("FAIL hid-probe ");
    chiffon_puts(stage);
    chiffon_puts("\n");
    return 1;
}

static int only_keys(const struct hid_keyboard_keys *keys, unsigned a, unsigned b) {
    unsigned i;
    for (i = 0; i < 256; i++) {
        int expected = (a && i == a) || (b && i == b);
        if (!!(keys->bits[i / 8] & (1u << (i % 8))) != expected) return 0;
    }
    return 1;
}

int main(int argc, char **argv, char **envp) {
    struct hid_keyboard_layout layout;
    struct hid_keyboard_keys keys = {{0}};
    uint8_t id = 0xff;
    const uint8_t press[] = { 0x02, 0, 0x04, 0, 0, 0, 0, 0 };
    const uint8_t rollover[] = { 0, 0, 0x01, 0, 0, 0, 0, 0 };
    const uint8_t release[] = { 0, 0, 0, 0, 0, 0, 0, 0 };
    uint8_t nkro[29] = { 1 };
    const uint8_t mouse[] = { 2, 0xff };
    (void)argc; (void)argv; (void)envp;

    if (hid_keyboard_parse(&layout, boot_descriptor, sizeof(boot_descriptor)))
        return fail("boot descriptor");
    if (hid_keyboard_decode(&layout, press, sizeof(press), &keys, &id) ||
        id != 0 || !only_keys(&keys, 4, 0xe1)) return fail("boot press");
    id = 0xff;
    if (hid_keyboard_decode(&layout, press, sizeof(press) - 1, &keys, &id) != -1 ||
        id != 0xff || !only_keys(&keys, 4, 0xe1)) return fail("truncated report");
    if (hid_keyboard_decode(&layout, rollover, sizeof(rollover), &keys, &id) != -1 ||
        id != 0xff || !only_keys(&keys, 4, 0xe1)) return fail("rollover state");
    if (hid_keyboard_decode(&layout, release, sizeof(release), &keys, &id) ||
        id != 0 || !only_keys(&keys, 0, 0)) return fail("boot release");

    if (hid_keyboard_parse(&layout, nkro_descriptor, sizeof(nkro_descriptor)))
        return fail("nkro descriptor");
    nkro[1 + 4 / 8] |= 1u << (4 % 8);
    nkro[1 + 200 / 8] |= 1u << (200 % 8);
    if (hid_keyboard_decode(&layout, nkro, sizeof(nkro), &keys, &id) ||
        id != 1 || !only_keys(&keys, 4, 200)) return fail("nkro press");
    id = 0xff;
    if (hid_keyboard_decode(&layout, mouse, sizeof(mouse), &keys, &id) != -1 ||
        id != 0xff || !only_keys(&keys, 4, 200)) return fail("unrelated report");
    nkro[1 + 4 / 8] = 0;
    nkro[1 + 200 / 8] = 0;
    if (hid_keyboard_decode(&layout, nkro, sizeof(nkro), &keys, &id) ||
        id != 1 || !only_keys(&keys, 0, 0)) return fail("nkro release");
    const uint8_t selection = 2;
    if (hid_keyboard_parse(&layout, list_descriptor, sizeof(list_descriptor)) ||
        hid_keyboard_decode(&layout, &selection, 1, &keys, &id) ||
        id != 0 || !only_keys(&keys, 0x1d, 0)) return fail("usage list");
    chiffon_puts("PASS hid-probe\n");
    return 0;
}
