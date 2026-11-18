/*
 * RoboOS program SDK - Arduino-style API.
 *
 * A program is a flat binary (.bin) loaded at APP_BASE and started by the
 * shell ("run name") or automatically from the /RUN folder on the SD card.
 * Write either setup() + loop() (Arduino style) or main().
 * Ctrl+C stops the program whenever it calls delay(), getKey() or stopRequested().
 */
#ifndef ROBOS_H
#define ROBOS_H

typedef unsigned char      uint8_t;
typedef unsigned short     uint16_t;
typedef unsigned int       uint32_t;
typedef unsigned long      uint64_t;
typedef signed int         int32_t;
typedef unsigned long      size_t;

#define APP_BASE    0x01000000UL
#define APP_MAGIC   "ROBOAPP1"
#define API_VERSION 10

#define INPUT          0
#define OUTPUT         1
#define INPUT_PULLUP   2
#define INPUT_PULLDOWN 3
#define LOW            0
#define HIGH           1

struct robos_api {
    uint32_t version;
    uint32_t size;
    /* console */
    void     (*print)(const char *s);
    void     (*printf)(const char *fmt, ...);     /* %d %u %x %s %c %p, width, 0-pad, l */
    void     (*putc)(char c);
    int      (*getKey)(void);                     /* -1 = no key */
    void     (*clear)(void);
    void     (*color)(int c);                     /* 0 default 1 green 2 cyan 3 yellow 4 red 5 gray 6 white */
    /* GPIO */
    void     (*pinMode)(int pin, int mode);
    void     (*digitalWrite)(int pin, int value);
    int      (*digitalRead)(int pin);
    void     (*led)(int on);                      /* green ACT LED */
    /* time */
    void     (*delay)(uint32_t ms);
    void     (*delayMicroseconds)(uint32_t us);
    uint64_t (*millis)(void);
    uint64_t (*micros)(void);
    /* control */
    int      (*stopRequested)(void);              /* polls input; Ctrl+C ends the program */
    void     (*exit)(int code);
    /* misc */
    int      (*temperature)(void);                /* CPU temperature in milli-degrees C */
    /* network (API version 2): messages between RoboOS boards */
    int      (*netSend)(const char *to, const char *text);        /* "all" = everybody; 0 delivered, 1 sent to all, <0 error */
    int      (*netReceive)(char *from, char *text, int max);      /* from[16]; returns length, -1 = nothing */
    const char *(*netName)(void);                 /* this board's name */
    /* TCP/IP (API version 3). Handles are 1..8, errors are negative (see sockError).
     * Calls that wait (connect, name lookup, a full send buffer) keep the network
     * running and stop the program on Ctrl+C. Close what you open; everything a
     * program opened is closed when it ends. */
    int      (*tcpConnect)(const char *host, int port);              /* host: "192.168.1.5" or a name */
    int      (*tcpListen)(int port);                                 /* server: listening handle */
    int      (*tcpAccept)(int listener);                             /* new connection, 0 = nobody yet */
    int      (*udpOpen)(int localPort, const char *host, int remotePort);  /* host 0 = answer the last sender */
    int      (*sockWrite)(int h, const void *data, int len);         /* bytes sent, <0 error */
    int      (*sockPrint)(int h, const char *text);                  /* sockWrite of a C string */
    int      (*sockRead)(int h, void *buf, int max);                 /* TCP: bytes, 0 nothing yet, -1 closed;
                                                                        UDP: one datagram, -1 none */
    int      (*sockAvailable)(int h);                                /* bytes waiting, -1 closed */
    int      (*udpSendTo)(int h, const char *host, int port, const void *data, int len);
    int      (*udpReceiveFrom)(int h, void *buf, int max, char *fromIp, int *fromPort);  /* fromIp[16] */
    void     (*sockClose)(int h);
    const char *(*sockError)(int err);            /* text for a negative result */
    const char *(*ipAddress)(void);               /* this board, e.g. "192.168.1.37" */
    /* sound (API version 4): square-wave tones on the 3.5 mm jack (GPIO18/19 on boards without one) */
    void     (*tone)(int hz, int ms);             /* starts and returns at once; ms 0 = until noTone() */
    void     (*noTone)(void);
    int      (*playNotes)(const char *music);     /* waits; "T120 O4 L4 C D E8 F#8 G2 P4 >C", 0 = ok */
    void     (*soundVolume)(int percent);         /* 0..100 */
    /* API version 5: WAV files (PCM 8/16 bit, mono/stereo, 8-48 kHz) on the current sound
     * output (jack, GPIO18/19 or an I2S DAC); waits until the end, 0 = ok, < 0 = error */
    int      (*playWav)(const char *path);
    /* API version 6: pictures (JPEG, PNG, BMP, GIF) on the HDMI screen, scaled down to fit
     * the box w x h (0 = up to the screen edge); 0 = ok, < 0 = error (imageError() says why) */
    int      (*drawImage)(const char *path, int x, int y, int w, int h);
    const char *(*imageError)(void);
    /* API version 7: video (.mpg = MPEG-1 video + MP2 sound) on the whole HDMI screen; waits
     * until the end or a key (q / Enter / Ctrl+C): 0 = end, 1 = stopped, < 0 = error (videoError()) */
    int      (*playVideo)(const char *path);
    const char *(*videoError)(void);
    /* API version 8: files on the SD card (paths like the shell: relative to the current
     * directory, or "/ALARM.TXT"). readFile: bytes read (at most max), < 0 = not found / error.
     * writeFile: creates or replaces the file (8.3 name, the directory must exist), 0 = ok */
    int      (*readFile)(const char *path, void *buf, int max);
    int      (*writeFile)(const char *path, const void *data, int len);
    /* API version 9: the date and time (NTP or 'time set'; zone from /TIME.TXT).
     * unixTime: seconds since 1970 UTC, 0 = not known yet. timeText: local
     * "YYYY-MM-DD HH:MM:SS" into buf[20], returns 0 when the time is not known. */
    uint64_t (*unixTime)(void);
    int      (*timeText)(char *buf);
    /* API version 10: IoT. sensorRead(n): 0 CPU temperature, 1 temperature (0.01 C, BME280),
     * 2 humidity (0.01 %), 3 pressure (Pa), 4-7 ADS1115 A0-A3 (mV); SENSOR_NONE = no sensor.
     * mqttPublish: topic under roboos/<name>/ (or "/full/topic"), 0 = sent, < 0 = not connected.
     * mqttReceive: the next message sent to roboos/<name>/cmd/..., topic like "cmd/led";
     * payload length, -1 = nothing waiting. The broker is set with 'mqtt broker <host>'. */
    int      (*sensorRead)(int n);
    int      (*mqttConnected)(void);
    int      (*mqttPublish)(const char *topic, const char *text, int retain);
    int      (*mqttReceive)(char *topic, int tmax, char *payload, int pmax);
};
#define SENSOR_NONE (-999999)

