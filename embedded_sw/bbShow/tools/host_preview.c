/******************************************************************************
*
* host_preview.c - PC-side preview of the bbShow scenes.
*
* Builds the same sources the RISC-V target builds, replaces the BitBlt driver
* with a small model that
*   - applies exactly the clipping of bitblt_driver.c rel(),
*   - applies exactly the SUBMIT parameter checks of fb_regs_axil.v,
*   - draws FILL / COPY / KEY / ALPHA into a simulated DDR window,
* and writes one PPM per scene so the layout can be checked without hardware.
*
* Build (from this directory):
*   gcc -O2 -I../src -o host_preview host_preview.c ../src/font.c ../src/hwt.c \
*       ../src/swr.c ../src/scenes.c ../src/content.c
* Run:  ./host_preview      (writes sceneN.ppm)
*
******************************************************************************/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "bbshow.h"
#include "bitblt_regs.h"
#include "game.h"
#include "art.h"

#define DDR_LO 0x01000000u
#define DDR_HI 0x07600000u

static unsigned char *arena;
static uint32_t       back_page = FB0_ADDR;

static long n_task, n_bad, n_param, n_src, n_dst;

static inline uint16_t *A(uint32_t a) { return (uint16_t *)(arena + (a - DDR_LO)); }

static int rel_ok(uint32_t op, uint32_t src, uint32_t sstride, int x, int y, int w, int h, int rel)
{
    int w_ok = (w <= 1280), h_ok = (h <= 720);
    long rbytes = 2L * w;
    int src_ok = (op == BB_OP_FILL) ||
                 ((src % 2) == 0 && (sstride % 2) == 0 && sstride <= 0xFFFF &&
                  (long)sstride >= rbytes && src >= BB_RD_LO &&
                  (long)src + (long)(h - 1) * sstride + rbytes <= (long)BB_DDR_TOP);
    int dst_ok;
    if (rel) dst_ok = (x + w <= 1280) && (y + h <= 720);
    else      dst_ok = 0;
    if (!(w_ok && h_ok)) { n_param++; return 0; }
    if (!src_ok)         { n_src++;   return 0; }
    if (!dst_ok)         { n_dst++;   return 0; }
    return 1;
}

/* ---- model of bitblt_driver.c rel() ---- */
static int rel(uint32_t op, uint32_t src, uint32_t sstride, int x, int y, int w, int h,
               uint16_t color, uint32_t alpha)
{
    if (x < 0) { src += (uint32_t)(-2 * x); w += x; x = 0; }
    if (y < 0) { src += (uint32_t)(-y) * sstride; h += y; y = 0; }
    if (x + w > 1280) w = 1280 - x;
    if (y + h > 720)  h = 720 - y;
    if (w <= 0 || h <= 0) return 0;

    n_task++;
    if (!rel_ok(op, src, sstride, x, y, w, h, 1)) { n_bad++; return -1; }

    for (int r = 0; r < h; r++) {
        uint16_t *d = A(back_page + (uint32_t)(y + r) * 2560u) + x;
        const uint16_t *s = (const uint16_t *)(A(src) + (uint32_t)r * (sstride / 2u));
        for (int c = 0; c < w; c++) {
            switch (op) {
            case BB_OP_FILL:
                d[c] = color;
                break;
            case BB_OP_COPY:
                d[c] = s[c];
                break;
            case BB_OP_KEY:
                if (s[c] != color) d[c] = s[c];
                break;
            default: {
                uint32_t a = alpha > 255 ? 255 : alpha;
                uint32_t ia = 255 - a;
                uint32_t sv = s[c], dv = d[c];
                uint32_t r5 = (((sv >> 11) & 31) * a + ((dv >> 11) & 31) * ia + 127) / 255;
                uint32_t g6 = (((sv >> 5) & 63) * a + ((dv >> 5) & 63) * ia + 127) / 255;
                uint32_t b5 = ((sv & 31) * a + (dv & 31) * ia + 127) / 255;
                d[c] = (uint16_t)((r5 << 11) | (g6 << 5) | b5);
            }
            }
        }
    }
    return 0;
}

