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

/* Soft white bloom, drawn underneath a kill so the frame flashes before the
 * debris.  Two sizes: one for a foe, one for the boss core. */
extern Spr G_BLOOM[2];

/* Boss arms: a mirrored pair of rail cannons that hang off the alien head, so
 * the fight reads as a machine rather than a floating skull. */
extern Spr G_ARM[2];

/* Survivors scene.  These are deliberately small: the horde reaches 760 heads,
 * and at 24 px each the whole screen of enemies costs about what one 224 px boss
 * does.  Big enemies would make the scene a benchmark again. */
extern Spr G_SWARM[3];      /* grunt / brute / elite   */
extern Spr G_GEM;           /* experience pickup       */
extern Spr G_BLADE;         /* orbiting aura blade     */

/* Survivors scene, second design: a fake-3D (oblique) cast on a real map.
 * G_ZOMB_HIT is the same silhouette in white, used for the hit flash - a colour
 * change needs a second sprite because the engine blends nothing. */
extern Spr G_HERO[2];       /* idle / hit              */
extern Spr G_ZOMB[3];
extern Spr G_ZOMB_HIT[3];
extern Spr G_KNIFE;         /* melee, orbits the player */
extern Spr G_SHOOT;         /* gun bullet              */
extern Spr G_GROUND;        /* 1536x864 baked map      */

/* Hand-painted pack (RUOK / Kenney, CC0) vs the procedural art.
 *
 * 0 = procedural.  The pack was wired up and measured, and at 720p its 64 px
 * neon sprites read as busy colour noise next to a 1280 px playfield: the ships
 * are drawn for a 320x240 screen, and upscaling them 3x does not add detail, it
 * adds blockiness while the outlines fight the starfield.  The procedural set
 * stays because it is drawn at the display's own scale.
 *
 * Flip to 1 and run tools/load_game_assets.ps1 first to compare the two. */
#define USE_ART_PACK      0

#define GAME_ASSET_ADDR   0x07400000u
#define GAME_ASSET_MAGIC  0x4B415047u   /* 'GPAK' */
extern int art_pack_loaded;

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
