#include "kernel.h"
#include "usb.h"
#include "sd.h"

#define VERSION "0.15"

const char *os_version(void) { return VERSION; }

static void status(int good)
{
    con_color(COL_GRAY);  con_puts("  [");
    con_color(good ? COL_GREEN : COL_YELLOW); con_puts(good ? " OK " : "WARN");
    con_color(COL_GRAY);  con_puts("] ");
    con_color(COL_DEFAULT);
}

static void banner(void)
{
    con_color(COL_CYAN);
    con_puts("\n"
             "   ____       _            ___  ____  \n"
             "  |  _ \\ ___ | |__   ___  / _ \\/ ___| \n"
             "  | |_) / _ \\| '_ \\ / _ \\| | | \\___ \\ \n"
             "  |  _ < (_) | |_) | (_) | |_| |___) |\n"
             "  |_| \\_\\___/|_.__/ \\___/ \\___/|____/ \n\n");
    con_color(COL_GRAY);
    con_puts("  bare-metal mini OS for Raspberry Pi  -  version " VERSION "\n");
    con_puts("  running on ");
    con_puts(board_name());
    con_puts("\n\n");
    con_color(COL_DEFAULT);
}

void kernel_main(void)
{
    uart_init();
    board_init();

    /* find the end of ARM RAM (the rest belongs to the GPU) */
    u32 mem[2] = { 0, 0 };
    mbox_prop(0x00010005, mem, 2);
    u64 ram_end = (u64)mem[0] + mem[1];
    if (ram_end == 0) ram_end = 0x3B400000;
    mmu_init(ram_end);

    int fb_ok = fb_init(1024, 768);

    banner();
    status(1); con_puts("UART0 (PL011) 115200 8N1 on GPIO14/15\n");
    status(1); con_puts("MMU + caches enabled\n");
    status(fb_ok);
    if (fb_ok) kprintf("HDMI framebuffer %ux%u\n", fb_width(), fb_height());
    else       con_puts("HDMI not available, UART only\n");
    int nusb = usb_init();
    status(nusb >= 0 && kbd_present());
    if (nusb < 0)           con_puts("USB controller failed to initialize\n");
    else if (kbd_present()) kprintf("USB keyboard: %s\n", kbd_name());
    else                    con_puts("No USB keyboard found (you can plug one in now)\n");
    if (eth_present()) {
        int code; u32 hc;
        const char *bad = eth_error(&code, &hc);
        const u8 *m = eth_mac();
        status(!bad);
        if (bad) kprintf("Ethernet: init failed at %s (%d, HCINT %x)\n", bad, code, hc);
        else kprintf("Ethernet: %s, MAC %02x:%02x:%02x:%02x:%02x:%02x\n", eth_chip_name(), m[0], m[1], m[2], m[3], m[4], m[5]);
    }
    int sde = sd_init();
    if (sde == SD_OK && fat_mount() == 0) {
        status(1);
        kprintf("SD card: %s, %u MB\n", fat_type(), (u32)(fat_volume_bytes() >> 20));
    } else {
        status(0);
        con_puts("SD card: ");
        if (sde) sd_print_fail(sde); else con_puts("no FAT16/FAT32 partition\n");
    }
    kprintf("  ARM memory: %u MB, booted in %u ms\n\n",
            (u32)(ram_end >> 20), (u32)(timer_us() / 1000));
    con_puts("  Type 'help' for a list of commands.\n\n");
    app_autostart();

    shell_run();
}

static const char *ec_name(u32 ec)
{
    switch (ec) {
    case 0x00: return "undefined instruction";
    case 0x15: return "SVC";
    case 0x20: case 0x21: return "instruction abort";
    case 0x22: return "PC alignment fault";
    case 0x24: case 0x25: return "data abort";
    case 0x26: return "SP alignment fault";
    case 0x3C: return "BRK";
    default:   return "other";
    }
}

void exc_handler(u64 type, u64 esr, u64 elr, u64 far)
{
    u32 ec = (esr >> 26) & 0x3F;
    con_color(COL_RED);
    kprintf("\n!!! EXCEPTION #%u: %s (EC=0x%x)\n", (u32)type, ec_name(ec), ec);
    kprintf("    ESR=0x%lx  ELR=0x%lx  FAR=0x%lx\n", esr, elr, far);
    con_color(COL_DEFAULT);
    app_crashed();                 /* inside a program: back to the loader */
}