int bb_fill(int x, int y, int w, int h, uint16_t color, uint32_t last)
{ (void)last; return rel(BB_OP_FILL, 0, 0, x, y, w, h, color, 0); }

int bb_copy(uint32_t src, uint32_t sstride, int x, int y, int w, int h, uint32_t last)
{ (void)last; return rel(BB_OP_COPY, src, sstride, x, y, w, h, 0, 0); }

int bb_key(uint32_t src, uint32_t sstride, int x, int y, int w, int h, uint16_t key, uint32_t last)
{ (void)last; return rel(BB_OP_KEY, src, sstride, x, y, w, h, key, 0); }

int bb_alpha(uint32_t src, uint32_t sstride, int x, int y, int w, int h, uint32_t alpha, uint32_t last)
{ (void)last; return rel(BB_OP_ALPHA, src, sstride, x, y, w, h, 0, alpha); }

/* ---- unused by the scenes, present so the file links stand-alone ---- */
void     bb_init(void) {}
uint32_t bb_status(void) { return BB_ST_HW_MODE | BB_ST_CAN_SUBMIT; }
uint32_t bb_error(void) { return 0; }
uint32_t bb_frames(void) { return 0; }
uint32_t bb_capability(void) { return BB_CAP_VALUE; }
void     bb_fence(void) {}
int      bb_idle(void) { return 1; }        /* the model always finishes instantly */

/* ---- absolute-addressed tasks: the self-check scene drives these ----
 *
 * The board variant waits for the engine and then reads DDR; here the same
 * operation is applied straight to the arena, so the check scene ends up with
 * the verdict the hardware gives instead of four FAIL rows.  Rounding follows
 * the SUBMIT-side spec: (src*a + dst*(255-a) + 127) / 255, per channel. */
static void px_blend(uint16_t *d, uint16_t s, uint32_t a)
{
    uint32_t ia = 255u - a, sv = s, dv = *d;
    uint32_t r5 = (((sv >> 11) & 31u) * a + ((dv >> 11) & 31u) * ia + 127u) / 255u;
    uint32_t g6 = (((sv >> 5) & 63u) * a + ((dv >> 5) & 63u) * ia + 127u) / 255u;
    uint32_t b5 = ((sv & 31u) * a + (dv & 31u) * ia + 127u) / 255u;
    *d = (uint16_t)((r5 << 11) | (g6 << 5) | b5);
}

int bb_fill_abs(uint32_t d, uint32_t ds, int w, int h, uint16_t c)
{
    if (w <= 0 || h <= 0) return 0;
    n_task++;
    for (int r = 0; r < h; r++) {
        uint16_t *row = (uint16_t *)((unsigned char *)A(d) + (uint32_t)r * ds);
        for (int x = 0; x < w; x++) row[x] = c;
    }
    return 0;
}

int bb_copy_abs(uint32_t s, uint32_t ss, uint32_t d, uint32_t ds, int w, int h)
{
    if (w <= 0 || h <= 0) return 0;
    n_task++;
    for (int r = 0; r < h; r++) {
        const uint16_t *src = (const uint16_t *)((const unsigned char *)A(s) + (uint32_t)r * ss);
        uint16_t *dst = (uint16_t *)((unsigned char *)A(d) + (uint32_t)r * ds);
        for (int x = 0; x < w; x++) dst[x] = src[x];
    }
    return 0;
}

int bb_key_abs(uint32_t s, uint32_t ss, uint32_t d, uint32_t ds, int w, int h, uint16_t key)
{
    if (w <= 0 || h <= 0) return 0;
    n_task++;
    for (int r = 0; r < h; r++) {
        const uint16_t *src = (const uint16_t *)((const unsigned char *)A(s) + (uint32_t)r * ss);
        uint16_t *dst = (uint16_t *)((unsigned char *)A(d) + (uint32_t)r * ds);
        for (int x = 0; x < w; x++) if (src[x] != key) dst[x] = src[x];
    }
    return 0;
}

