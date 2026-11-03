/******************************************************************************
*
* @file scenes.c: the demonstration scenes.
*
* Two kinds of scene:
*
*  - "split" scenes (SPLIT_FILL, SPLIT_COPY) put the same content on both halves
*    of the screen.  The left half is produced by the ordinary C rasteriser
*    (swr.c, plain load/store, no DMA); the right half by BitBlt tasks.  The
*    engine recomposes the screen 60 times a second, so the hardware half runs at
*    a full 60 Hz while the left half can only be refreshed as fast as the CPU
*    finishes it - the difference is what the demo is about.
*
*  - "engine" scenes (KEY, ALPHA, GAME) use the accelerator only.
*
******************************************************************************/
#include "bbshow.h"
#include "bitblt_driver.h"
#include "game.h"

/* ------------------------------------------------------------------ tuning */
#define MAX_SPLIT   1024

#define SPLIT_BG    C_DARKBG
#define PANEL       C_PANEL

/* ------------------------------------------------------------------ state */
uint32_t g_frame;
uint32_t g_sprites;
uint32_t g_cpu_ms;
uint32_t g_cpu_hz;
uint32_t g_hw_fps;
uint32_t g_max_sprites;
int      g_count;

static int      cur;
static int      repaint;

/* split scenes: the same squares on both halves */
typedef struct { int16_t x, y; int8_t dx, dy; uint8_t size; uint8_t pad; uint16_t color; } sobj_t;
static sobj_t sobj[MAX_SPLIT];
static sobj_t sobj_snap[MAX_SPLIT];
static int    n_split;
static uint32_t cpu_img;

/* key scene */
typedef struct { int16_t x, y; int8_t dx, dy; uint8_t type; uint8_t pad; } kobj_t;
static kobj_t kobj[512];
static int    n_key;

/* alpha scene */
static kobj_t mobj[16];



/* ------------------------------------------------------------------ helpers */
static char *app_str(char *p, const char *s) { while (*s) *p++ = *s++; return p; }

static char *app_u32(char *p, uint32_t v)
{
    char t[10];
    int  n = 0;
    if (v == 0) { *p++ = '0'; return p; }
    while (v) { t[n++] = (char)('0' + (v % 10u)); v /= 10u; }
    while (n) *p++ = t[--n];
    return p;
}

static char *app_hex(char *p, uint32_t v)
{
    static const char hx[] = "0123456789ABCDEF";
    for (int i = 28; i >= 0; i -= 4) *p++ = hx[(v >> i) & 15u];
    return p;
}

static uint32_t lcg_s = 0x12345678u;
static uint32_t lcg(void) { lcg_s = lcg_s * 1664525u + 1013904223u; return lcg_s >> 8; }
static int rnd(int lo, int hi) { return lo + (int)(lcg() % (uint32_t)(hi - lo + 1)); }

/* ================================================================ HUD ==== */
/*
 * One layout system for every scene.
 *
 * The old overlays were hand-placed with fixed-size panels, so a title that grew
 * past its panel simply ran off it - "8  SPRITE LIMIT  KEY + ALPHA" is 27
 * characters, 648 px of glyphs, in a 664 px panel that started at x = 652, i.e.
 * 36 px past the right edge of the screen.  Everything below is sized from the
 * text it has to hold, so overflow cannot happen by construction:
 *
 *   - panels are sized from the widest string they contain
 *   - values are right-aligned inside the panel
 *   - all scenes share the same margins, so the eye can follow from scene to
 *     scene in a recording
 */
#define UI_MARGIN   8
#define UI_TOP_Y    8
#define UI_TOP_H    48
#define UI_BOT_H    48
#define UI_BOT_Y    (SCREEN_H - UI_MARGIN - UI_BOT_H)
#define UI_PAD      12
#define UI_ROW      32
#define UI_FPS_W    240            /* white chip the engine overlay writes into */
#define TXT_ADV     24             /* hwt_text advance (cell 24x32) */
#define TXT_H       28             /* visible glyph height          */
#define BIG_ADV     44             /* hwt_text_big advance          */
#define BIG_H       56
#define CPU_ADV     18             /* swr_text advance (5x7 at x3)  */
#define CPU_TXT_H   21

typedef struct { const char *k, *v; uint8_t vpal; }  uikv_t;
typedef struct { const char *k, *v; uint16_t vcol; } uicpu_t;

/* The FPGA draws "FPS: NN" over the picture in the pixel path (fps_overlay.v),
 * after the framebuffer, in pure black, so the glyphs themselves cannot be
 * touched by software.  A white plate is the only thing that makes those black
 * digits readable, so every scene paints one - the chip is part of the design,
 * not something to hide. */
static void hw_fps_plate(void)
{
    bb_fill(UI_MARGIN, UI_TOP_Y, UI_FPS_W, UI_TOP_H, C_WHITE, 0);
}



/* panel + text on the hardware-rendered side */
static void hw_panel(int x, int y, int w, int h) { bb_fill(x, y, w, h, PANEL, 0); }

/* 3 px rule down the left edge of a panel: cheap way to make flat rectangles
 * read as deliberate design rather than as leftover boxes */
static void hw_rule(int x, int y, int h, uint16_t c) { bb_fill(x, y, 3, h, c, 0); }

/* Scene name across the top, to the right of the frame-rate plate.  The plate
 * itself is left to the caller: see hw_fps_plate(). */
static void hw_top(const char *title)
{
    int x = UI_MARGIN + UI_FPS_W + 8;
    int w = SCREEN_W - x - UI_MARGIN;

    hw_panel(x, UI_TOP_Y, w, UI_TOP_H);
    hw_rule(x, UI_TOP_Y, UI_TOP_H, C_CYAN);
    hwt_text(x + UI_PAD, UI_TOP_Y + 10, title, PAL_WHITE);
}

/* one explanatory line along the bottom.  A recording has no narration, so the
 * frame has to say what it is showing. */
static void hw_bottom(const char *line)
{
    hw_panel(UI_MARGIN, UI_BOT_Y, SCREEN_W - 2 * UI_MARGIN, UI_BOT_H);
    hw_rule(UI_MARGIN, UI_BOT_Y, UI_BOT_H, C_GREEN);
    hwt_text(UI_MARGIN + UI_PAD, UI_BOT_Y + 10, line, PAL_WHITE);
}

/* key/value card with right-aligned values.  Pass w = 0 to size to content and
 * get the width back. */
static int hw_card(int x, int y, int w, const char *title, const uikv_t *kv, int n)
{
    int i, tw = title ? hwt_len(title) : 0;
    int h, ty;

    for (i = 0; i < n; i++) {
        int rw = hwt_len(kv[i].k) + 28 + (kv[i].v ? hwt_len(kv[i].v) : 0);
        if (rw > tw) tw = rw;
    }
    /* A supplied width may be a minimum, never a crop: content always wins, and
     * the card is slid back inside the screen afterwards. */
    if (w <= 0 || w < tw + 2 * UI_PAD + 3) w = tw + 2 * UI_PAD + 3;
    if (x + w > SCREEN_W - UI_MARGIN) x = SCREEN_W - UI_MARGIN - w;
    if (x < UI_MARGIN) x = UI_MARGIN;

    h = UI_PAD - 4 + (title ? TXT_H + 8 : 0) + n * UI_ROW + UI_PAD - 6;
    hw_panel(x, y, w, h);
    hw_rule(x, y, h, C_GREEN);

    ty = y + UI_PAD - 4;
    if (title) { hwt_text(x + UI_PAD, ty, title, PAL_CYAN); ty += TXT_H + 8; }
    for (i = 0; i < n; i++) {
        hwt_text(x + UI_PAD, ty, kv[i].k, PAL_WHITE);
        if (kv[i].v && kv[i].v[0])
            hwt_text_right(x + w - UI_PAD, ty, kv[i].v, kv[i].vpal);
        ty += UI_ROW;
    }
    return w;
}

/* ---------------------------------------------------------------- left half
 * The CPU half has its own rasteriser and a smaller font (swr_text is a 5x7
 * glyph at x3, 18 px advance), so it gets its own card.  Same margins and same
 * row rhythm, which is what makes the two halves read as one interface even
 * though one of them is drawn by software.
 */
