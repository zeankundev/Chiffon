/* USB device classes over xhci.c: HID keyboards and boot mice (keys reach the console and
 * /dev/input/event0 through kbd_key_source(), pointer motion through input_rel()/input_btn()), and
 * bulk-only mass storage, mounted read-only under /media/usbN when the VFS is up and unmounted when the
 * device is removed.  Descriptors are parsed by hand; only the interfaces this driver can use are configured. */
#include "kernel.h"
#include "fat.h"
#include "drivers/usb.h"
#include "drivers/hid_keyboard.h"

#define USB_MAX_KEYBOARDS 8
struct usb_keyboard {
    uint8_t ep, iface, boot, active;
    uint16_t report_len;
    struct hid_keyboard_layout layout;
    struct hid_keyboard_keys reports[HID_KEYBOARD_MAX_REPORTS];
};

struct usbdev {
    int attached, port, speed, unit, unit_set;
    uint16_t vid, pid;
    uint8_t mouse_ep, mouse_if, msc_in, msc_out;
    uint8_t prev_btn;
    struct usb_keyboard *keyboards[USB_MAX_KEYBOARDS];
    unsigned nkbd;
    struct hid_keyboard_keys keys;
    uint16_t repeat_code;
    uint64_t repeat_at;
    int msc, mount_pending;
    void *vol;
    uint32_t tag, blksz;
    uint64_t nblk;
};
static struct usbdev devs[USB_MAX_PORTS + 1];

static const char *speed_name(int s) {
    return s == USB_LS ? "low" : s == USB_FS ? "full" : s == USB_HS ? "high" : "super";
}
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

static int ctrl(int p, uint8_t rt, uint8_t req, uint16_t val, uint16_t idx, void *buf, uint16_t len) {
    struct usb_setup s = { rt, req, val, idx, len };
    return xhci_control(p, &s, buf);
}