extern const struct robos_api *api;

/* Arduino-style names */
#define print(s)              (api->print)(s)
#define println(s)            ((api->print)(s), (api->print)("\n"))
#define printf(...)           (api->printf)(__VA_ARGS__)
#define getKey()              (api->getKey)()
#define clearScreen()         (api->clear)()
#define textColor(c)          (api->color)(c)
#define pinMode(p, m)         (api->pinMode)(p, m)
#define digitalWrite(p, v)    (api->digitalWrite)(p, v)
#define digitalRead(p)        (api->digitalRead)(p)
#define actLed(on)            (api->led)(on)
#define delay(ms)             (api->delay)(ms)
#define delayMicroseconds(us) (api->delayMicroseconds)(us)
#define millis()              (api->millis)()
#define micros()              (api->micros)()
#define stopRequested()       (api->stopRequested)()
#define exit(code)            (api->exit)(code)
#define cpuTemperature()      (api->temperature)()
#define netSend(to, text)      (api->netSend)(to, text)
#define netReceive(f, t, max)  (api->netReceive)(f, t, max)
#define netName()             (api->netName)()
#define tcpConnect(host, port)          (api->tcpConnect)(host, port)
#define tcpListen(port)                 (api->tcpListen)(port)
#define tcpAccept(l)                    (api->tcpAccept)(l)
#define udpOpen(lport, host, rport)     (api->udpOpen)(lport, host, rport)
#define sockWrite(h, d, n)              (api->sockWrite)(h, d, n)
#define sockPrint(h, s)                 (api->sockPrint)(h, s)
#define sockRead(h, b, max)             (api->sockRead)(h, b, max)
#define sockAvailable(h)                (api->sockAvailable)(h)
#define udpSendTo(h, host, port, d, n)  (api->udpSendTo)(h, host, port, d, n)
#define udpReceiveFrom(h, b, max, ip, port) (api->udpReceiveFrom)(h, b, max, ip, port)
#define sockClose(h)                    (api->sockClose)(h)
#define sockError(e)                    (api->sockError)(e)
#define ipAddress()                     (api->ipAddress)()
#define tone(hz, ms)                    (api->tone)(hz, ms)
#define noTone()                        (api->noTone)()
#define playNotes(m)                    (api->playNotes)(m)
#define soundVolume(p)                  (api->soundVolume)(p)
#define playWav(path)                   (api->playWav)(path)
#define drawImage(p, x, y, w, h)        (api->drawImage)(p, x, y, w, h)
#define imageError()                    (api->imageError)()
#define playVideo(path)                 (api->playVideo)(path)
#define videoError()                    (api->videoError)()
#define readFile(path, buf, max)        (api->readFile)(path, buf, max)
#define writeFile(path, data, len)      (api->writeFile)(path, data, len)
#define unixTime()                      (api->unixTime)()
#define timeText(buf)                   (api->timeText)(buf)
#define sensorRead(n)                   (api->sensorRead)(n)
#define mqttConnected()                 (api->mqttConnected)()
#define mqttPublish(t, text, retain)    (api->mqttPublish)(t, text, retain)
#define mqttReceive(t, tmax, p, pmax)   (api->mqttReceive)(t, tmax, p, pmax)

void *memset(void *d, int c, size_t n);
void *memcpy(void *d, const void *s, size_t n);

#endif
