/******************************************************************************
*
* @file art.c: procedural sprite generation for the danmaku game.
*
* Everything is painted at start-up into DDR.  Sprite backgrounds are the key
* colour, so the engine drops them while blitting (colour keying).  Soft edges
* are faked with a 4x4 ordered dither between the sprite colour and the key
* colour: on screen that reads as a glow, and it still works with a 1-bit key.
*
******************************************************************************/
#include "art.h"

#define MAG        0xF81Fu
#define ART_BASE   0x05000000u

static uint32_t mem_delta;

void art_set_delta(uint32_t d) { mem_delta = d; }

static inline uint16_t *A(uint32_t a)
{
    return (uint16_t *)(uintptr_t)(a + mem_delta);
}

static uint16_t mix565(uint16_t a, uint16_t b, int t)      /* t = 0..255 */
{
    int ia = 255 - t;
    int r = (((a >> 11) & 31) * ia + ((b >> 11) & 31) * t) >> 8;
    int g = (((a >> 5) & 63) * ia + ((b >> 5) & 63) * t) >> 8;
    int bl = ((a & 31) * ia + (b & 31) * t) >> 8;
    if (r > 31) r = 31;
    if (g > 63) g = 63;
    if (bl > 31) bl = 31;
    return (uint16_t)((r << 11) | (g << 5) | bl);
}

static int isqrt_i(int v)
{
    int r = 0, bit = 1 << 30;
    if (v <= 0) return 0;
    while (bit > v) bit >>= 2;
    while (bit) {
        if (v >= r + bit) { v -= r + bit; r = (r >> 1) + bit; }
        else r >>= 1;
        bit >>= 2;
    }
    return r;
}

/* ------------------------------------------------------------- allocation */
static uint32_t g_top = ART_BASE;

static Spr spr_new(int w, int h, uint16_t bg)
{
    Spr s;
    s.w = (uint16_t)w;
    s.h = (uint16_t)h;
    s.stride = (uint16_t)(((w * 2 + 7) & ~7));
    s.addr = g_top;
    g_top += (uint32_t)s.stride * (uint32_t)h;
    if (bg) {
        uint16_t *b = A(s.addr);
        int st = s.stride / 2;
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) b[y * st + x] = bg;
    }
    return s;
}

static void pset(const Spr *s, int x, int y, uint16_t c)
{
    if (x < 0 || y < 0 || x >= s->w || y >= s->h) return;
    A(s->addr)[(uint32_t)y * (s->stride / 2) + (uint32_t)x] = c;
}

static void pdisc(const Spr *s, int cx, int cy, int r, uint16_t c)
{
    for (int y = -r; y <= r; y++)
        for (int x = -r; x <= r; x++)
            if (x * x + y * y <= r * r) pset(s, cx + x, cy + y, c);
}

static void pring(const Spr *s, int cx, int cy, int r0, int r1, uint16_t c)
{
    for (int y = -r1; y <= r1; y++)
        for (int x = -r1; x <= r1; x++) {
            int d2 = x * x + y * y;
            if (d2 <= r1 * r1 && d2 >= r0 * r0) pset(s, cx + x, cy + y, c);
        }
}

static void pline(const Spr *s, int x0, int y0, int x1, int y1, int wid, uint16_t c)
{
    int dx = x1 - x0, dy = y1 - y0;
    int n = (dx < 0 ? -dx : dx);
    if ((dy < 0 ? -dy : dy) > n) n = (dy < 0 ? -dy : dy);
    if (n == 0) n = 1;
    for (int i = 0; i <= n; i++)
        pdisc(s, x0 + dx * i / n, y0 + dy * i / n, wid, c);
}

static void mirror_x(Spr *s)
{
    uint16_t *b = A(s->addr);
    int st = s->stride / 2;
    for (int y = 0; y < s->h; y++)
        for (int x = 0; x < s->w / 2; x++) {
            uint16_t t = b[y * st + x];
            b[y * st + x] = b[y * st + (s->w - 1 - x)];
            b[y * st + (s->w - 1 - x)] = t;
        }
}

