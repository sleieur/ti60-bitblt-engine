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
/* Sprite heap.  It used to start at 0x05000000, which is exactly CHK_SRC: the
 * self-check scene wrote 160 KiB over the sprites, so anything drawn after
 * scene 9 was corrupt.  0x04400000 is the gap between the CPU compare pages
 * (end 0x04300000) and the picture strip (0x06000000). */
#define ART_BASE   0x04400000u

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

/* ------------------------------------------------- arcade pixel-art pass ---
 * The shape code below paints with analytic shading.  On a 720p panel that
 * reads as vector art, not as an arcade sprite, and the difference is not the
 * drawing - it is three properties of the *output*:
 *
 *   chunky   every feature is a whole cell x cell block, so nothing is sub-pixel
 *   flat     a 16-colour palette, no smooth gradients, no dither noise
 *   inked    a dark outline around the silhouette, which is what makes a sprite
 *            pop off a busy starfield
 *
 * All three are post passes over a finished sprite, so every shape routine above
 * keeps working unchanged and the sprites stay generated (no asset files).
 */
static const uint16_t APAL[16] = {
    0x0000u,   /* 0  black          */
    0x0841u,   /* 1  deep navy      */
    0x1082u,   /* 2  navy           */
    0x18E3u,   /* 3  dark slate     */
    0x2965u,   /* 4  slate          */
    0x4A69u,   /* 5  steel          */
    0x7BEFu,   /* 6  silver         */
    0xFFFFu,   /* 7  white          */
    0x07FFu,   /* 8  cyan           */
    0x03EFu,   /* 9  teal           */
    0x07E0u,   /* 10 green          */
    0xFFE0u,   /* 11 yellow         */
    0xFE60u,   /* 12 amber          */
    0xFC00u,   /* 13 orange         */
    0xF800u,   /* 14 red            */
    0xA01Fu    /* 15 magenta        */
};

static uint16_t qpal(uint16_t c)
{
    int r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
    int best = 0, bd = 1 << 30;
    for (int i = 0; i < 16; i++) {
        int pr = (APAL[i] >> 11) & 31, pg = (APAL[i] >> 5) & 63, pb = APAL[i] & 31;
        int dr = r - pr, dg = g - pg, db = b - pb;
        int d = dr * dr * 3 + dg * dg * 2 + db * db * 3;
        if (d < bd) { bd = d; best = i; }
    }
    return APAL[best];
}

/* Flatten to cell x cell blocks.  Each block takes the *most frequent* palette
 * colour of its pixels rather than their average: averaging mixes a white
 * highlight with the hull blue and produces a muddy grey, which is exactly what
 * turned the first attempt into dark blobs.  Taking the mode keeps highlights
 * and accent stripes alive, and ties go to the brighter entry because APAL is
 * ordered dark to light.  Blocks that are mostly background become background,
 * which keeps the silhouette from growing a fringe. */
static void pixelate(Spr *s, int cell)
{
    int st = s->stride / 2;
    uint16_t *b = A(s->addr);
    for (int by = 0; by < s->h; by += cell) {
        for (int bx = 0; bx < s->w; bx += cell) {
            int hist[16], tot = 0, ink = 0;
            for (int i = 0; i < 16; i++) hist[i] = 0;
            for (int y = by; y < by + cell && y < s->h; y++)
                for (int x = bx; x < bx + cell && x < s->w; x++) {
                    uint16_t c = b[y * st + x];
                    tot++;
                    if (c == MAG) continue;
                    ink++;
                    uint16_t q = qpal(c);
                    for (int i = 0; i < 16; i++)
                        if (APAL[i] == q) { hist[i]++; break; }
                }
            uint16_t out = MAG;
            if (ink * 2 >= tot) {
                int best = 0;
                for (int i = 0; i < 16; i++) if (hist[i] >= hist[best]) best = i;
                out = APAL[best];
            }
            for (int y = by; y < by + cell && y < s->h; y++)
                for (int x = bx; x < bx + cell && x < s->w; x++)
                    b[y * st + x] = out;
        }
    }
}

static inline int ink(uint16_t c) { return c != MAG && c != 0x0001u; }

/* One-cell-thick ink line around the silhouette.  Written in two passes through
 * a sentinel so the line cannot crawl over itself and double in thickness. */
static void ink_outline(Spr *s, int cell, uint16_t col)
{
    int st = s->stride / 2;
    uint16_t *b = A(s->addr);
    for (int y = 0; y < s->h; y++)
        for (int x = 0; x < s->w; x++) {
            if (ink(b[y * st + x])) continue;
            int hit = 0;
            if (x >= cell      && ink(b[y * st + x - cell]))      hit = 1;
            if (!hit && x + cell < s->w && ink(b[y * st + x + cell])) hit = 1;
            if (!hit && y >= cell      && ink(b[(y - cell) * st + x])) hit = 1;
            if (!hit && y + cell < s->h && ink(b[(y + cell) * st + x])) hit = 1;
            if (hit) b[y * st + x] = 0x0001u;
        }
    for (int i = 0; i < st * s->h; i++)
        if (b[i] == 0x0001u) b[i] = col;
}

/* Anything within a hair of the key colour becomes exactly the key colour.
 * The glossy orb shader fades out through magenta, so its faint outer cells sat
 * one or two codes away from 0xF81F: not equal, so the key did not fire, and
 * every bullet kept a pink halo.  Snapping the neighbourhood clears it without
 * touching the bright core. */