static int cpu_len(const char *s) { int n = 0; while (s[n]) n++; return n * CPU_ADV; }

static int cpu_card(int x, int y, int w, const char *title, const uicpu_t *kv, int n)
{
    int i, tw = title ? cpu_len(title) : 0;
    int h, ty;

    for (i = 0; i < n; i++) {
        int rw = cpu_len(kv[i].k) + 20 + (kv[i].v ? cpu_len(kv[i].v) : 0);
        if (rw > tw) tw = rw;
    }
    if (w <= 0 || w < tw + 2 * UI_PAD + 3) w = tw + 2 * UI_PAD + 3;
    if (x + w > CPU_REG_W - UI_MARGIN) x = CPU_REG_W - UI_MARGIN - w;
    if (x < UI_MARGIN) x = UI_MARGIN;

    h = UI_PAD - 6 + (title ? CPU_TXT_H + 10 : 0) + n * UI_ROW + UI_PAD - 6;
    swr_fill(x, y, w, h, PANEL);
    swr_fill(x, y, 3, h, C_GREEN);

    ty = y + UI_PAD - 6;
    if (title) { swr_text(x + UI_PAD, ty, title, C_CYAN); ty += CPU_TXT_H + 10; }
    for (i = 0; i < n; i++) {
        swr_text(x + UI_PAD, ty, kv[i].k, C_WHITE);
        if (kv[i].v && kv[i].v[0]) {
            int n2 = 0;
            while (kv[i].v[n2]) n2++;
            swr_text(x + w - UI_PAD - n2 * CPU_ADV, ty, kv[i].v, kv[i].vcol);
        }
        ty += UI_ROW;
    }
    return w;
}

/* one line along the bottom of the CPU half */
static void cpu_bottom(const char *line)
{
    swr_fill(UI_MARGIN, UI_BOT_Y, CPU_REG_W - 2 * UI_MARGIN, UI_BOT_H, PANEL);
    swr_fill(UI_MARGIN, UI_BOT_Y, 3, UI_BOT_H, C_GREEN);
    swr_text(UI_MARGIN + UI_PAD, UI_BOT_Y + 14, line, C_WHITE);
}

/* ================================================================ INTRO === */

/* Nine rows, and the badge on each row is the hot key that jumps to it: this
 * page is 1, so the list runs 1..9 and the summary - which has no row of its
 * own - is the 0 in the bar underneath. */
static const char *INTRO_L[] = {
    "INTRO         OVERVIEW OF THE DEMO",
    "SPLIT FILL    CPU VS HARDWARE FRAME RATE",
    "SPLIT COPY    SAME, 640X720 PICTURES",
    "IMAGES        STILLS AND GIF FROM DDR",
    "ALPHA BLEND   0 64 128 192 255",
    "COLOUR KEY    300 SPRITES, NO MAGENTA",
    "STARFALL      DANMAKU GAME, AUTO PILOT",
    "SPRITE LIMIT  COUNT HELD AT 60 FPS",
    "SELF CHECK    EVERY PIXEL COMPARED",
};
#define N_INTRO_ROW ((int)(sizeof(INTRO_L) / sizeof(INTRO_L[0])))
static const char *INTRO_KEY[] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9"
};

static void intro_hw(void)
{
    int y;

    bb_fill(0, 0, SCREEN_W, SCREEN_H, 0x0841u, 0);
    /* a lighter band behind the title so the two blocks read as separate */
    bb_fill(0, 0, SCREEN_W, 4, C_CYAN, 0);
    bb_fill(0, 100, SCREEN_W, 2, C_PANEL, 0);   /* under the title, above the specs */

    /* starts to the right of the frame-rate plate (which ends at x = 248) so the
     * hardware-stamped digits never sit on top of the title */
    hwt_text_big(280, 26, "BITBLT ENGINE DEMO", 0);
    hwt_text(64, 116, "EFINIX TI60F225  +  SAPPHIRE RISC-V SOC", PAL_CYAN);
    hwt_text(64, 152, "HDMI 1280X720P60  RGB565  DOUBLE BUFFERED", PAL_YELLOW);

    y = 200;
    for (int i = 0; i < N_INTRO_ROW; i++, y += 42) {
        /* key badge, then the row text: fixed columns keep the list tidy */
        hw_panel(60, y - 4, 44, 36);
        hw_rule(60, y - 4, 36, C_YELLOW);
        hwt_text(70, y, INTRO_KEY[i], PAL_YELLOW);
        hwt_text(120, y, INTRO_L[i], PAL_WHITE);
    }

    hw_fps_plate();
    hw_bottom("SPACE NEXT  1-9 JUMP  0 SUMMARY  A AUTO");
}

/* ============================================================ SPLIT FILL === */

static void split_fill_hw(void)
{
    char nb[16], *p;
    uikv_t kv[3];
    int    n = n_split;

    bb_fill(640, 0, 640, SCREEN_H, SPLIT_BG, 0);
    for (int i = 0; i < n; i++) {
        sobj_t *o = &sobj[i];
        bb_fill(640 + o->x, o->y, o->size, o->size, o->color, 0);
    }
    bb_fill(640, 0, 4, SCREEN_H, C_WHITE, 0);            /* seam */

    p = app_u32(nb, (uint32_t)n); *p = 0;
    kv[0].k = "FRAME RATE"; kv[0].v = "60 FPS";  kv[0].vpal = PAL_YELLOW;
    kv[1].k = "SQUARES";    kv[1].v = nb;        kv[1].vpal = PAL_WHITE;
    kv[2].k = "COMMAND";    kv[2].v = "FILL";    kv[2].vpal = PAL_CYAN;
    hw_card(656, UI_TOP_Y + UI_TOP_H + 12, 0, "HARDWARE BITBLT", kv, 3);

    hw_panel(648, UI_BOT_Y, SCREEN_W - 648 - UI_MARGIN, UI_BOT_H);
    hw_rule(648, UI_BOT_Y, UI_BOT_H, C_GREEN);
    hwt_text(648 + UI_PAD, UI_BOT_Y + 10, "ONE FILL TASK PER SQUARE", PAL_WHITE);

    hw_fps_plate();
    g_sprites = (uint32_t)n + 1u;
}

static void split_fill_cpu(void)
{
    char nb[16], *p;
    uicpu_t kv[3];
    int     n = n_split;

    swr_fill(0, 0, 640, SCREEN_H, SPLIT_BG);
    for (int i = 0; i < n; i++) {
        sobj_t *o = &sobj_snap[i];
        swr_fill(o->x, o->y, o->size, o->size, o->color);
    }

    p = app_u32(nb, (uint32_t)n); *p = 0;
    {
        static char ms[16], hz[16];
        char *q;
        q = app_u32(ms, g_cpu_ms); q = app_str(q, " MS"); *q = 0;
        q = app_u32(hz, g_cpu_hz); q = app_str(q, " HZ"); *q = 0;
        kv[0].k = "FRAME TIME"; kv[0].v = ms; kv[0].vcol = C_WHITE;
        kv[1].k = "REFRESH";    kv[1].v = hz; kv[1].vcol = C_YELLOW;
    }
    kv[2].k = "SQUARES"; kv[2].v = nb; kv[2].vcol = C_WHITE;
    cpu_card(UI_MARGIN, UI_TOP_Y + UI_TOP_H + 12, 0, "SOFTWARE  C RASTERISER", kv, 3);

    cpu_bottom("PLAIN C LOOP, NO DMA, NO ENGINE");
    /* the engine half hides the badge (it prints its own frame rate), so this
     * half must hide it too or the two halves would disagree */
    swr_fill(UI_MARGIN, UI_TOP_Y, UI_FPS_W, UI_TOP_H, C_WHITE);
}


/* ============================================================ SPLIT COPY === */

