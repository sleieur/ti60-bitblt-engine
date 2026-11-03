/******************************************************************************
*
* @file bbshow.h: shared definitions for the BitBlt demonstration program
*
* Target: Ti60F225 + Sapphire RISC-V, HDMI 1280x720@60, RGB565, double buffer.
* The BitBlt engine (rtl/bitblt) owns the two framebuffers; the CPU only submits
* tasks.  This program demonstrates every item of the contest brief:
*   - block copy / solid fill          (scenes SPLIT_FILL, SPLIT_COPY)
*   - colour keying                    (scene KEY)
*   - hardware alpha blending          (scene ALPHA)
*   - high sprite-count game demo      (scene GAME)
*   - on-screen CPU-vs-hardware rate   (scenes SPLIT_*)
*
* DDR layout (CPU address == DDR byte address, everything below 0x1000_0000):
*
*   0x0100_0000  FB0            1280x720x2  (0x1C2000)  engine page 0
*   0x0180_0000  FB1            1280x720x2  (0x1C2000)  engine page 1
*   0x0300_0000  IMG0..IMG3     1280x720x2 each, 2 MiB apart (background images)
*   0x0380_0000  SHEET48        8 cells of 48x48, stride 768
*   0x0382_0000  SHEET32        8 cells of 32x32, stride 512
*   0x0384_0000  ALPHA96        96x96 sprite, stride 192
*   0x03A0_0000  GLYPH          6 palettes x 0x20000, 48 cells of 24x32, stride 2688
*   0x0400_0000  CPUBUF         640x720x2 (left half), stride 1280
*
******************************************************************************/
#ifndef BBSHOW_H
#define BBSHOW_H

#include <stdint.h>

#define SCREEN_W   1280
#define SCREEN_H   720

/* ---- framebuffers (must match fb_driver.h / fb_regs_axil defaults) ---- */
#define FB0_ADDR   0x01000000u
#define FB1_ADDR   0x01800000u

/* ---- CPU software-rendered half of the screen (left) ---- */
#define CPU_REG_X      0
#define CPU_REG_W      640
#define CPU_REG_H      720
/* Three buffers, rotated: the CPU draws into one, the engine blits another.
 * One buffer is not enough - the CPU would overwrite the very rows the blit
 * DMA is still fetching (the topmost band is rewritten first, so it tore). */
#define CPU_ADDR       0x04000000u
#define CPU_ADDR2      0x04100000u
#define CPU_ADDR3      0x04200000u
#define CPU_STRIDE     1280            /* 640 * 2 */

/* ---- background image bank: 4 images, 1280x720 RGB565, 2 MiB apart ---- */
#define IMG_STRIDE     2560
#define IMG_ADDR(i)    (0x03000000u + (uint32_t)(i) * 0x00200000u)

/* ---- sprite sheets (magenta 0xF81F background) ---- */
#define SHEET48_ADDR   0x03800000u
#define SHEET48_STRIDE 768             /* 8 cells of 48 px */
#define SHEET48_CELL   48
#define SHEET32_ADDR   0x03820000u
#define SHEET32_STRIDE 512             /* 8 cells of 32 px */
#define SHEET32_CELL   32
#define ALPHA96_ADDR   0x03840000u
#define ALPHA96_STRIDE 192
#define ALPHA96_CELL   96
/* Soft radial orbs, used by the sprite-limit scene.  The 32 px cell sheet is
 * hard edged - cropped to 24x24 it reads as a pile of small squares, which is
 * why that scene looked crude.  These are radial glows keyed on magenta: the
 * colour still comes out exact after the keyer, but the silhouette is round and
 * the falloff reads as light rather than as blocks. */
#define ORB32_ADDR     0x03860000u
#define ORB32_STRIDE   512             /* 8 cells of 32 px, same as SHEET32 */
#define ORB32_CELL     32
#define ORB32_NCELL    8

/* ---- self-check scratch buffers (scene 9) ----
 * Two 640x64 RGB565 windows the CPU and the engine both write, so the result can
 * be compared pixel by pixel without touching either display page.  0x05000000
 * is free: images start at 0x03000000, the CPU pages at 0x04000000, the UI
 * texture band at 0x05200000 and the picture strip at 0x06000000. */
#define CHK_SRC        0x05000000u
#define CHK_DST        0x05040000u
#define CHK_W          640
#define CHK_H          64
#define CHK_STRIDE     1280            /* 640 px * 2 bytes */

