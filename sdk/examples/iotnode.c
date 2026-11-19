/*
 * iotnode.c - a small MQTT node (needs 'mqtt broker <host>' in the shell first).
 *   every 10 s: roboos/<name>/node   {"temp":21.53,"hum":40.12,"a0":1234,"count":7}
 *   commands:   roboos/<name>/cmd/led    on | off | blink   (the LED on GPIO23)
 *               roboos/<name>/cmd/reset  sets the counter back to 0
 * Try it from a PC:  mosquitto_sub -h <broker> -t 'roboos/#' -v
 *                    mosquitto_pub -h <broker> -t roboos/<name>/cmd/led -m blink
 */
#include "robos.h"

#define LED 23

static uint64_t next;
static int count, blink;

static int same(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
static int add(char *b, int n, const char *s) { while (*s) b[n++] = *s++; return n; }

static int addnum(char *b, int n, int v, int dec)          /* dec = 2: v is in 0.01 */
{
    char d[16];
    int k = 0;
    if (v < 0) { b[n++] = '-'; v = -v; }
    for (int i = 0; i < dec + 1 || v; i++) { d[k++] = (char)('0' + v % 10); v /= 10; if (i == dec - 1) d[k++] = '.'; }
    while (k) b[n++] = d[--k];
    return n;
}

void setup(void)
{
    pinMode(LED, OUTPUT);
    printf("iotnode: MQTT %s, messages to roboos/%s/node (Ctrl+C stops)\n",
           mqttConnected() ? "connected" : "NOT connected yet ('mqtt broker <host>')", netName());
}

void loop(void)
{
    char t[64], p[128];
    int n = mqttReceive(t, sizeof(t), p, sizeof(p));
    if (n >= 0) {
        printf("got %s = %s\n", t, p);
        if (same(t, "cmd/led")) {
            blink = same(p, "blink");
            digitalWrite(LED, same(p, "on") || blink);
        } else if (same(t, "cmd/reset")) count = 0;
    }
    if (blink) digitalWrite(LED, (int)(millis() / 250) & 1);
    if (millis() >= next) {
        next = millis() + 10000;
        int k = add(p, 0, "{");
        int v = sensorRead(1);
        if (v != SENSOR_NONE) { k = add(p, k, "\"temp\":"); k = addnum(p, k, v, 2); k = add(p, k, ","); }
        v = sensorRead(2);
        if (v != SENSOR_NONE) { k = add(p, k, "\"hum\":"); k = addnum(p, k, v, 2); k = add(p, k, ","); }
        v = sensorRead(4);
        if (v != SENSOR_NONE) { k = add(p, k, "\"a0\":"); k = addnum(p, k, v, 0); k = add(p, k, ","); }
        k = add(p, k, "\"cpu\":"); k = addnum(p, k, sensorRead(0), 2);
        k = add(p, k, ",\"count\":"); k = addnum(p, k, count++, 0);
        k = add(p, k, "}");
        p[k] = 0;
        printf("%s %s\n", mqttPublish("node", p, 0) ? "(not sent)" : "sent", p);
    }
    delay(20);
}