static void split_copy_hw(void)
{
    char nb[16], *p;
    uikv_t kv[3];

    bb_copy(IMG_ADDR(g_frame & 3u) + 1280, IMG_STRIDE, 640, 0, 640, SCREEN_H, 0);
    bb_fill(640, 0, 4, SCREEN_H, C_WHITE, 0);

    p = app_str(nb, "PIC "); p = app_u32(p, (g_frame & 3u) + 1u); p = app_str(p, "/4"); *p = 0;
    kv[0].k = "FRAME RATE"; kv[0].v = "60 FPS"; kv[0].vpal = PAL_YELLOW;
    kv[1].k = "PICTURE";    kv[1].v = nb;       kv[1].vpal = PAL_WHITE;
    kv[2].k = "COMMAND";    kv[2].v = "COPY";   kv[2].vpal = PAL_CYAN;
    hw_card(656, UI_TOP_Y + UI_TOP_H + 12, 0, "HARDWARE COPY", kv, 3);

    hw_panel(648, UI_BOT_Y, SCREEN_W - 648 - UI_MARGIN, UI_BOT_H);
    hw_rule(648, UI_BOT_Y, UI_BOT_H, C_GREEN);
    hwt_text(648 + UI_PAD, UI_BOT_Y + 10, "640X720 IN ONE TASK", PAL_WHITE);

    hw_fps_plate();
}

static void split_copy_cpu(void)
{
    char nb[16], *p;
    uicpu_t kv[3];
    static char ms[16], hz[16];
    char *q;

    swr_copy(IMG_ADDR(cpu_img), IMG_STRIDE, 0, 0, 0, 0, 640, SCREEN_H);

    p = app_str(nb, "PIC "); p = app_u32(p, cpu_img + 1u); p = app_str(p, "/4"); *p = 0;
    q = app_u32(ms, g_cpu_ms); q = app_str(q, " MS"); *q = 0;
    q = app_u32(hz, g_cpu_hz); q = app_str(q, " HZ"); *q = 0;
    kv[0].k = "FRAME TIME"; kv[0].v = ms; kv[0].vcol = C_WHITE;
    kv[1].k = "REFRESH";    kv[1].v = hz; kv[1].vcol = C_YELLOW;
    kv[2].k = "PICTURE";    kv[2].v = nb; kv[2].vcol = C_GREEN;
    cpu_card(UI_MARGIN, UI_TOP_Y + UI_TOP_H + 12, 0, "SOFTWARE MEMCPY", kv, 3);

    cpu_bottom("C MEMCPY, ONE FRAME PER SECOND");
    swr_fill(UI_MARGIN, UI_TOP_Y, UI_FPS_W, UI_TOP_H, C_WHITE);
}

/* ================================================================== KEY === */

static void key_hw(void)
{
    char  nb[16], *p;
    uikv_t kv[4];
    int    cw;

    bb_copy(IMG_ADDR(1), IMG_STRIDE, 0, 0, SCREEN_W, SCREEN_H, 0);

    for (int i = 0; i < n_key; i++) {
        kobj_t *o = &kobj[i];
        bb_key(SHEET48_ADDR + (uint32_t)o->type * (SHEET48_CELL * 2), SHEET48_STRIDE,
               o->x, o->y, SHEET48_CELL, SHEET48_CELL, C_MAGENTA, 0);
    }
    /* a row along the bottom: odd x positions, first and last half off screen */
    for (int i = 0; i < 20; i++) {
        int x = (i == 0) ? -24 : (i == 19) ? SCREEN_W - 24 : 4 + 62 * i;
        bb_key(SHEET48_ADDR, SHEET48_STRIDE, x, 616, SHEET48_CELL, SHEET48_CELL, C_MAGENTA, 0);
    }

    p = app_u32(nb, (uint32_t)n_key); *p = 0;
    kv[0].k = "SPRITES";    kv[0].v = nb;        kv[0].vpal = PAL_WHITE;
    kv[1].k = "KEY COLOUR"; kv[1].v = "MAGENTA"; kv[1].vpal = PAL_CYAN;
    kv[2].k = "FRAME RATE"; kv[2].v = "60 FPS";  kv[2].vpal = PAL_YELLOW;
    kv[3].k = "EDGE CLIP";  kv[3].v = "ON";      kv[3].vpal = PAL_GREEN;

    /* bottom-right, sitting directly above the strip of sprites that hangs off
     * both screen edges - that row demonstrates clipping, so the card must not
     * cover it */
    cw = 0;
    {
        int i, tw = hwt_len("KEY SPRITES");
        for (i = 0; i < 4; i++) {
            int rw = hwt_len(kv[i].k) + 28 + hwt_len(kv[i].v);
            if (rw > tw) tw = rw;
        }
        cw = tw + 2 * UI_PAD + 3;
    }
    hw_card(SCREEN_W - UI_MARGIN - cw, UI_TOP_Y + UI_TOP_H + 12,
            cw, "KEY SPRITES", kv, 4);

    hw_top("6  COLOUR KEY   300 SPRITES");
    hw_bottom("MAGENTA NEVER REACHES THE SCREEN");
    hw_fps_plate();        /* no frame rate of its own: keep the hardware badge */
    g_sprites = (uint32_t)n_key + 20u;
}

/* ================================================================ ALPHA === */

static int alpha_fixed(int i) { return (i == 4) ? 255 : 64 * i; }
static int alpha_moving(int i, uint32_t f)
{
    int p = (int)((f * 4u + (uint32_t)(64 * i)) % 510u);
    return (p > 255) ? 510 - p : p;
}

static void alpha_hw(void)
{
    char b[32], *p;

    bb_copy(IMG_ADDR(0), IMG_STRIDE, 0, 0, SCREEN_W, SCREEN_H, 0);

    for (int i = 0; i < 5; i++) {
        int a = alpha_fixed(i);
        int x = 140 + 220 * i;
        bb_alpha(ALPHA96_ADDR, ALPHA96_STRIDE, x, 300, ALPHA96_CELL, ALPHA96_CELL,
                 (uint32_t)a, 0);
        bb_fill(x - 6, 252, 108, 36, C_BLACK, 0);
        p = app_u32(b, (uint32_t)a); *p = 0;
        hwt_text(x + 2, 256, b, PAL_YELLOW);
    }

    for (int i = 0; i < 8; i++) {
        int a = alpha_moving(i, g_frame);
        bb_alpha(ALPHA96_ADDR, ALPHA96_STRIDE, mobj[i].x, mobj[i].y,
                 ALPHA96_CELL, ALPHA96_CELL, (uint32_t)a, 0);
    }

    /* one legend bar, three lines, sitting under the moving squares.
     * The squares sweep y 400..600, so the bar starts at 604; its height is
     * 3 rows of 30 plus padding, and the last row of glyphs ends at 704, i.e.
     * inside the bar and inside the screen.  The old bar was 560..652 with a
     * row drawn at 628, so the last line hung 4 px below the panel - that is
     * the green text that looked detached from its background. */
    hw_panel(UI_MARGIN, 604, SCREEN_W - 2 * UI_MARGIN, 104);
    hw_rule(UI_MARGIN, 604, 104, C_GREEN);
    hwt_text(UI_MARGIN + UI_PAD, 612, "FIXED   5 SQUARES AT ALPHA 0 64 128 192 255", PAL_WHITE);
    hwt_text(UI_MARGIN + UI_PAD, 642, "MOVING  8 SQUARES, ALPHA SWEEPS 0 TO 255", PAL_CYAN);
    hwt_text(UI_MARGIN + UI_PAD, 672, "OUT = SRC X A + DST X (1-A), ONE TASK EACH", PAL_GREEN);

    hw_top("5  ALPHA BLEND   HARDWARE MIXER");
    hw_fps_plate();
    g_sprites = 13u;
}

/* ============================================================== SUMMARY === */

