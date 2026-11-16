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

#endif
