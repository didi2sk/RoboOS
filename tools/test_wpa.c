/*
 * PC test of src/wpa.c (the WPA2 supplicant). Line protocol on stdin / stdout, driven by
 * tools/test_wpa.py, which plays the access point with Python's own crypto:
 *   start <pass> <ssid> <own mac hex>   -> "ok"
 *   rx <frame hex>                      -> "used <out hex|-> ptk=<0/1> gtk=<0/1> <idx> <gtk hex|-> <tk hex>" or "drop <reason>"
 *   prf / pmk vectors are checked by "selftest"
 * build + run: tools/test_wpa.sh
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "wpa.h"

void wpa_random(unsigned char *out, int n) { for (int i = 0; i < n; i++) out[i] = (unsigned char)rand(); }

static int unhex(const char *s, unsigned char *o) { int n = 0; while (s[0] && s[1] && s[0] != '\n') { sscanf(s, "%2hhx", &o[n++]); s += 2; } return n; }
static void hex(const unsigned char *b, int n) { for (int i = 0; i < n; i++) printf("%02x", b[i]); }

int main(void)
{
    static struct wpa s;
    char line[8192];
    while (fgets(line, sizeof(line), stdin)) {
        if (!strncmp(line, "selftest", 8)) {
            unsigned char pmk[32], prf[64], k[20];
            wpa_pmk("password", "IEEE", 4, pmk);
            printf("pmk "); hex(pmk, 32); printf("\n");
            memset(k, 0x0b, 20);
            wpa_prf(k, 20, "prefix", (const unsigned char *)"Hi There", 8, prf, 64);
            printf("prf "); hex(prf, 64); printf("\n");
        } else if (!strncmp(line, "start ", 6)) {
            char pass[80], ssid[40], mac[20];
            unsigned char m[6], pmk[32];
            sscanf(line + 6, "%79s %39s %19s", pass, ssid, mac);
            unhex(mac, m);
            wpa_pmk(pass, ssid, (int)strlen(ssid), pmk);
            wpa_start(&s, pmk, m, 0, 0);
            printf("ok\n");
        } else if (!strncmp(line, "rx ", 3)) {
            static unsigned char f[4096];
            int n = unhex(line + 3, f);
            s.set_ptk = s.set_gtk = 0;
            if (!wpa_rx(&s, f, n)) { printf("drop %s\n", s.err ? s.err : "?"); }
            else {
                printf("used ");
                if (s.outlen) hex(s.out, s.outlen); else printf("-");
                printf(" ptk=%d gtk=%d %d ", s.set_ptk, s.set_gtk, s.gtk_idx);
                if (s.set_gtk) hex(s.gtk, s.gtk_len); else printf("-");
                printf(" "); hex(s.ptk + 32, 16); printf("\n");
            }
        }
        fflush(stdout);
    }
    return 0;
}
