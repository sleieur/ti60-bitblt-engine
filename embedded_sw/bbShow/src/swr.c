/******************************************************************************
*
* @file swr.c: plain-C rasteriser that produces the left half of the screen
*              (CPUBUF, 640x720 RGB565).  This is the "pure software CPU
*              rendering" side of the on-screen comparison: every pixel is
*              produced by ordinary load/store instructions, no DMA, no engine.
*
* Everything is clipped to x in [0,640) and y in [clip_y0,clip_y1) so that the
* image can be produced band by band (BAND_H rows at a time) across several
* display frames; that keeps the frame submission loop running at 60 Hz while
* the CPU crawls through its much larger workload.
*
******************************************************************************/
#include "bbshow.h"

static int clip_y0 = 0;
static int clip_y1 = CPU_REG_H;

/* only used by the PC preview build; 0 on the target */
static uint32_t mem_delta;
void swr_set_delta(uint32_t d) { mem_delta = d; }

void swr_setclip(int y0, int y1)
{
    clip_y0 = y0;
    clip_y1 = y1;
}

/* Clip a rectangle; sx/sy (may be 0) are moved with the clipped left/top edge. */
static int clipr(int *x, int *y, int *w, int *h, int *sx, int *sy)
{
    if (*x < 0)             { int d = -*x;  *x = 0;        *w -= d; if (sx) *sx += d; }
    if (*y < clip_y0)       { int d = clip_y0 - *y; *y = clip_y0; *h -= d; if (sy) *sy += d; }
    if (*x + *w > CPU_REG_W) *w = CPU_REG_W - *x;
    if (*y + *h > clip_y1)   *h = clip_y1 - *y;
    return (*w > 0 && *h > 0);
}

/* The CPU buffer rotates between three copies so the blit DMA never reads
 * memory that is being rewritten under it (see CPU_ADDR in bbshow.h). */
static uint32_t swr_base = CPU_ADDR;

void swr_set_base(uint32_t addr) { swr_base = addr; }

static inline uint16_t *dst_row(int y)
{
    return (uint16_t *)(uintptr_t)(swr_base + (uint32_t)y * CPU_STRIDE + mem_delta);
}

void swr_fill(int x, int y, int w, int h, uint16_t color)
{
    if (!clipr(&x, &y, &w, &h, 0, 0)) return;

    uint32_t c2 = ((uint32_t)color << 16) | color;
    int head = (x & 1) ? 1 : 0;

    for (int r = 0; r < h; r++) {
        uint16_t *d = dst_row(y + r) + x;
        int n = w;
        if (head) { *d++ = color; n--; }
        uint32_t *d32 = (uint32_t *)d;
        int n2 = n >> 1;
        for (int i = 0; i < n2; i++) *d32++ = c2;
        if (n & 1) *(uint16_t *)d32 = color;
    }
}

void swr_copy(uint32_t src, int sstride, int sx, int sy, int dx, int dy, int w, int h)
{
    if (!clipr(&dx, &dy, &w, &h, &sx, &sy)) return;

    for (int r = 0; r < h; r++) {
        const uint16_t *s = (const uint16_t *)(uintptr_t)
                            (src + mem_delta + (uint32_t)(sy + r) * (uint32_t)sstride) + sx;
        uint16_t *d = dst_row(dy + r) + dx;
        if (((dx | sx) & 1) == 0) {
            const uint32_t *s32 = (const uint32_t *)s;
            uint32_t *d32 = (uint32_t *)d;
            int n2 = w >> 1;
            for (int i = 0; i < n2; i++) d32[i] = s32[i];
            if (w & 1) d[w - 1] = s[w - 1];
        } else {
            for (int i = 0; i < w; i++) d[i] = s[i];
        }
    }
}

void swr_key(uint32_t src, int sstride, int sx, int sy, int dx, int dy, int w, int h, uint16_t key)
{
    if (!clipr(&dx, &dy, &w, &h, &sx, &sy)) return;

    for (int r = 0; r < h; r++) {
        const uint16_t *s = (const uint16_t *)(uintptr_t)
                            (src + mem_delta + (uint32_t)(sy + r) * (uint32_t)sstride) + sx;
        uint16_t *d = dst_row(dy + r) + dx;
        for (int i = 0; i < w; i++) {
            uint16_t v = s[i];
            if (v != key) d[i] = v;
        }
    }
}

/* v * 255 / 255 without a divide: exact enough for 0..65025 */
#define DIV255(v)  (((v) + 1 + ((v) >> 8)) >> 8)

void swr_alpha(uint32_t src, int sstride, int sx, int sy, int dx, int dy, int w, int h, int alpha)
{
    if (!clipr(&dx, &dy, &w, &h, &sx, &sy)) return;
    if (alpha <= 0) return;
    if (alpha > 255) alpha = 255;

    uint32_t a = (uint32_t)alpha, ia = 255u - a;

    for (int r = 0; r < h; r++) {
        const uint16_t *s = (const uint16_t *)(uintptr_t)
                            (src + mem_delta + (uint32_t)(sy + r) * (uint32_t)sstride) + sx;
        uint16_t *d = dst_row(dy + r) + dx;
        for (int i = 0; i < w; i++) {
            uint16_t sv = s[i], dv = d[i];
            uint32_t r5 = DIV255(((sv >> 11) & 0x1F) * a + ((dv >> 11) & 0x1F) * ia);
            uint32_t g6 = DIV255(((sv >> 5)  & 0x3F) * a + ((dv >> 5)  & 0x3F) * ia);
            uint32_t b5 = DIV255(( sv        & 0x1F) * a + ( dv        & 0x1F) * ia);
            if (r5 > 31) r5 = 31;
            if (g6 > 63) g6 = 63;
            if (b5 > 31) b5 = 31;
            d[i] = (uint16_t)((r5 << 11) | (g6 << 5) | b5);
        }
    }
}

void swr_text(int x, int y, const char *s, uint16_t color)
{
    if (y + 21 <= clip_y0 || y >= clip_y1) return;          /* nothing of this band */

    for (; *s; s++, x += 18) {
        if (*s == ' ') continue;
        const uint8_t *g = FONT_GLYPHS[font_index(*s)];
        for (int yy = 0; yy < 7; yy++) {
            uint8_t bits = g[yy];
            if (!bits) continue;
            for (int xx = 0; xx < 5; xx++)
                if (bits & (0x10u >> xx))
                    swr_fill(x + xx * 3, y + yy * 3, 3, 3, color);
        }
    }
}

void swr_clear_all(uint16_t color)
{
    swr_setclip(0, CPU_REG_H);
    swr_fill(0, 0, CPU_REG_W, CPU_REG_H, color);
}