static void summary_hw(void)
{
    char   b[8][24], *p;
    char   big[16];
    uikv_t kv[2][4];
    uint32_t cpu_fps = g_cpu_ms ? 1000u / g_cpu_ms : 0;
    uint32_t ratio   = (cpu_fps && g_hw_fps) ? (g_hw_fps + cpu_fps - 1u) / cpu_fps : 0;

    bb_fill(0, 0, SCREEN_W, SCREEN_H, 0x0841u, 0);

    /* every number below comes from a register or a timer, nothing is quoted
     * from a datasheet - the printed page has to survive a judge recomputing it */
    p = app_u32(b[0], g_hw_fps); p = app_str(p, " FPS"); *p = 0;
    p = app_u32(b[1], (uint32_t)limit_best()); *p = 0;
    p = app_u32(b[2], bb_frames()); *p = 0;
    p = app_str(b[3], "0X"); p = app_hex(p, bb_capability()); *p = 0;

    kv[0][0].k = "FRAME RATE";  kv[0][0].v = b[0]; kv[0][0].vpal = PAL_YELLOW;
    kv[0][1].k = "SPRITES";     kv[0][1].v = b[1]; kv[0][1].vpal = PAL_YELLOW;
    kv[0][2].k = "FRAMES";      kv[0][2].v = b[2]; kv[0][2].vpal = PAL_WHITE;
    kv[0][3].k = "CAPABILITY";  kv[0][3].v = b[3]; kv[0][3].vpal = PAL_CYAN;

    p = app_u32(b[4], g_cpu_ms); p = app_str(p, " MS"); *p = 0;
    p = app_u32(b[5], cpu_fps);  p = app_str(p, " FPS"); *p = 0;
    if (ratio) { p = app_str(b[6], "X"); p = app_u32(p, ratio); }
    else       { p = app_str(b[6], "N/A"); }
    *p = 0;
    p = app_str(b[7], "640X720"); *p = 0;

    kv[1][0].k = "RENDER ONE 640X720"; kv[1][0].v = b[4]; kv[1][0].vpal = PAL_RED;
    kv[1][1].k = "CPU LIMIT";           kv[1][1].v = b[5]; kv[1][1].vpal = PAL_RED;
    kv[1][2].k = "ENGINE IS FASTER";    kv[1][2].v = b[6]; kv[1][2].vpal = PAL_GREEN;
    kv[1][3].k = "AT";                  kv[1][3].v = b[7]; kv[1][3].vpal = PAL_WHITE;

    hw_card(60, 96, 0, "HARDWARE ENGINE", kv[0], 4);
    hw_card(700, 96, 0, "SOFTWARE CPU", kv[1], 4);

    /* headline: one number that carries the whole story, on its own panel so the
     * caption below it is not floating on the background */
    p = app_str(big, "X"); p = app_u32(p, ratio ? ratio : 1u); *p = 0;
    {
        int n = 0, bwid, bw, bh, x, y;
        const char *cap = "FASTER THAN SOFTWARE";

        while (big[n]) n++;
        bwid = n * BIG_ADV;
        bw = bwid + 48;
        if (bw < hwt_len(cap) + 2 * UI_PAD) bw = hwt_len(cap) + 2 * UI_PAD;
        bh = BIG_H + 20 + UI_ROW + UI_PAD;

        x = SCREEN_W / 2 - bw / 2;
        y = 372;
        hw_panel(x, y, bw, bh);
        hw_rule(x, y, bh, C_YELLOW);
        hwt_text_big(SCREEN_W / 2 - bwid / 2 + 4, y + 14, big, 1);
        hwt_text(SCREEN_W / 2 - hwt_len(cap) / 2, y + BIG_H + 22, cap, PAL_CYAN);
    }

    hw_top("0  SUMMARY   MEASURED ON BOARD");
    hw_bottom("DDR3 256MB  AXI4 128BIT  3 MASTERS");
    hw_fps_plate();
}

/* ================================================ sprite count limit === */
/*
 * The contest asks for the maximum number of live sprites that still holds
 * 60 FPS *with hardware alpha blending and colour keying both enabled*.  This
 * scene does exactly that and nothing else:
 *
 *   - background is a single FILL (cheapest possible full-screen task)
 *   - every sprite is colour keyed (one KEY task)
 *   - every 8th sprite also gets a translucent halo (one ALPHA task)
 *   - the sprite count is driven by an AIMD probe: grow fast while the engine
 *     keeps up, back off and halve the step when the frame rate drops
 *
 * The number on screen is measured, not assumed.  Everything the engine has to
 * do per frame is in the counters, so the DDR bandwidth is the real limit.
 */
/* The engine issues one descriptor per picture row, so a 24x24 sprite costs 24
 * descriptors.  At 100 MHz a 16.7 ms frame can drain roughly 4000 of them,
 * which is why the practical ceiling is a few hundred elements, not thousands.
 * The probe is allowed to climb well past that so the frame rate really does
 * fall - that is the "until it drops" part of the challenge. */
#define LIM_MAX       2048
#define LIM_CELL      24            /* sprite blit size            */
#define LIM_GLOW_DIV  4             /* one ALPHA slab every 4 elements */

typedef struct { int16_t x, y; int8_t dx, dy; uint8_t cell; uint8_t pad; } lim_t;

static lim_t lim[LIM_MAX];
static int   lim_n;                 /* sprites alive right now         */
static int   lim_step;              /* AIMD step                       */
static int   lim_best;              /* best count that held 60 FPS     */
static int   lim_key, lim_alpha;    /* tasks issued in the last frame  */
static int   lim_locked;            /* the probe has settled           */

static void limit_reset(void)
{
    lim_n    = 64;
    lim_step = 64;
    lim_best = 0;
    lim_locked = 0;
    for (int i = 0; i < LIM_MAX; i++) {
        lim[i].x    = (int16_t)rnd(0, SCREEN_W - LIM_CELL);
        lim[i].y    = (int16_t)rnd(0, SCREEN_H - LIM_CELL);
        lim[i].dx   = (int8_t)(rnd(1, 4) * ((lcg() & 1) ? 1 : -1));
        lim[i].dy   = (int8_t)(rnd(1, 4) * ((lcg() & 1) ? 1 : -1));
        lim[i].cell = (uint8_t)rnd(0, ORB32_NCELL - 1);
    }
}

static void limit_update(void)
{
    for (int i = 0; i < lim_n; i++) {
        lim_t *o = &lim[i];
        o->x = (int16_t)(o->x + o->dx);
        o->y = (int16_t)(o->y + o->dy);
        if (o->x < 0)                 { o->x = 0; o->dx = (int8_t)-o->dx; }
        if (o->x > SCREEN_W - LIM_CELL) { o->x = SCREEN_W - LIM_CELL; o->dx = (int8_t)-o->dx; }
        if (o->y < 0)                 { o->y = 0; o->dy = (int8_t)-o->dy; }
        if (o->y > SCREEN_H - LIM_CELL) { o->y = SCREEN_H - LIM_CELL; o->dy = (int8_t)-o->dy; }
    }
}