/* ----------------------------------------------------------- bullet shapes */
/* A bullet is a glossy orb: rim, body, hot core and a specular dot. */
static void orb(Spr *s, int r, uint16_t rim, uint16_t body, uint16_t core)
{
    int cx = s->w / 2, cy = s->h / 2;
    pdisc(s, cx, cy, r, rim);
    pdisc(s, cx, cy, r - 1, body);
    pdisc(s, cx, cy, (r * 55) / 100, core);
    int hx = cx - r / 3, hy = cy - r / 3, hr = (r <= 5) ? 1 : (r / 5 + 1);
    pdisc(s, hx, hy, hr, 0xFFFFu);              /* specular, upper left */
}

/* --------------------------------------------------------------- hulls ---
 * A hull is described by its half width per row, so the silhouette stays
 * perfectly symmetric and the shading follows the distance to the centre line:
 * bright rim, dark crease, then a body that darkens towards the rim.
 */
typedef struct {
    const uint8_t *half;
    uint16_t rim, lit, mid, dk, acc, eng;
    int cock_y, cock_rx, cock_ry;
} Hull;

static void draw_hull(Spr *s, const Hull *h)
{
    int st = s->stride / 2;
    uint16_t *b = A(s->addr);
    int cx = s->w / 2;

    for (int y = 0; y < 64 && y < s->h; y++) {
        int hh = h->half[y];
        if (hh <= 0) continue;
        if (hh > cx) hh = cx;
        for (int d = 0; d <= hh; d++) {
            uint16_t c;
            if (d == hh)          c = h->rim;                   /* lit edge */
            else if (d == hh - 1) c = mix565(h->dk, 0x0000u, 120);
            else {
                int t = 210 - (d * 165) / (hh ? hh : 1) - (y > 34 ? (y - 34) / 2 : 0);
                if (t < 45) t = 45;
                if (t > 255) t = 255;
                c = mix565(h->dk, h->mid, t);
            }
            /* spine highlight: the middle of the fuselage catches the light */
            if (d <= 1 && y > 3 && y < 52) c = mix565(c, h->lit, 170);
            /* sharp leading edge of the wing */
            if (y >= 33 && y <= 44 && d >= hh - 7 && d < hh) c = mix565(c, h->acc, 120);
            if (y > 52 && d <= 5) c = h->eng;                    /* engine bell */
            b[y * st + cx + d] = c;
            if (d) b[y * st + cx - d] = c;
        }
    }
    /* canopy: dark frame, glass gradient, one specular dot */
    for (int pass = 0; pass < 2; pass++) {
        int rx = h->cock_rx + (pass ? 0 : 1), ry = h->cock_ry + (pass ? 0 : 1);
        for (int y = -ry; y <= ry; y++)
            for (int x = -rx; x <= rx; x++) {
                int yy = h->cock_y + y;
                if (yy < 0 || yy >= 64) continue;
                if ((x < 0 ? -x : x) > h->half[yy]) continue;
                int d2 = (x * x * 100) / (rx * rx) + (y * y * 100) / (ry * ry);
                if (d2 > 100) continue;
                if (!pass) { b[yy * st + cx + x] = 0x0841u; continue; }
                uint16_t c = mix565(0x02B0u, 0x07FFu, 240 - d2 * 2);
                b[yy * st + cx + x] = c;
            }
    }
    pset(s, cx - h->cock_rx / 2, h->cock_y - h->cock_ry / 2, 0xFFFFu);
    pset(s, cx - h->cock_rx / 2 + 1, h->cock_y - h->cock_ry / 2, 0xBFFFu);
    /* accent stripes down the fuselage */
    for (int y = 8; y < 48; y++) {
        int hh = h->half[y];
        for (int d = 3; d <= 4; d++)
            if (d <= hh) {
                b[y * st + cx + d] = h->acc;
                b[y * st + cx - d] = h->acc;
            }
    }
    pset(s, cx, 1, 0xFFFFu);                                  /* nose light */
}

