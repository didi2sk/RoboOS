/*
 * voice.c - DFRobot Gravity offline voice recognition module (DF2301Q, SKU SEN0539) on I2C (0.80).
 *
 * The module recognises by itself (no network): a wake word ("Hello robot" or a learned one), 121 fixed
 * English commands and up to 17 learned ones. Over I2C (address 0x64):
 *   reg 0x02  read: the id of the last recognised command (0 = nothing new)
 *   reg 0x03  write: play the module's answer for an id
 *   reg 0x04  write: mute 1 / 0     reg 0x05  write: volume 1-7     reg 0x06  write: wake time (s)
 * RoboOS reads reg 0x02 every 100 ms (core 0, from con_trygetc) and runs the shell command that
 * /VOICE.TXT gives for that id ("<id> <command line>", e.g. "5 gpio 17 1" or "6 radio off").
 * Commands run only at the shell prompt (not inside a program / BASIC); one heard meanwhile waits up to 10 s.
 */
#include "kernel.h"
#ifndef VOICE_HOST_TEST
#include "sd.h"
#endif

#define VADDR 0x64
#define MAXMAP 32

static int present, last_id, busy;
static u32 n_heard;
static u64 next_poll, pend_at;
static int pend_id = -1;
static struct { u8 id; char cmd[56]; } map[MAXMAP];
static int nmap;
static volatile int at_prompt;
static void (*reprompt)(void);

void voice_at_prompt(int on, void (*redraw)(void)) { at_prompt = on; reprompt = redraw; }

static void load(void)
{
    struct fat_file f;
    static char b[2048];
    nmap = 0;
    if (!fat_mounted() || fat_open("/VOICE.TXT", &f) || f.is_dir) return;
    int n = fat_read(&f, b, sizeof(b) - 1);
    if (n <= 0) return;
    b[n] = 0;
    for (char *l = b; *l && nmap < MAXMAP; ) {
        char *e = l; while (*e && *e != '\n' && *e != '\r') e++;
        char sv = *e; *e = 0;
        int id = 0; char *p = l;
        while (*p >= '0' && *p <= '9') id = id * 10 + (*p++ - '0');
        if (p != l && *p == ' ' && id > 0 && id < 256) {
            while (*p == ' ') p++;
            int k = 0; while (p[k] && k < 55) { map[nmap].cmd[k] = p[k]; k++; }
            map[nmap].cmd[k] = 0; map[nmap].id = (u8)id;
            if (k) nmap++;
        }
        *e = sv; l = *e ? e + 1 : e; while (*l == '\n' || *l == '\r') l++;
    }
}

static void save(void)
{
    static char b[2048];
    int n = 0;
    for (int i = 0; i < nmap; i++) {
        int id = map[i].id;
        if (id >= 100) b[n++] = (char)('0' + id / 100);
        if (id >= 10) b[n++] = (char)('0' + id / 10 % 10);
        b[n++] = (char)('0' + id % 10);
        b[n++] = ' ';
        for (const char *c = map[i].cmd; *c && n < (int)sizeof(b) - 2; ) b[n++] = *c++;
        b[n++] = '\n';
    }
    if (fat_mounted()) fat_write_file("/VOICE.TXT", b, (u32)n);
}

static const char *find(int id) { for (int i = 0; i < nmap; i++) if (map[i].id == id) return map[i].cmd; return 0; }

void voice_boot(void)
{
    u8 id;
    present = i2c_read_reg(VADDR, 0x02, &id, 1) == 0;
    if (!present) return;
    load();
    kprintf("voice module (DF2301Q) found, %d commands in /VOICE.TXT\n", nmap);
}

static void run(int id)
{
    const char *c = find(id);
    if (!c) { kprintf("\n[VOICE] heard command %d (no action: 'voice add %d <command>')\n", id, id); if (reprompt) reprompt(); return; }
    kprintf("\n[VOICE] %d -> %s\n", id, c);
    char line[64];
    int k = 0; while (c[k] && k < 63) { line[k] = c[k]; k++; } line[k] = 0;
    shell_execute(line);
    if (reprompt) reprompt();
}