/* ---- HID boot keyboard: USB usage (page 0x07) -> scancode set 1; bit 8 = E0 prefix ---- */
static const uint16_t hid_sc[256] = {
    [0x04]=0x1E, [0x05]=0x30, [0x06]=0x2E, [0x07]=0x20, [0x08]=0x12, [0x09]=0x21, [0x0A]=0x22, [0x0B]=0x23,
    [0x0C]=0x17, [0x0D]=0x24, [0x0E]=0x25, [0x0F]=0x26, [0x10]=0x32, [0x11]=0x31, [0x12]=0x18, [0x13]=0x19,
    [0x14]=0x10, [0x15]=0x13, [0x16]=0x1F, [0x17]=0x14, [0x18]=0x16, [0x19]=0x2F, [0x1A]=0x11, [0x1B]=0x2D,
    [0x1C]=0x15, [0x1D]=0x2C,
    [0x1E]=0x02, [0x1F]=0x03, [0x20]=0x04, [0x21]=0x05, [0x22]=0x06, [0x23]=0x07, [0x24]=0x08, [0x25]=0x09,
    [0x26]=0x0A, [0x27]=0x0B,
    [0x28]=0x1C, [0x29]=0x01, [0x2A]=0x0E, [0x2B]=0x0F, [0x2C]=0x39, [0x2D]=0x0C, [0x2E]=0x0D, [0x2F]=0x1A,
    [0x30]=0x1B, [0x31]=0x2B, [0x33]=0x27, [0x34]=0x28, [0x35]=0x29, [0x36]=0x33, [0x37]=0x34, [0x38]=0x35,
    [0x39]=0x3A,
    [0x3A]=0x3B, [0x3B]=0x3C, [0x3C]=0x3D, [0x3D]=0x3E, [0x3E]=0x3F, [0x3F]=0x40, [0x40]=0x41, [0x41]=0x42,
    [0x42]=0x43, [0x43]=0x44, [0x44]=0x57, [0x45]=0x58,
    [0x46]=0x137, [0x47]=0x46, [0x49]=0x152, [0x4A]=0x147, [0x4B]=0x149, [0x4C]=0x153, [0x4D]=0x14F,
    [0x4E]=0x151, [0x4F]=0x14D, [0x50]=0x14B, [0x51]=0x150, [0x52]=0x148,
    [0x53]=0x45, [0x54]=0x135, [0x55]=0x37, [0x56]=0x4A, [0x57]=0x4E, [0x58]=0x11C,
    [0x59]=0x4F, [0x5A]=0x50, [0x5B]=0x51, [0x5C]=0x4B, [0x5D]=0x4C, [0x5E]=0x4D, [0x5F]=0x47, [0x60]=0x48,
    [0x61]=0x49, [0x62]=0x52, [0x63]=0x53,
};
static uint16_t usage_sc(unsigned usage) {
    static const uint16_t mods[8] = { 0x1D, 0x2A, 0x38, 0x15B, 0x11D, 0x36, 0x138, 0x15C };
    return usage >= 0xE0 && usage <= 0xE7 ? mods[usage - 0xE0] : hid_sc[usage];
}
static void kbd_transitions(struct usbdev *d, const struct hid_keyboard_keys *keys,
                            unsigned first, unsigned end, int down) {
    for (unsigned u = first; u < end; u++) {
        uint8_t mask = (uint8_t)(1u << (u & 7));
        int old = !!(d->keys.bits[u >> 3] & mask);
        int now = !!(keys->bits[u >> 3] & mask);
        uint16_t sc = usage_sc(u);
        if (!sc || old == now || now != down) continue;
        kbd_key_source((unsigned)d->port, sc, down);
        if (!down && sc == d->repeat_code) d->repeat_code = 0;
        if (down && u < 0xE0 && u != 0x39 && u != 0x47 && u != 0x53) {
            d->repeat_code = sc;
            d->repeat_at = ticks + HZ / 2;
        }
    }
}
static void kbd_merge(struct usbdev *d) {
    struct hid_keyboard_keys keys = { { 0 } };
    for (unsigned i = 0; i < d->nkbd; i++) {
        struct usb_keyboard *k = d->keyboards[i];
        if (!k->active) continue;
        unsigned count = k->boot ? 1 : k->layout.report_count;
        for (unsigned j = 0; j < count; j++)
            for (unsigned b = 0; b < sizeof keys.bits; b++) keys.bits[b] |= k->reports[j].bits[b];
    }
    /* Release ordinary keys before modifiers; press modifiers before ordinary keys. */
    kbd_transitions(d, &keys, 0, 0xE0, 0);
    kbd_transitions(d, &keys, 0xE8, 256, 0);
    kbd_transitions(d, &keys, 0xE0, 0xE8, 0);
    kbd_transitions(d, &keys, 0xE0, 0xE8, 1);
    kbd_transitions(d, &keys, 0, 0xE0, 1);
    kbd_transitions(d, &keys, 0xE8, 256, 1);
    d->keys = keys;
}
static void kbd_report(struct usbdev *d, struct usb_keyboard *k, const uint8_t *r, uint32_t len) {
    struct hid_keyboard_keys keys = { { 0 } };
    unsigned slot = 0;
    if (k->boot) {
        if (len < 8) return;
        for (unsigned i = 2; i < 8; i++) if (r[i] >= 1 && r[i] <= 3) return;
        for (unsigned i = 0; i < 8; i++) if (r[0] & (1u << i)) keys.bits[(0xE0 + i) >> 3] |= 1u << ((0xE0 + i) & 7);
        for (unsigned i = 2; i < 8; i++) if (r[i]) keys.bits[r[i] >> 3] |= 1u << (r[i] & 7);
    } else {
        uint8_t id;
        if (hid_keyboard_decode(&k->layout, r, len, &keys, &id)) return;
        while (slot < k->layout.report_count && k->layout.report_ids[slot] != id) slot++;
        if (slot == k->layout.report_count) return;
    }
    k->reports[slot] = keys;
    kbd_merge(d);
}