static void key_clean(Spr *s)
{
    int st = s->stride / 2;
    uint16_t *b = A(s->addr);
    for (int i = 0; i < st * s->h; i++) {
        uint16_t c = b[i];
        if (c == MAG) continue;
        int r = (c >> 11) & 31, g = (c >> 5) & 63, bl = c & 31;
        /* magenta family, however the shader mixed it: red and blue both high,
         * green near zero.  A narrow band around 0xF81F missed the halo the orb
         * shader leaves at its outer cells. */
        if (g <= 10 && r >= 22 && bl >= 12 && (r + bl) > 3 * g + 40) b[i] = MAG;
    }
}

/* Every sprite that ends up on screen goes through the same three passes. */
static void arcade(Spr *s, int cell)
{
    pixelate(s, cell);
    ink_outline(s, cell, 0x0000u);
}

/* Bullets get the grid and the flat palette but no ink line: a ring around a
 * 18 px orb reads as a target, and in a dense pattern the rings merge into
 * noise.  They stay bright dots, which is what the eye needs to dodge. */
static void arcade_soft(Spr *s, int cell)
{
    pixelate(s, cell);
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
            /* Spine highlight only on the upper fuselage.  Running it the whole
             * length turned into a bright stripe that beat the hull colour once
             * the sprite was flattened, and the ship read as a white dart. */
            if (d <= 1 && y > 5 && y < 26) c = mix565(c, h->lit, 150);
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
                /* Frame is a dark *tinted* ring, not black: a black frame plus a
                 * dark glass gradient quantises into a hole, and the enemy read
                 * as a flying donut. */
                if (!pass) { b[yy * st + cx + x] = mix565(h->dk, 0x0000u, 70); continue; }
                uint16_t c = mix565(0x04B0u, 0x9FFFu, 235 - d2 * 2);
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
/* Silhouettes.  These read as three different roles at a glance, which is what
 * a shooter needs: a fast dart, a broad-winged bomber and a small drone.  The
 * previous version tapered evenly in both directions, which on screen made all
 * three look like flying discs - the "ring" shape people noticed. */
static const uint8_t FA_HALF[64] = {
    1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,
    17,18,19,19,19,19,18,17,16,15,14,13,12,11,10,9,
    8,7,6,5,4,3,2,1,1,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
};
static const uint8_t FB_HALF[64] = {
    3,4,5,6,8,9,10,11,12,13,14,15,16,17,18,19,
    20,22,24,26,28,30,31,32,32,32,32,31,30,29,28,27,
    26,25,24,23,22,21,20,19,18,17,16,15,14,13,12,11,
    10,9,8,7,6,5,4,3,2,1,0,0,0,0,0,0
};
static const uint8_t FC_HALF[64] = {
    1,2,3,4,5,6,7,8,9,10,11,12,12,12,12,12,
    12,12,11,11,10,10,9,9,8,8,7,7,6,6,5,5,
    4,4,3,3,2,2,1,1,1,0,0,0,0,0,0,0,
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
Spr G_PLANET;
Spr G_SWARM[3];
Spr G_GEM;
Spr G_BLADE;
Spr G_HERO[2];
Spr G_ZOMB[3];
Spr G_ZOMB_HIT[3];
Spr G_KNIFE;
Spr G_SHOOT;
Spr G_GROUND;
Spr G_STREAK[2];
Spr G_BLOOM[2];
Spr G_ARM[2];
int  art_pack_loaded;

/* ------------------------------------------------------------ sprite pack ---
 * The RUOK / Kenney CC0 set is packed by tools/pack_game.py into a small blob
 * that lives at GAME_ASSET_ADDR in DDR.  Reading it here is what decides whether
 * the game draws hand-painted sprites or the procedural fallback, so a missing
 * load is a degradation, never a crash.
 */
static uint8_t  *pack_base;
static uint32_t  pack_data;

static uint32_t rd32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static Spr pack_spr(int idx)
{
    Spr s;
    const uint8_t *r = pack_base + 0x10 + idx * 12;
    /* pack_game.py stores already-absolute offsets (they start at 0x1000), so
     * adding the data base again would slide every sprite 4 KiB into the next
     * one - which showed up as diagonal rainbow garbage inside the ships. */
    s.addr   = GAME_ASSET_ADDR + rd32(r);
    s.w      = rd16(r + 4);
    s.h      = rd16(r + 6);
    s.stride = rd16(r + 8);
    return s;
}

static int art_load_pack(void)
{
#if !USE_ART_PACK
    return 0;
#else
    pack_base = (uint8_t *)(uintptr_t)(GAME_ASSET_ADDR + mem_delta);
    if (rd32(pack_base) != GAME_ASSET_MAGIC || rd32(pack_base + 4) != 1) return 0;
    if (rd32(pack_base + 8) < 20) return 0;
    pack_data = rd32(pack_base + 0x0C);

    /* indices are the order of SPRITES[] in pack_game.py */
    G_FOE[FOE_A] = pack_spr(0);
    G_FOE[FOE_B] = pack_spr(2);
    G_FOE[FOE_C] = pack_spr(3);
    G_BOSS[0] = pack_spr(4);
    G_BOSS[1] = pack_spr(5);
    G_ARM[0]  = pack_spr(6);
    G_ARM[1]  = pack_spr(7);

    G_BUL[BK_R]      = pack_spr(8);     /* small: bright tip only  10x18 */
    G_BUL[BK_B]      = pack_spr(9);
    G_BUL[BK_C]      = pack_spr(10);
    G_BUL[BK_P]      = pack_spr(11);    /* medium                   10x30 */
    G_BUL[BK_Y]      = pack_spr(12);
    G_BUL[BK_RICE]   = pack_spr(13);
    G_BUL[BK_SCALE]  = pack_spr(14);    /* large                    10x50 */
    G_BUL[BK_STAR]   = pack_spr(15);
    G_BUL[BK_BIG_R]  = pack_spr(16);
    G_BUL[BK_BIG_P]  = pack_spr(14);

    G_PB[PB_NEEDLE] = pack_spr(17);     /* neon bolt 17x47 */
    G_PB[PB_BALL]   = pack_spr(18);

    /* The RUOK planet ships on a solid black background rather than with alpha,
     * so blitting it puts a black box in the starfield.  The procedural one is
     * transparent by construction; keep it. */

    art_pack_loaded = 1;
    return 1;
#endif
}

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
    /* Rim and lit are deliberately *not* near-white any more.  Once the sprite
     * is flattened to whole blocks, a bright rim wins the block and the hull
     * collapses into a white paper plane; keeping the rim one step above the
     * body leaves the body colour in charge, which is what reads as a painted
     * arcade sprite.  Only the hit-flash hull is allowed to go all white. */
    Hull hp = { P_HALF, 0x4D3Fu, 0x8FFFu, 0x2A6Fu, 0x10A2u, 0x07FFu, 0xFE60u, 19, 6, 10 };
    Hull hl = { P_HALF, 0x3CFFu, 0x6DFFu, 0x224Fu, 0x0861u, 0x07FFu, 0xFE60u, 19, 5, 9  };
    Hull hh = { P_HALF, 0xFFFFu, 0xFFFFu, 0xDEFBu, 0x9492u, 0xFFFFu, 0xFFFFu, 19, 7, 11 };
    Hull ea = { FA_HALF, 0xB3E0u, 0xFD60u, 0x8A20u, 0x4000u, 0xFFE0u, 0xFE60u, 17, 4, 5 };
    Hull eb = { FB_HALF, 0x90CFu, 0xD31Fu, 0x600Eu, 0x2008u, 0xC81Fu, 0xFE60u, 17, 5, 6 };
    Hull ec = { FC_HALF, 0x2F60u, 0x4FE0u, 0x04C0u, 0x0120u, 0xAFE0u, 0xFE60u, 11, 3, 3 };

    G_SHIP[SHIP_IDLE] = spr_new(64, 64, MAG);
    draw_hull(&G_SHIP[SHIP_IDLE], &hp);
    G_SHIP[SHIP_LEFT] = spr_new(64, 64, MAG);
    draw_hull(&G_SHIP[SHIP_LEFT], &hl);
    G_SHIP[SHIP_RIGHT] = G_SHIP[SHIP_LEFT];
    mirror_x(&G_SHIP[SHIP_RIGHT]);
    G_SHIP[SHIP_HIT] = spr_new(64, 64, MAG);
    draw_hull(&G_SHIP[SHIP_HIT], &hh);

    if (!art_pack_loaded) {
        G_FOE[FOE_A] = spr_new(64, 64, MAG);
        draw_hull(&G_FOE[FOE_A], &ea);
        G_FOE[FOE_B] = spr_new(64, 64, MAG);
        draw_hull(&G_FOE[FOE_B], &eb);
        G_FOE[FOE_C] = spr_new(64, 64, MAG);
        draw_hull(&G_FOE[FOE_C], &ec);
    }

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
                /* Tail was magenta-pink (0xFC1F), which is one step from the key
                 * colour and left a visible pink halo around every bullet.  Amber
                 * reads as heat and keys cleanly. */
                uint16_t c = (y < 8) ? 0xFFFFu : ((y < 18) ? 0xFE60u : 0xE300u);
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

/* --------------------------------------------------------------- nebulae ---
 * A nebula is the one place where "more octaves" is free: it is baked once at
 * boot into a 256x256 sprite, so the cost of the maths never reaches the frame
 * budget.  Three octaves of value noise, ridged, mapped through a four-step
 * ramp - that is what turns a radial gradient into something with structure.
 */
static uint32_t nseed = 0x9E3779B9u;
static uint32_t nrand(void)
{
    nseed ^= nseed << 13; nseed ^= nseed >> 17; nseed ^= nseed << 5;
    return nseed;
}

static uint16_t lerp565(uint16_t a, uint16_t b, int t)
{
    int ar = (a >> 11) & 31, ag = (a >> 5) & 63, ab = a & 31;
    int br = (b >> 11) & 31, bg = (b >> 5) & 63, bb = b & 31;
    int r = ar + ((br - ar) * t) / 256;
    int g = ag + ((bg - ag) * t) / 256;
    int bl = ab + ((bb - ab) * t) / 256;
    return (uint16_t)((r << 11) | (g << 5) | bl);
}

/* hash noise on a lattice of `period` cells, softened with a second fetch so
 * the lattice never shows up as a visible grid */
static int onoise(int x, int y, int period, uint32_t salt)
{
    int gx = x * 64 / period, gy = y * 64 / period;
    uint32_t h = (uint32_t)(gx * 73856093u ^ gy * 19349663u ^ salt);
    h ^= h >> 13; h *= 0x5bd1e995u; h ^= h >> 15;
    int v = (int)(h & 255);
    /* soften with a second fetch so the lattice does not show as a grid */
    h = (uint32_t)(gx * 83492791u ^ gy * 2654435761u ^ (salt + 7u));
    h ^= h >> 11; h *= 0x27d4eb2du; h ^= h >> 15;
    int v2 = (int)(h & 255);
    return (v * 3 + v2) / 4;
}

static void build_nebula(int idx, uint32_t salt, uint16_t inner, uint16_t mid,
                         uint16_t outer, int cx, int cy, int radius)
{
    Spr *s = &G_NEB[idx];
    *s = spr_new(256, 256, MAG);
    int st = s->stride / 2;
    uint16_t *b = A(s->addr);

    for (int y = 0; y < 256; y++) {
        for (int x = 0; x < 256; x++) {
            int dx = x - cx, dy = y - cy;
            int d2 = dx * dx + dy * dy;
            if (d2 > radius * radius) continue;
            /* radial falloff, then ridged noise on top */
            int rad = 255 - (isqrt_i(d2) * 255) / radius;
            if (rad <= 0) continue;
            int n = onoise(x, y, 64, salt) * 3 + onoise(x, y, 24, salt + 13) * 2
                    + onoise(x, y, 8, salt + 29);
            n = n / 6;                       /* 0..255 */
            /* The noise has to *own* the edge: any term proportional to the
             * radius alone reproduces a circle no matter how it is faded.  So the
             * shape is noise-gated first (cells below the gate simply do not
             * exist) and the radius only modulates the brightness. */
            int gate = n - (255 - rad);
            if (gate < 0) continue;
            int v = (gate * 3) / 4 + rad / 3;
            if (v < 8) continue;
            if (v > 240) v = 240;

            /* Density dithering instead of a solid disc: keep the cell only if a
             * fixed per-cell threshold falls inside the density.  The result is
             * a cloud of dots whose density follows the noise, which is how you
             * fake alpha with a 1-bit key. */
            int hash = ((x * 12289 + y * 3079) ^ (x * y)) & 255;
            if (hash > v + 40) continue;
            if (hash > v && ((x ^ y) & 1)) continue;

            uint16_t c;
            if (v < 70)       c = lerp565(outer, mid, v * 3);
            else if (v < 150) c = lerp565(mid, inner, (v - 70) * 3);
            else              c = lerp565(inner, 0xFFFFu, (v - 150) * 4);
            b[y * st + x] = c;
        }
    }
}

static void build_nebulae(void)
{
    build_nebula(0, 0x51A3u, 0xFF5Fu, 0xA81Fu, 0x3808u, 128, 128, 126);
    build_nebula(1, 0x7C1Du, 0x2FFFu, 0x0C7Fu, 0x0421u, 96, 150, 118);
}

/* ------------------------------------------------------------- gas giant ---
 * A ringed gas giant: the only thing in the scene that does not move, so it has
 * to carry the depth on its own.
 *
 * Three properties do the work:
 *   - bands that bow with the sphere instead of running dead straight, which is
 *     what separates "painted ball" from "planet" at 100 px across;
 *   - a terminator, so the light has a direction and the sphere has a volume;
 *   - a perspective ring, drawn in two passes around the body so the near half
 *     crosses the disc and the far half disappears behind it.
 */
static void build_planet(void)
{
    static const int BAND_R[6] = { 226, 205, 186, 168, 148, 126 };
    static const int BAND_G[6] = { 186, 163, 142, 124, 104,  86 };
    static const int BAND_B[6] = { 132, 110,  94,  78,  64,  52 };

    const int W = 420, H = 300;
    const int cx = 210, cy = 150, R = 104;
    const int rx = 205, ry = 46;              /* ring outline, near edge-on */

    Spr *s = &G_PLANET;
    *s = spr_new(W, H, MAG);
    int st = s->stride / 2;
    uint16_t *b = A(s->addr);

    /* --- far half of the ring, above the planet's centre line --- */
    for (int y = 0; y < cy; y++) {
        for (int x = 0; x < W; x++) {
            int nx = (x - cx) * 1000 / rx, ny = (y - cy) * 1000 / ry;
            int d = isqrt_i(nx * nx + ny * ny);          /* 0 .. 1000 */
            if (d < 610 || d > 1000) continue;
            int seg = (d - 610) / 78;                    /* four gaps */
            if (seg == 2) continue;
            /* warm dust, not grey: the ring has to belong to the same planet as
             * the tan bands, otherwise it reads as a separate object */
            int tone = 120 + 60 * ((d * 7) & 3) / 3;
            uint16_t c = mix565(0x0000u, (uint16_t)((23 << 11) | (30 << 5) | 18), tone);
            if (((x + y) & 3) == 0) c = mix565(c, 0xFFFFu, 70);
            b[y * st + x] = c;
        }
    }

    /* --- the body --- */
    for (int y = 0; y < s->h; y++) {
        int dy = y - cy;
        if (dy < -R || dy > R) continue;
        int hw = isqrt_i(R * R - dy * dy);
        int dz2 = R * R - dy * dy;
        for (int x = cx - hw; x <= cx + hw; x++) {
            int dx = x - cx;
            int dz = isqrt_i(dz2 - dx * dx);            /* < 0 never happens */

            /* bands bow downwards at the limbs: the latitude of a point on a
             * sphere is its height, not its y */
            int lat = dy + dx * dx / (6 * R);
            int idx = (lat + 600) / 34 % 6;
            if (idx < 0) idx += 6;
            uint16_t c = (uint16_t)((BAND_R[idx] >> 3 << 11) |
                                    (BAND_G[idx] >> 2 << 5) |
                                    (BAND_B[idx] >> 3));

            /* Lambert with a light from the upper left and slightly behind.
             * The dot product is scaled to 0..150 first: computing it directly in
             * integers truncates to 0 or 1 across the whole disc, which paints the
             * planet black and leaves only the rim lit. */
            int lam = (-dx * 3 - dy * 2 + dz * 4) * 100 / (R * 6);
            int k = 55 + lam * 4 / 3;
            c = mix565(0x0000u, c, k > 255 ? 255 : (k < 25 ? 25 : k));

            /* limb: cool rim light where the atmosphere thins out */
            if (dz < 10) c = mix565(c, 0x9FFFu, 150 - dz * 12);
            b[y * st + x] = c;
        }
    }

    /* --- near half of the ring, drawn over the disc --- */
    for (int y = cy; y < s->h; y++) {
        for (int x = 0; x < W; x++) {
            int nx = (x - cx) * 1000 / rx, ny = (y - cy) * 1000 / ry;
            int d = isqrt_i(nx * nx + ny * ny);
            if (d < 610 || d > 1000) continue;
            int seg = (d - 610) / 78;
            if (seg == 2) continue;
            int tone = 150 + 60 * ((d * 7) & 3) / 3;
            uint16_t c = mix565(0x0000u, (uint16_t)((23 << 11) | (30 << 5) | 18), tone);
            /* Where the near half crosses the disc it is backlit, not black: keep
             * it warm but dim, so it reads as a translucent ring passing in front
             * of the planet instead of a painted grey stripe. */
            int dx = x - cx, dy = y - cy;
            int dd = isqrt_i(dx * dx + dy * dy);
            if (dd < R) c = mix565(0x0000u, c, 105);
            if (((x + y) & 3) == 0) c = mix565(c, 0xFFFFu, 60);
            b[y * st + x] = c;
        }
    }

    /* Chunk it up to match the sprite grid, but average in RGB instead of
     * snapping to the 16-colour sprite palette: that palette has no warm tones,
     * so a tan gas giant came out grey. Blocks more than half empty stay empty,
     * which keeps the limb from growing a fringe. */
    for (int by = 0; by < s->h; by += 2) {
        for (int bx = 0; bx < s->w; bx += 2) {
            int rs = 0, gs = 0, bs = 0, n = 0;
            for (int y = by; y < by + 2 && y < s->h; y++)
                for (int x = bx; x < bx + 2 && x < s->w; x++) {
                    uint16_t c = b[y * st + x];
                    if (c == MAG) continue;
                    rs += (c >> 11) & 31; gs += (c >> 5) & 63; bs += c & 31; n++;
                }
            uint16_t out = MAG;
            if (n >= 3) out = (uint16_t)(((rs / n) << 11) | ((gs / n) << 5) | (bs / n));
            for (int y = by; y < by + 2 && y < s->h; y++)
                for (int x = bx; x < bx + 2 && x < s->w; x++)
                    b[y * st + x] = out;
        }
    }
}

/* A kill has to *flash* before it scatters, otherwise the debris just appears.
 * Bloom is a white core with a dithered halo, so it stays a KEY sprite and
 * costs no destination reads. */
static void build_bloom(int idx, int size, uint16_t mid)
{
    Spr *s = &G_BLOOM[idx];
    *s = spr_new(size, size, MAG);
    int c = size / 2;
    int st = s->stride / 2;
    uint16_t *b = A(s->addr);
    for (int y = 0; y < size; y++)
        for (int x = 0; x < size; x++) {
            int dx = x - c, dy = y - c;
            int d2 = dx * dx + dy * dy;
            int r2 = c * c;
            if (d2 > r2) continue;
            int d = (isqrt_i(d2 * 1000 / (r2 ? r2 : 1)));     /* 0 centre .. 31 rim */
            if (d > 30) continue;
            uint16_t col;
            if (d < 6)        col = 0xFFFFu;
            else if (d < 12)  col = lerp565(0xFFFFu, mid, (d - 6) * 42);
            else              col = lerp565(mid, 0x0000u, (d - 12) * 14);
            /* dissolve outwards: every other cell at 50 %, three of four at the rim */
            int keep = (d < 12) ? 1 : ((d < 20) ? (((x + y) & 1) == 0)
                                                : (((x * 3 + y * 5) & 3) == 0));
            if (!keep) continue;
            b[y * st + x] = col;
        }
}

/* -------------------------------------------------- survivors horde sprites ---
 * Three silhouettes, three colours, one grid.  At 24 px a shape is read almost
 * entirely by its outline, so the three are deliberately different shapes
 * (round, jagged, angular) rather than the same blob in different colours:
 * a horde of 600 identical dots turns into noise, a horde of three recognisable
 * kinds reads as an army.
 */
static void build_swarm(void)
{
    static const uint16_t body[3] = { 0x2D28u, 0xD300u, 0x7A5Fu };   /* green / amber / violet */
    static const uint16_t lit[3]  = { 0xAFF5u, 0xFFD0u, 0xC79Fu };

    for (int k = 0; k < 3; k++) {
        Spr *s = &G_SWARM[k];
        *s = spr_new(30, 30, MAG);
        int st = s->stride / 2;
        uint16_t *b = A(s->addr);
        int c = 15, r = k == 1 ? 14 : 12;

        for (int y = 0; y < 30; y++) {
            for (int x = 0; x < 30; x++) {
                int dx = x - c, dy = y - c;
                int d2 = dx * dx + dy * dy;
                int in = 0;
                if (k == 0) {
                    in = d2 < r * r;
                } else if (k == 1) {
                    /* brute: round body with four horns */
                    in = d2 < r * r;
                    if (!in && d2 < (r + 3) * (r + 3) &&
                        (dx * dx > 6 * 6 || dy * dy > 6 * 6)) in = 0;
                    if (!in && ((dx > -4 && dx < 4 && dy < -9) || (dx > -4 && dx < 4 && dy > 9)))
                        in = 1;
                } else {
                    /* elite: hexagon, reads angular even at this size */
                    int ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;
                    in = (ax * 3 + ay * 2) < r * 4;
                }
                if (!in) continue;
                /* lit from the upper left, three steps: enough to look solid
                 * without a gradient that the palette would flatten anyway */
                int lam = (-dx - dy) * 100 / (r * 2);
                uint16_t col;
                if (lam > 55)      col = lit[k];
                else if (lam > 0)  col = mix565(body[k], lit[k], 120);
                else               col = mix565(0x0000u, body[k], 210);
                b[y * st + x] = col;
            }
        }
        /* two eyes, always facing the player: the only detail that survives the
         * scale, and it is what makes a dot look alive */
        pset(s, c - 4, c - 4, 0xFFFFu);
        pset(s, c + 4, c - 4, 0xFFFFu);
        arcade(s, 2);
    }

    /* gem: a 14 px diamond, bright enough to read against the horde */
    {
        Spr *s = &G_GEM;
        *s = spr_new(14, 14, MAG);
        for (int y = 0; y < 14; y++)
            for (int x = 0; x < 14; x++) {
                int d = (x > 6 ? x - 6 : 6 - x) + (y > 6 ? y - 6 : 6 - y);
                if (d > 6) continue;
                pset(s, x, y, d < 2 ? 0xFFFFu : (d < 4 ? 0x07FFu : 0x03EFu));
            }
        arcade(s, 1);
    }

    /* aura blade: a small spinning disc, no orientation needed */
    {
        Spr *s = &G_BLADE;
        *s = spr_new(18, 18, MAG);
        for (int y = 0; y < 18; y++)
            for (int x = 0; x < 18; x++) {
                int dx = x - 9, dy = y - 9;
                int d2 = dx * dx + dy * dy;
                if (d2 > 81) continue;
                pset(s, x, y, d2 > 49 ? 0xFC00u : (d2 > 20 ? 0xFFE0u : 0xFFFFu));
            }
        arcade_soft(s, 1);
    }
}


/* -------------------------------------------------- survivors, oblique view ---
 * The genre is drawn in a fake-3D projection: a character is seen from slightly
 * above and in front, so you get a head, a body and feet, and it stands on a
 * shadow.  A true top-down sprite of the same character is an anonymous disc -
 * that is why the first attempt "looked like a plane carrying a magnet".
 *
 * Every sprite here is built the same way: feet ellipse -> body -> head -> a rim
 * light on the upper left, so the whole cast shares one light direction.
 */
static void vs_shadow(Spr *s, int cx, int cy, int rx, int ry)
{
    for (int y = cy - ry; y <= cy + ry; y++) {
        for (int x = cx - rx; x <= cx + rx; x++) {
            int dx = (x - cx) * 100 / rx, dy = (y - cy) * 100 / ry;
            if (dx * dx + dy * dy > 10000) continue;
            /* dotted, half density at the rim: reads as a soft contact shadow
             * without needing alpha blending */
            if (dx * dx + dy * dy > 5500 && ((x + y) & 1)) continue;
            pset(s, x, y, 0x0841u);
        }
    }
}

static void build_hero(void)
{
    for (int f = 0; f < 2; f++) {
        Spr *s = &G_HERO[f];
        *s = spr_new(32, 46, MAG);
        int cx = 16, feet = 42;
        vs_shadow(s, cx, feet, 9, 3);

        /* legs: one forward in the walk frame, so the idle pose reads as a pose */
        int leg = f ? 1 : 0;
        for (int y = 27; y < feet - 1; y++)
            for (int x = cx - 8; x <= cx + 7; x++) {
                int off = (y > 27 && x > cx) ? leg : ((y > 27 && x < cx) ? -leg : 0);
                pset(s, x + off, y, x < cx + 1 ? 0x2945u : 0x18C3u);
            }
        /* torso: dark blue jacket with an orange strap, lit from the upper left */
        for (int y = 14; y < 29; y++)
            for (int x = cx - 8; x <= cx + 7; x++) {
                int lam = (cx - x) + (22 - y);
                uint16_t c = lam > 10 ? 0x5ADFu : (lam > 2 ? 0x2A5Fu : 0x192Fu);
                if (y > 18 && y < 22 && x > cx - 6) c = 0xFC60u;      /* strap */
                pset(s, x, y, c);
            }
        /* head: helmet + visor, the brightest thing so it draws the eye */
        for (int y = 3; y < 16; y++)
            for (int x = cx - 5; x <= cx + 4; x++) {
                int dx = x - cx, dy = y - 9;
                if (dx * dx + dy * dy * 3 > 44) continue;
                uint16_t c = (dx + dy < -2) ? 0xDFF7u : (dy > 1 ? 0x9C93u : 0xB5B6u);
                if (dy >= 0 && dy <= 2 && dx >= -4 && dx <= 2) c = 0x07FFu;  /* visor */
                pset(s, x, y, c);
            }
        /* rifle held across the body, pointing right */
        for (int x = cx + 5; x < 30; x++) pset(s, x, 23, 0x7BEFu);
        for (int x = cx + 5; x < 14; x++) pset(s, x, 24, 0x39E7u);
        /* a white silhouette of the same shape is the damage flash */
        if (f == 0) {
            G_HERO[1] = spr_new(26, 38, MAG);
            for (int y = 0; y < 46; y++)
                for (int x = 0; x < 32; x++) {
                    uint16_t c = A(s->addr)[(uint32_t)y * (s->stride / 2) + x];
                    if (c != MAG) pset(&G_HERO[1], x, y, 0xFFFFu);
                }
        }
    }
}

static void build_zombies(void)
{
    /* 0 grunt / 1 brute / 2 armoured, plus a white flash version of each */
    static const uint16_t base[3] = { 0x2E44u, 0xD321u, 0x8A5Fu };
    static const uint16_t lite[3] = { 0x7FE9u, 0xFFD3u, 0xC6BFu };

    for (int k = 0; k < 3; k++) {
        Spr *s = &G_ZOMB[k];
        int w = k == 1 ? 34 : 28, h = k == 1 ? 42 : 34;
        *s = spr_new(w, h, MAG);
        int cx = w / 2, feet = h - 3;
        vs_shadow(s, cx, feet, k == 1 ? 9 : 7, 3);

        /* legs */
        for (int y = feet - 8; y < feet; y++)
            for (int x = cx - 4; x <= cx + 3; x++)
                pset(s, x, y, 0x2104u);
        /* body: wider for the brute, blockier for the armoured one */
        for (int y = 8; y < feet - 7; y++)
            for (int x = cx - (k == 1 ? 8 : 6); x <= cx + (k == 1 ? 7 : 5); x++) {
                int dx = x - cx, dy = y - 12;
                if (k == 2 && ((x + y) & 1) == 0) continue;    /* plate texture */
                int lam = (cx - x) + (14 - y);
                uint16_t c = lam > 6 ? lite[k] : (lam > -2 ? base[k]
                              : mix565(0x0000u, base[k], 150));
                pset(s, x, y, c);
            }
        /* head: a smaller blob on top, so the silhouette reads as a person */
        for (int y = 1; y < 10; y++)
            for (int x = cx - 4; x <= cx + 3; x++) {
                int dx = x - cx, dy = y - 5;
                if (dx * dx + dy * dy * 2 > 20) continue;
                pset(s, x, y, (dx + dy < -1) ? lite[k] : base[k]);
            }
        /* two red eyes: at this size they are the only facial detail that lands */
        pset(s, cx - 2, 5, 0xF800u);
        pset(s, cx + 2, 5, 0xF800u);

        /* white-on-hit copy */
        G_ZOMB_HIT[k] = spr_new(w, h, MAG);
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                uint16_t c = A(s->addr)[(uint32_t)y * (s->stride / 2) + x];
                if (c != MAG) pset(&G_ZOMB_HIT[k], x, y, 0xFFFFu);
            }
    }
}

static void build_weapons(void)
{
    /* knife: blade + guard + handle, drawn vertical so the orbit needs no maths */
    {
        Spr *s = &G_KNIFE;
        *s = spr_new(12, 28, MAG);
        for (int y = 1; y < 18; y++)
            for (int x = 4; x < 9; x++) {
                int taper = x - 4 + (y < 4 ? (4 - y) * 2 : 0);
                if (taper > 4) continue;
                pset(s, x, y, x < 6 ? 0xE7FFu : 0x9CF3u);
            }
        for (int x = 2; x < 10; x++) pset(s, x, 18, 0xFC60u);
        for (int y = 19; y < 26; y++)
            for (int x = 5; x < 8; x++) pset(s, x, y, 0x39E7u);
        arcade(s, 1);
    }
    /* bullet: a short tracer, wide enough to see at speed */
    {
        Spr *s = &G_SHOOT;
        *s = spr_new(8, 8, MAG);
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++) {
                int dx = x - 3, dy = y - 3;
                if (dx * dx + dy * dy > 9) continue;
                pset(s, x, y, dx * dx + dy * dy < 3 ? 0xFFFFu : 0xFFE0u);
            }
    }
}