/* ---- photographs pushed into DDR by the debugger (gdb 'restore') ----
 * Nothing of this lives in the program image: 1280x720 RGB565 is 1.75 MiB per
 * picture, far more than the 124 KiB of RAM the program is linked into.
 * The debug launcher restores tools/assets.bin to 0x06000000 before starting,
 * so the pictures are plain DDR data with a small header:
 *
 *   0x06000000  u32 magic 'SFA1'
 *   0x06000004  u32 picture count
 *   0x06000008  u32 width, 0x0600000C u32 height
 *   0x06000010  u32 playback rate in pictures/s (0 = not recorded, use the default)
 *   0x06001000  picture 0, then picture 1 ...
 *   each picture is IMG_PIC_STRIDE bytes apart, top-down RGB565, stride 2560
 *
 * The rate is what the strip must be played at to look like the source: a
 * 14-frame GIF with 70 ms frames is 14.29 fps, and playing those 14 pictures at
 * 60 fps is 4.2x too fast.  img2assets.py writes it, --fps <n>.
 */
/* The strip is stored HALF SCREEN WIDE (640x720) because scene 8 is a CPU vs
 * engine split: both halves consume the same frames, the left one through the
 * software rasteriser, the right one as a single COPY task.  Storing 640 wide
 * instead of 1280 also halves the load time. */
#define IMG_PIC_W      640
#define IMG_PIC_H      720
#define IMG_PIC_STRIDE 0x000E1000u      /* 640*720*2, a multiple of 16 */
#define IMG_DATA_BASE  0x06000000u
#define IMG_DATA_HDR   0x00001000u
#define IMG_DATA_MAGIC 0x53464131u      /* 'SFA1' */
#define IMG_HDR_FPS    0x00000010u      /* u32, pictures/s, 0 = not recorded */
/* Played back when the header has no rate.
 *
 * 60 is deliberate, not a leftover: the strip holds 14 sampled frames of a
 * 70 ms GIF, and playing those 14 pictures at the display rate is what makes
 * the engine half look like it is running flat out - at 60 Hz the punch combo
 * smears into motion blur, which is the point of the side-by-side.  The source rate
 * (14.29 fps) is what a faithful playback would need, so img2assets.py --fps
 * can still record it if a "same speed as the source" comparison is wanted
 * instead.  The current tools/assets.bin has 0 in the header, so this default
 * applies. */
#define IMG_DEF_FPS    60
#define IMG_PIC_ADDR(i) (IMG_DATA_BASE + IMG_DATA_HDR + (uint32_t)(i) * IMG_PIC_STRIDE)
/* 168 x 0.88 MiB = 148 MiB: 0x0600_1000 .. 0x0F3A_8FFF, inside the BitBlt
 * window and inside the 256 MB part.  Measured 406 KiB/s -> about 6 min. */
#define IMG_MAX_PICS   168

/* ---- glyph atlas ---- */
#define GLYPH_ADDR     0x03A00000u
#define GLYPH_PAL      0x00020000u     /* palette stride */
#define GLYPH_NCELL    56
#define GLYPH_CELL_W   24
#define GLYPH_CELL_H   32
#define GLYPH_STRIDE   (GLYPH_NCELL * GLYPH_CELL_W * 2)
#define GLYPH_KEY      0x0001u

/* big glyph atlas, 8x scale, used for the game score */
#define GLYPH_BIG_ADDR 0x03B00000u
#define GLYPH_BIG_PAL  0x00050000u
#define GLYPH_BIG_W    48
#define GLYPH_BIG_H    64
#define GLYPH_BIG_STRIDE (GLYPH_NCELL * GLYPH_BIG_W * 2)
#define GLYPH_BIG_KEY  0x0001u
#define N_PAL_BIG      2               /* 0 white, 1 gold */

/* ---- colours (RGB565) ---- */
#define C_BLACK        0x0000u
#define C_WHITE        0xFFFFu
#define C_MAGENTA      0xF81Fu
#define C_YELLOW       0xFFE0u
#define C_CYAN         0x07FFu
#define C_RED          0xF800u
#define C_GREEN        0x07E0u
#define C_BLUE         0x001Fu
#define C_DARK         0x0861u
#define C_DARKBG       0x1106u
#define C_PANEL        0x18E3u

/* HUD text palettes */
#define PAL_WHITE      0
#define PAL_BLACK      1
#define PAL_YELLOW     2
#define PAL_CYAN       3
#define PAL_RED        4
#define PAL_GREEN      5
#define N_PAL          6

/* ---- font (font.c) ---- */
extern const char    FONT_CHARS[];
extern const uint8_t FONT_GLYPHS[][7];
int font_index(char c);