static const uint8_t P_HALF[64] = {
    0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,
    7,8,8,9,9,10,10,10,11,11,12,12,13,13,14,15,
    16,18,20,22,24,26,27,27,26,25,24,23,21,19,17,15,
    14,13,12,12,11,10,10,9,9,8,8,7,7,6,5,4
};
static const uint8_t FA_HALF[64] = {
    20,24,26,27,27,26,25,24,23,22,21,20,19,18,17,16,
    15,14,13,12,11,11,10,10,9,9,8,8,7,7,6,6,
    5,5,4,4,4,3,3,3,2,2,2,2,1,1,1,1,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
};
static const uint8_t FB_HALF[64] = {
    6,8,10,12,14,16,18,20,22,24,26,27,28,29,30,30,
    30,30,30,30,30,30,30,30,29,29,28,28,27,27,26,26,
    25,25,24,23,22,21,20,19,18,17,16,15,14,13,12,11,
    10,9,8,7,6,5,4,3,2,1,0,0,0,0,0,0
};
static const uint8_t FC_HALF[64] = {
    1,2,3,4,5,6,7,8,9,10,11,12,13,13,13,13,
    13,13,13,13,12,12,11,11,10,10,9,9,8,8,7,7,
    6,6,5,5,4,4,3,3,2,2,1,1,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
};

/* ---------------------------------------------------------------- sprites */
Spr G_BUL[BK_N];
Spr G_PB[PB_N];
Spr G_SHIP[SHIP_N];
Spr G_FOE[FOE_N];
Spr G_BOSS[2];
Spr G_FLAME[3];
Spr G_BOOM[6];
Spr G_HIT[3];
Spr G_RING[3];
Spr G_STAR[4];
Spr G_NEB[2];

/* 4x4 ordered dither, used to fake a soft edge with a 1-bit key */
static const uint8_t DITH[4][4] = {
    {   0, 128,  32, 160 },
    { 192,  64, 224,  96 },
    {  48, 176,  16, 144 },
    { 240, 112, 208,  80 }
};

static void glow_blob(Spr *s, uint16_t c1, uint16_t c2, int seed)
{
    int cx = s->w / 2, cy = s->h / 2;
    int rmax = (s->w < s->h ? s->w : s->h) / 2 - 2;
    uint32_t rnd = (uint32_t)seed * 1103515245u + 12345u;
    for (int y = 0; y < s->h; y++) {
        for (int x = 0; x < s->w; x++) {
            int dx = x - cx, dy = y - cy;
            int r = isqrt_i(dx * dx + dy * dy);
            if (r > rmax) continue;
            rnd = rnd * 1664525u + 1013904223u;
            int jitter = (int)((rnd >> 24) & 31) - 16;
            int a = 255 - (r * 300) / rmax + jitter;
            if (a <= 10) continue;
            if (a > 255) a = 255;
            if (a < DITH[y & 3][x & 3]) continue;
            pset(s, x, y, mix565(c1, c2, a));
        }
    }
}

