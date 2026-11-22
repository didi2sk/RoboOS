/* PC test of src/voice.c: a fake DF2301Q (reg 0x02 = last id, cleared on read), /VOICE.TXT in memory,
 * commands run only at the prompt, a heard id waits while not at the prompt, unknown ids are reported.
 *   cc -O1 -w -o /tmp/test_voice tools/test_voice.c && /tmp/test_voice */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
typedef uint8_t u8; typedef uint32_t u32; typedef uint64_t u64;
#define KERNEL_H
#define VOICE_HOST_TEST
static u64 now_us; static u64 timer_us(void) { return now_us; }
#define kprintf printf
static int parse_num(const char *s, u64 *o) { char *e; *o = strtoull(s, &e, 0); return e != s && !*e; }
static int io_held(void) { return 0; }
static u8 next_id; static u8 regs[8];
static int i2c_read_reg(u8 a, u8 r, void *b, int n) { if (a != 0x64) return -1; *(u8 *)b = r == 2 ? next_id : 0; if (r == 2) next_id = 0; return 0; }
static int i2c_write_reg(u8 a, u8 r, const void *b, int n) { if (a != 0x64) return -1; regs[r & 7] = *(const u8 *)b; return 0; }
static char ran[8][64]; static int nran;
static void shell_execute(const char *c) { strcpy(ran[nran++], c); }
struct fat_file { int is_dir; int pos; };
static char card[2048]; static int card_len = -1;
static int fat_mounted(void) { return 1; }
static int fat_open(const char *p, struct fat_file *f) { f->is_dir = 0; f->pos = 0; return card_len < 0; }
static int fat_read(struct fat_file *f, void *b, u32 n) { int k = card_len < (int)n ? card_len : (int)n; memcpy(b, card, (size_t)k); return k; }
static int fat_write_file(const char *p, const void *d, u32 n) { memcpy(card, d, n); card_len = (int)n; return 0; }
#include "../src/voice.c"
static int errors; static void check(const char *w, int ok) { printf("  %-46s %s\n", w, ok ? "ok" : "WRONG"); if (!ok) errors++; }
static void redraw(void) {}
static void tick(int n) { for (int i = 0; i < n; i++) { now_us += 100000; voice_poll(); } }
int main(void)
{
    strcpy(card, "5 gpio 17 1\r\n103 radio on\n\nbad line\n"); card_len = (int)strlen(card);
    voice_boot();
    check("found + 2 actions loaded", present && nmap == 2);
    voice_at_prompt(1, redraw);
    next_id = 5; tick(2);
    check("id 5 -> gpio 17 1", nran == 1 && !strcmp(ran[0], "gpio 17 1"));
    next_id = 103; tick(2);
    check("id 103 -> radio on", nran == 2 && !strcmp(ran[1], "radio on"));
    next_id = 40; tick(2);
    check("unknown id runs nothing", nran == 2);
    voice_at_prompt(0, 0);
    next_id = 5; tick(5);
    check("not at the prompt: waits", nran == 2);
    voice_at_prompt(1, redraw); tick(1);
    check("back at the prompt: runs", nran == 3);
    voice_at_prompt(0, 0); next_id = 5; tick(120); voice_at_prompt(1, redraw); tick(1);
    check("dropped after 10 s away from the prompt", nran == 3);
    char *a1[] = { "voice", "add", "7", "display", "text", "1", "ahoj" }; voice_cmd(7, a1);
    char *a2[] = { "voice", "del", "5" }; voice_cmd(3, a2);
    nmap = 0; load();
    check("saved file: 103 + 7, no 5", nmap == 2 && find(103) && find(7) && !find(5) && !strcmp(find(7), "display text 1 ahoj"));
    char *a3[] = { "voice", "volume", "4" }; voice_cmd(3, a3);
    check("volume -> reg 5", regs[5] == 4);
    printf(errors ? "\n%d ERRORS\n" : "\nall OK\n", errors);
    return errors != 0;
}