/* ------------------------------------------------------------- ground map ---
 * 1536x864 baked once at boot; the scene blits a 1280x720 window out of it each
 * frame, so a scrolling map costs exactly one task.  Grass is two-tone noise,
 * plus patches, a worn path and scattered stones: enough variety that the eye
 * does not lock onto a repeated tile.
 */
static void build_ground(void)
{
    const int W = 1536, H = 864;
    Spr *s = &G_GROUND;
    *s = spr_new(W, H, 0);
    int st = s->stride / 2;
    uint16_t *b = A(s->addr);

    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            int n = onoise(x, y, 48, 0x5A17u) * 3 + onoise(x, y, 16, 0x5A17u + 7);
            n /= 4;                                   /* 0..255 */
            /* Dark olive, not lawn green: the RGB565 green channel is 6 bits and
             * anything above 20% reads as neon next to the sprites. */
            int g = 6 + n / 12;
            int r = 3 + n / 26;
            int bl = 2 + n / 40;
            /* One road that curves across the map.  Two things to get right:
             * a modulo band tiles into parallel highways, and a per-cell hash
             * gives a jagged stripe - so the curve is a plain parabola, which
             * is smooth by construction. */
            int dx2 = x - W / 2;
            int wob = dx2 * dx2 / 26000;
            int d = y - (H / 2 + wob);
            if (d < 0) d = -d;
            if (d < 40) {
                /* dirt in 565 units: 40 in the red field would clamp to full red
                 * and turn the road salmon-pink */
                int k2 = 255 - d * 5;
                r = (r * (255 - k2) + 11 * k2) / 255;
                g = (g * (255 - k2) + 9 * k2) / 255;
                bl = (bl * (255 - k2) + 6 * k2) / 255;
            }
            uint16_t c = (uint16_t)(((r > 31 ? 31 : r) << 11) |
                                    ((g > 63 ? 63 : g) << 5) |
                                    (bl > 31 ? 31 : bl));
            b[y * st + x] = c;
        }
    }
    /* stones and tufts, placed by hash so the pattern is stable across builds */
    uint32_t h = 0x1234567u;
    for (int i = 0; i < 1600; i++) {
        h = h * 1664525u + 1013904223u;
        int x = (int)(h >> 8) % (W - 6);
        int y = (int)(h >> 20) % (H - 6);
        h = h * 1664525u + 1013904223u;
        int kind = (int)(h >> 12) & 3;
        if (kind < 2) {                               /* stone */
            for (int dy = 0; dy < 3; dy++)
                for (int dx = 0; dx < 4; dx++) {
                    if ((dx == 3 && dy == 2) || (dx == 0 && dy == 0)) continue;
                    pset(s, x + dx, y + dy, dy ? 0x52A9u : 0x7BEFu);
                }
        } else {                                      /* tuft */
            for (int k2 = 0; k2 < 4; k2++) pset(s, x + k2, y + (k2 & 1), 0x2D26u);
        }
    }
}

