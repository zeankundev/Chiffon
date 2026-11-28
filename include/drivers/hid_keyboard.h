/* Bounded USB HID keyboard report descriptor parser. No allocation or I/O. */
#pragma once
#include <stdint.h>
#include <stddef.h>

#define HID_KEYBOARD_MAX_REPORTS 16
#define HID_KEYBOARD_MAX_FIELDS 128
#define HID_KEYBOARD_MAX_BITS 4096

struct hid_keyboard_keys { uint8_t bits[32]; };
struct hid_keyboard_field {
    uint16_t offset, count, usage_min, usage_max;
    uint16_t list_start, list_count;
    int32_t logical_min;
    uint8_t size, report_id, variable;
};
struct hid_keyboard_layout {
    struct hid_keyboard_field fields[HID_KEYBOARD_MAX_FIELDS];
    uint16_t report_bits[256];
    uint8_t report_ids[HID_KEYBOARD_MAX_REPORTS];
    uint8_t report_count, field_count, numbered;
    uint16_t list_used, array_usages[256];
};

struct hid_kbd_global { uint32_t page, size, count, id; int32_t logical_min; };
struct hid_kbd_local { uint32_t usages[64], min, max; uint8_t n, has_min, has_max; };

static inline uint32_t hid_kbd_usage(uint32_t value, unsigned bytes, uint32_t page)
{
    return bytes == 4 ? value : ((page << 16) | value);
}

/* Reject malformed, oversized, or unsupported keyboard layouts. Input offsets include
 * all fields in each report, including fields belonging to other collections. */
static inline int hid_keyboard_parse(struct hid_keyboard_layout *out, const uint8_t *desc, size_t len)
{
    struct hid_kbd_global g = {0}, stack[4];
    struct hid_kbd_local local = {0};
    uint8_t collection[16] = {0}, depth = 0, sp = 0, saw_unnumbered = 0;
    size_t pos = 0;
    unsigned i;
    if (!out || !desc) return -1;
    for (i = 0; i < 256; i++) out->report_bits[i] = 0;
    out->report_count = out->field_count = out->numbered = out->list_used = 0;
    while (pos < len) {
        uint8_t prefix = desc[pos++];
        unsigned size, type, tag;
        uint32_t value = 0;
        int32_t signed_value;
        if (prefix == 0xfe) {
            unsigned n;
            if (len - pos < 2) return -1;
            n = desc[pos]; pos += 2;
            if (len - pos < n) return -1;
            pos += n; continue;
        }
        size = prefix & 3; if (size == 3) size = 4;
        type = (prefix >> 2) & 3; tag = prefix >> 4;
        if (len - pos < size) return -1;
        for (i = 0; i < size; i++) value |= (uint32_t)desc[pos++] << (8 * i);
        signed_value = (int32_t)value;
        if (size && size < 4 && (value & (1u << (size * 8 - 1))))
            signed_value = (int32_t)(value | (~0u << (size * 8)));
        if (type == 1) {
            switch (tag) {
            case 0: if (value > 0xffff) return -1; g.page = value; break;
            case 1: g.logical_min = signed_value; break;
            case 7: g.size = value; break;
            case 8:
                if (!value || value > 255 || saw_unnumbered) return -1;
                g.id = value; out->numbered = 1; break;
            case 9: g.count = value; break;
            case 10: if (sp == 4) return -1; stack[sp++] = g; break;
            case 11: if (!sp) return -1; g = stack[--sp]; break;
            default: break;
            }
        } else if (type == 2) {
            switch (tag) {
            case 0:
                if (local.n == 64) return -1;
                local.usages[local.n++] = hid_kbd_usage(value, size, g.page); break;
            case 1: local.min = hid_kbd_usage(value, size, g.page); local.has_min = 1; break;
            case 2: local.max = hid_kbd_usage(value, size, g.page); local.has_max = 1; break;
            default: break;
            }
        } else if (type == 0) {
            if (tag == 10) { /* Collection: recognize a Keyboard application, including nested collections. */
                uint32_t usage = local.n ? local.usages[0] : (local.has_min ? local.min : 0);
                uint8_t parent = depth ? collection[depth - 1] : 0;
                if (depth == 16) return -1;
                collection[depth++] = parent || (value == 1 && (usage == 0x00010006u || usage == 0x00010007u));
            } else if (tag == 12) {
                if (!depth) return -1;
                depth--;
            } else if (tag == 8) { /* Input */
                uint32_t start, total;
                int keyboard = depth && collection[depth - 1];
                if (!g.id) { if (out->numbered) return -1; saw_unnumbered = 1; }
                if (g.size > 32 || g.count > HID_KEYBOARD_MAX_BITS ||
                    (g.size && g.count > HID_KEYBOARD_MAX_BITS / g.size)) return -1;
                total = g.size * g.count; start = out->report_bits[g.id];
                if (total > HID_KEYBOARD_MAX_BITS - start) return -1;
                out->report_bits[g.id] = (uint16_t)(start + total);
                if (keyboard && !(value & 1) && g.size && g.count) {
                    uint32_t j;
                    int variable = !!(value & 2);
                    if (local.has_min != local.has_max ||
                        (local.has_min && (local.min > local.max || (local.min >> 16) != (local.max >> 16)))) return -1;
                    for (j = 0; j < (variable ? g.count : 1); j++) {
                        uint32_t u, end, list_count = 0;
                        struct hid_keyboard_field *f;
                        if (variable) {
                            if (j < local.n) u = local.usages[j];
                            else if (local.has_min && j - local.n <= local.max - local.min) u = local.min + j - local.n;
                            else continue;
                            end = u;
                        } else {
                            if (local.has_min && !local.n) { u = local.min; end = local.max; }
                            else if (local.n && !local.has_min) {
                                unsigned k = 0;
                                while (k < local.n && (local.usages[k] >> 16) != 7) k++;
                                if (k == local.n) continue;
                                u = end = local.usages[k];
                                list_count = local.n;
                            } else if (!local.n && !local.has_min) continue;
                            else return -1;
                        }
                        if ((u >> 16) != 7) continue;
                        /* NKRO keyboards often describe the whole usage page as one bitmap. */
                        if (variable && out->field_count) {
                            f = &out->fields[out->field_count - 1];
                            if (f->variable && f->report_id == g.id && f->size == g.size &&
                                (uint32_t)f->usage_max + 1 == (uint16_t)u &&
                                f->offset + (uint32_t)f->count * f->size == start + j * g.size) {
                                f->count++;
                                f->usage_max = (uint16_t)u;
                                continue;
                            }
                        }
                        if (out->field_count == HID_KEYBOARD_MAX_FIELDS) return -1;
                        f = &out->fields[out->field_count++];
                        f->offset = (uint16_t)(start + (variable ? j * g.size : 0));
                        f->count = (uint16_t)(variable ? 1 : g.count);
                        f->usage_min = (uint16_t)u; f->usage_max = (uint16_t)end;
                        f->list_start = out->list_used;
                        f->list_count = list_count;
                        if (list_count > 256u - out->list_used) return -1;
                        for (unsigned k = 0; k < list_count; k++)
                            out->array_usages[out->list_used++] =
                                (local.usages[k] >> 16) == 7 ? (uint16_t)local.usages[k] : 0;
                        f->logical_min = g.logical_min; f->size = (uint8_t)g.size;
                        f->report_id = (uint8_t)g.id; f->variable = (uint8_t)variable;
                    }
                }
            }
            local.n = local.has_min = local.has_max = 0;
        }
    }
    if (depth || sp || !out->field_count) return -1;
    for (i = 0; i < out->field_count; i++) {
        unsigned j; uint8_t id = out->fields[i].report_id;
        for (j = 0; j < out->report_count && out->report_ids[j] != id; j++) {}
        if (j == out->report_count) {
            if (j == HID_KEYBOARD_MAX_REPORTS) return -1;
            out->report_ids[out->report_count++] = id;
        }
    }
    return 0;
}

