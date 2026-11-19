/*
 * BME280 / BMP280 compensation, the integer formulas from the Bosch datasheet
 * (BST-BME280-DS002, section 4.2.3). Plain C with no other includes, so that
 * tools/test_bme280.c can check it on the PC against the datasheet example.
 */
#ifndef BME280_MATH_H
#define BME280_MATH_H

struct bme280_cal {
    unsigned short t1; short t2, t3;
    unsigned short p1; short p2, p3, p4, p5, p6, p7, p8, p9;
    unsigned char h1, h3; short h2, h4, h5; signed char h6;
};

/* temperature in 0.01 degC; *t_fine is needed by the other two */
static inline int bme280_temp(const struct bme280_cal *c, int adc_t, int *t_fine)
{
    int v1 = ((((adc_t >> 3) - ((int)c->t1 << 1))) * (int)c->t2) >> 11;
    int v2 = (((((adc_t >> 4) - (int)c->t1) * ((adc_t >> 4) - (int)c->t1)) >> 12) * (int)c->t3) >> 14;
    *t_fine = v1 + v2;
    return (*t_fine * 5 + 128) >> 8;
}

/* pressure in Pa * 256 (Q24.8) */
static inline unsigned bme280_press(const struct bme280_cal *c, int adc_p, int t_fine)
{
    long long v1 = (long long)t_fine - 128000, v2, p;
    v2 = v1 * v1 * (long long)c->p6;
    v2 = v2 + ((v1 * (long long)c->p5) << 17);
    v2 = v2 + (((long long)c->p4) << 35);
    v1 = ((v1 * v1 * (long long)c->p3) >> 8) + ((v1 * (long long)c->p2) << 12);
    v1 = (((((long long)1) << 47) + v1)) * ((long long)c->p1) >> 33;
    if (v1 == 0) return 0;
    p = 1048576 - adc_p;
    p = (((p << 31) - v2) * 3125) / v1;
    v1 = (((long long)c->p9) * (p >> 13) * (p >> 13)) >> 25;
    v2 = (((long long)c->p8) * p) >> 19;
    p = ((p + v1 + v2) >> 8) + (((long long)c->p7) << 4);
    return (unsigned)p;
}

/* relative humidity in % * 1024 (Q22.10) */
static inline unsigned bme280_hum(const struct bme280_cal *c, int adc_h, int t_fine)
{
    int v = t_fine - 76800;
    v = (((((adc_h << 14) - (((int)c->h4) << 20) - (((int)c->h5) * v)) + 16384) >> 15) *
         (((((((v * (int)c->h6) >> 10) * (((v * (int)c->h3) >> 11) + 32768)) >> 10) + 2097152) *
           (int)c->h2 + 8192) >> 14));
    v = v - (((((v >> 15) * (v >> 15)) >> 7) * (int)c->h1) >> 4);
    if (v < 0) v = 0;
    if (v > 419430400) v = 419430400;
    return (unsigned)(v >> 12);
}

#endif