static void limit_hw(void)
{
    char b[40];
    char *p;

    /* Vertical gradient instead of one flat colour.  It costs 24 fills (well
     * under 1% of the frame) and turns the background from a solid slab into a
     * depth cue, so the field of particles has something to sit in. */
    for (int b = 0; b < 24; b++) {
        int t  = b;                                  /* 0 at the top */
        int r  = 1 + (3  * t) / 24;
        int g  = 1 + (5  * t) / 24;
        int bl = 10 + (14 * t) / 24;
        bb_fill(0, b * 30, SCREEN_W, 30,
                (uint16_t)((r << 11) | (g << 5) | bl), 0);
    }

    int nk = 0, na = 0;
    for (int i = 0; i < lim_n; i++) {
        const lim_t *o = &lim[i];
        if ((i & (LIM_GLOW_DIV - 1)) == 0) {
            /* every 4th element is a translucent halo, so both accelerated
             * operations (ALPHA and KEY) are live in the same frame.  The
             * window is taken from the centre of the 96x96 radial ramp so the
             * halo is symmetric around the orb. */
            bb_alpha(ALPHA96_ADDR + 36u * ALPHA96_STRIDE + 72u, ALPHA96_STRIDE,
                     o->x, o->y, LIM_CELL, LIM_CELL, 120u, 0);
            na++;
        } else {
            /* centre 24x24 of a 32x32 glow orb, keyed on magenta */
            bb_key(ORB32_ADDR + (uint32_t)o->cell * ORB32_CELL * ORB32_STRIDE
                             + 4u * ORB32_STRIDE + 8u,
                   ORB32_STRIDE, o->x, o->y, LIM_CELL, LIM_CELL, C_MAGENTA, 0);
            nk++;
        }
    }
    lim_key   = nk;
    lim_alpha = na;
    g_sprites = (uint32_t)(nk + na);

    /* ---- HUD ---- */
    {
        char    fpsb[24], spb[24], kab[32], stb[24], big[16];
        char   *q;
        uikv_t  kv[3];

        q = app_u32(fpsb, g_hw_fps);
        q = app_str(q, g_hw_fps >= 59u ? " FPS  OK" : " FPS  LOW"); *q = 0;
        q = app_u32(spb, (uint32_t)lim_n); *q = 0;
        q = app_str(kab, "KEY "); q = app_u32(q, (uint32_t)nk);
        q = app_str(q, "  A ");  q = app_u32(q, (uint32_t)na); *q = 0;
        q = app_str(stb, lim_locked ? "BACKING OFF" : "CLIMBING"); *q = 0;

        kv[0].k = "FRAME RATE"; kv[0].v = fpsb; kv[0].vpal = (g_hw_fps >= 59u) ? PAL_GREEN : PAL_RED;
        kv[1].k = "SPRITES";    kv[1].v = spb;  kv[1].vpal = PAL_WHITE;
        kv[2].k = "TASKS";      kv[2].v = kab;  kv[2].vpal = PAL_CYAN;
        hw_card(UI_MARGIN, UI_TOP_Y + UI_TOP_H + 12, 0, "PROBE", kv, 3);

        /* One panel holds the big number and its two captions.  The captions
         * used to be drawn straight onto the scene, on top of the field of
         * moving orbs, and were unreadable - white text over coloured light. */
        q = app_u32(big, (uint32_t)(lim_best > 0 ? lim_best : lim_n)); *q = 0;
        {
            int n = 0, bwid, bw, bh, x, y;

            while (big[n]) n++;
            bwid = n * BIG_ADV;
            bw = bwid + 48;
            if (bw < hwt_len("KEY AND ALPHA ACTIVE TOGETHER") + 2 * UI_PAD)
                bw = hwt_len("KEY AND ALPHA ACTIVE TOGETHER") + 2 * UI_PAD;
            bh = BIG_H + 20 + 2 * UI_ROW + UI_PAD;

            x = SCREEN_W / 2 - bw / 2;
            y = 252;
            hw_panel(x, y, bw, bh);
            hw_rule(x, y, bh, C_CYAN);
            hwt_text_big(SCREEN_W / 2 - bwid / 2 + 4, y + 14, big, 1);
            hwt_text(SCREEN_W / 2 - hwt_len("SPRITES AT 60 FPS") / 2, y + BIG_H + 22,
                     "SPRITES AT 60 FPS", PAL_WHITE);
            hwt_text(SCREEN_W / 2 - hwt_len("KEY AND ALPHA ACTIVE TOGETHER") / 2,
                     y + BIG_H + 22 + UI_ROW,
                     "KEY AND ALPHA ACTIVE TOGETHER", PAL_CYAN);
        }

        hw_top("8  SPRITE LIMIT   KEY + ALPHA");
        hw_bottom("CLIMBS UNTIL 60 FPS BREAKS, THEN BACKS OFF");
    }
}

int limit_best(void) { return lim_best; }

static void limit_tick(void)
{
    if (g_hw_fps >= 59u) {
        if (lim_n > lim_best) lim_best = lim_n;      /* this count is proven */
        lim_n += lim_step;
        if (lim_n > LIM_MAX) lim_n = LIM_MAX;
        if (lim_step < 256) lim_step *= 2;
        lim_locked = 0;
    } else {
        lim_n -= lim_step;
        if (lim_n < 32) lim_n = 32;
        lim_step /= 2;
        if (lim_step < 8) lim_step = 8;
        lim_locked = 1;
    }
}

/* =============================================================== images === */
/*
 * Photographs.  They are not part of the program image - a single 1280x720
 * RGB565 picture is 1.75 MiB and the program is linked into 124 KiB of RAM.
 * tools/img2assets.py packs them into tools/assets.bin with a small header and
 * the debug launcher restores that file to 0x06000000 with
 *
 *     -ex "restore .../assets.bin binary 0x06000000"
 *
 * so the scene only has to COPY screenfuls out of DDR.  Each picture gets its
 * own front/back pair so switching is a plain COPY, not a CPU memcpy.
 */
static uint32_t img_count;
static uint32_t img_cur;
static uint32_t img_cpu_frame;  /* frame the CPU half is drawing (locked) */
static uint32_t img_delta;      /* 0 on the board, !=0 in the PC previewer */
static uint32_t chk_delta;      /* same, for the self-check scratch buffers */

/* Playback: the strip is advanced on the display clock, not on wall time, so
 * the rate is exact and independent of how busy the CPU is.  At 60 Hz output a
 * 12 fps clip means "advance one picture every 5 frames". */
static int      img_playing = 1;
static int      img_fps     = IMG_DEF_FPS;  /* 1 .. 60, set from the header */
static uint32_t img_acc;                /* in 1/60 frame units */

#define IMG_FPS_MIN   1
#define IMG_FPS_MAX   60

void scenes_set_delta(uint32_t d) { img_delta = d; chk_delta = d; }

static uint32_t img_header(uint32_t off)
{
    return *(volatile uint32_t *)(uintptr_t)(IMG_DATA_BASE + off + img_delta);
}

static void images_reset(void)
{
    img_cur       = 0;
    img_cpu_frame = 0;
    img_acc       = 0;
    img_count     = 0;
    /* always start playing: leaving this alone meant that a pause in one visit
     * made the strip look dead the next time the scene was entered */
    img_playing   = 1;
    img_fps       = IMG_DEF_FPS;
    if (img_header(0) == IMG_DATA_MAGIC) {
        img_count = img_header(4);
        if (img_count > IMG_MAX_PICS) img_count = IMG_MAX_PICS;
        /* honour a rate recorded in the blob (img2assets.py --fps); 0 means
         * "not recorded" and keeps IMG_DEF_FPS, which is 60 on purpose: the
         * engine half is meant to look like it is running flat out */
        uint32_t h = img_header(IMG_HDR_FPS);
        if (h >= IMG_FPS_MIN && h <= IMG_FPS_MAX) img_fps = (int)h;
    }
}

void images_toggle_play(void) { img_playing = !img_playing; img_acc = 0; }

void images_fps_step(int delta)
{
    img_fps += delta;
    if (img_fps < IMG_FPS_MIN) img_fps = IMG_FPS_MIN;
    if (img_fps > IMG_FPS_MAX) img_fps = IMG_FPS_MAX;
}

/* advance the strip one picture and restart the fade */
void images_next(void)
{
    if (img_count == 0) return;
    img_cur = (img_cur + 1u) % img_count;
}

/* called once per displayed frame: keeps playback locked to the 60 Hz output */
static void images_advance(void)
{
    if (!img_playing || img_count < 2) return;
    img_acc += (uint32_t)img_fps;
    while (img_acc >= 60u) {
        img_acc -= 60u;
        images_next();
    }
}

/* ---- the CPU half: the same 640x720 frames, copied by C instead of by the
 * engine.  This is the whole point of the scene: identical data, two ways of
 * getting it to the screen, and the left one cannot keep up. ---- */
static void images_cpu_note(void);

void images_cpu_band(int band)
{
    int y0 = band * BAND_H;
    int h  = (y0 + BAND_H > SCREEN_H) ? (SCREEN_H - y0) : BAND_H;
    char b[40];
    char *p;

    swr_setclip(y0, y0 + h);
    swr_fill(0, y0, CPU_REG_W, h, C_BLACK);
    if (img_count > 0) {
        /* img_cpu_frame, not img_cur: the CPU needs ~20 display frames to draw
         * one 640x720 image band by band, and the engine side advances img_cur
         * every frame.  Reading img_cur here made the top band come from one
         * picture and the bottom from another - that was the tearing. */
        swr_copy(IMG_PIC_ADDR(img_cpu_frame), (uint32_t)IMG_PIC_W * 2u,
                 0, y0, 0, y0, IMG_PIC_W, h);
    }

    /* Two chips pinned to the top of the half, on a solid plate: one line of
     * numbers straight on the picture was unreadable over bright frames.  The
     * chip is small on purpose - the picture is the demo, the numbers are the
     * annotation. */
    p = app_str(b, "CPU ");
    p = app_u32(p, g_cpu_ms); p = app_str(p, "MS ");
    p = app_u32(p, g_cpu_hz); p = app_str(p, "HZ"); *p = 0;
    /* both halves print their own rate, so the hardware badge is switched off */
    swr_fill(UI_MARGIN, UI_TOP_Y, UI_FPS_W, UI_TOP_H, C_WHITE);
    {
        /* the chip is sized from its own string: "CPU 71MS 6HZ" at 18 px per
         * character needs more room than a fixed 176 px box had */
        int n = 0, w;
        while (b[n]) n++;
        w = n * CPU_ADV + 2 * UI_PAD;
        swr_fill(UI_MARGIN + UI_FPS_W + 8, UI_TOP_Y, w, UI_TOP_H, PANEL);
        swr_fill(UI_MARGIN + UI_FPS_W + 8, UI_TOP_Y, 3, UI_TOP_H, C_GREEN);
        swr_text(UI_MARGIN + UI_FPS_W + 8 + UI_PAD, UI_TOP_Y + 14, b, C_YELLOW);
    }

    images_cpu_note();          /* clipped away on every band but the last */
}

