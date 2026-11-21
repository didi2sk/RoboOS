/* PC test of src/bmp180_math.h against the example in the Bosch BMP180 datasheet:
 *   cc -O2 -o /tmp/t tools/test_bmp180.c && /tmp/t */
#include <stdio.h>
#include "../src/bmp180_math.h"
int main(void)
{
    struct bmp180_cal c = { 408, -72, -14383, 32741, 32757, 23153, 6190, 4, -32768, -8711, 2868 };
    int b5, t = bmp180_temp(&c, 27898, &b5);
    int p = bmp180_press(&c, 23843, b5, 0);
    printf("T = %d.%d C (datasheet 15.0), B5 %d (datasheet 2399 rounds X2 down, C division 2400)\n", t / 10, t % 10, b5);
    printf("p = %d Pa (datasheet 69964)\n", p);
    int ok = t == 150 && (b5 == 2399 || b5 == 2400) && p == 69964;
    printf("%s\n", ok ? "OK" : "MISMATCH");
    return !ok;
}