void voice_poll(void)                                   /* core 0, con_trygetc */
{
    if (!present || busy || io_held()) return;
    u64 now = timer_us();
    if (now < next_poll) return;
    next_poll = now + 100000;
    busy = 1;
    u8 id = 0;
    if (i2c_read_reg(VADDR, 0x02, &id, 1) == 0 && id) {
        last_id = id; n_heard++;
        pend_id = id; pend_at = now;
    }
    if (pend_id > 0) {
        if (at_prompt) { int i = pend_id; pend_id = -1; run(i); }
        else if (now - pend_at > 10000000) pend_id = -1;   /* a program ran all that time: drop it */
    }
    busy = 0;
}

static void wr(u8 reg, u8 v) { i2c_write_reg(VADDR, reg, &v, 1); }

void voice_cmd(int argc, char **argv)
{
    const char *a = argc > 1 ? argv[1] : "";
    if (!*a || !strcmp(a, "scan")) {
        if (!*a || !present) { u8 id; present = i2c_read_reg(VADDR, 0x02, &id, 1) == 0; if (present && !nmap) load(); }
        if (!present) kprintf("no voice module (DFRobot DF2301Q at I2C 0x64; set its switch to I2C, power 3.3-5 V)\n");
        else kprintf("voice module DF2301Q at 0x64: heard %u commands, last id %d\n", n_heard, last_id);
        kprintf("  say the wake word (\"Hello robot\"), then a command; the ids are in the module's list\n");
        kprintf("  (1 = wake word, 5-21 = learned commands, 22+ = fixed English ones)\n");
        if (!nmap) kprintf("  no actions yet: 'voice add 5 gpio 17 1' runs that when command 5 is heard\n");
        for (int i = 0; i < nmap; i++) kprintf("  %3d -> %s\n", map[i].id, map[i].cmd);
        return;
    }
    if (!strcmp(a, "add") && argc > 3) {
        u64 id;
        if (!parse_num(argv[2], &id) || id < 1 || id > 255) { kprintf("voice add <id 1-255> <command>\n"); return; }
        char cmd[56]; int k = 0;
        for (int i = 3; i < argc && k < 54; i++) {
            if (i > 3) cmd[k++] = ' ';
            for (const char *c = argv[i]; *c && k < 55; ) cmd[k++] = *c++;
        }
        cmd[k] = 0;
        int i = 0; while (i < nmap && map[i].id != id) i++;
        if (i == nmap) { if (nmap >= MAXMAP) { kprintf("too many (max %d)\n", MAXMAP); return; } nmap++; }
        map[i].id = (u8)id;
        for (k = 0; cmd[k]; k++) map[i].cmd[k] = cmd[k];
        map[i].cmd[k] = 0;
        save();
        kprintf("%d -> %s (saved in /VOICE.TXT)\n", (int)id, map[i].cmd);
        return;
    }
    if (!strcmp(a, "del") && argc > 2) {
        u64 id;
        if (!parse_num(argv[2], &id)) return;
        for (int i = 0; i < nmap; i++) if (map[i].id == id) { map[i] = map[--nmap]; save(); kprintf("removed %d\n", (int)id); return; }
        kprintf("%d has no action\n", (int)id);
        return;
    }
    u64 v;
    if (!present) { kprintf("no voice module found ('voice' to look again)\n"); return; }
    if (!strcmp(a, "volume") && argc > 2 && parse_num(argv[2], &v) && v >= 1 && v <= 7) { wr(0x05, (u8)v); kprintf("volume %d\n", (int)v); return; }
    if (!strcmp(a, "mute") && argc > 2) { wr(0x04, !strcmp(argv[2], "on")); kprintf("mute %s\n", argv[2]); return; }
    if (!strcmp(a, "wake") && argc > 2 && parse_num(argv[2], &v) && v <= 255) { wr(0x06, (u8)v); kprintf("awake for %d s after the wake word\n", (int)v); return; }
    if (!strcmp(a, "play") && argc > 2 && parse_num(argv[2], &v) && v <= 255) { wr(0x03, (u8)v); return; }
    kprintf("usage: voice                      status, heard commands, actions\n"
            "       voice add <id> <command>   run a shell command when the id is heard (/VOICE.TXT)\n"
            "       voice del <id>\n"
            "       voice volume 1-7 | mute on|off | wake <s> | play <id>\n");
}