/* left half of the bottom bar; the engine half is drawn by images_hw */
static void images_cpu_note(void)
{
    swr_fill(UI_MARGIN, UI_BOT_Y, 640 - 2 * UI_MARGIN, UI_BOT_H, PANEL);
    swr_fill(UI_MARGIN, UI_BOT_Y, 3, UI_BOT_H, C_GREEN);
    swr_text(UI_MARGIN + UI_PAD, UI_BOT_Y + 14, "C MEMCPY, ONE FRAME AT A TIME", C_WHITE);
}

static void images_hw(void)
{
    char b[40];
    char *p;

    if (img_count == 0) {
        bb_fill(0, 0, SCREEN_W, SCREEN_H, 0x0841u, 0);
        hw_top("4  IMAGES   NOTHING LOADED");
        hwt_text(SCREEN_W / 2 - hwt_len("RESTORE TOOLS/ASSETS.BIN TO 0X06000000") / 2, 300,
                 "RESTORE TOOLS/ASSETS.BIN TO 0X06000000", PAL_YELLOW);
        hwt_text(SCREEN_W / 2 - hwt_len("RUN TOOLS/LOAD_ASSETS.PS1") / 2, 340,
                 "RUN TOOLS/LOAD_ASSETS.PS1", PAL_CYAN);
        hw_bottom("THE STRIP LIVES IN DDR, NOT IN THE PROGRAM IMAGE");
        hw_fps_plate();
        return;
    }

    /* right half only: the same frame, one COPY task */
    bb_copy((uint32_t)IMG_PIC_ADDR(img_cur), (uint32_t)IMG_PIC_W * 2u,
            640, 0, IMG_PIC_W, IMG_PIC_H, 0);

    /* seam between the two halves */
    bb_fill(639, 0, 2, SCREEN_H, C_WHITE, 0);

    /* Same chip as the CPU half, but the two chips must not collide: the engine
     * rate chip starts at the left edge of the engine half and the picture index
     * is pinned to the right edge, so together they use 240 + 216 px of the
     * 632 px available and can never overlap however long the strings get. */
    {
        char nbr[16], *q;

        if (img_playing && img_count > 1) {
            p = app_str(b, "ENG ");
            p = app_u32(p, (uint32_t)img_fps); p = app_str(p, "FPS");
        } else {
            p = app_str(b, "ENG PAUSED");
        }
        *p = 0;
        q = app_str(nbr, "PIC "); q = app_u32(q, img_cur + 1u);
        q = app_str(q, "/"); q = app_u32(q, img_count); *q = 0;

        int ex = 648;                                  /* left edge of engine half */
        int ew = hwt_len(b) + 2 * UI_PAD;
        int pw = hwt_len(nbr) + 2 * UI_PAD;
        int px = SCREEN_W - UI_MARGIN - pw;

        if (ex + ew > px - 8) ew = px - 8 - ex;        /* never reach the index chip */

        hw_panel(ex, UI_TOP_Y, ew, UI_TOP_H);
        hw_rule(ex, UI_TOP_Y, UI_TOP_H, C_CYAN);
        hwt_text(ex + UI_PAD, UI_TOP_Y + 10, b, PAL_YELLOW);

        hw_panel(px, UI_TOP_Y, pw, UI_TOP_H);
        hw_rule(px, UI_TOP_Y, UI_TOP_H, C_CYAN);
        hwt_text(px + UI_PAD, UI_TOP_Y + 10, nbr, PAL_WHITE);
    }

    /* right half of the bottom bar only: the CPU half is blitted over x 0..639
     * after this, so a full-width bar here would be sliced in half mid-word */
    hw_panel(648, UI_BOT_Y, SCREEN_W - 648 - UI_MARGIN, UI_BOT_H);
    hw_rule(648, UI_BOT_Y, UI_BOT_H, C_GREEN);
    hwt_text(648 + UI_PAD, UI_BOT_Y + 10, "ONE COPY TASK PER FRAME", PAL_WHITE);
    hw_fps_plate();

    images_advance();          /* stays locked to the 60 Hz frame clock */
    g_sprites = 1;
}

/* ============================================================ self check === */
/*
 * Functional verification, on the board, of the thing the numbers claim.
 *
 * Every accelerated operation is run into a scratch buffer and the result is
 * compared against what the CPU expects, pixel by pixel.  This is the only
 * evidence in the demo that the engine computes the right answer rather than
 * just a fast one, and it is what turns "60 FPS" from a claim into a
 * measurement: a wrong pixel anywhere shows up as a count on screen.
 *
 * The scratch buffers are addressed absolutely, so neither display page is
 * touched and the check cannot disturb what the audience is watching.
 *
 * The two ALPHA cases use 255 and 0 on purpose: whatever rounding the hardware
 * uses for a partial mix, blending at full and zero opacity must reproduce the
 * source and the destination exactly.  Any implementation that fails those two
 * is broken regardless of what it does in between, so the test needs no
 * assumption about the mixer's internal precision.
 */
typedef struct {
    const char *name;
    uint8_t     pass;
    uint32_t    px;            /* pixels compared */
    uint32_t    bad;           /* mismatches */
    int32_t     first;         /* index of the first mismatch, -1 if none */
} chkt_t;

static chkt_t chk[4];
static uint32_t chk_total_px, chk_total_bad;

static uint16_t *chk_p(uint32_t addr, int x, int y)
{
    return (uint16_t *)(uintptr_t)(addr + chk_delta +
                                   (uint32_t)y * CHK_STRIDE + (uint32_t)x * 2u);
}

/* Bounded wait: a wedged engine must fail the check, not hang the demo. */
static int chk_wait(void)
{
    for (uint32_t i = 0; i < 4000000u; i++)
        if (bb_idle()) return 0;
    return -1;
}

/* Compares the engine's output against a CPU-built reference and records the
 * verdict for the screen. */
static void chk_compare(int t, uint32_t expect_addr)
{
    uint32_t bad = 0;
    int32_t  first = -1;

    for (int y = 0; y < CHK_H; y++) {
        for (int x = 0; x < CHK_W; x++) {
            uint16_t got = *chk_p(CHK_DST, x, y);
            uint16_t exp = *chk_p(expect_addr, x, y);

            if (got != exp) {
                if (first < 0) first = y * CHK_W + x;
                bad++;
            }
        }
    }
    chk[t].px    = (uint32_t)CHK_W * CHK_H;
    chk[t].bad   = bad;
    chk[t].first = first;
    chk[t].pass  = (bad == 0);
    chk_total_px  += chk[t].px;
    chk_total_bad += bad;
}

/* Deterministic pattern: neighbour pixels differ in every channel, so a shifted
 * copy, a stuck bit or a channel swap all show up instead of cancelling out. */
static uint16_t chk_pat(int x, int y)
{
    return (uint16_t)((((x * 7 + y * 13) & 0x1Fu) << 11) |
                      (((x * 3 + y * 5)  & 0x3Fu) << 5)  |
                       ((x + y * 2)      & 0x1Fu));
}

#define CHK_REF (CHK_DST + 0x20000u)     /* CPU-built expectation buffer */

static void chk_fill_expect(uint16_t colour)
{
    for (int y = 0; y < CHK_H; y++)
        for (int x = 0; x < CHK_W; x++)
            *chk_p(CHK_REF, x, y) = colour;
}