/* Decode one complete report. On rollover/error or truncation, leave keys unchanged.
 * For multiple report IDs, the caller retains each ID's state and unions the sets. */
static inline int hid_keyboard_decode(const struct hid_keyboard_layout *layout, const uint8_t *data,
                                      size_t len, struct hid_keyboard_keys *keys, uint8_t *report_id)
{
    struct hid_keyboard_keys result = {{0}};
    uint8_t id = 0;
    unsigned i, j;
    if (!layout || !data || !keys || !report_id) return -1;
    if (layout->numbered) { if (!len) return -1; id = *data++; len--; }
    for (i = 0; i < layout->report_count && layout->report_ids[i] != id; i++) {}
    if (i == layout->report_count || len < (size_t)((layout->report_bits[id] + 7u) / 8u)) return -1;
    for (i = 0; i < layout->field_count; i++) {
        const struct hid_keyboard_field *f = &layout->fields[i];
        if (f->report_id != id) continue;
        for (j = 0; j < f->count; j++) {
            uint32_t v = 0, bit = f->offset + j * f->size;
            unsigned k, usage;
            for (k = 0; k < f->size; k++)
                if (data[(bit + k) / 8] & (1u << ((bit + k) % 8))) v |= 1u << k;
            if (f->variable) { if (!v) continue; usage = f->usage_min + j; }
            else {
                int64_t raw = v;
                int64_t mapped;
                if (f->logical_min < 0 && f->size < 32 && (v & (1u << (f->size - 1))))
                    raw = (int32_t)(v | (~0u << f->size));
                else if (f->logical_min < 0 && f->size == 32) raw = (int32_t)v;
                mapped = raw - f->logical_min;
                if (mapped < 0) continue;
                if (f->list_count) {
                    if (mapped >= f->list_count) continue;
                    usage = layout->array_usages[f->list_start + mapped];
                } else {
                    mapped += f->usage_min;
                    if (mapped > f->usage_max) continue;
                    usage = (unsigned)mapped;
                }
                if (usage >= 1 && usage <= 3) return -1; /* HID ErrorRollOver, POSTFail, ErrorUndefined */
            }
            if (usage < 256 && usage) result.bits[usage / 8] |= (uint8_t)(1u << (usage % 8));
        }
    }
    *keys = result; *report_id = id;
    return 0;
}

