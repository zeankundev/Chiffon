/* USB host stack for x86_64.  xhci.c is the host controller (rings, slots, transfers, root-port hotplug);
 * usb.c is the device side (descriptors, HID boot keyboards and mice, bulk-only mass storage).  Everything
 * runs from the timer tick: no USB interrupts, and no sleeping, because kernel code runs with interrupts
 * masked.  A synchronous step spins on the event ring with a bounded count. */
#pragma once
#include <stdint.h>
#include <stddef.h>

enum usb_speed { USB_FS = 1, USB_LS = 2, USB_HS = 3, USB_SS = 4 };      /* same encoding as the xHCI PORTSC speed field */

struct usb_setup { uint8_t bmRequestType, bRequest; uint16_t wValue, wIndex, wLength; } __attribute__((packed));

/* one endpoint from a configuration descriptor */
struct usb_ep { uint8_t addr, attr; uint16_t mps; uint8_t interval; };

#define USB_MAX_PORTS 16

/* usb.c, called by xhci.c */
void usb_attach(int port, int speed);                                    /* slot addressed; enumerate the device */
void usb_detach(int port);                                               /* device removed */
void usb_report(int port, uint8_t ep_addr, const uint8_t *data, uint32_t len);   /* interrupt IN data */
void usb_poll(void);                                                     /* mount media waiting for the VFS */

/* xhci.c services for the device classes; all synchronous.  Return bytes moved, or -errno. */
int  xhci_control(int port, const struct usb_setup *s, void *buf);       /* EP0; buf may be NULL when wLength is 0 */
int  xhci_set_ep0_mps(int port, uint16_t mps);
int  xhci_configure(int port, const struct usb_ep *eps, int n);          /* Configure Endpoint for the given endpoints */
int  xhci_bulk(int port, uint8_t ep_addr, void *buf, uint32_t len);
int  xhci_interrupt_start(int port, uint8_t ep_addr);                    /* keep an IN interrupt endpoint polled */
