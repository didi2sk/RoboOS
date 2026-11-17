/*
 * stb_image (third_party/stb_image.h, public domain / MIT) configured for
 * RoboOS: JPEG (baseline + progressive), PNG, BMP, GIF (first frame); memory
 * from heap.c, no stdio, no HDR. This file is built with floating point
 * allowed (see the Makefile) - the decoder is not written for -mgeneral-regs-only.
 */
#include <stddef.h>
void *heap_alloc(size_t n);
void *heap_realloc(void *p, size_t n);
void  heap_free(void *p);

#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_ONLY_BMP
#define STBI_ONLY_GIF
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_NO_THREAD_LOCALS
#define STBI_MAX_DIMENSIONS 16384
#define STBI_ASSERT(x)      ((void)0)
#define STBI_MALLOC(n)      heap_alloc(n)
#define STBI_REALLOC(p, n)  heap_realloc(p, n)
#define STBI_FREE(p)        heap_free(p)
#define STB_IMAGE_IMPLEMENTATION
#include "../third_party/stb_image.h"
