/* Decode copied USB interrupt reports without access to the host controller. */
#include "libchiffon.h"
#include "drivers/hid_keyboard.h"

static uint8_t descriptor[4096], report[1024];
static struct hid_keyboard_layout layout;
static unsigned cached_descriptor;
static int cached_valid;

static int number(const char *s) {
    unsigned value = 0;
    if (!*s) return -1;
    for (; *s; s++) {
        if (*s < '0' || *s > '9' || value > (2147483647U - (*s - '0')) / 10) return -1;
        value = value * 10 + (*s - '0');
    }
    return value;
}
static int read_bytes(int port, int op, unsigned serial, uint8_t *dest, unsigned len) {
    for (unsigned offset = 0; offset < len;) {
        struct chi_hid_slice slice = {serial, offset};
        struct chi_ipc_msg req = { .tag = op, .len = sizeof slice }, rep;
        memcpy(req.data, &slice, sizeof slice);
        if (chiffon_ipc_call(port, &req, &rep) || !rep.len || rep.len > len - offset) return -1;
        memcpy(dest + offset, rep.data, rep.len);
        offset += rep.len;
    }
    return 0;
}
static int decode(int port, const struct chi_hid_report *info, struct chi_hid_result *out) {
    unsigned n = info->length;
    if (!n || n > sizeof report || read_bytes(port, HID_BUS_REPORT, info->serial, report, n)) return 0;
    if (info->kind == HID_BOOT_MOUSE) {
        if (n < 3) return 0;
        out->buttons = report[0] & 7;
        out->dx = (int8_t)report[1];
        out->dy = (int8_t)report[2];
        return 1;
    }
    if (info->kind == HID_BOOT_KEY) {
        if (n < 8) return 0;
        out->keys[0xe0 / 8] = report[0];
        for (unsigned i = 2; i < 8; i++) {
            unsigned u = report[i];
            if (u && u < 4) return 0;
            if (u) out->keys[u / 8] |= 1u << (u & 7);
        }
        return 1;
    }
    unsigned d = info->descriptor_length;
    if (info->kind != HID_REPORT_KEY || !d || d > sizeof descriptor || !info->descriptor_id) return 0;
    if (cached_descriptor != info->descriptor_id) {
        if (read_bytes(port, HID_BUS_DESC, info->serial, descriptor, d)) return 0;
        cached_valid = !hid_keyboard_parse(&layout, descriptor, d);
        cached_descriptor = info->descriptor_id;
    }
    if (!cached_valid) return 0;
    struct hid_keyboard_keys keys;
    uint8_t id;
    if (hid_keyboard_decode(&layout, report, n, &keys, &id)) return 0;
    for (unsigned slot = 0; slot < layout.report_count; slot++) {
        if (layout.report_ids[slot] != id) continue;
        out->slot = slot;
        memcpy(out->keys, keys.bits, sizeof keys.bits);
        return 1;
    }
    return 0;
}
static void drain(int bus) {
    for (;;) {
        struct chi_ipc_msg req = { .tag = HID_BUS_HEAD }, rep;
        struct chi_hid_report info;
        if (chiffon_ipc_call(bus, &req, &rep) || rep.len != sizeof info) return;
        memcpy(&info, rep.data, sizeof info);
        struct chi_hid_result result = { .serial = info.serial };
        result.valid = decode(bus, &info, &result);
        req.tag = HID_BUS_DONE;
        req.len = sizeof result;
        memcpy(req.data, &result, sizeof result);
        chiffon_ipc_call(bus, &req, &rep);
    }
}
int main(int argc, char **argv, char **envp) {
    if (argc != 3) return 1;
    int ready = number(argv[1]), loader = number(argv[2]);
    if (ready < 0 || loader <= 0) return 1;
    int port = chiffon_ipc_create(IPC_SVC_HID);
    if (chiffon_write(ready, &port, sizeof port) != sizeof port || port < 0) return 1;
    chiffon_close(ready);
    int bus = chiffon_ipc_lookup(IPC_SVC_HID_BUS);
    struct chi_ipc_msg req;
    while (!chiffon_ipc_recv(port, &req, 0)) {
        if (!req.src && req.tag == HID_OP_WAKE && !req.len) {
            if (bus > 0) drain(bus);
            continue;
        }
        if (req.src == loader && req.tag == HID_OP_EXIT && !req.len) return 0;
        struct chi_ipc_msg rep = { .dst = req.src };
        if (req.tag != HID_OP_STATUS || req.len) rep.tag = -1;
        chiffon_ipc_reply(&rep);
    }
    return 1;
}
