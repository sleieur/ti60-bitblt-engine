/******************************************************************************
*
* @file hwt.c: hardware text renderer.
*
* Every palette is pre-rendered once into DDR as a sprite sheet: GLYPH_NCELL
* cells of 24x32 pixels, the 5x7 font scaled x4, the rest of the cell painted in
* the key colour.  A glyph is then drawn with one BitBlt COLOUR KEY task, so the
* on-screen text costs the CPU nothing per pixel and never disturbs the scene
* behind it (the keyed background is skipped).
*
******************************************************************************/
#include "bbshow.h"
#include "bitblt_driver.h"

static const uint16_t pal_color[N_PAL] = {
    C_WHITE, C_BLACK, C_YELLOW, C_CYAN, C_RED, C_GREEN
};

/* only used by the PC preview build; 0 on the target */
static uint32_t mem_delta;
void hwt_set_delta(uint32_t d) { mem_delta = d; }

static inline uint16_t *P(uint32_t addr)
{
    return (uint16_t *)(uintptr_t)(addr + mem_delta);
}

/* fill one cell row with the key colour */
static void clear_row(uint32_t base, int row, uint16_t color, int n)
{
    uint16_t *p = P(base + (uint32_t)row * GLYPH_STRIDE);
    for (int i = 0; i < n; i++) p[i] = color;
}

void hwt_init(void)
{
    for (int p = 0; p < N_PAL; p++) {
        uint32_t base = GLYPH_ADDR + (uint32_t)p * GLYPH_PAL;
        uint16_t col  = pal_color[p];

        for (int r = 0; r < GLYPH_CELL_H; r++)
            clear_row(base, r, GLYPH_KEY, GLYPH_NCELL * GLYPH_CELL_W);

        for (int ci = 0; FONT_CHARS[ci]; ci++) {
            const uint8_t *g = FONT_GLYPHS[ci];
            int cx = ci * GLYPH_CELL_W + 2;
            for (int yy = 0; yy < 7; yy++) {
                uint8_t bits = g[yy];
                for (int xx = 0; xx < 5; xx++) {
                    if ((bits & (0x10u >> xx)) == 0) continue;
                    /* scale x4 */
                    for (int sy = 0; sy < 4; sy++) {
                        uint16_t *row = P(base + (uint32_t)(2 + yy * 4 + sy) * GLYPH_STRIDE);
                        for (int sx = 0; sx < 4; sx++)
                            row[cx + xx * 4 + sx] = col;
                    }
                }
            }
        }
    }
}

void hwt_text(int x, int y, const char *s, int pal)
{
    uint32_t base = GLYPH_ADDR + (uint32_t)pal * GLYPH_PAL;

    for (; *s; s++, x += GLYPH_CELL_W) {
        if (*s == ' ') continue;
        int i = font_index(*s);
        bb_key(base + (uint32_t)i * (GLYPH_CELL_W * 2), GLYPH_STRIDE,
               x, y, GLYPH_CELL_W, GLYPH_CELL_H, GLYPH_KEY, 0);
    }
}

void hwt_text_right(int xr, int y, const char *s, int pal)
{
    int n = 0;
    for (const char *p = s; *p; p++) n++;
    hwt_text(xr - n * GLYPH_CELL_W, y, s, pal);
}

/* 8x scale atlas, used for the score.  Same glyph art, 48x64 cells. */
void hwt_init_big(void)
{
    static const uint16_t col_big[N_PAL_BIG] = { 0xFFFFu, 0xFFE0u };

    for (int p = 0; p < N_PAL_BIG; p++) {
        uint32_t base = GLYPH_BIG_ADDR + (uint32_t)p * GLYPH_BIG_PAL;
        uint16_t col  = col_big[p];

        for (int r = 0; r < GLYPH_BIG_H; r++) {
            uint16_t *row = P(base + (uint32_t)r * GLYPH_BIG_STRIDE);
            for (int i = 0; i < GLYPH_NCELL * GLYPH_BIG_W; i++) row[i] = GLYPH_BIG_KEY;
        }
        for (int ci = 0; FONT_CHARS[ci]; ci++) {
            const uint8_t *g = FONT_GLYPHS[ci];
            int cx = ci * GLYPH_BIG_W + 4;
            for (int yy = 0; yy < 7; yy++) {
                uint8_t bits = g[yy];
                for (int xx = 0; xx < 5; xx++) {
                    if ((bits & (0x10u >> xx)) == 0) continue;
                    for (int sy = 0; sy < 8; sy++) {
                        uint16_t *row = P(base + (uint32_t)(4 + yy * 8 + sy) * GLYPH_BIG_STRIDE);
                        for (int sx = 0; sx < 8; sx++)
                            row[cx + xx * 8 + sx] = col;
                    }
                }
            }
        }
    }
}

void hwt_text_big(int x, int y, const char *s, int pal)
{
    uint32_t base = GLYPH_BIG_ADDR + (uint32_t)pal * GLYPH_BIG_PAL;

    for (; *s; s++, x += 44) {
        if (*s == ' ') continue;
        int i = font_index(*s);
        bb_key(base + (uint32_t)i * (GLYPH_BIG_W * 2), GLYPH_BIG_STRIDE,
               x, y, GLYPH_BIG_W, GLYPH_BIG_H, GLYPH_BIG_KEY, 0);
    }
}
