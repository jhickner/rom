#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "rom.h"

#define LUT_N 65536

// Oklab chroma the tint mode paints with. Enough to read clearly as a colour
// wash; much more and it stops looking like the terminal's own background.
#define TINT_CHROMA 0.08

typedef struct { double L, a, b; } Lab;

// ------------------------------------------------------------ colour space

static double srgb_to_linear(double c) {
    return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}

static double linear_to_srgb(double c) {
    if (c <= 0.0) return 0.0;
    if (c >= 1.0) return 1.0;
    return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1.0 / 2.4) - 0.055;
}

// Ottosson's Oklab. Operates on linear sRGB.
static Lab rgb_to_lab(double r, double g, double b) {
    r = srgb_to_linear(r);
    g = srgb_to_linear(g);
    b = srgb_to_linear(b);
    double l = 0.4122214708 * r + 0.5363325363 * g + 0.0514459929 * b;
    double m = 0.2119034982 * r + 0.6806995451 * g + 0.1073969566 * b;
    double s = 0.0883024619 * r + 0.2817188376 * g + 0.6299787005 * b;
    double l_ = cbrt(l), m_ = cbrt(m), s_ = cbrt(s);
    Lab o;
    o.L = 0.2104542553 * l_ + 0.7936177850 * m_ - 0.0040720468 * s_;
    o.a = 1.9779984951 * l_ - 2.4285922050 * m_ + 0.4505937099 * s_;
    o.b = 0.0259040371 * l_ + 0.7827717662 * m_ - 0.8086757660 * s_;
    return o;
}

static void lab_to_rgb8(Lab c, uint8_t out[3]) {
    double l_ = c.L + 0.3963377774 * c.a + 0.2158037573 * c.b;
    double m_ = c.L - 0.1055613458 * c.a - 0.0638541728 * c.b;
    double s_ = c.L - 0.0894841775 * c.a - 1.2914855480 * c.b;
    double l = l_ * l_ * l_, m = m_ * m_ * m_, s = s_ * s_ * s_;
    double r =  4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s;
    double g = -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s;
    double b = -0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s;
    out[0] = (uint8_t)lround(linear_to_srgb(r) * 255.0);
    out[1] = (uint8_t)lround(linear_to_srgb(g) * 255.0);
    out[2] = (uint8_t)lround(linear_to_srgb(b) * 255.0);
}

static Lab lab_of_u8(const uint8_t c[3]) {
    return rgb_to_lab(c[0] / 255.0, c[1] / 255.0, c[2] / 255.0);
}

static double lab_dist2(Lab p, Lab q) {
    double dl = p.L - q.L, da = p.a - q.a, db = p.b - q.b;
    return dl * dl + da * da + db * db;
}

static Lab lab_mix(Lab p, Lab q, double t) {
    Lab o;
    o.L = p.L + (q.L - p.L) * t;
    o.a = p.a + (q.a - p.a) * t;
    o.b = p.b + (q.b - p.b) * t;
    return o;
}

// ------------------------------------------------------------------ modes

static const char *mode_names[RECOLOR_COUNT] = {
    "off", "hue", "nearest", "duotone", "tint", "dither"
};

const char *recolor_mode_name(int mode) {
    return (mode >= 0 && mode < RECOLOR_COUNT) ? mode_names[mode] : "off";
}

int recolor_mode_from_name(const char *s) {
    if (!s) return -1;
    for (int i = 0; i < RECOLOR_COUNT; i++)
        if (!strcasecmp(s, mode_names[i])) return i;
    return -1;
}

void recolor_free(Recolor *rc) {
    free(rc->lut);
    free(rc->pair);
    rc->lut = NULL;
    rc->pair = NULL;
    rc->mode = RECOLOR_OFF;
}

