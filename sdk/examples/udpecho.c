/*
 * udpecho.c - answers UDP datagrams on port 5000.
 * From a PC:  nc -u <board address> 5000   then type lines.
 * The board prints who sent what and answers "<name> got: <text>".
 */
#include "robos.h"

static int h;

void setup(void)
{
    h = udpOpen(5000, 0, 0);
    if (h < 0) { printf("udpOpen: %s\n", sockError(h)); exit(1); }
    printf("udpecho: on a PC type  nc -u %s 5000   (Ctrl+C stops)\n", ipAddress());
}

void loop(void)
{
    char b[256], out[300], ip[16];
    int port;
    int n = udpReceiveFrom(h, b, sizeof(b) - 1, ip, &port);
    if (n < 0) { delay(5); return; }
    while (n && (b[n - 1] == '\n' || b[n - 1] == '\r')) n--;
    b[n] = 0;
    printf("%s:%d says: %s\n", ip, port, b);

    int k = 0;                                     /* "<name> got: <text>" back to the sender */
    for (const char *s = netName(); *s; s++) out[k++] = *s;
    for (const char *s = " got: "; *s; s++) out[k++] = *s;
    for (int i = 0; i < n; i++) out[k++] = b[i];
    out[k++] = '\n';
    sockWrite(h, out, k);
}