int bb_alpha_abs(uint32_t s, uint32_t ss, uint32_t d, uint32_t ds, int w, int h, uint32_t a)
{
    if (w <= 0 || h <= 0) return 0;
    n_task++;
    if (a > 255u) a = 255u;
    for (int r = 0; r < h; r++) {
        const uint16_t *src = (const uint16_t *)((const unsigned char *)A(s) + (uint32_t)r * ss);
        uint16_t *dst = (uint16_t *)((unsigned char *)A(d) + (uint32_t)r * ds);
        for (int x = 0; x < w; x++) px_blend(&dst[x], src[x], a);
    }
    return 0;
}

/* ------------------------------------------------------------------ output */
static void write_ppm(const char *name, uint32_t page)
{
    FILE *f = fopen(name, "wb");
    if (!f) { perror(name); return; }
    fprintf(f, "P6\n%d %d\n255\n", SCREEN_W, SCREEN_H);
    for (int y = 0; y < SCREEN_H; y++) {
        const uint16_t *row = A(page + (uint32_t)y * 2560u);
        for (int x = 0; x < SCREEN_W; x++) {
            uint16_t v = row[x];
            unsigned char px[3] = {
                (unsigned char)(((v >> 11) & 31) * 255 / 31),
                (unsigned char)(((v >> 5) & 63) * 255 / 63),
                (unsigned char)((v & 31) * 255 / 31)
            };
            fwrite(px, 1, 3, f);
        }
    }
    fclose(f);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    arena = (unsigned char *)calloc(1, DDR_HI - DDR_LO);
    if (!arena) { fprintf(stderr, "no memory\n"); return 1; }

    uint32_t delta = (uint32_t)(uintptr_t)arena - DDR_LO;
    content_set_delta(delta);
    swr_set_delta(delta);
    hwt_set_delta(delta);
    scenes_set_delta(delta);

    /* the debugger restores game_assets.bin to GAME_ASSET_ADDR on the board;
     * copying it in here is what lets the preview show the hand-painted sprites
     * instead of the procedural fallback */
    {
        FILE *fp = fopen("game_assets.bin", "rb");
        if (fp) {
            fseek(fp, 0, SEEK_END);
            long n = ftell(fp);
            fseek(fp, 0, SEEK_SET);
            long cap = (long)(DDR_HI - GAME_ASSET_ADDR);
            if (n > cap) n = cap;
            if (fread(A(GAME_ASSET_ADDR), 1, (size_t)n, fp) != (size_t)n)
                fprintf(stderr, "short read on game_assets.bin\n");
            fclose(fp);
            printf("game_assets.bin: %ld bytes at 0x%08X\n", n, GAME_ASSET_ADDR);
        } else {
            printf("game_assets.bin not found - procedural art\n");
        }
    }

    content_init();
    game_init();

    /* the debugger normally restores assets.bin to 0x06000000 - here we just
     * copy it in so the IMAGES scene shows the same thing as on the board */
    {
        FILE *fp = fopen("assets.bin", "rb");
        if (fp) {
            fseek(fp, 0, SEEK_END);
            long n = ftell(fp);
            fseek(fp, 0, SEEK_SET);

            /* The real strip can be 150 MiB.  The preview arena is only a few
             * tens of MiB, so load the header plus however many whole frames
             * fit and rewrite the header's count.  Enough to check the picture
             * pipeline; the board has the whole thing. */
            long cap   = (long)(DDR_HI - IMG_DATA_BASE) - 0x1000;
            long frames = (n - 0x1000) / 0xE1000;
            long maxf   = cap / 0xE1000;
            if (frames > maxf) frames = maxf;
            if (frames < 1) frames = 1;

            long take = 0x1000 + frames * 0xE1000;
            if (take > n) take = n;
            fread(A(IMG_DATA_BASE), 1, (size_t)take, fp);
            fclose(fp);

            uint32_t *hdr = (uint32_t *)A(IMG_DATA_BASE);
            if (hdr[0] == IMG_DATA_MAGIC) {
                hdr[1] = (uint32_t)frames;          /* only what is in memory */
                printf("assets.bin: %ld bytes, using the first %ld frames "
                       "(0x%08X .. 0x%08X)\n",
                       n, frames, IMG_DATA_BASE,
                       IMG_DATA_BASE + (uint32_t)(0x1000 + frames * 0xE1000) - 1u);
                printf("  magic %08X  count %u  %u x %u\n",
                       hdr[0], hdr[1], hdr[2], hdr[3]);
            } else {
                printf("assets.bin: bad magic %08X\n", hdr[0]);
            }
        } else {
            printf("assets.bin not found - the IMAGES scene will say NONE LOADED\n");
        }
    }

    /* CPU-path numbers as measured on the board, per split scene: the FILL half
     * writes 300 solid squares, the COPY half pushes a whole 640x720 page. */
    static const int cpu_ms[N_SCENE] = { 0, 50, 64, 76, 0, 0, 0, 0, 0, 0 };
    static const int cpu_hz[N_SCENE] = { 0, 8, 8, 6, 0, 0, 0, 0, 0, 0 };

    for (int s = 0; s < N_SCENE; s++) {
        static const int counts[N_SCENE] = { 0, 300, 0, 300, 0, 0, 0, 0, 0, 0 };
        char name[64];
        int  frames = (s == SCENE_DANMAKU) ? 900 : 30;
        /* the survivors scene runs 1620 frames of its own; snapshot each stage
         * so the load curve can be checked without a board */
        if (s == SCENE_LIMIT) frames = 1620;

        g_count   = counts[s] ? counts[s] : 1;
        g_cpu_ms  = cpu_ms[s] ? cpu_ms[s] : 38;  /* plausible numbers so the HUD looks right */
        g_cpu_hz  = cpu_hz[s] ? cpu_hz[s] : 3;
        g_hw_fps  = 60;
        scene_enter(s);
        back_page = FB0_ADDR;

        for (int f = 0; f < frames; f++) {
            scene_update();
            scene_hw_frame();
            if (scene_is_split(s)) {
                scene_cpu_begin_frame();
                for (int b = 0; b < N_BANDS; b++) scene_cpu_band(b);
                bb_copy(CPU_ADDR, CPU_STRIDE, 0, 0, CPU_REG_W, CPU_REG_H, 0);
            }
            back_page = (back_page == FB0_ADDR) ? FB1_ADDR : FB0_ADDR;

            /* one second of wall clock = 60 frames, matches main.c */
            if (s == SCENE_LIMIT && (f % 60) == 0) scene_tick();

            /* the game: grab a few moments instead of only the last frame */
            if (s == SCENE_DANMAKU && (f == 260 || f == 620 || f == 899)) {
                sprintf(name, "scene%d_%d.ppm", s, f);
                write_ppm(name, back_page == FB0_ADDR ? FB1_ADDR : FB0_ADDR);
                printf("  game frame %d written\n", f);
            }
            /* survivors: one snapshot per stage, at its last frame */
            if (s == SCENE_LIMIT && f > 0 && ((f + 1) % 270) == 0) {
                sprintf(name, "scene%d_%d.ppm", s, f);
                write_ppm(name, back_page == FB0_ADDR ? FB1_ADDR : FB0_ADDR);
                printf("  survivors stage %d: %ld sprites\n", (f + 1) / 270, (long)g_sprites);
            }
        }

        sprintf(name, "scene%d.ppm", s);
        write_ppm(name, back_page == FB0_ADDR ? FB1_ADDR : FB0_ADDR);

        /* magenta leak check on the key scene */
        if (s == SCENE_KEY) {
            long mag = 0;
            for (int y = 0; y < SCREEN_H; y++) {
                const uint16_t *row = A(FB0_ADDR + (uint32_t)y * 2560u);
                for (int x = 0; x < SCREEN_W; x++) if (row[x] == C_MAGENTA) mag++;
            }
            printf("scene %d magenta pixels = %ld\n", s, mag);
        }
        printf("scene %d (%s): tasks=%ld rejected=%ld (param %ld src %ld dst %ld)\n",
               s, SCENE_NAME[s], n_task, n_bad, n_param, n_src, n_dst);
        n_task = n_bad = n_param = n_src = n_dst = 0;
    }
    return 0;
}