static void build_ships(void)
{
    Hull hp = { P_HALF, 0xE7FFu, 0xDFFBu, 0x62CEu, 0x10A2u, 0x07FFu, 0xFE60u, 19, 6, 10 };
    Hull hl = { P_HALF, 0xE7FFu, 0xDFFBu, 0x52AAu, 0x0861u, 0x07FFu, 0xFE60u, 19, 5, 9  };
    Hull hh = { P_HALF, 0xFFFFu, 0xFFFFu, 0xDEFBu, 0x9492u, 0xFFFFu, 0xFFFFu, 19, 7, 11 };
    Hull ea = { FA_HALF, 0xFEA0u, 0xFD60u, 0xA2C0u, 0x4000u, 0xFFE0u, 0xFE60u, 18, 5, 6 };
    Hull eb = { FB_HALF, 0xE81Fu, 0xB8B6u, 0x606Eu, 0x2008u, 0xC81Fu, 0xFE60u, 18, 7, 7 };
    Hull ec = { FC_HALF, 0x87F0u, 0x4FE0u, 0x04C0u, 0x0120u, 0xAFE0u, 0xFE60u, 12, 3, 4 };

    G_SHIP[SHIP_IDLE] = spr_new(64, 64, MAG);
    draw_hull(&G_SHIP[SHIP_IDLE], &hp);
    G_SHIP[SHIP_LEFT] = spr_new(64, 64, MAG);
    draw_hull(&G_SHIP[SHIP_LEFT], &hl);
    G_SHIP[SHIP_RIGHT] = G_SHIP[SHIP_LEFT];
    mirror_x(&G_SHIP[SHIP_RIGHT]);
    G_SHIP[SHIP_HIT] = spr_new(64, 64, MAG);
    draw_hull(&G_SHIP[SHIP_HIT], &hh);

    G_FOE[FOE_A] = spr_new(64, 64, MAG);
    draw_hull(&G_FOE[FOE_A], &ea);
    G_FOE[FOE_B] = spr_new(64, 64, MAG);
    draw_hull(&G_FOE[FOE_B], &eb);
    G_FOE[FOE_C] = spr_new(64, 64, MAG);
    draw_hull(&G_FOE[FOE_C], &ec);

    /* engine flames, three animation frames */
    for (int f = 0; f < 3; f++) {
        G_FLAME[f] = spr_new(24, 48, MAG);
        int len = 30 + f * 8;
        for (int y = 0; y < len; y++) {
            int w = (len - y) * 7 / len;
            for (int x = -w; x <= w; x++) {
                int t = 255 - (y * 235) / len;
                uint16_t c = mix565(0xF800u, 0xFFE0u, t);
                if (y < 7) c = 0xFFFFu;
                else if ((x < 0 ? -x : x) >= w) c = mix565(c, 0xF800u, 90);
                pset(&G_FLAME[f], 12 + x, y, c);
            }
        }
    }
}

static void build_boss(void)
{
    static const int8_t DX[8] = { 100, 92, 71, 38, 0, -38, -71, -92 };
    static const int8_t DY[8] = { 0, 38, 71, 92, 100, 92, 71, 38 };
    static const int8_t EX[4] = { 71, 0, -71, 0 };
    static const int8_t EY[4] = { 0, 71, 0, -71 };

    for (int f = 0; f < 2; f++) {
        Spr *s = &G_BOSS[f];
        int cx = 112, cy = 88;
        uint16_t core = f ? 0xFB60u : 0xFFE0u;

        *s = spr_new(224, 176, MAG);
        pdisc(s, cx, cy, 86, 0x2104u);
        pring(s, cx, cy, 82, 86, 0x39E7u);
        pdisc(s, cx, cy, 78, 0x18E3u);
        for (int k = 0; k < 8; k++)
            pline(s, cx + DX[k] * 24 / 100, cy + DY[k] * 24 / 100,
                     cx + DX[k] * 78 / 100, cy + DY[k] * 78 / 100, 5, 0x4228u);
        pring(s, cx, cy, 60, 66, 0x6B4Du);
        pdisc(s, cx, cy, 60, 0x1082u);
        for (int k = 0; k < 4; k++) {
            int ex = cx + EX[k] * 48 / 100, ey = cy + EY[k] * 48 / 100;
            pdisc(s, ex, ey, 13, 0x8000u);
            pdisc(s, ex, ey, 10, 0xFBC0u);
            pdisc(s, ex, ey, 5, 0xFFFFu);
        }
        pring(s, cx, cy, 24, 30, 0x8410u);
        pdisc(s, cx, cy, 24, 0x18C3u);
        for (int r = 20; r >= 3; r -= 4)
            pdisc(s, cx, cy, r, mix565(core, 0xFFFFu, 255 - r * 9));
        pdisc(s, cx, cy, 6, 0xFFFFu);
        for (int k = 0; k < 4; k++) {
            pline(s, cx + EX[k] * 64 / 100, cy + EY[k] * 64 / 100,
                     cx + EX[k] * 132 / 100, cy + EY[k] * 132 / 100, 4, 0x630Cu);
            pdisc(s, cx + EX[k] * 132 / 100, cy + EY[k] * 132 / 100, 6, 0x8410u);
        }
    }
}

