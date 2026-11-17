/*
 * tcpserv.c - a tiny TCP server on port 23.
 * From a PC in the same network:  nc <board address> 23   (or telnet)
 * Every line you type comes back in capitals, "temp" answers the CPU
 * temperature, "bye" closes the connection. Ctrl+C on the board stops it.
 */
#include "robos.h"

static int listener, client;
static char line[128];
static int len;

static int same(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static void put_num(char *out, int *n, int v)
{
    char d[12];
    int k = 0;
    if (v < 0) { out[(*n)++] = '-'; v = -v; }
    do { d[k++] = '0' + v % 10; v /= 10; } while (v);
    while (k) out[(*n)++] = d[--k];
}

static void answer(void)
{
    char out[160];
    int n = 0;
    line[len] = 0;
    printf("  > %s\n", line);
    if (same(line, "bye")) {
        sockPrint(client, "bye!\r\n");
        sockClose(client);
        client = 0;
        println("client closed");
        return;
    }
    if (same(line, "temp")) {
        int t = cpuTemperature();
        for (const char *s = "CPU "; *s; s++) out[n++] = *s;
        put_num(out, &n, t / 1000);
        out[n++] = ' '; out[n++] = 'C';
    } else {
        for (int i = 0; i < len; i++) {
            char c = line[i];
            out[n++] = (c >= 'a' && c <= 'z') ? c - 32 : c;
        }
    }
    out[n++] = '\r'; out[n++] = '\n';
    sockWrite(client, out, n);
}

void setup(void)
{
    listener = tcpListen(23);
    if (listener < 0) { printf("tcpListen: %s\n", sockError(listener)); exit(1); }
    printf("tcpserv: on a PC type  nc %s 23   (Ctrl+C stops)\n", ipAddress());
}

void loop(void)
{
    if (client <= 0) {
        client = tcpAccept(listener);
        if (client > 0) {
            println("client connected");
            sockPrint(client, "Hello from RoboOS! Type a line, 'temp' or 'bye'.\r\n");
            len = 0;
        }
        delay(10);
        return;
    }
    char b[64];
    int n = sockRead(client, b, sizeof(b));
    if (n < 0) {                                   /* the PC closed the connection */
        println("client left");
        sockClose(client);
        client = 0;
        return;
    }
    for (int i = 0; i < n && client > 0; i++) {
        if (b[i] == '\n') { answer(); len = 0; }
        else if (b[i] != '\r' && len < (int)sizeof(line) - 1) line[len++] = b[i];
    }
    if (!n) delay(5);
}
