#include "kernel.h"
#include "usb.h"
#include "sd.h"

#define VERSION "0.112"

const char *os_version(void) { return VERSION; }
/* found by 'update' / POST /api/update in a kernel8.img: only a RoboOS kernel may replace the running one */
__attribute__((used)) const char kernel_tag[] = "ROBOOS-KERNEL:" VERSION ":AARCH64";

/* /SCREEN.TXT: "1280x720", or "auto" = the monitor's own resolution (0 x 0) */
void screen_choice(u32 *w, u32 *h)
{
    struct fat_file f;
    char b[24];
    if (fat_open("/SCREEN.TXT", &f) || f.is_dir) return;
    int n = fat_read(&f, b, sizeof(b) - 1);
    if (n <= 0) return;
    b[n] = 0;
    if (b[0] == 'a' || b[0] == 'A') { *w = 0; *h = 0; return; }
    u32 x = 0, y = 0;
    const char *p = b;
    while (*p >= '0' && *p <= '9') x = x * 10 + (u32)(*p++ - '0');
    if (*p == 'x' || *p == 'X') p++;
    while (*p >= '0' && *p <= '9') y = y * 10 + (u32)(*p++ - '0');
    if (x >= 320 && y >= 200 && x <= 2048 && y <= 1280) { *w = x; *h = y; }
}

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

    /* the SD card first: /SCREEN.TXT picks the resolution, and a boot picture
     * (/SPLASH.PNG|JPG|BMP) can cover the screen while USB starts; the messages
     * wait in memory behind it */
    int sde = sd_init();
    int fs_ok = sde == SD_OK && fat_mount() == 0;
    u32 sw = 1024, sh = 768;
    if (fs_ok) screen_choice(&sw, &sh);
    int fb_ok = fb_init(sw, sh);
    if (fs_ok) img_splash_start();

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
    if (fs_ok) {
        status(1);
        kprintf("SD card: %s, %u MB (%s)\n", fat_type(), (u32)(fat_volume_bytes() >> 20), sd_controller());
        if (sd_sdhost_error()) {             /* the Arasan works, SDHOST did not (WiFi needs SDHOST for the card) */
            status(0);
            con_puts("SDHOST: "); sdhost_print_fail(sd_sdhost_error());
        }
    } else {
        status(0);
        con_puts("SD card: ");
        if (sde) sd_print_fail(sde); else con_puts("no FAT16/FAT32 partition\n");
    }
    wifi_boot();                         /* /WIFI.TXT: start the WiFi chip and join (core 0, before core 1) */
    bt_boot();                           /* /BT.TXT "auto on": Bluetooth at boot (console -> mini UART) */
    net_name();                          /* /NAME.TXT and /IP.TXT are read here, on core 0 */
    clock_prepare();                     /* /TIME.TXT */
    sensors_probe();                     /* I2C: BME280, ADS1115, DS3231 */
    sensors_log_boot();                  /* /SENSLOG.TXT: the sensor log */
    motion_boot();                       /* I2C 0x68/0x69: MPU-6050 (GY-521), /MOTION.TXT */
    thermal_boot();                      /* I2C 0x69: AMG8833 thermal camera */
    display_boot();                      /* I2C: OLED / LCD display (/DISPLAY.TXT) */
    doorbell_boot();                     /* /DOORBELL.TXT: doorbell button + door radar */
    radio_boot();                        /* I2C: RDA5807M FM radio (/RADIO.TXT) */
    { u64 rt = rtc_read(); if (rt) clock_set_time(rt, 3); }   /* the time before NTP answers */
    voice_boot();                        /* I2C 0x64: DFRobot offline voice module (/VOICE.TXT) */
    dcf_boot();                          /* /DCF.TXT: DCF77 receiver pin (core 1 samples it) */
    ip_prepare();
    mqtt_prepare();                      /* /MQTT.TXT */
    users_load();                        /* /USERS.TXT: logins for the web page + API */
    api_sound_scan();                    /* /SOUNDS for the JSON API, before core 1 starts */
    int io = smp_start_io();             /* core 1 takes over USB + network */
    status(io);
    if (io) con_puts("Core 1: USB + network\n");
    else    con_puts("Core 1 did not start - core 0 does USB + network\n");
    int wk = smp_start_workers();        /* cores 2 + 3: helpers (video) */
    status(wk == 2);
    kprintf("Cores 2-3: %d worker%s ready\n", wk, wk == 1 ? "" : "s");
    kprintf("  ARM memory: %u MB, booted in %u ms\n\n",
            (u32)(ram_end >> 20), (u32)(timer_us() / 1000));
    con_puts("  Type 'help' for a list of commands.\n\n");
    img_splash_end();                    /* the picture stays at least 2.5 s (a key ends it) */
    { u64 end = timer_us() + 20000;      /* what came in during boot (noise, keyboard junk) is dropped */
      while (timer_us() < end) con_trygetc(); }
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
    int core = smp_core();
    if (core) {                    /* core 1: core 0 takes its work over; 2-3: workers given up */
        smp_crashed(core);
        con_color(COL_RED);
        kprintf("\n!!! EXCEPTION on core %d: %s (EC=0x%x) ELR=0x%lx FAR=0x%lx\n", core, ec_name(ec), ec, elr, far);
        if (core == 1) kprintf("    core 1 stopped; core 0 now does USB and network itself\n");
        else           kprintf("    core %d stopped; its work is done on core 0 now\n", core);
        con_color(COL_DEFAULT);
        for (;;) __asm__ volatile("wfe");
    }
    con_color(COL_RED);
    kprintf("\n!!! EXCEPTION #%u: %s (EC=0x%x)\n", (u32)type, ec_name(ec), ec);
    kprintf("    ESR=0x%lx  ELR=0x%lx  FAR=0x%lx\n", esr, elr, far);
    con_color(COL_DEFAULT);
    app_crashed();                 /* inside a program: back to the loader */
}
