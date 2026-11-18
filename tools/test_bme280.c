/* PC test of src/bme280_math.h against the example in the Bosch BMP280/BME280 datasheet:
 *   cc -O2 -o /tmp/t tools/test_bme280.c && /tmp/t */
#include <stdio.h>
#include "../src/bme280_math.h"
int main(void)
{
    struct bme280_cal c = { 27504, 26435, -1000, 36477, -10685, 3024, 2855, 140, -7, 15500, -14600, 6000 };
    int tf, t = bme280_temp(&c, 519888, &tf);
    unsigned p = bme280_press(&c, 415148, tf);
    printf("T = %d.%02d C (datasheet 25.08), t_fine %d (128422)\n", t / 100, t % 100, tf);
    printf("p = %u.%02u Pa (datasheet 100653.27)\n", p / 256, (p % 256) * 100 / 256);
    /* humidity: no datasheet example - a plausible sensor's calibration, checked against the
     * floating-point formula of the same datasheet */
    c.h1 = 75; c.h2 = 362; c.h3 = 0; c.h4 = 324; c.h5 = 50; c.h6 = 30;
    unsigned h = bme280_hum(&c, 30000, tf);
    double vx = tf - 76800.0;
    double hf = (30000 - (c.h4 * 64.0 + c.h5 / 16384.0 * vx)) *
                (c.h2 / 65536.0 * (1.0 + c.h6 / 67108864.0 * vx * (1.0 + c.h3 / 67108864.0 * vx)));
    hf = hf * (1.0 - c.h1 * hf / 524288.0);
    printf("H = %u.%02u %% (float formula %.2f)\n", h / 1024, (h % 1024) * 100 / 1024, hf);
    int ok = t == 2508 && tf == 128422 && p / 256 == 100653 && (int)(h / 1024) == (int)hf;
    printf("%s\n", ok ? "OK" : "MISMATCH");
    return !ok;
}
