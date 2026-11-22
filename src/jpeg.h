/* jpeg.h - baseline JPEG encoder (jpeg.c); no kernel headers so the host test can build it */
#ifndef JPEG_H
#define JPEG_H
/* rgb: 0x00RRGGBB, stride in pixels; returns the JPEG size, -1 bad size, -2 out buffer too small */
int jpeg_encode(const unsigned *rgb, int w, int h, int stride, int quality, unsigned char *out, int max);
#endif