static void mouse_report(struct usbdev *d, const uint8_t *r, uint32_t len) {
    if (len < 3) return;
    uint8_t changed = d->prev_btn ^ r[0];
    for (int b = 0; b < 3; b++)
        if (changed & (1u << b)) input_btn((unsigned)b, (r[0] >> b) & 1);
    input_rel(0, (int8_t)r[1]);
    input_rel(1, (int8_t)r[2]);
    d->prev_btn = r[0] & 7;
}

void usb_report(int p, uint8_t addr, const uint8_t *data, uint32_t len) {
    if (p < 1 || p > USB_MAX_PORTS) return;
    struct usbdev *d = &devs[p];
    if (!d->attached) return;
    for (unsigned i = 0; i < d->nkbd; i++)
        if (d->keyboards[i]->active && addr == d->keyboards[i]->ep) {
            kbd_report(d, d->keyboards[i], data, len);
            return;
        }
    if (addr == d->mouse_ep) mouse_report(d, data, len);
}

/* ---- bulk-only transport for mass storage ---- */
static int bot(struct usbdev *d, const uint8_t *cdb, uint8_t cdblen, void *data, uint32_t dlen, int in) {
    uint8_t cbw[31];
    memset(cbw, 0, sizeof cbw);
    uint32_t tag = ++d->tag;
    cbw[0] = 'U'; cbw[1] = 'S'; cbw[2] = 'B'; cbw[3] = 'C';
    for (int i = 0; i < 4; i++) { cbw[4 + i] = (uint8_t)(tag >> (8 * i)); cbw[8 + i] = (uint8_t)(dlen >> (8 * i)); }
    cbw[12] = in ? 0x80 : 0x00;
    cbw[13] = 0;
    cbw[14] = cdblen;
    memcpy(cbw + 15, cdb, cdblen);
    if (xhci_bulk(d->port, d->msc_out, cbw, sizeof cbw) != (int)sizeof cbw) return -EIO;
    int moved = 0;
    if (dlen) {
        moved = xhci_bulk(d->port, in ? d->msc_in : d->msc_out, data, dlen);
        if (moved < 0) return -EIO;
    }
    uint8_t csw[13];
    if (xhci_bulk(d->port, d->msc_in, csw, sizeof csw) != (int)sizeof csw) return -EIO;
    if (csw[0] != 'U' || csw[1] != 'S' || csw[2] != 'B' || csw[3] != 'S' ||
        le32(csw + 4) != tag || le32(csw + 8) != 0 || csw[12] != 0 || moved != (int)dlen) return -EIO;
    return 0;
}

/* sector reader for the FAT driver: READ(10) in runs of up to 64 blocks */
static int msc_rd(void *ctx, uint64_t lba, uint32_t n, void *buf) {
    struct usbdev *d = ctx;
    if (!d->attached || !d->msc || d->blksz != 512 || lba > d->nblk || n > d->nblk - lba) return -EIO;
    uint8_t *out = buf;
    while (n) {
        uint32_t k = n > 64 ? 64 : n;
        uint8_t c[10] = { 0x28, 0, (uint8_t)(lba >> 24), (uint8_t)(lba >> 16), (uint8_t)(lba >> 8), (uint8_t)lba,
                          0, (uint8_t)(k >> 8), (uint8_t)k, 0 };
        if (bot(d, c, 10, out, k * 512, 1)) return -EIO;
        out += k * 512; lba += k; n -= k;
    }
    return 0;
}

