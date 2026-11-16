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
#define API_VERSION 2

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
};

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

void *memset(void *d, int c, size_t n);
void *memcpy(void *d, const void *s, size_t n);

#endif