int recolor_build(Recolor *rc, int mode, const Theme *t, double strength) {
    recolor_free(rc);
    rc->mode = mode;
    if (mode == RECOLOR_OFF) return 0;
    if (strength < 0.0) strength = 0.0;
    if (strength > 1.0) strength = 1.0;

    // Candidate set: the 16 ANSI colours plus foreground and background.
    Lab cand[18];
    uint8_t cand_rgb[18][3];
    int ncand = 0;
    for (int i = 0; i < 16; i++) {
        memcpy(cand_rgb[ncand], t->pal[i], 3);
        cand[ncand++] = lab_of_u8(t->pal[i]);
    }
    memcpy(cand_rgb[ncand], t->fg, 3);
    cand[ncand++] = lab_of_u8(t->fg);
    memcpy(cand_rgb[ncand], t->bg, 3);
    cand[ncand++] = lab_of_u8(t->bg);

    // Chromatic candidates only, for hue snapping. A grey has no meaningful
    // hue to snap to and would otherwise pull colours at random.
    int hue_idx[18], nhue = 0;
    for (int i = 0; i < ncand; i++) {
        double c = hypot(cand[i].a, cand[i].b);
        if (c > 0.03) hue_idx[nhue++] = i;
    }

    Lab bg = lab_of_u8(t->bg), fg = lab_of_u8(t->fg);
    double Lspan = fg.L - bg.L;
    if (fabs(Lspan) < 1e-6) Lspan = 1.0;

    if (mode == RECOLOR_DITHER) {
        rc->pair = malloc((size_t)LUT_N * 7);
        if (!rc->pair) return -1;
    } else {
        rc->lut = malloc((size_t)LUT_N * 3);
        if (!rc->lut) return -1;
    }

    for (int i = 0; i < LUT_N; i++) {
        // Unpack RGB565.
        unsigned r5 = (unsigned)(i >> 11) & 0x1f;
        unsigned g6 = (unsigned)(i >> 5) & 0x3f;
        unsigned b5 = (unsigned)i & 0x1f;
        uint8_t sr = (uint8_t)((r5 << 3) | (r5 >> 2));
        uint8_t sg = (uint8_t)((g6 << 2) | (g6 >> 4));
        uint8_t sb = (uint8_t)((b5 << 3) | (b5 >> 2));
        Lab src = rgb_to_lab(sr / 255.0, sg / 255.0, sb / 255.0);

        if (mode == RECOLOR_DITHER) {
            // Two closest candidates, plus where the source falls on the
            // segment between them: ordered dithering recovers the tones a
            // hard 16-colour mapping would flatten.
            int i0 = 0, i1 = 0;
            double d0 = 1e30, d1 = 1e30;
            for (int c = 0; c < ncand; c++) {
                double d = lab_dist2(src, cand[c]);
                if (d < d0) { d1 = d0; i1 = i0; d0 = d; i0 = c; }
                else if (d < d1) { d1 = d; i1 = c; }
            }
            double ax = cand[i1].L - cand[i0].L;
            double ay = cand[i1].a - cand[i0].a;
            double az = cand[i1].b - cand[i0].b;
            double len2 = ax * ax + ay * ay + az * az;
            double tpar = 0.0;
            if (len2 > 1e-12) {
                tpar = ((src.L - cand[i0].L) * ax + (src.a - cand[i0].a) * ay +
                        (src.b - cand[i0].b) * az) / len2;
                if (tpar < 0.0) tpar = 0.0;
                if (tpar > 1.0) tpar = 1.0;
            }
            tpar *= strength;
            uint8_t *p = rc->pair + (size_t)i * 7;
            uint8_t ca[3], cb[3];
            lab_to_rgb8(lab_mix(src, cand[i0], strength), ca);
            lab_to_rgb8(lab_mix(src, cand[i1], strength), cb);
            memcpy(p, ca, 3);
            memcpy(p + 3, cb, 3);
            p[6] = (uint8_t)lround(tpar * 16.0);
            continue;
        }

        Lab out;
        if (mode == RECOLOR_NEAREST) {
            int best = 0;
            double bd = 1e30;
            for (int c = 0; c < ncand; c++) {
                double d = lab_dist2(src, cand[c]);
                if (d < bd) { bd = d; best = c; }
            }
            out = cand[best];
        } else if (mode == RECOLOR_DUOTONE) {
            double tt = (src.L - bg.L) / Lspan;
            if (tt < 0.0) tt = 0.0;
            if (tt > 1.0) tt = 1.0;
            out = lab_mix(bg, fg, tt);
        } else if (mode == RECOLOR_TINT) {
            // Every colour becomes a tone of the background: its hue over the
            // source's own lightness, so shading and structure survive while
            // the palette collapses to one colour.
            out.L = src.L;
            // The background's own chroma is no use as the tint strength.
            // Terminal backgrounds are nearly neutral - a dark blue-grey like
            // #1e1e2e carries a chroma of about 0.01, which is invisible - so
            // take only the hue and give it a chroma that reads. Tapered
            // towards black and white, where a saturated tone would clip on
            // the way back to sRGB.
            double bc = hypot(bg.a, bg.b);
            if (bc > 1e-4) {
                double taper = 2.0 * (1.0 - fabs(2.0 * src.L - 1.0));
                if (taper > 1.0) taper = 1.0;
                double c = TINT_CHROMA * taper;
                out.a = bg.a / bc * c;
                out.b = bg.b / bc * c;
            } else {
                out.a = out.b = 0.0;    // a grey background can only mean grey
            }
        } else {
            // Hue mode: keep the source's lightness and saturation, adopt the
            // nearest theme hue. Structure and shading survive intact.
            double chroma = hypot(src.a, src.b);
            if (chroma < 0.02 || nhue == 0) {
                // Neutral: ride the background-to-foreground ramp instead.
                double tt = (src.L - bg.L) / Lspan;
                if (tt < 0.0) tt = 0.0;
                if (tt > 1.0) tt = 1.0;
                out = lab_mix(bg, fg, tt);
            } else {
                double sa = src.a / chroma, sb2 = src.b / chroma;
                int best = hue_idx[0];
                double bd = -2.0;
                for (int k = 0; k < nhue; k++) {
                    int c = hue_idx[k];
                    double cc = hypot(cand[c].a, cand[c].b);
                    double dot = (cand[c].a / cc) * sa + (cand[c].b / cc) * sb2;
                    if (dot > bd) { bd = dot; best = c; }
                }
                double cc = hypot(cand[best].a, cand[best].b);
                out.L = src.L;
                out.a = cand[best].a / cc * chroma;
                out.b = cand[best].b / cc * chroma;
            }
        }

        lab_to_rgb8(lab_mix(src, out, strength), rc->lut + (size_t)i * 3);
    }
    return 0;
}
