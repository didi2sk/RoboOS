/*
 * BMP180 (and BMP085) integer compensation from the Bosch BMP180 datasheet (section 3.5),
 * shifts as in the Bosch reference driver. Checked on the PC by tools/test_bmp180.c
 * against the datasheet example (15.0 C, 69964 Pa). Plain C, no kernel headers.
 */
#ifndef BMP180_MATH_H
#define BMP180_MATH_H

struct bmp180_cal { short ac1, ac2, ac3; unsigned short ac4, ac5, ac6; short b1, b2, mb, mc, md; };

/* temperature in 0.1 C; *b5 is needed for the pressure */
static inline int bmp180_temp(const struct bmp180_cal *c, int ut, int *b5)
{
    int x1 = ((ut - (int)c->ac6) * (int)c->ac5) >> 15;
    if (x1 + c->md == 0) { *b5 = 0; return 0; }
    int x2 = (c->mc * 2048) / (x1 + c->md);
    *b5 = x1 + x2;
    return (*b5 + 8) >> 4;
}

/* pressure in Pa; up = raw pressure already shifted right by (8 - oss) */
static inline int bmp180_press(const struct bmp180_cal *c, int up, int b5, int oss)
{
    int b6 = b5 - 4000;
    int x1 = (c->b2 * ((b6 * b6) >> 12)) >> 11;
    int x2 = (c->ac2 * b6) >> 11;
    int x3 = x1 + x2;
    int b3 = ((((int)c->ac1 * 4 + x3) << oss) + 2) / 4;
    x1 = (c->ac3 * b6) >> 13;
    x2 = (c->b1 * ((b6 * b6) >> 12)) >> 16;
    x3 = ((x1 + x2) + 2) >> 2;
    unsigned b4 = ((unsigned)c->ac4 * (unsigned)(x3 + 32768)) >> 15;
    if (!b4) return 0;
    unsigned b7 = ((unsigned)up - (unsigned)b3) * (unsigned)(50000 >> oss);
    int p = b7 < 0x80000000u ? (int)((b7 * 2) / b4) : (int)((b7 / b4) * 2);
    x1 = (p >> 8) * (p >> 8);
    x1 = (x1 * 3038) >> 16;
    x2 = (-7357 * p) >> 16;
    return p + ((x1 + x2 + 3791) >> 4);
}
#endif
