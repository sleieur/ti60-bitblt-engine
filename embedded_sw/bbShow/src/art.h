/******************************************************************************
*
* @file art.h: sprite assets of the danmaku game.
*
* Every sprite is generated into DDR at start-up (no SD card, no image files) by
* art.c and described by a Spr {address, width, height, row stride}.  The stride
* is the sheet row pitch, so a sprite can be stencilled out of a larger picture
* with one BitBlt task per draw.
*
* Transparent pixels are the key colour 0xF81F; the engine drops them.
*
******************************************************************************/
#ifndef ART_H
#define ART_H

#include <stdint.h>

typedef struct {
    uint32_t addr;
    uint16_t w, h, stride;
} Spr;

/* ---- enemy bullet kinds (indices into G_BUL) ---- */
enum {
    BK_R = 0,       /* small red orb        12x12 */
    BK_B,           /* small blue orb       12x12 */
    BK_C,           /* small cyan orb       12x12 */
    BK_P,           /* medium purple orb    16x16 */
    BK_Y,           /* medium yellow orb    16x16 */
    BK_RICE,        /* rice bullet           8x16 */
    BK_SCALE,       /* scale bullet         12x12 */
    BK_STAR,        /* star bullet          14x14 */
    BK_BIG_R,       /* boss big orb         26x26 */
    BK_BIG_P,       /* boss big purple orb  26x26 */
    BK_N
};

/* ---- player bullet kinds ---- */
enum { PB_NEEDLE = 0, PB_BALL, PB_N };

/* ---- ships ---- */
enum { SHIP_IDLE = 0, SHIP_LEFT, SHIP_RIGHT, SHIP_HIT, SHIP_N };
enum { FOE_A = 0, FOE_B, FOE_C, FOE_N };

extern Spr G_BUL[BK_N];
extern Spr G_PB[PB_N];
extern Spr G_SHIP[SHIP_N];
extern Spr G_FOE[FOE_N];
extern Spr G_BOSS[2];
extern Spr G_FLAME[3];
extern Spr G_BOOM[6];
extern Spr G_HIT[3];
extern Spr G_RING[3];
extern Spr G_STAR[4];
extern Spr G_NEB[2];

/* Arcade extras: one distant planet and two speed streaks.  A static planet
 * gives the scene depth without costing a task per frame; the streaks sell the
 * scroll direction far better than another parallax layer would. */
extern Spr G_PLANET;
extern Spr G_STREAK[2];

/* translucent panel bank: 4 colour bands of 224 rows, 960 px wide.
 * Draw a panel with bb_alpha(UI_TEX + band*224*UI_TEX_STRIDE + x*2, ...). */
#define UI_TEX_BASE    0x05200000u
#define UI_TEX_STRIDE  1920u            /* 960 px */
#define UI_TEX_BAND_H  224
#define UI_BAND_BLACK  0
#define UI_BAND_NAVY   1
#define UI_BAND_RED    2
#define UI_BAND_WHITE  3
#define UI_PANEL_ADDR(band, x, w) \
    (UI_TEX_BASE + (uint32_t)(band) * UI_TEX_BAND_H * UI_TEX_STRIDE + (uint32_t)(x) * 2u)

void art_build(void);
void art_set_delta(uint32_t d);         /* PC preview only, stays 0 on target */

#endif
