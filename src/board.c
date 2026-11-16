/*
 * Board detection - one kernel8.img for every BCM2837-based board:
 * Raspberry Pi 3B / 3B+ / 3A+, Zero 2 W, Pi 2B v1.2, CM3 / CM3+.
 * The differences that matter to RoboOS are the ACT LED and the USB topology.
 */
#include "kernel.h"

static u32 rev;
static int type = -1;

/* ACT LED: GPIO number, or -1 = via the firmware GPIO expander (pin 130) */
static int led_pin = -1, led_active_low;

void board_init(void)
{
    u32 v[1] = { 0 };
    mbox_prop(0x00010002, v, 1);
    rev = v[0];
    type = (rev & (1 << 23)) ? (int)((rev >> 4) & 0xFF) : -1;

    switch (type) {
    case 0x04: led_pin = 47; led_active_low = 0; break;  /* Pi 2B v1.2 */
    case 0x0D: case 0x0E: led_pin = 29; break;           /* 3B+, 3A+ */
    case 0x12: led_pin = 29; led_active_low = 1; break;  /* Zero 2 W */
    default:   led_pin = -1; break;                      /* 3B, CM3: firmware expander */
    }
}

u32 board_rev(void)  { return rev; }
int board_type(void) { return type; }

/* boards whose USB port is wired straight to the SoC (no LAN9514/LAN7515 hub) */
int board_usb_direct(void) { return type == 0x12 || type == 0x0E; }

const char *board_name(void)
{
    switch (type) {
    case 0x04: return "Raspberry Pi 2 Model B v1.2";
    case 0x08: return "Raspberry Pi 3 Model B";
    case 0x0A: return "Compute Module 3";
    case 0x0D: return "Raspberry Pi 3 Model B+";
    case 0x0E: return "Raspberry Pi 3 Model A+";
    case 0x10: return "Compute Module 3+";
    case 0x12: return "Raspberry Pi Zero 2 W";
    case -1:   return "Raspberry Pi (old-style revision)";
    default:   return "unknown model";
    }
}

void board_led(int on)
{
    if (led_pin >= 0) {
        gpio_func(led_pin, 1);
        gpio_set(led_pin, led_active_low ? !on : on);
    } else {
        u32 v[2] = { 130, (u32)on };
        mbox_prop(0x00038041, v, 2);
    }
}

/* short host name for the shell prompt */
const char *board_host(void)
{
    switch (type) {
    case 0x04: return "rpi2";
    case 0x12: return "zero2";
    case 0x0D: return "rpi3b+";
    case 0x0E: return "rpi3a+";
    case 0x0A: case 0x10: return "cm3";
    default:   return "rpi3";
    }
}