static int msc_init(struct usbdev *d) {
    for (int i = 0; i < 3; i++) {                        /* devices report "becoming ready" first */
        uint8_t c[6] = { 0 };
        if (!bot(d, c, 6, 0, 0, 0)) break;
    }
    uint8_t inq[36] = { 0 }, icdb[6] = { 0x12, 0, 0, 0, 36, 0 };
    if (!bot(d, icdb, 6, inq, 36, 1)) {                 /* vendor (8) and product (16) strings, space padded */
        char vendor[9], product[17];
        for (int i = 0; i < 8; i++) vendor[i] = inq[8 + i] >= 32 && inq[8 + i] < 127 ? (char)inq[8 + i] : '?';
        for (int i = 0; i < 16; i++) product[i] = inq[16 + i] >= 32 && inq[16 + i] < 127 ? (char)inq[16 + i] : '?';
        vendor[8] = 0; product[16] = 0;
        kprintf("[usb] port %d: mass storage \"%s %s\"\n", d->port, vendor, product);
    }
    uint8_t cap[8] = { 0 }, ccdb[10] = { 0x25 };
    if (bot(d, ccdb, 10, cap, 8, 1)) { kprintf("[usb] port %d: READ CAPACITY failed\n", d->port); return -EIO; }
    if (be32(cap) == 0xFFFFFFFFu) return -ENODEV; /* READ CAPACITY(16) is not implemented */
    d->nblk = (uint64_t)be32(cap) + 1;
    d->blksz = be32(cap + 4);
    if (d->blksz != 512) { kprintf("[usb] port %d: %u-byte blocks not supported\n", d->port, d->blksz); return -ENODEV; }
    d->msc = 1;
    d->mount_pending = 1;
    kprintf("[usb] port %d: %lu MiB of storage\n", d->port, (unsigned long)(d->nblk / 2048));
    return 0;
}

/* ---- enumeration ---- */
static int endpoint_valid(int speed, uint8_t addr, uint8_t attr, uint16_t raw_mps, uint8_t interval) {
    unsigned mps = raw_mps & 0x7FF;
    if ((addr & 0x70) || !(addr & 0x0F) || !mps || (raw_mps & 0xF800)) return 0;
    if (attr == 3) {
        if (!interval || ((speed == USB_HS || speed == USB_SS) && interval > 16)) return 0;
        return mps <= (speed == USB_LS ? 8u : speed == USB_FS ? 64u : 1024u);
    }
    if (attr != 2 || speed == USB_LS) return 0;
    if (speed == USB_FS) return mps == 8 || mps == 16 || mps == 32 || mps == 64;
    return mps == (speed == USB_HS ? 512u : 1024u);
}

static int unit_alloc(void) {
    for (int u = 0; u < 8; u++) {
        int used = 0;
        for (int p = 1; p <= USB_MAX_PORTS; p++) if (devs[p].unit_set && devs[p].unit == u) used = 1;
        if (!used) return u;
    }
    return -1;
}

