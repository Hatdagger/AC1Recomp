/* A small PNG reader for the textures acr-import writes (and any the player
 * edits and saves again): 8-bit indexed, grey, RGB or RGBA, not interlaced,
 * with a full inflate, so a re-compressed file still loads. No OS calls. */
#ifndef ACR_PNG_H
#define ACR_PNG_H

#include <stddef.h>
#include <stdint.h>

typedef struct acr_image {
    int w, h;
    int indexed;            /* 1: pixels are palette indices (1 byte each) */
    uint8_t *pixels;        /* indexed: w*h bytes; otherwise RGBA, w*h*4 bytes */
    uint8_t palette[256][4];/* RGBA; tRNS fills alpha, otherwise 255 */
} acr_image;

/* 1 on success (free with acr_image_free); 0 if the file is not one it reads. */
int  acr_png_decode(const uint8_t *data, size_t size, acr_image *out);
void acr_image_free(acr_image *img);

/* zlib stream -> out. Returns bytes written, or -1 on a bad stream / overflow. */
long acr_inflate_zlib(const uint8_t *in, size_t in_size, uint8_t *out, size_t out_size);

#endif