static void check_run(void)
{
    chk_total_px  = 0;
    chk_total_bad = 0;
    for (int i = 0; i < 4; i++) {
        chk[i].pass = 0; chk[i].px = 0; chk[i].bad = 0; chk[i].first = -1;
    }
    chk[0].name = "FILL";
    chk[1].name = "COPY";
    chk[2].name = "KEY";
    chk[3].name = "ALPHA";

    if (chk_wait() != 0) return;

    /* 1: FILL puts one colour in every pixel */
    bb_fill_abs(CHK_DST, CHK_STRIDE, CHK_W, CHK_H, 0x1234u);
    chk_fill_expect(0x1234u);
    if (chk_wait() != 0) return;
    chk_compare(0, CHK_REF);

    /* 2: COPY moves the pattern unchanged */
    for (int y = 0; y < CHK_H; y++)
        for (int x = 0; x < CHK_W; x++) {
            uint16_t v = chk_pat(x, y);
            *chk_p(CHK_SRC, x, y) = v;
            *chk_p(CHK_REF, x, y) = v;
        }
    bb_fence();
    if (bb_copy_abs(CHK_SRC, CHK_STRIDE, CHK_DST, CHK_STRIDE, CHK_W, CHK_H) != 0) return;
    if (chk_wait() != 0) return;
    chk_compare(1, CHK_REF);

    /* 3: KEY leaves the destination alone where the source is magenta */
    for (int y = 0; y < CHK_H; y++)
        for (int x = 0; x < CHK_W; x++) {
            int      keyed = ((x / 8 + y / 8) & 1) == 0;   /* checkerboard */
            uint16_t pat   = chk_pat(x, y);

            *chk_p(CHK_SRC, x, y) = keyed ? C_MAGENTA : pat;
            *chk_p(CHK_DST, x, y) = 0x0A5Au;               /* must survive */
            *chk_p(CHK_REF, x, y) = keyed ? 0x0A5Au : pat;
        }
    bb_fence();
    if (bb_key_abs(CHK_SRC, CHK_STRIDE, CHK_DST, CHK_STRIDE, CHK_W, CHK_H,
                   C_MAGENTA) != 0) return;
    if (chk_wait() != 0) return;
    chk_compare(2, CHK_REF);

    /* 4: ALPHA at 255 over black must reproduce the source exactly */
    for (int y = 0; y < CHK_H; y++)
        for (int x = 0; x < CHK_W; x++) {
            uint16_t v = chk_pat(x, y);
            *chk_p(CHK_SRC, x, y) = v;
            *chk_p(CHK_DST, x, y) = 0x0000u;
            *chk_p(CHK_REF, x, y) = v;
        }
    bb_fence();
    if (bb_alpha_abs(CHK_SRC, CHK_STRIDE, CHK_DST, CHK_STRIDE, CHK_W, CHK_H, 255u) != 0) return;
    if (chk_wait() != 0) return;
    chk_compare(3, CHK_REF);
}

static int  chk_ran;
static uint32_t chk_bad_px, chk_tested_px;

/* Fixed columns, so nothing can overlap however the numbers grow:
 *
 *   x  80   operation name
 *   x 320   pixels compared
 *   x 600   verdict
 *   x 848   right-hand totals card (424 px wide, ends at 1272)
 */
#define CHK_ROW0   188
#define CHK_ROWH   42
#define CHK_C_NAME 80
#define CHK_C_PX   320
#define CHK_C_RES  600
#define CHK_TAB_X  56
#define CHK_TAB_W  744
#define CHK_TAB_Y  172
#define CHK_TAB_H  (4 * CHK_ROWH + 56)

static void check_hw(void)
{
    char    nb[24], nb2[24], vs[12];
    char   *p, *q;
    uikv_t  kv[2];
    int     i, ok, fail_row;

    if (!chk_ran) {
        check_run();
        chk_ran = 1;
        chk_bad_px    = chk_total_bad;
        chk_tested_px = chk_total_px;
    }
    ok = (chk_bad_px == 0 && chk_tested_px > 0);

    bb_fill(0, 0, SCREEN_W, SCREEN_H, 0x0841u, 0);

    /* headline: the verdict, in the biggest type on the screen */
    {
        const char *verdict = ok ? "PASS" : "FAIL";
        int n = 0, w;

        while (verdict[n]) n++;
        w = n * BIG_ADV;
        hw_panel(SCREEN_W / 2 - w / 2 - 30, 80, w + 60, BIG_H + 36);
        hw_rule(SCREEN_W / 2 - w / 2 - 30, 80, BIG_H + 36, ok ? C_GREEN : C_RED);
        hwt_text_big(SCREEN_W / 2 - w / 2 + 4, 96, verdict, 1);
    }

    /* results table */
    hw_panel(CHK_TAB_X, CHK_TAB_Y, CHK_TAB_W, CHK_TAB_H);
    hw_rule(CHK_TAB_X, CHK_TAB_Y, CHK_TAB_H, ok ? C_GREEN : C_RED);
    hwt_text(CHK_C_NAME, CHK_TAB_Y + 14, "OPERATION", PAL_CYAN);
    hwt_text(CHK_C_PX,   CHK_TAB_Y + 14, "PIXELS", PAL_CYAN);
    hwt_text(CHK_C_RES,  CHK_TAB_Y + 14, "RESULT", PAL_CYAN);

    fail_row = -1;
    for (i = 0; i < 4; i++) {
        chkt_t *c = &chk[i];
        int y = CHK_TAB_Y + 56 + i * CHK_ROWH;

        hwt_text(CHK_C_NAME, y, c->name, PAL_WHITE);

        q = app_u32(nb, c->px); *q = 0;
        hwt_text(CHK_C_PX, y, nb, PAL_WHITE);

        q = app_str(vs, c->pass ? "PASS" : "FAIL"); *q = 0;
        hwt_text(CHK_C_RES, y, vs, c->pass ? PAL_GREEN : PAL_RED);

        if (!c->pass && fail_row < 0) fail_row = i;
    }

    /* totals on the right */
    p = app_u32(nb, chk_tested_px); p = app_str(p, " PX"); *p = 0;
    p = app_u32(nb2, chk_bad_px);   *p = 0;
    kv[0].k = "PIXELS";   kv[0].v = nb;  kv[0].vpal = PAL_CYAN;
    kv[1].k = "MISMATCH"; kv[1].v = nb2;
    kv[1].vpal = ok ? PAL_GREEN : PAL_RED;
    hw_card(820, CHK_TAB_Y, 452, "VERIFIED ON BOARD", kv, 2);

    /* where the first bad pixel is, if there is one */
    if (fail_row >= 0) {
        q = app_str(nb, "FIRST MISMATCH PIXEL ");
        q = app_u32(q, (uint32_t)chk[fail_row].first);
        q = app_str(q, " IN "); q = app_str(q, chk[fail_row].name); *q = 0;
        hwt_text(CHK_TAB_X + UI_PAD, CHK_TAB_Y + CHK_TAB_H + 12, nb, PAL_YELLOW);
    }

    hw_top("9  SELF CHECK   ENGINE VS CPU");
    hw_bottom("EVERY PIXEL COMPARED, NOT SAMPLED");
    hw_fps_plate();
    g_sprites = 4;
}

/* ============================================================ dispatch === */

const char *SCENE_NAME[N_SCENE] = {
    "INTRO", "SPLIT FILL", "SPLIT COPY", "IMAGES", "ALPHA BLEND",
    "COLOUR KEY", "STARFALL", "SPRITE LIMIT", "SELF CHECK", "SUMMARY"
};

const char *scene_info(int s)
{
    switch (s) {
    case SCENE_INTRO:      return "overview";
    case SCENE_SPLIT_FILL: return "left half rendered by C, right half by FILL tasks";
    case SCENE_SPLIT_COPY: return "left half copy in C, right half by COPY tasks";
    case SCENE_KEY:        return "colour keyed sprites, no magenta on screen";
    case SCENE_ALPHA:      return "alpha blending 0/64/128/192/255";
    case SCENE_LIMIT:      return "sprite count limit with KEY + ALPHA, probed live";
    case SCENE_CHECK:      return "engine output compared against the CPU, pixel by pixel";
    case SCENE_DANMAKU:    return "STARFALL danmaku game, auto pilot, boss fight";
    case SCENE_IMAGES:     return "photographs restored into DDR";
    case SCENE_SUMMARY:    return "measured numbers";
    default:               return "";
    }
}