int usb_attach(int p, int speed) {
    if (p < 1 || p > USB_MAX_PORTS || speed < USB_FS || speed > USB_SS) return -EINVAL;
    struct usbdev *d = &devs[p];
    memset(d, 0, sizeof *d);
    d->port = p; d->speed = speed; d->attached = 1;
    uint8_t dd[18], *cfg = 0;
    int err = -EIO, activated = 0;
    if (ctrl(p, 0x80, 6, 0x0100, 0, dd, 8) != 8) goto fail;
    if (dd[0] != 18 || dd[1] != 1) goto fail;
    uint16_t want = speed == USB_SS ? (dd[7] == 9 ? 512 : 0) : dd[7];
    if ((speed == USB_LS && want != 8) || (speed == USB_FS && want != 8 && want != 16 && want != 32 && want != 64) ||
        (speed == USB_HS && want != 64) || (speed == USB_SS && want != 512)) goto fail;
    uint16_t def = speed == USB_SS ? 512 : speed == USB_HS ? 64 : 8;
    if (want != def && xhci_set_ep0_mps(p, want)) goto fail;
    if (ctrl(p, 0x80, 6, 0x0100, 0, dd, 18) != 18) goto fail;
    if (dd[0] != 18 || dd[1] != 1 || !dd[17] ||
        (speed == USB_SS ? dd[7] != 9 : dd[7] != want)) goto fail;
    d->vid = le16(dd + 8); d->pid = le16(dd + 10);
    kprintf("[usb] port %d: %s-speed device %04x:%04x\n", p, speed_name(speed), d->vid, d->pid);
    uint8_t hdr[9];
    if (ctrl(p, 0x80, 6, 0x0200, 0, hdr, 9) != 9) goto fail;
    uint16_t total = le16(hdr + 2);
    if (hdr[0] != 9 || hdr[1] != 2 || !hdr[5] || total < 9) goto fail;
    if (total > 1024) { err = -ENODEV; goto fail; }
    cfg = kmalloc(total);
    if (!cfg) { err = -ENOMEM; goto fail; }
    if (ctrl(p, 0x80, 6, 0x0200, 0, cfg, total) != total ||
        cfg[0] != 9 || cfg[1] != 2 || le16(cfg + 2) != total || cfg[5] != hdr[5]) goto fail;

    struct usb_ep eps[8]; int n = 0, curif = 0, have_mouse = 0, have_msc = 0;
    uint32_t seen_eps = 0;
    int companion = 0;
    struct usb_keyboard *k = 0;
    for (uint32_t o = 0; o < total;) {
        if (total - o < 2) goto fail;
        uint8_t l = cfg[o], t = cfg[o + 1];
        if (l < 2 || o + l > total) goto fail;
        if (companion) {
            if (t != 0x30 || l < 6 || cfg[o + 2] || cfg[o + 3] ||
                (eps[n - 1].attr == 3 && le16(cfg + o + 4) > eps[n - 1].mps)) goto fail;
            companion = 0;
        }
        if ((t == 4 && l < 9) || (t == 5 && l < 7)) goto fail;
        if (t == 4 && l >= 9) {
            curif = 0; k = 0;
            uint8_t cls = cfg[o + 5], sub = cfg[o + 6], pr = cfg[o + 7];
            if (cfg[o + 3] == 0) {
                if (cls == 3 && sub == 1 && pr == 2 && !have_mouse) {
                    curif = 2; have_mouse = 1; d->mouse_if = cfg[o + 2];
                } else if (cls == 3 && d->nkbd < USB_MAX_KEYBOARDS) {
                    k = kmalloc(sizeof *k);
                    if (!k) { err = -ENOMEM; goto fail; }
                    memset(k, 0, sizeof *k);
                    k->iface = cfg[o + 2]; k->boot = sub == 1 && pr == 1;
                    d->keyboards[d->nkbd++] = k; curif = 1;
                } else if (cls == 8 && sub == 6 && pr == 0x50 && !have_msc) {
                    curif = 3; have_msc = 1;
                }
            }
        } else if (t == 0x21 && l >= 6 && curif == 1 && k) {
            for (unsigned j = 0; j < cfg[o + 5] && 6 + j * 3 + 3 <= l; j++)
                if (cfg[o + 6 + j * 3] == 0x22) k->report_len = le16(cfg + o + 7 + j * 3);
        } else if (t == 5 && l >= 7) {
            uint8_t addr = cfg[o + 2], attr = cfg[o + 3] & 3;
            uint16_t mps = le16(cfg + o + 4); int use = 0;
            if (curif == 1 && k && !k->ep && attr == 3 && (addr & 0x80)) { k->ep = addr; use = 1; }
            else if (curif == 2 && !d->mouse_ep && attr == 3 && (addr & 0x80)) { d->mouse_ep = addr; use = 1; }
            else if (curif == 3 && attr == 2) {
                if ((addr & 0x80) && !d->msc_in) { d->msc_in = addr; use = 1; }
                else if (!(addr & 0x80) && !d->msc_out) { d->msc_out = addr; use = 1; }
            }
            if (use) {
                unsigned index = (addr & 15) * 2 + !!(addr & 0x80);
                if (n == 8) { err = -ENODEV; goto fail; }
                if ((seen_eps & (1u << index)) || !endpoint_valid(speed, addr, attr, mps, cfg[o + 6])) goto fail;
                seen_eps |= 1u << index;
                eps[n++] = (struct usb_ep){ addr, attr, mps, cfg[o + 6] };
                companion = speed == USB_SS;
            }
        }
        o += l;
    }
    if (companion) goto fail;
    if (!n) { err = -ENODEV; goto fail; }
    if (xhci_configure(p, eps, n) || ctrl(p, 0x00, 9, cfg[5], 0, 0, 0)) goto fail;
    err = -ENODEV;
    for (unsigned i = 0; i < d->nkbd; i++) {
        k = d->keyboards[i];
        if (!k->ep) continue;
        if (k->boot && ctrl(p, 0x21, 0x0B, 0, k->iface, 0, 0) == 0) {
            k->active = 1;
        } else if (k->report_len && k->report_len <= 4096) {
            uint8_t *report = kmalloc(k->report_len);
            if (report) {
                int got = ctrl(p, 0x81, 6, 0x2200, k->iface, report, k->report_len);
                if (got != k->report_len) err = -EIO;
                else if (!hid_keyboard_parse(&k->layout, report, k->report_len)) {
                    k->boot = 0; k->active = 1;
                }
                kfree(report);
            } else err = -ENOMEM;
        } else if (k->boot) {
            err = -EIO;
        }
        if (k->active) {
            ctrl(p, 0x21, 0x0A, 0, k->iface, 0, 0);
            if (xhci_interrupt_start(p, k->ep)) { k->active = 0; err = -EIO; }
            else { activated++; kprintf("[usb] port %d: keyboard interface %u\n", p, k->iface); }
        }
    }
    if (d->mouse_ep) {
        if (ctrl(p, 0x21, 0x0B, 0, d->mouse_if, 0, 0) || xhci_interrupt_start(p, d->mouse_ep)) {
            d->mouse_ep = 0; err = -EIO;
        } else {
            activated++;
            kprintf("[usb] port %d: mouse\n", p);
        }
    }
    if (d->msc_in && d->msc_out) {
        int r = msc_init(d);
        if (!r) activated++;
        else if (r != -ENODEV) err = r;
    }
    if (!activated) goto fail;
    kfree(cfg);
    return 0;
fail:
    if (cfg) kfree(cfg);
    usb_detach(p);
    return err;
}
void usb_detach(int p) {
    if (p < 1 || p > USB_MAX_PORTS) return;
    struct usbdev *d = &devs[p];
    for (unsigned i = 0; i < d->nkbd; i++) d->keyboards[i]->active = 0;
    kbd_merge(d);
    for (unsigned i = 0; i < d->nkbd; i++) kfree(d->keyboards[i]);
    for (unsigned b = 0; b < 3; b++) if (d->prev_btn & (1u << b)) input_btn(b, 0);
    if (d->vol) { fat_unmount(d->vol); kprintf("[usb] port %d: /media/usb%d unmounted\n", p, d->unit); }
    memset(d, 0, sizeof *d);
}

void usb_poll(void) {
    if (!vfs_ready()) return;                             /* volumes wait until the namespace exists */
    for (int p = 1; p <= USB_MAX_PORTS; p++) {
        struct usbdev *d = &devs[p];
        if (d->attached && d->repeat_code && ticks >= d->repeat_at) {
            kbd_key_source((unsigned)p, d->repeat_code, 1);
            d->repeat_at = ticks + (HZ + 24) / 25;
        }
        if (!d->attached || !d->mount_pending) continue;
        d->mount_pending = 0;
        int u = unit_alloc();
        if (u < 0) { kprintf("[usb] port %d: no free /media slot\n", p); continue; }
        d->unit = u; d->unit_set = 1;
        char dir[24];
        ksnprintf(dir, sizeof dir, "/media/usb%d", u);
        void *h = 0;
        int r = fat_mount(dir, msc_rd, d, d->nblk, &h);
        if (r) { d->unit_set = 0; kprintf("[usb] port %d: no FAT32 or exFAT volume (%d)\n", p, r); }
        else d->vol = h;
    }
}
