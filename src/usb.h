#ifndef USB_H
#define USB_H
#include "kernel.h"

#define USB_SPEED_HS 0
#define USB_SPEED_FS 1
#define USB_SPEED_LS 2

#define USB_OK      0
#define USB_NAK    -1
#define USB_STALL  -2
#define USB_ERR    -3
#define USB_TIMEOUT -4

#define EP_CONTROL   0
#define EP_BULK      2
#define EP_INTERRUPT 3

#define PID_DATA0 0
#define PID_DATA1 2
#define PID_SETUP 3

struct usb_dev {
    u8  used, addr, speed, mps0;
    u8  tt_hub, tt_port;          /* transaction translator (for splits) */
    u8  parent, port, depth;
    u8  dev_class, is_hub, nports;
    u16 vid, pid;
    u8  nif, ifc[4][3];           /* interface class/subclass/protocol */
    u8  dead;                     /* hub stopped responding */
    u8  tries[8];                 /* attach attempts per port (hot-plug) */
    u16 pst[8];                   /* last seen hub port status */
    char name[40];
};

/* enumeration failure records (diagnostics) */
struct usb_fail { u8 hub, port, speed, stage; int code; u32 hcint; char what[24]; };
#define USB_MAX_FAILS 8
extern struct usb_fail usb_fails[USB_MAX_FAILS];
extern int usb_nfails;
void usb_print_tree(void);
u32  usb_last_hcint(void);
void usb_hotplug(void);          /* call periodically */
void kbd_selftest(u32 ms);

#define USB_MAX_DEVS 16
extern struct usb_dev usb_devs[USB_MAX_DEVS];

int  usb_init(void);              /* returns device count, <0 on error */
int  usb_control(struct usb_dev *d, u8 reqtype, u8 req, u16 val, u16 idx, void *data, u16 len);
int  usb_interrupt_in(struct usb_dev *d, u8 ep, u16 mps, u8 *pid, void *data, u16 len);
/* bulk transfer to/from a device that needs no split transactions (HS, or FS/LS on the
 * root port); len up to 64 KB, IN ends on a short packet. Returns bytes or USB_xxx. */
int  usb_bulk(struct usb_dev *d, u8 ep, int in, u8 *pid, void *data, u32 len, u32 ms);
/* bulk IN left running on channel 1 between calls (devices that NAK when empty) */
int  usb_bulk_in_async(struct usb_dev *d, u8 ep, u8 *pid, void *data, u32 len);
void usb_bulk_in_cancel(void);
const char *usb_speed_name(int s);
extern int usb_debug;

/* keyboard (kbd.c) */
void kbd_reset(void);
int  kbd_attach(struct usb_dev *d, const u8 *cfg, int cfglen);
int  kbd_present(void);
int  kbd_is(const struct usb_dev *d);   /* is d the active keyboard? */
const char *kbd_name(void);
void kbd_poll(void);
int  kbd_getc(void);              /* -1 if nothing */
const char *kbd_mode(void);

/* Ethernet (eth.c) - LAN9514 */
struct eth_stats { u32 rx_ok, rx_bad, rx_empty, rx_err, tx_ok, tx_err; int last_err; u32 last_hcint; };
extern struct eth_stats eth_stat;
void eth_reset(void);
int  eth_attach(struct usb_dev *d, const u8 *cfg, int cfglen);   /* 1 ok, 0 not ours, -1 failed */
int  eth_present(void);
int  eth_is(const struct usb_dev *d);
const u8 *eth_mac(void);
u32  eth_chip(void);
const char *eth_chip_name(void);                 /* "LAN9514 (SMSC)" / "LAN7515 (LAN7800)" */
const char *eth_error(int *code, u32 *hcint);    /* 0 = fine, else the failed init step */
int  eth_link(int *full);                        /* 0 = down, else 10 / 100 / 1000 */
int  eth_set_gigabit(int on);                    /* LAN7515 only; -1 = not this chip */
int  eth_gigabit(void);
int  eth_link_cached(void);                      /* last known speed, no USB traffic */
int  eth_send(const void *frame, int len);       /* frame = dst, src, type, payload */
int  eth_recv(void *buf, int max);               /* length, 0 = nothing, <0 = error */

#endif