static void build_streaks(void)
{
    /* Two lengths so the near ones read as faster than the far ones. */
    for (int k = 0; k < 2; k++) {
        int w = k ? 2 : 3, h = k ? 20 : 34;
        G_STREAK[k] = spr_new(w, h, MAG);
        for (int y = 0; y < h; y++) {
            uint16_t c = (y < h / 3) ? 0xFFFFu : ((y < h * 2 / 3) ? 0x7BEFu : 0x39E7u);
            if (k) c = (y < h / 2) ? 0x07FFu : 0x03EFu;
            for (int x = 0; x < w; x++) pset(&G_STREAK[k], x, y, c);
        }
    }
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
    /* Hand-painted sprites first: if the DDR blob is there, everything the pack
     * covers is skipped below and the RAM it would have taken stays free. */
    art_load_pack();

    build_ships();
    if (!art_pack_loaded) {
        build_boss();
        build_bullets();
    }
    build_fx();
    build_stars();
    build_nebulae();
    build_planet();
    build_bloom(0, 96, 0xFFE0u);      /* foe pop: warm white */
    build_bloom(1, 240, 0xFEA0u);     /* boss core: amber */
    build_streaks();
    build_swarm();
    build_hero();
    build_zombies();
    build_weapons();
    build_ground();
    build_ui_texture();

    /* Flatten to the arcade grid.  Cell 2 for the small sprites keeps the shape
     * readable at 64 px; the boss and the planet get cell 3 so their size reads
     * as "big" rather than "detailed". */
    /* Sizes stay as painted: doubling the player to 128 px and the boss to 448 px
     * made both of them sit on top of the playfield instead of in it.  On a
     * 720p panel the arcade proportions are a ~64 px player and a ~224 px boss;
     * the pixel-art grid is what carries the style, not the size. */
    arcade(&G_SHIP[SHIP_IDLE], 2);
    arcade(&G_SHIP[SHIP_LEFT], 2);
    arcade(&G_SHIP[SHIP_RIGHT], 2);
    arcade(&G_SHIP[SHIP_HIT], 2);
    if (!art_pack_loaded) {
        for (int i = 0; i < FOE_N; i++) arcade(&G_FOE[i], 2);
        for (int i = 0; i < 2; i++)     arcade(&G_BOSS[i], 3);
    }
    for (int i = 0; i < 6; i++)     arcade(&G_BOOM[i], 2);
    for (int i = 0; i < 3; i++)     arcade(&G_RING[i], 2);
    for (int i = 0; i < 3; i++)     arcade(&G_HIT[i], 1);
    /* Bullets, blooms and engine flames are deliberately left as painted.
     * Dropping an 18 px orb onto a 2 px grid turns its rim into a dark ring -
     * it reads as a target mark, and a screenful of them reads as noise.  The
     * smooth glossy orb is what the eye can track in a dense pattern, and it
     * sits next to the chunky ships the way arcade sprites and glow hardware
     * always did. */

    for (int i = 0; i < BK_N; i++) key_clean(&G_BUL[i]);
    key_clean(&G_PB[PB_NEEDLE]);
    key_clean(&G_PB[PB_BALL]);
    for (int i = 0; i < 6; i++) key_clean(&G_BOOM[i]);
    for (int i = 0; i < 3; i++) key_clean(&G_RING[i]);
    key_clean(&G_NEB[0]);
    key_clean(&G_NEB[1]);
    key_clean(&G_PLANET);
    for (int i = 0; i < 2; i++) key_clean(&G_BLOOM[i]);
    for (int i = 0; i < 2; i++) key_clean(&G_STREAK[i]);
}
