#include <string.h>
#include "emu.h"

static const uint8_t bayer4[16] = {
     0,  8,  2, 10,
    12,  4, 14,  6,
     3, 11,  1,  9,
    15,  7, 13,  5,
};

// Recolouring is a pure lookup: RGB565 indexes a table built once per theme,
// so it costs no more than the bit expansion it replaces.
static inline void emit(uint8_t *dst, size_t *o, const Recolor *rc,
                        uint8_t r, uint8_t g, uint8_t b, unsigned x, unsigned y) {
    if (!rc || rc->mode == RECOLOR_OFF) {
        dst[(*o)++] = r;
        dst[(*o)++] = g;
        dst[(*o)++] = b;
        return;
    }
    // 565 round-trips exactly through the 888 expansion above, so this
    // recovers the original index without loss for 565 sources.
    unsigned idx = ((unsigned)(r >> 3) << 11) | ((unsigned)(g >> 2) << 5) | (b >> 3);
    const uint8_t *c;
    if (rc->mode == RECOLOR_DITHER && rc->pair) {
        const uint8_t *p = rc->pair + (size_t)idx * 7;
        c = (bayer4[(y & 3) * 4 + (x & 3)] < p[6]) ? p + 3 : p;
    } else if (rc->lut) {
        c = rc->lut + (size_t)idx * 3;
    } else {
        dst[(*o)++] = r;
        dst[(*o)++] = g;
        dst[(*o)++] = b;
        return;
    }
    dst[(*o)++] = c[0];
    dst[(*o)++] = c[1];
    dst[(*o)++] = c[2];
}

size_t video_convert(uint8_t *dst, const void *src, unsigned w, unsigned h,
                     size_t pitch, unsigned pixfmt, const Recolor *rc) {
    const uint8_t *s = (const uint8_t *)src;
    size_t o = 0;

    switch (pixfmt) {
    case RETRO_PIXEL_FORMAT_0RGB1555:
        for (unsigned y = 0; y < h; y++) {
            const uint16_t *row = (const uint16_t *)(s + (size_t)y * pitch);
            for (unsigned x = 0; x < w; x++) {
                uint16_t p = row[x];
                uint8_t r = (p >> 10) & 0x1f, g = (p >> 5) & 0x1f, b = p & 0x1f;
                emit(dst, &o, rc,
                     (uint8_t)((r << 3) | (r >> 2)),
                     (uint8_t)((g << 3) | (g >> 2)),
                     (uint8_t)((b << 3) | (b >> 2)), x, y);
            }
        }
        return o;

    case RETRO_PIXEL_FORMAT_RGB565:
        for (unsigned y = 0; y < h; y++) {
            const uint16_t *row = (const uint16_t *)(s + (size_t)y * pitch);
            for (unsigned x = 0; x < w; x++) {
                uint16_t p = row[x];
                uint8_t r = (p >> 11) & 0x1f, g = (p >> 5) & 0x3f, b = p & 0x1f;
                emit(dst, &o, rc,
                     (uint8_t)((r << 3) | (r >> 2)),
                     (uint8_t)((g << 2) | (g >> 4)),
                     (uint8_t)((b << 3) | (b >> 2)), x, y);
            }
        }
        return o;

    case RETRO_PIXEL_FORMAT_XRGB8888:
        for (unsigned y = 0; y < h; y++) {
            const uint32_t *row = (const uint32_t *)(s + (size_t)y * pitch);
            for (unsigned x = 0; x < w; x++) {
                uint32_t p = row[x];
                emit(dst, &o, rc, (uint8_t)((p >> 16) & 0xff),
                     (uint8_t)((p >> 8) & 0xff), (uint8_t)(p & 0xff), x, y);
            }
        }
        return o;
    }
    return 0;
}

void video_upscale(uint8_t *dst, const uint8_t *src, int w, int h, int scale) {
    if (scale < 1) scale = 1;
    int dw = w * scale;
    for (int y = 0; y < h; y++) {
        const uint8_t *srow = src + (size_t)y * w * 3;
        uint8_t *drow = dst + (size_t)y * scale * dw * 3;
        for (int x = 0; x < w; x++) {
            const uint8_t *p = srow + x * 3;
            uint8_t *d = drow + (size_t)x * scale * 3;
            for (int i = 0; i < scale; i++) {
                d[i * 3] = p[0];
                d[i * 3 + 1] = p[1];
                d[i * 3 + 2] = p[2];
            }
        }
        // Replicate the finished row down for the remaining scale-1 rows.
        for (int i = 1; i < scale; i++)
            memcpy(drow + (size_t)i * dw * 3, drow, (size_t)dw * 3);
    }
}

void video_downscale(uint8_t *dst, int dw, int dh,
                     const uint8_t *src, int sw, int sh) {
    if (dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0) return;
    for (int dy = 0; dy < dh; dy++) {
        int y0 = dy * sh / dh, y1 = (dy + 1) * sh / dh;
        if (y1 <= y0) y1 = y0 + 1;
        if (y1 > sh) y1 = sh;
        for (int dx = 0; dx < dw; dx++) {
            int x0 = dx * sw / dw, x1 = (dx + 1) * sw / dw;
            if (x1 <= x0) x1 = x0 + 1;
            if (x1 > sw) x1 = sw;
            unsigned r = 0, g = 0, b = 0, n = 0;
            for (int y = y0; y < y1; y++) {
                const uint8_t *p = src + ((size_t)y * sw + x0) * 3;
                for (int x = x0; x < x1; x++) {
                    r += p[0]; g += p[1]; b += p[2];
                    p += 3; n++;
                }
            }
            uint8_t *o = dst + ((size_t)dy * dw + dx) * 3;
            o[0] = (uint8_t)(r / n);
            o[1] = (uint8_t)(g / n);
            o[2] = (uint8_t)(b / n);
        }
    }
}