static void build_bullets(void)
{
    G_BUL[BK_R] = spr_new(18, 18, MAG); orb(&G_BUL[BK_R], 8, 0x5800u, 0xF800u, 0xFFE0u);
    G_BUL[BK_B] = spr_new(18, 18, MAG); orb(&G_BUL[BK_B], 8, 0x0010u, 0x03FFu, 0xE7FFu);
    G_BUL[BK_C] = spr_new(18, 18, MAG); orb(&G_BUL[BK_C], 8, 0x02A0u, 0x07E0u, 0xE7FFu);
    G_BUL[BK_P] = spr_new(24, 24, MAG); orb(&G_BUL[BK_P], 11, 0x4010u, 0xB01Fu, 0xFFE0u);
    G_BUL[BK_Y] = spr_new(24, 24, MAG); orb(&G_BUL[BK_Y], 11, 0x7A00u, 0xFFE0u, 0xFFFFu);
    G_BUL[BK_BIG_R] = spr_new(34, 34, MAG); orb(&G_BUL[BK_BIG_R], 16, 0x5000u, 0xF800u, 0xFFF0u);
    G_BUL[BK_BIG_P] = spr_new(34, 34, MAG); orb(&G_BUL[BK_BIG_P], 16, 0x4010u, 0xB81Fu, 0xFFF0u);

    /* rice bullet: vertical capsule, white hot tip */
    G_BUL[BK_RICE] = spr_new(12, 24, MAG);
    for (int y = 0; y < 24; y++)
        for (int x = 0; x < 12; x++) {
            int dx = x - 5, dy = y - 11;
            if (dx * dx + (dy * dy) / 2 <= 12) {
                uint16_t c = (y < 8) ? 0xFFFFu : ((y < 18) ? 0xFC1Fu : 0xE01Fu);
                pset(&G_BUL[BK_RICE], x, y, c);
            }
        }

    /* scale bullet: teardrop pointing up */
    G_BUL[BK_SCALE] = spr_new(18, 18, MAG);
    for (int y = 0; y < 18; y++)
        for (int x = 0; x < 18; x++) {
            int dx = x - 8, dy = y - 8;
            int w2 = 6 - (dy < 0 ? 0 : dy / 4);
            if ((dx < 0 ? -dx : dx) <= w2 && dx * dx + dy * dy <= 58) {
                int d = isqrt_i(dx * dx + dy * dy);
                pset(&G_BUL[BK_SCALE], x, y, d > 4 ? 0x02C0u : (d > 2 ? 0x07FFu : 0xE7FFu));
            }
        }

    /* star bullet */
    G_BUL[BK_STAR] = spr_new(22, 22, MAG);
    for (int y = 0; y < 22; y++)
        for (int x = 0; x < 22; x++) {
            int ax = (x - 10) * 2, ay = (y - 10) * 2;
            if (ax < 0) ax = -ax;
            if (ay < 0) ay = -ay;
            if (ax + ay < 21 || (ax < 4 && ay < 21) || (ay < 4 && ax < 21)) {
                int d = ax + ay;
                pset(&G_BUL[BK_STAR], x, y, d < 7 ? 0xFFFFu : (d < 13 ? 0xFFE0u : 0xFA00u));
            }
        }

    /* player shots */
    G_PB[PB_NEEDLE] = spr_new(8, 24, MAG);
    for (int y = 0; y < 24; y++)
        for (int x = 0; x < 8; x++) {
            int dx = (x - 3) * 2;
            int w2 = 6 - (y * 3) / 24;
            if ((dx < 0 ? -dx : dx) <= w2) {
                uint16_t c = (y < 6) ? 0xFFFFu : ((y < 17) ? 0x07FFu : 0x03EFu);
                pset(&G_PB[PB_NEEDLE], x, y, c);
            }
        }
    G_PB[PB_BALL] = spr_new(12, 12, MAG);
    orb(&G_PB[PB_BALL], 5, 0x0210u, 0x07FFu, 0xFFFFu);
}

