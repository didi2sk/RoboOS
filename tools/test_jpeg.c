/* test_jpeg.c - host test of src/jpeg.c: encodes test pictures, tools/test_jpeg.py decodes them with PIL.
 * cc -O2 -Isrc tools/test_jpeg.c src/jpeg.c -o /tmp/test_jpeg && /tmp/test_jpeg /tmp && python3 tools/test_jpeg.py /tmp */
#include <stdio.h>
#include <stdlib.h>
#include "jpeg.h"

static void save(const char *dir, const char *name, const unsigned *p, int w, int h, int q)
{
    static unsigned char out[4 << 20];
    int n = jpeg_encode(p, w, h, w, q, out, sizeof(out));
    char fn[256];
    if (n <= 0) { printf("FAIL %s: %d\n", name, n); exit(1); }
    snprintf(fn, sizeof(fn), "%s/%s.jpg", dir, name);
    FILE *f = fopen(fn, "wb"); fwrite(out, 1, (size_t)n, f); fclose(f);
    snprintf(fn, sizeof(fn), "%s/%s.raw", dir, name);
    f = fopen(fn, "wb");
    for (int i = 0; i < w * h; i++) { unsigned char c[3] = { p[i] >> 16, p[i] >> 8, p[i] }; fwrite(c, 1, 3, f); }
    fclose(f);
    printf("%s %dx%d q%d: %d bytes\n", name, w, h, q, n);
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : "/tmp";
    int w = 648, h = 486;
    unsigned *p = malloc((size_t)w * h * 4);
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {      /* smooth gradients + a few edges */
        unsigned r = (unsigned)(x * 255 / (w - 1)), g = (unsigned)(y * 255 / (h - 1)), b = (unsigned)((x + y) & 0xFF);
        if ((x / 81 + y / 81) % 2 && x > w / 2) r = g = b = 200;
        p[y * w + x] = (r << 16) | (g << 8) | b;
    }
    save(dir, "jt_grad", p, w, h, 85);
    save(dir, "jt_grad_q30", p, w, h, 30);
    for (int i = 0; i < 7 * 5; i++) p[i] = (i * 37) % 256 * 0x010101u;   /* odd size, not a multiple of 16 */
    save(dir, "jt_small", p, 7, 5, 95);
    srand(1); for (int i = 0; i < w * h; i++) p[i] = (unsigned)rand() & 0xFFFFFF;   /* noise: big codes, stuffing */
    save(dir, "jt_noise", p, w, h, 100);
    for (int i = 0; i < w * h; i++) p[i] = 0;
    save(dir, "jt_black", p, w, h, 75);
    return 0;
}
