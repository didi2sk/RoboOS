/* C runtime for RoboOS programs */
#include "robos.h"

const struct robos_api *api;

void setup(void) __attribute__((weak));
void loop(void)  __attribute__((weak));
int  main(void)  __attribute__((weak));

int __robos_start(const struct robos_api *a)
{
    api = a;
    if (main) return main();
    if (setup) setup();
    if (loop) for (;;) loop();
    return 0;
}

/* the compiler may emit calls to these */
void *memset(void *d, int c, size_t n)
{
    unsigned char *p = d;
    while (n--) *p++ = (unsigned char)c;
    return d;
}

void *memcpy(void *d, const void *s, size_t n)
{
    unsigned char *a = d;
    const unsigned char *b = s;
    while (n--) *a++ = *b++;
    return d;
}