/* ---- hardware glyph renderer (hwt.c) ---- */
void hwt_init(void);
void hwt_init_big(void);
void hwt_text(int x, int y, const char *s, int pal);   /* one KEY task per glyph */
void hwt_text_big(int x, int y, const char *s, int pal);
void hwt_text_right(int xr, int y, const char *s, int pal);   /* right aligned */
int  hwt_len(const char *s);

/* ---- DDR content (content.c) ---- */
void content_init(void);

/* ---- address relocation, only used by the PC preview build ----
 * On the target all three stay 0 and the DDR addresses are used as they are. */
void content_set_delta(uint32_t d);
void swr_set_delta(uint32_t d);
void hwt_set_delta(uint32_t d);

/* ---- software rasteriser into CPUBUF (swr.c) ----
 * All coordinates are clipped to x in [0,640) and y in [clip_y0,clip_y1). */
void swr_setclip(int y0, int y1);
void swr_set_base(uint32_t addr);        /* which CPU buffer to draw into */
void swr_fill(int x, int y, int w, int h, uint16_t color);
void swr_copy(uint32_t src, int sstride, int sx, int sy, int dx, int dy, int w, int h);
void swr_key (uint32_t src, int sstride, int sx, int sy, int dx, int dy, int w, int h, uint16_t key);
void swr_alpha(uint32_t src, int sstride, int sx, int sy, int dx, int dy, int w, int h, int alpha);
void swr_text(int x, int y, const char *s, uint16_t color);   /* 15x21 glyphs, 18 px advance */
void swr_clear_all(uint16_t color);

/* ---- scenes (scenes.c) ---- */
/* Presentation order, which is also the order of the hot keys (1..9, then 0):
 *
 *   1 INTRO / 2 FILL / 3 COPY / 4 IMAGES / 5 ALPHA / 6 KEY / 7 GAME
 *   8 LIMIT / 9 CHECK / 0 SUMMARY
 *
 * Chosen for recording: basics first, then the picture strip while the DDR
 * asset window is loaded with photographs, then the two accelerated blend
 * modes, then the game as the peak of the show, then the stress test, then the
 * self-check that proves the numbers are real, and the measured numbers last.
 *
 * The strip is recorded twice - once with the photographs, once with the GIF
 * frames - because both live in the same DDR window; repack assets.bin between
 * the two takes.
 */
#define N_SCENE 10
#define SCENE_INTRO      0
#define SCENE_SPLIT_FILL 1
#define SCENE_SPLIT_COPY 2
#define SCENE_IMAGES     3      /* photographs / GIF frames from DDR         */
#define SCENE_ALPHA      4
#define SCENE_KEY        5      /* colour keying, 300 sprites                */
#define SCENE_DANMAKU    6      /* STARFALL, the full game demo              */
#define SCENE_LIMIT      7      /* sprite count limit, KEY + ALPHA together  */
#define SCENE_CHECK      8      /* engine output compared against the CPU    */
#define SCENE_SUMMARY    9

extern const char *SCENE_NAME[N_SCENE];

int  scene_is_split(int s);
void images_next(void);
void images_toggle_play(void);
void images_fps_step(int delta);
void scenes_set_delta(uint32_t d);
int  limit_best(void);       /* sprite count that held 60 fps in scene 6 */
void scene_enter(int s);                 /* reset per-scene state */
void scene_hw_frame(void);               /* submit the hardware tasks of one frame */
void scene_cpu_band(int band);           /* render rows [band*BAND_H, +BAND_H) of CPUBUF */
void scene_cpu_begin_frame(void);        /* snapshot the objects of a new CPU frame */
void scene_update(void);                 /* per-frame logic (motion), called once a frame */
void scene_tick(void);                   /* once a second: statistics-driven ramps */
const char *scene_info(int s);           /* one-line description for the UART log */

#define BAND_H   36
#define N_BANDS  (CPU_REG_H / BAND_H)

/* ---- shared state ---- */
extern uint32_t g_frame;                 /* software frame counter */
extern uint32_t g_sprites;               /* sprites submitted in the last frame */
extern int      g_count;                 /* scene tunable (object / sprite count) */
extern uint32_t g_cpu_ms;                /* measured CPU render time of one full CPU frame */
extern uint32_t g_cpu_hz;                /* how often the CPU half is actually refreshed */
extern uint32_t g_hw_fps;                /* engine frame rate, 1 s window */
extern uint32_t g_max_sprites;           /* highest sprite count seen at 60 fps */
void bb_fail(const char *why);

#endif