static void build_fx(void)
{
    /* explosion: a hot core that dies while a ring expands and thins out */
    for (int f = 0; f < 6; f++) {
        G_BOOM[f] = spr_new(88, 88, MAG);
        int cx = 44, cy = 44;
        int r = 6 + f * 7;
        uint16_t ring = (f < 2) ? 0xFFFFu : ((f < 4) ? 0xFFE0u : 0xFB00u);
        pring(&G_BOOM[f], cx, cy, r - 2, r + 1, ring);
        if (f < 2) pring(&G_BOOM[f], cx, cy, r + 1, r + 4, 0xFFE0u);
        if (f < 3) pdisc(&G_BOOM[f], cx, cy, 14 - f * 4, f == 0 ? 0xFFFFu : 0xFFE0u);
        for (int k = 0; k < 14; k++) {
            int a = (k * 137) & 255;
            int rr = r + ((k * 29) & 15) - 6;
            if (rr < 2) rr = 2;
            int dx = (((a * 7) & 63) - 32) * rr / 32;
            int dy = (((a * 11) & 63) - 32) * rr / 32;
            pdisc(&G_BOOM[f], cx + dx, cy + dy, k & 1, ring);
        }
    }
    for (int f = 0; f < 3; f++) {
        static const int8_t DX[4] = { 1, 0, -1, 0 }, DY[4] = { 0, 1, 0, -1 };
        G_HIT[f] = spr_new(20, 20, MAG);
        int r = 4 + f * 3;
        for (int k = 0; k < 4; k++)
            pline(&G_HIT[f], 10, 10, 10 + DX[k] * r, 10 + DY[k] * r, 1, 0xFFFFu);
        pdisc(&G_HIT[f], 10, 10, f + 1, 0xFFE0u);
    }
    for (int f = 0; f < 3; f++) {
        G_RING[f] = spr_new(140, 140, MAG);
        int r = 20 + f * 20;
        pring(&G_RING[f], 70, 70, r, r + 4, f == 0 ? 0xFFFFu : 0x07FFu);
        pring(&G_RING[f], 70, 70, r + 4, r + 7, 0x03EFu);
    }
}

static void build_stars(void)
{
    G_STAR[0] = spr_new(6, 6, MAG);
    pset(&G_STAR[0], 3, 3, 0x8410u);
    G_STAR[1] = spr_new(6, 6, MAG);
    pset(&G_STAR[1], 3, 3, 0xC618u);
    pset(&G_STAR[1], 2, 3, 0x8C71u);
    pset(&G_STAR[1], 4, 3, 0x8C71u);
    G_STAR[2] = spr_new(6, 6, MAG);
    pset(&G_STAR[2], 3, 3, 0xFFFFu);
    pset(&G_STAR[2], 2, 2, 0xE71Cu);
    pset(&G_STAR[2], 4, 4, 0xE71Cu);
    G_STAR[3] = spr_new(6, 6, MAG);
    for (int i = 1; i < 5; i++) {
        pset(&G_STAR[3], i, 2, 0x9D13u);
        pset(&G_STAR[3], i, 3, 0xFFFFu);
        pset(&G_STAR[3], i, 4, 0x9D13u);
        pset(&G_STAR[3], 2, i, 0x9D13u);
        pset(&G_STAR[3], 3, i, 0xFFFFu);
        pset(&G_STAR[3], 4, i, 0x9D13u);
    }
    pset(&G_STAR[3], 3, 3, 0xFFFFu);
}

static void build_nebula(void)
{
    G_NEB[0] = spr_new(256, 256, MAG);
    glow_blob(&G_NEB[0], 0x0804u, 0x300Cu, 7);
    G_NEB[1] = spr_new(256, 256, MAG);
    glow_blob(&G_NEB[1], 0x0421u, 0x108Au, 21);
}

static void build_ui_texture(void)
{
    static const uint16_t band[4] = { 0x0000u, 0x1084u, 0x8000u, 0xFFFFu };
    for (int b = 0; b < 4; b++) {
        uint16_t *p = A(UI_TEX_BASE + (uint32_t)b * UI_TEX_BAND_H * UI_TEX_STRIDE);
        for (int i = 0; i < 960 * UI_TEX_BAND_H; i++) p[i] = band[b];
    }
}

void art_build(void)
{
    build_ships();
    build_boss();
    build_bullets();
    build_fx();
    build_stars();
    build_nebula();
    build_ui_texture();
}