int scene_is_split(int s)
{
    return (s == SCENE_SPLIT_FILL || s == SCENE_SPLIT_COPY || s == SCENE_IMAGES);
}

void scene_enter(int s)
{
    cur = s;
    repaint = 2;

    if (s == SCENE_SPLIT_FILL) {
        static const uint16_t pal[16] = {
            0xF800u, 0xFC00u, 0xFFE0u, 0x07E0u, 0x07FFu, 0x001Fu, 0x03EFu, 0xC618u,
            0x8410u, 0x8C71u, 0xFD20u, 0x0410u, 0x0400u, 0x0010u, 0x780Fu, 0xB300u
        };
        n_split = g_count;
        if (n_split > MAX_SPLIT) n_split = MAX_SPLIT;
        for (int i = 0; i < MAX_SPLIT; i++) {
            sobj[i].size  = (uint8_t)rnd(14, 52);
            sobj[i].x     = (int16_t)rnd(0, 640 - sobj[i].size);
            sobj[i].y     = (int16_t)rnd(0, SCREEN_H - sobj[i].size);
            sobj[i].dx    = (int8_t)(rnd(1, 7) * ((lcg() & 1) ? 1 : -1));
            sobj[i].dy    = (int8_t)(rnd(1, 7) * ((lcg() & 1) ? 1 : -1));
            sobj[i].color = pal[lcg() & 15u];
        }
    } else if (s == SCENE_SPLIT_COPY) {
        cpu_img = g_frame & 3u;
    } else if (s == SCENE_KEY) {
        n_key = g_count;
        if (n_key > 300) n_key = 300;
        for (int i = 0; i < 512; i++) {
            kobj[i].x    = (int16_t)rnd(0, SCREEN_W - SHEET48_CELL);
            kobj[i].y    = (int16_t)rnd(48, SCREEN_H - SHEET48_CELL - 60);
            kobj[i].dx   = (int8_t)(rnd(1, 5) * ((lcg() & 1) ? 1 : -1));
            kobj[i].dy   = (int8_t)(rnd(1, 5) * ((lcg() & 1) ? 1 : -1));
            kobj[i].type = (uint8_t)rnd(0, 2);
        }
    } else if (s == SCENE_ALPHA) {
        for (int i = 0; i < 8; i++) {
            mobj[i].x  = (int16_t)rnd(0, SCREEN_W - ALPHA96_CELL);
            mobj[i].y  = (int16_t)rnd(400, 500);
            mobj[i].dx = (int8_t)(rnd(2, 6) * ((lcg() & 1) ? 1 : -1));
            mobj[i].dy = (int8_t)(rnd(1, 3) * ((lcg() & 1) ? 1 : -1));
        }
    } else if (s == SCENE_LIMIT) {
        limit_reset();
    } else if (s == SCENE_CHECK) {
        chk_ran = 0;                    /* re-run the comparison on every visit */
    } else if (s == SCENE_DANMAKU) {
        game_reset();
    } else if (s == SCENE_IMAGES) {
        images_reset();
    }
}

void scene_update(void)
{
    if (cur == SCENE_SPLIT_FILL) {
        for (int i = 0; i < n_split; i++) {
            sobj_t *o = &sobj[i];
            int s = o->size;
            o->x = (int16_t)(o->x + o->dx);
            o->y = (int16_t)(o->y + o->dy);
            if (o->x < 0)          { o->x = 0;          o->dx = (int8_t)-o->dx; }
            if (o->x > 640 - s)    { o->x = (int16_t)(640 - s); o->dx = (int8_t)-o->dx; }
            if (o->y < 0)          { o->y = 0;          o->dy = (int8_t)-o->dy; }
            if (o->y > SCREEN_H - s) { o->y = (int16_t)(SCREEN_H - s); o->dy = (int8_t)-o->dy; }
        }
    } else if (cur == SCENE_KEY) {
        for (int i = 0; i < n_key; i++) {
            kobj_t *o = &kobj[i];
            o->x = (int16_t)(o->x + o->dx);
            o->y = (int16_t)(o->y + o->dy);
            if (o->x < 0)                  { o->x = 0; o->dx = (int8_t)-o->dx; }
            if (o->x > SCREEN_W - SHEET48_CELL) { o->x = SCREEN_W - SHEET48_CELL; o->dx = (int8_t)-o->dx; }
            if (o->y < 48)                 { o->y = 48; o->dy = (int8_t)-o->dy; }
            if (o->y > SCREEN_H - SHEET48_CELL - 60) { o->y = SCREEN_H - SHEET48_CELL - 60; o->dy = (int8_t)-o->dy; }
        }
    } else if (cur == SCENE_ALPHA) {
        for (int i = 0; i < 8; i++) {
            kobj_t *o = &mobj[i];
            o->x = (int16_t)(o->x + o->dx);
            o->y = (int16_t)(o->y + o->dy);
            if (o->x < 0)                        { o->x = 0; o->dx = (int8_t)-o->dx; }
            if (o->x > SCREEN_W - ALPHA96_CELL)  { o->x = SCREEN_W - ALPHA96_CELL; o->dx = (int8_t)-o->dx; }
            if (o->y < 400)                      { o->y = 400; o->dy = (int8_t)-o->dy; }
            if (o->y > 504)                      { o->y = 504; o->dy = (int8_t)-o->dy; }
        }
    } else if (cur == SCENE_LIMIT) {
        limit_update();
    } else if (cur == SCENE_DANMAKU) {
        game_update();
    }

    /* the summary page is static apart from the counters: refresh it once a
     * second by repainting both back pages twice */
    if (cur == SCENE_SUMMARY && (g_frame % 60u) == 0u) repaint = 2;

    g_frame++;
}

/* snapshot of the objects used for the CPU frame currently being drawn */
void scene_cpu_begin_frame(void)
{
    if (cur == SCENE_SPLIT_FILL)
        for (int i = 0; i < n_split; i++) sobj_snap[i] = sobj[i];
    else if (cur == SCENE_SPLIT_COPY)
        /* Advance by one per CPU frame, not by g_frame.
         *
         * A CPU frame takes SLICE_TICKS worth of work per display frame, so the
         * two counters are separated by a fixed stride - around 10 display
         * frames.  Sampling g_frame & 3 therefore always returned the same value
         * (10 is not coprime with 4), and the CPU half sat on one picture for
         * the whole scene while the engine half cycled through all four. */
        cpu_img = (cpu_img + 1u) & 3u;
    else if (cur == SCENE_IMAGES)
        img_cpu_frame = img_cur;      /* lock the frame for all 20 bands */
}

void scene_hw_frame(void)
{
    g_sprites = 0;

    switch (cur) {
    case SCENE_INTRO:
        /* Two full repaints fill both back pages; after that nothing may touch
         * the screen.  Drawing just the small FPS plate on the following frames
         * was what made the menu flicker: it modified one page and not the
         * other, so the two pages alternated with and without the plate. */
        if (repaint > 0) { repaint--; intro_hw(); }
        break;
    case SCENE_SPLIT_FILL: split_fill_hw();     break;
    case SCENE_SPLIT_COPY: split_copy_hw();     break;
    case SCENE_KEY:        key_hw();            break;
    case SCENE_ALPHA:      alpha_hw();          break;
    case SCENE_LIMIT:      limit_hw();          break;
    case SCENE_DANMAKU:    game_draw();         break;
    case SCENE_IMAGES:     images_hw();         break;
    case SCENE_CHECK:      check_hw();          break;
    case SCENE_SUMMARY:
        /* same rule as INTRO: numbers change once a second, so repaint both
         * pages twice per update and leave the screen alone in between */
        if (repaint > 0) { repaint--; summary_hw(); }
        break;
    default: break;
    }
}

void scene_cpu_band(int band)
{
    swr_setclip(band * BAND_H, (band + 1) * BAND_H);
    if (cur == SCENE_SPLIT_FILL)      split_fill_cpu();
    else if (cur == SCENE_SPLIT_COPY) split_copy_cpu();
    else if (cur == SCENE_IMAGES)     images_cpu_band(band);
    swr_setclip(0, CPU_REG_H);
}

/* ------------------------------------------------------------ once a second */
void scene_tick(void)
{
    if (cur == SCENE_LIMIT)   limit_tick();
    if (cur == SCENE_DANMAKU) game_tick();
}
