/*
 * chat.c - talk to the other RoboOS boards on the network.
 * Type a line and press Enter: it goes to everybody. Messages from the
 * others appear as they arrive. Ctrl+C ends the program.
 */
#include "robos.h"

static char line[160];
static int  len;

void setup(void)
{
    printf("chat: I am %s. Type a message + Enter (goes to all), Ctrl+C to stop.\n", netName());
}

void loop(void)
{
    char from[16], text[201];
    while (netReceive(from, text, sizeof(text)) >= 0) {
        textColor(2);
        printf("\r%s: ", from);
        textColor(0);
        printf("%s\n", text);
        line[len] = 0;
        print(line);                         /* put the half-typed line back */
    }

    int c = getKey();
    if (c == '\r' || c == '\n') {
        println("");
        line[len] = 0;
        if (len && netSend("all", line) < 0) println("(not sent: no network)");
        len = 0;
    } else if ((c == 0x7F || c == '\b') && len) {
        len--;
        print("\b \b");
    } else if (c >= 32 && c < 127 && len < (int)sizeof(line) - 1) {
        line[len++] = (char)c;
        char s[2] = { (char)c, 0 };
        print(s);
    }
    delay(5);
}
