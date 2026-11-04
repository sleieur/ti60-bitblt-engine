/******************************************************************************
*
* @file game.c: STARFALL - vertical danmaku shooter, BitBlt rendered.
*
* Design notes
* ------------
* The CPU never touches a pixel: a frame is described as a list of blits and the
* engine paints it while the CPU already works on the next one.  The CPU only
* owns a few kilobytes of entity state, so a 100 MHz single-issue core can drive
* well over a thousand simultaneous sprites at 60 Hz.
*
* Entities live in flat arrays and are removed by swapping with the last element,
* so there is no dynamic allocation and no fragmentation.  Positions are Q4
* fixed point, which keeps slow drifting bullet patterns smooth.
*
* Every spawn rate is scaled by density, which game_tick() adapts to the measured
* refresh rate: the demo never tears or stutters, it thins the danmaku out when
* the DDR bandwidth budget is not enough.
*
******************************************************************************/
#include "bbshow.h"
#include "bitblt_driver.h"
#include "art.h"
#include "game.h"

/* ------------------------------------------------------------------ config */
#define MAX_EBUL   1100
/* RAM headroom matters: the art pass pushed .text up and the region is 124 KB.
 * A pixel-art pass costs code, so the pools were trimmed to their real high
 * water marks (measured on the busiest wave) to keep 4 KB of stack free. */
#define MAX_PBUL   64
#define MAX_FOE    20
#define MAX_PART   96
#define MAX_STAR   160

#define PLAY_TOP   72
#define PLAY_L     26
#define PLAY_R     (SCREEN_W - 26)
#define PLAY_BOT   (SCREEN_H - 46)

#define Q4         4
#define FX(v)      ((v) << Q4)
#define PX(v)      ((v) >> Q4)
/* Rounding variant for the boss.  Truncation turns a sub-pixel motion into
 * "hold for three frames, then jump one pixel", which the eye reads as a
 * nervous shudder; rounding at least splits the step evenly between the
 * neighbouring pixels.  Bullets keep the plain PX(), their steps are large. */
#define PXR(v)     (((v) + 8) >> Q4)

#define NC         52                    /* danger map columns, 1280/32 + padding */

/* ------------------------------------------------------------------- state */
typedef struct { int16_t x, y, vx, vy; uint8_t kind, ttl, fl; } EBul;
typedef struct { int16_t x, y, vx, vy; uint8_t kind, ttl; }      PBul;
typedef struct { int16_t x, y, vx, vy; uint8_t kind, ttl, hp, fl; } Foe;
typedef struct { int16_t x, y, vx, vy; uint8_t kind, ttl; }      Part;
typedef struct { int16_t x, y, vy; uint8_t kind; }               Star;

static EBul  eb[MAX_EBUL];  static int neb;
static PBul  pb[MAX_PBUL];  static int npb;
static Foe   fo[MAX_FOE];   static int nfo;
static Part  pt[MAX_PART];  static int npt;
static Star  st[MAX_STAR];

/* Speed streaks: the cheapest way to make a static background read as motion.
 * Ten 3x34 sprites cost about as much as one enemy, and they only ever move
 * straight down, so the parallax stays believable without another layer. */
#define MAX_STK  10
typedef struct { int16_t x, y; uint8_t k; uint8_t vy; } Stk;
static Stk stk[MAX_STK];

static int px, py, pvx;
static int php, pmaxhp, plives;
static int pinv, pdead, pshot_cd, pdie_t;
static int score, hiscore, graze, deaths;

static int wave, wave_t, stage, spawn_t;
static int boss_alive, boss_dying, boss_hp, boss_maxhp, boss_phase, boss_t;
static int boss_x, boss_y;
static int boss_arrived;        /* entry finished, weave may start */

static int shake, flash;
/* Boss cleared: hold a STAGE CLEAR card for a few seconds, then ask main.c to
 * move on.  The game used to just bump `stage` and start over, so the demo
 * looped the same fight forever and there was no visible end to the run. */
static int clear_t, clear_req;
static int autopilot = 1, manual;
static int density = 100;
static int frozen;              /* 'f' freezes the simulation, keeps drawing */
/* Bisect switch for the whole-screen brightness shimmer.
 *
 * Every scene reaches the screen through the same pipeline, so the only thing
 * that sets STARFALL apart is that it rewrites the entire page every frame.
 * These four modes take that apart without touching anything else - the
 * simulation keeps running at the same rate in all of them, so the frame
 * stream going to the display path is identical:
 *
 *   0  normal                      full redraw, ~300 sprites
 *   1  'x' background fills only   24 full-width bb_fill, zero sprites
 *   2  'z' sprites only            no background fill, ~300 sprites
 *   3  'b' one flat full-screen     1.84 MB of grey per frame, zero sprites
 *
 * Together with 'g' in main.c (grey page, frames stopped = no DDR writes at
 * all) these isolate "full-page write bandwidth" from "sprite count" from
 * "no writes".  If only mode 3 flickers the trigger is the write traffic. */
int game_bisect_mode;
static int gameover_t, ready_t;

static uint32_t rnd_state = 0x12345678u;
static uint32_t rnd(void) { rnd_state = rnd_state * 1664525u + 1013904223u; return rnd_state >> 8; }
static int rr(int lo, int hi) { return lo + (int)(rnd() % (uint32_t)(hi - lo + 1)); }

/* --------------------------------------------------------------- maths ---- */
static const int16_t COS_T[17] = {
    1024, 1019, 1004, 980, 946, 903, 851, 792, 724, 650, 569, 483, 392, 297, 200, 100, 0
};
/* cos(ang * 2pi/256) * 1024
 *
 * COS_T holds cos over one quadrant (0..90 deg in 17 samples).  Inside a
 * quadrant the angle is q*90 + k*90/64, so |cos| in the second and fourth
 * quadrant must be read with the table index running BACKWARDS - reading it
 * forwards returned COS_T[0] = 1024 where the true value is 0.  The result was
 * a discontinuity of a whole quadrant at k == 0, which is what the "boss
 * teleports" reports were: the weave jumped ~150 px sideways every time the
 * phase crossed 64, 128 and 192, and every circular bullet pattern came out
 * distorted for the same reason.
 */
static int icos(int ang)
{
    int a = ang & 255;                   /* negative angles have to wrap */
    int q = a >> 6;                      /* quadrant */
    int k = a & 63;                      /* position inside the quadrant */
    int i = k >> 2, f = k & 3;
    int v;

    if (q == 0 || q == 2) {              /* 0..90, 180..270: index runs forward */
        v = COS_T[i] + ((COS_T[i + 1] - COS_T[i]) * f) / 4;
        return (q == 0) ? v : -v;
    }
    /* 90..180, 270..360: |cos| grows again, so walk the table backwards */
    v = COS_T[16 - i] + ((COS_T[15 - i] - COS_T[16 - i]) * f) / 4;
    return (q == 3) ? v : -v;
}
static int isin(int ang) { return icos(ang - 64); }

/* ------------------------------------------------------------------- sky */
/* 48 bands instead of 24: 30 px steps on a dark blue gradient show as visible
 * stripes on a 720p panel, 15 px steps do not, and 24 extra fILL tasks of
 * 1280x15 are still cheaper than a single full-screen copy. */
#define N_BG_BAND 48
static uint16_t bg_band[N_BG_BAND];

static void make_sky(void)
{
    /* almost black, with a faint blue haze towards the bottom */
    for (int i = 0; i < N_BG_BAND; i++) {
        int y = i * (SCREEN_H / N_BG_BAND) + 7;
        int r = (y * 2) / SCREEN_H;
        int g = 1 + (y * 4) / SCREEN_H;
        int b = 6 + (y * 13) / SCREEN_H;
        bg_band[i] = (uint16_t)((r << 11) | (g << 5) | b);
    }
}

void game_init(void)
{
    make_sky();
    for (int i = 0; i < MAX_STAR; i++) {
        st[i].x = (int16_t)rr(0, SCREEN_W - 8);
        st[i].y = (int16_t)rr(0, SCREEN_H - 8);
        st[i].kind = (uint8_t)(i & 3);
        st[i].vy = (int16_t)(((i & 3) == 0) ? 5 : (((i & 3) == 1) ? 12 : (((i & 3) == 2) ? 24 : 40)));
    }
    for (int i = 0; i < MAX_STK; i++) {
        stk[i].x = (int16_t)rr(0, SCREEN_W - 8);
        stk[i].y = (int16_t)rr(0, SCREEN_H);
        stk[i].k = (uint8_t)(i & 1);
        stk[i].vy = (uint8_t)(i & 1 ? 16 : 26);
    }
    game_reset();
}

void game_reset(void)
{
    neb = npb = nfo = npt = 0;
    px = FX(SCREEN_W / 2); py = FX(620); pvx = 0;
    pmaxhp = php = 100;
    plives = 3; pinv = 150; pdead = 0; pshot_cd = 0; pdie_t = 0;
    score = 0; graze = 0; deaths = 0;
    wave = 0; wave_t = 0; stage = 1; spawn_t = 60;
    boss_alive = boss_dying = 0; boss_hp = boss_maxhp = 0; boss_phase = 0; boss_t = 0;
    boss_arrived = 0;
    boss_x = FX(SCREEN_W / 2); boss_y = FX(-200);
    shake = flash = 0;
    clear_t = 0; clear_req = 0;
    gameover_t = 0; ready_t = 110;
    manual = 0;
}

/* --------------------------------------------------------------- spawning */
static void ebul(int xq, int yq, int ang, int spd, int kind)
{
    if (neb >= MAX_EBUL) return;
    EBul *b = &eb[neb++];
    b->x = (int16_t)xq; b->y = (int16_t)yq;
    b->vx = (int16_t)((icos(ang) * spd) >> 10);
    b->vy = (int16_t)((isin(ang) * spd) >> 10);
    b->kind = (uint8_t)kind;
    b->ttl = 255;
    b->fl = 0;
}

static void pbul(int xq, int yq, int vx, int vy, int kind)
{
    if (npb >= MAX_PBUL) return;
    PBul *b = &pb[npb++];
    b->x = (int16_t)xq; b->y = (int16_t)yq;
    b->vx = (int16_t)vx; b->vy = (int16_t)vy;
    b->kind = (uint8_t)kind; b->ttl = 100;
}

static void part(int xq, int yq, int kind, int ttl, int spread)
{
    if (npt >= MAX_PART) return;
    Part *p = &pt[npt++];
    p->x = (int16_t)xq; p->y = (int16_t)yq;
    p->vx = (int16_t)rr(-spread, spread);
    p->vy = (int16_t)rr(-spread / 2, spread / 2);
    p->kind = (uint8_t)kind;
    p->ttl = (uint8_t)ttl;
}

static void burst(int xq, int yq, int n, int kind)
{
    for (int i = 0; i < n; i++) part(xq, yq, kind, (kind == 0) ? 8 + (i & 7) : 6, 80);
}

/* ------------------------------------------------------------------- foes */
static void foe_add(int xq, int yq, int kind)
{
    if (nfo >= MAX_FOE) return;
    Foe *f = &fo[nfo++];
    f->x = (int16_t)xq; f->y = (int16_t)yq;
    f->vx = 0; f->vy = 0;
    f->kind = (uint8_t)kind;
    f->ttl = 0;
    f->hp  = (uint8_t)(kind == FOE_B ? 16 : (kind == FOE_A ? 5 : 3));
    f->fl  = 0;
}

static void wave_start(int n)
{
    int pat = (n + stage) & 3, k;

    switch (pat) {
    case 0:
        for (k = 0; k < 5; k++) {
            foe_add(FX(-40 - k * 70), FX(PLAY_TOP + 30 + k * 14), FOE_A);
            fo[nfo - 1].vy = FX(1);
            fo[nfo - 1].vx = FX(3);
        }
        break;
    case 1:
        for (k = 0; k < 3; k++) {
            foe_add(FX(140), FX(PLAY_TOP + 10 - k * 90), FOE_A);
            fo[nfo - 1].vy = FX(1);
            foe_add(FX(SCREEN_W - 140), FX(PLAY_TOP + 10 - k * 90), FOE_A);
            fo[nfo - 1].vy = FX(1);
        }
        break;
    case 2:
        for (k = 0; k < 3; k++) {
            foe_add(FX(SCREEN_W / 2 - 200 + k * 200), FX(PLAY_TOP - 70 - (k == 1 ? 70 : 0)), FOE_B);
            fo[nfo - 1].vy = FX(1);
        }
        break;
    default:
        for (k = 0; k < 4; k++) {
            foe_add(FX(rr(220, SCREEN_W - 220)), FX(-60 - k * 60), FOE_C);
            fo[nfo - 1].vy = FX(3);
        }
        break;
    }
}

/* ------------------------------------------------------------------ boss */
static void boss_spawn(void)
{
    boss_alive = 1; boss_dying = 0;
    boss_maxhp = 380 * stage;
    boss_hp = boss_maxhp;
    boss_phase = 0; boss_t = 0;
    boss_arrived = 0;
    boss_x = FX(SCREEN_W / 2);
    boss_y = FX(-200);
}

static void boss_fire(void)
{
    int i, n;

    if (boss_phase == 0) {
        if (boss_t % 56 == 0) {
            n = 16 + stage * 2;
            if (n > 30) n = 30;
            for (i = 0; i < n; i++)
                ebul(boss_x, boss_y + FX(20), (boss_t * 3 + i * 256 / n) & 255,
                     FX(2) + stage * 5, BK_P);
        }
        if (boss_t % 24 == 0) {
            for (i = -1; i <= 1; i++)
                ebul(boss_x, boss_y + FX(24), 64 + i * 11, FX(3) + stage * 7, BK_R);
        }
    } else if (boss_phase == 1) {
        if (boss_t % 3 == 0) {                        /* two-arm spiral */
            int a = (boss_t * 6) & 255;
            ebul(boss_x, boss_y, a, FX(2) + stage * 4, BK_B);
            ebul(boss_x, boss_y, a + 128, FX(2) + stage * 4, BK_B);
        }
        if (boss_t % 110 == 0) {
            for (i = 0; i < 28; i++) ebul(boss_x, boss_y, i * 256 / 28, FX(2), BK_C);
        }
    } else {
        if (boss_t % 2 == 0) {                        /* three-arm spiral */
            int a = (boss_t * 4) & 255;
            ebul(boss_x, boss_y, a, FX(2) + stage * 5, BK_RICE);
            ebul(boss_x, boss_y, a + 85, FX(2) + stage * 5, BK_RICE);
            ebul(boss_x, boss_y, a + 170, FX(2) + stage * 5, BK_RICE);
        }
        if (boss_t % 90 == 0) {
            for (i = -2; i <= 2; i++)
                ebul(boss_x, boss_y, 64 + i * 9, FX(4) + stage * 6, BK_BIG_P);
        }
    }
}

static void boss_damage(int d)
{
    if (!boss_alive || boss_dying) return;
    boss_hp -= d;
    if (boss_hp <= 0) {
        boss_dying = 1;
        boss_t = 0;
        for (int i = 0; i < 18; i++) part(boss_x + FX(rr(-90, 90)), boss_y + FX(rr(-60, 60)), 0, 8 + (i & 7), 80);
        shake = 16; flash = 10;
    } else if (boss_hp < boss_maxhp * 2 / 3 && boss_phase == 0) {
        boss_phase = 1; boss_t = 0; shake = 8; flash = 6;
    } else if (boss_hp < boss_maxhp / 3 && boss_phase == 1) {
        boss_phase = 2; boss_t = 0; shake = 8; flash = 6;
    }
}

/* ---------------------------------------------------------------- player */
static void player_shoot(void)
{
    if (pshot_cd) { pshot_cd--; return; }
    pshot_cd = 4;
    pbul(px - FX(12), py - FX(26), 0, -FX(24), PB_NEEDLE);
    pbul(px + FX(12), py - FX(26), 0, -FX(24), PB_NEEDLE);
    if (php > 55) pbul(px, py - FX(30), 0, -FX(28), PB_BALL);
}

static void player_hit(void)
{
    if (pinv > 0 || pdead) return;
    php -= 14;
    pinv = 80;
    shake = 10; flash = 8;
    burst(px, py, 8, 1);
    if (php <= 0) {
        php = 0; pdead = 1; pdie_t = 90;
        deaths++;
        burst(px, py, 20, 0);
        shake = 18; flash = 14;
        if (plives > 0) plives--;
        else gameover_t = 260;
    }
}

/* ---------------------------------------------------------------- autopilot
 *
 * Two dimensional, and it actually keeps attacking.
 *
 * The old version had two defects that together made the demo look wrong:
 *
 *   - it only ever changed px.  py was never touched, so the ship slid along a
 *     single row like a turret on rails for the whole fight.
 *   - its score was danger*12 against distance/30, i.e. danger was weighted
 *     ~360x heavier than the aim term.  The moment the middle of the screen got
 *     busy it abandoned the boss completely and parked in a corner, where its
 *     shots (which fly straight up) could never reach the boss at all.
 *
 * Now the bullets are projected onto a coarse 2-D grid and the ship picks the
 * best cell in a small window around itself, trading danger against distance
 * to where it wants to be - and during the boss fight "wants to be" is directly
 * under the boss, offset down the screen.  Both axes move every frame, and even
 * with no enemies it follows a slow vertical bob so it never looks parked.
 */
#define GCELL 40                        /* grid cell, px */
#define GX    32                        /* 1280 / 40 */
#define GY    16                        /* (PLAY_BOT - PLAY_TOP) / 40 */
static uint8_t gdanger[GY][GX];

/* Project every bullet 12/24/36 frames ahead and stamp its cell.  Three samples
 * per bullet keeps this at ~1k operations instead of a per-candidate scan of
 * the whole bullet list. */
static void danger_grid(void)
{
    int r, c, i;

    for (r = 0; r < GY; r++)
        for (c = 0; c < GX; c++) gdanger[r][c] = 0;

    for (i = 0; i < neb; i++) {
        EBul *b = &eb[i];
        int bx = PX(b->x), by = PX(b->y);

        for (int s = 1; s <= 3; s++) {
            int t  = s * 12;
            int fx = bx + (b->vx * t) / 16;
            int fy = by + (b->vy * t) / 16;
            int cc = fx / GCELL;
            int rr = (fy - PLAY_TOP) / GCELL;
            int v;

            if (cc < 0 || cc >= GX || rr < 0 || rr >= GY) continue;
            v = gdanger[rr][cc] + (4 - s);      /* nearer-in-time counts more */
            gdanger[rr][cc] = (uint8_t)(v > 255 ? 255 : v);
        }
    }
}

/* Danger at a pixel position, centre cell weighted triple. */
static int danger_at(int x, int y)
{
    int c = x / GCELL;
    int r = (y - PLAY_TOP) / GCELL;
    int sum = 0;

    for (int dr = -1; dr <= 1; dr++) {
        int r2 = r + dr;
        if (r2 < 0 || r2 >= GY) continue;
        for (int dc = -1; dc <= 1; dc++) {
            int c2 = c + dc;
            if (c2 < 0 || c2 >= GX) continue;
            sum += gdanger[r2][c2] * ((dr == 0 && dc == 0) ? 3 : 1);
        }
    }
    return sum;
}

static void autopilot_move(void)
{
    int aimx, wanty;

    danger_grid();

    if (boss_alive && !boss_dying) {
        /* line up under the boss for the straight-up shots, and follow its
         * vertical weave at a distance so the ship moves on both axes */
        aimx  = PX(boss_x);
        wanty = PX(boss_y) + 300;
    } else if (nfo) {
        aimx  = PX(fo[0].x);
        wanty = PX(fo[0].y) + 340;
    } else {
        /* nothing to chase: slow bob, purely so the ship does not look frozen */
        aimx  = PX(px);
        wanty = PLAY_BOT - 60 + (isin((int)(g_frame * 2) & 255) * 40) / 1024;
    }
    if (wanty < PLAY_TOP + 100) wanty = PLAY_TOP + 100;
    if (wanty > PLAY_BOT)       wanty = PLAY_BOT;

    int bestx = px, besty = py, bestv = 1 << 30;
    int base_x = PX(px), base_y = PX(py);

    for (int ky = -3; ky <= 3; ky++) {
        int cy = base_y + ky * 14;
        if (cy < PLAY_TOP + 40) cy = PLAY_TOP + 40;
        if (cy > PLAY_BOT)      cy = PLAY_BOT;

        for (int kx = -3; kx <= 3; kx++) {
            int cx = base_x + kx * 22;
            if (cx < PLAY_L + 24) cx = PLAY_L + 24;
            if (cx > PLAY_R - 24) cx = PLAY_R - 24;

            int dx = cx - aimx; if (dx < 0) dx = -dx;
            int dy = cy - wanty; if (dy < 0) dy = -dy;
            int dv = danger_at(cx, cy) * 5 + dx * 2 + dy * 3;

            if (dv < bestv) { bestv = dv; bestx = FX(cx); besty = FX(cy); }
        }
    }

    int d = bestx - px, spd = FX(9);
    if (d > spd) d = spd;
    if (d < -spd) d = -spd;
    px += d;
    pvx = d;

    d = besty - py; spd = FX(6);
    if (d > spd) d = spd;
    if (d < -spd) d = -spd;
    py += d;
}

/* ------------------------------------------------------------------ logic */
static void update_bullets(void)
{
    int i;
    for (i = 0; i < neb; i++) {
        EBul *b = &eb[i];
        b->x = (int16_t)(b->x + b->vx);
        b->y = (int16_t)(b->y + b->vy);
        int x = PX(b->x), y = PX(b->y);
        if (y > SCREEN_H + 50 || y < -80 || x < -70 || x > SCREEN_W + 70) {
            eb[i] = eb[--neb]; i--; continue;
        }
        if (!pdead && pinv <= 0) {
            int dx = x - PX(px), dy = y - PX(py);
            int d2 = dx * dx + dy * dy;
            if (d2 < 11 * 11) player_hit();
            else if (d2 < 28 * 28 && !(b->fl & 1)) { b->fl |= 1; graze++; }
        }
    }
    for (i = 0; i < npb; i++) {
        PBul *b = &pb[i];
        b->x = (int16_t)(b->x + b->vx);
        b->y = (int16_t)(b->y + b->vy);
        if (--b->ttl == 0 || PX(b->y) < -40) { pb[i] = pb[--npb]; i--; }
    }
}

static void update_foes(void)
{
    int i, j;
    for (i = 0; i < nfo; i++) {
        Foe *f = &fo[i];
        f->ttl++;
        int fx = PX(f->x), fy = PX(f->y);

        if (f->kind == FOE_A) {
            f->y = (int16_t)(f->y + f->vy);
            f->x = (int16_t)(f->x + f->vx + (icos((f->ttl * 2) & 255) >> 2));
            if (f->ttl % 56 == 0 && fy < PX(py) - 60) {
                int ang = 64;
                int dx = PX(px) - fx, dy = PX(py) - fy;
                if (dy > 20) {
                    int a = 64 - (dx * 42) / dy;
                    if (a < 20) a = 20;
                    if (a > 108) a = 108;
                    ang = a;
                }
                for (j = -2; j <= 2; j++) ebul(f->x, f->y, ang + j * 11, FX(2) + stage * 4, BK_R);
            }
        } else if (f->kind == FOE_B) {
            f->y = (int16_t)(f->y + f->vy);
            if (f->ttl % 64 == 0 && f->ttl > 40) {
                for (j = 0; j < 16; j++)
                    ebul(f->x, f->y, (j * 256 / 16 + f->ttl) & 255, FX(1) + stage * 3, BK_P);
            }
        } else {
            if (f->ttl > 24) {
                int dx = PX(px) - fx, dy = PX(py) - fy;
                if (dy > 20) {
                    int a = 64 - (dx * 42) / dy;
                    if (a < 30) a = 30;
                    if (a > 98) a = 98;
                    f->vx = (int16_t)((icos(a) * FX(4)) >> 10);
                    f->vy = (int16_t)((isin(a) * FX(4)) >> 10);
                }
            }
            f->x = (int16_t)(f->x + f->vx);
            f->y = (int16_t)(f->y + f->vy);
        }

        fx = PX(f->x); fy = PX(f->y);
        if (fy > SCREEN_H + 80 || fy < -240 || fx < -120 || fx > SCREEN_W + 120) {
            fo[i] = fo[--nfo]; i--; continue;
        }

        for (j = 0; j < npb; j++) {
            int bx = PX(pb[j].x), by = PX(pb[j].y);
            if (bx > fx - 26 && bx < fx + 26 && by > fy - 26 && by < fy + 26) {
                part(pb[j].x, pb[j].y, 1, 6, 40);
                pb[j] = pb[--npb];
                if (--f->hp == 0) {
                    burst(f->x, f->y, 10, 0);
                    score += (f->kind == FOE_B) ? 300 : 100;
                    if (score > hiscore) hiscore = score;
                    fo[i] = fo[--nfo]; i--;
                    break;
                }
                j--;
            }
        }
    }
}

static void update_boss(void)
{
    int i;
    if (!boss_alive) return;
    boss_t++;

    if (boss_dying) {
        boss_y = (int16_t)(boss_y + FX(1));
        if (boss_t % 6 == 0)
            part(boss_x + FX(rr(-100, 100)), boss_y + FX(rr(-60, 60)), 0, 8 + (boss_t & 7), 80);
        if (boss_t % 34 == 0) part(boss_x, boss_y, 2, 14, 0);
        if (boss_t > 130) {
            boss_alive = 0; boss_dying = 0;
            score += 4000 * stage;
            if (score > hiscore) hiscore = score;
            /* no next stage here: show the clear card, then leave the scene */
            clear_t = 240;
        }
        return;
    }

    /* Entry: slide down the middle, then start weaving.
     *
     * The weave used icos(), so at the moment of arrival it evaluated to its
     * peak: x jumped from the centre (640) straight out to 797 in one frame -
     * that is the "boss teleports" that kept being reported.  Two fixes:
     *   - isin() starts at zero, so the weave begins exactly where the entry
     *     left the boss and the position stays continuous;
     *   - boss_t is reset on arrival, so the bullet pattern phase also starts
     *     from zero instead of from whatever the 130 entry frames left it at.
     */
    /* Entry finishes when the boss reaches its station, and that decision has to
     * be made by the boss_arrived flag - not by comparing boss_y against the
     * station.
     *
     * It used to be `if (boss_y < ty - FX(1)) slide else weave`, and the weave
     * swings up to 16 px above ty.  Every frame the weave went more than one
     * pixel above the station the condition became true again, so the boss fell
     * back into the entry branch, moved 3 px down, came back to the weave and
     * was yanked up again on the next frame - the box juddered up and down
     * against a completely static background.  While in that branch boss_x also
     * stopped updating and boss_fire() was skipped, so its bullets and its
     * horizontal motion stuttered on the same frames. */
    int ty = FX(PLAY_TOP + 150);
    if (!boss_arrived) {
        boss_x = (int16_t)FX(SCREEN_W / 2);
        boss_y = (int16_t)(boss_y + FX(3));
        if (boss_y >= ty) {
            boss_y = (int16_t)ty;
            boss_arrived = 1;
            boss_t    = 0;
        }
    } else {
        /* Elliptical weave: +-46 px across, +-16 px vertically, one common
         * period (128 frames, 2.13 s) so the path is an ellipse rather than two
         * independent bobs.  Both phase steps are 2, which divides 256, so the
         * phase wrap at 128 frames is not a step. */
        boss_x = (int16_t)(FX(SCREEN_W / 2) + (isin((boss_t * 2) & 255) * FX(46)) / 1024);
        boss_y = (int16_t)(ty + (icos((boss_t * 2) & 255) * FX(16)) / 1024);
        boss_fire();
    }

    for (i = 0; i < npb; i++) {
        int dx = PX(pb[i].x) - PX(boss_x), dy = PX(pb[i].y) - PX(boss_y);
        if (dx * dx + dy * dy < 72 * 72) {
            part(pb[i].x, pb[i].y, 1, 6, 40);
            pb[i] = pb[--npb];
            boss_damage(1);
            score += 10;
            if (score > hiscore) hiscore = score;
        }
    }
}

static void update_particles(void)
{
    int i;
    for (i = 0; i < npt; i++) {
        Part *p = &pt[i];
        p->x = (int16_t)(p->x + p->vx);
        p->y = (int16_t)(p->y + p->vy);
        p->vx = (int16_t)(p->vx * 15 / 16);
        p->vy = (int16_t)(p->vy * 15 / 16);
        if (--p->ttl == 0) { pt[i] = pt[--npt]; i--; }
    }
}

void game_update(void)
{
    int i;

    /* 'f' freeze: hold every entity and effect where it is, but keep drawing so
     * the display path is still exercised at full load. */
    if (frozen) return;

    /* STAGE CLEAR card: keep the starfield and the leftover particles alive so
     * the card does not sit on a dead picture, then hand control back to main.c
     * which switches to the next scene.  Everything else is parked. */
    if (clear_t > 0) {
        for (i = 0; i < MAX_STAR; i++) {
            st[i].y = (int16_t)(st[i].y + st[i].vy / 2);
            if (st[i].y > SCREEN_H + 4) {
                st[i].y = (int16_t)-8;
                st[i].x = (int16_t)rr(0, SCREEN_W - 8);
            }
        }
        for (i = 0; i < npb; i++)
            pb[i].y = (int16_t)(pb[i].y - FX(9));
        neb = 0;                             /* the screen was just cleared */
        nfo = 0;
        update_particles();
        if (shake > 0) shake--;
        if (--clear_t == 0) clear_req = 1;
        return;
    }

    if (shake > 0) shake--;
    if (flash > 0) flash--;
    for (i = 0; i < MAX_STAR; i++) {
        st[i].y = (int16_t)(st[i].y + st[i].vy / 2);
        if (st[i].y > SCREEN_H + 4) {
            st[i].y = (int16_t)-8;
            st[i].x = (int16_t)rr(0, SCREEN_W - 8);
        }
    }
    for (i = 0; i < MAX_STK; i++) {
        stk[i].y = (int16_t)(stk[i].y + stk[i].vy);
        if (stk[i].y > SCREEN_H + 4) {
            stk[i].y = (int16_t)-40;
            stk[i].x = (int16_t)rr(0, SCREEN_W - 8);
        }
    }

    if (gameover_t > 0) {
        update_particles();
        if (--gameover_t == 0) game_reset();
        return;
    }
    if (ready_t > 0) ready_t--;

    if (pdead) {
        if (--pdie_t <= 0) {
            pdead = 0;
            php = pmaxhp;
            pinv = 170;
            px = FX(SCREEN_W / 2); py = FX(620);
        }
    } else {
        if (pinv > 0) pinv--;
        if (autopilot && !manual) autopilot_move();
        if (px < FX(PLAY_L + 24)) px = FX(PLAY_L + 24);
        if (px > FX(PLAY_R - 24)) px = FX(PLAY_R - 24);
        if (py < FX(PLAY_TOP + 40)) py = FX(PLAY_TOP + 40);
        if (py > FX(PLAY_BOT)) py = FX(PLAY_BOT);
        player_shoot();
    }

    wave_t++;
    if (!boss_alive) {
        if (wave < 4) {
            if (--spawn_t <= 0) {
                wave_start(wave);
                wave++;
                spawn_t = 110 - stage * 6;
                if (spawn_t < 65) spawn_t = 65;
            }
        } else if (nfo == 0) {
            boss_spawn();
        }
    }

    update_foes();
    update_boss();
    update_bullets();
    update_particles();
}

/* ------------------------------------------------------------------- draw */
static int sx, sy;
static int ntask;
static int last_tasks;         /* task count of the previous frame, shown in the HUD */

static void blit(const Spr *s, int x, int y)
{
    if (x > SCREEN_W || y > SCREEN_H || x + s->w < 0 || y + s->h < 0) return;
    bb_key(s->addr, s->stride, x, y, s->w, s->h, C_MAGENTA, 0);
    ntask++;
}

/* HUD panels are OPAQUE, not hardware-alpha.
 *
 * ALPHA has to read the destination pixel back out of DDR and blend it with
 * the source before writing it out again.  In a frame that is already
 * submitting 350+ tasks the read-after-write of that round trip is not always
 * coherent - the whole HUD area shimmered slightly from frame to frame, which
 * is what "the game flickers faintly" was.  An opaque fill is a single
 * descriptor block, never shimmers, costs less DDR traffic, and at this size
 * looks the same.  A one-line highlight and shadow keep the panel from looking
 * like a flat rectangle.
 *
 * `band` and `alpha` are kept so the call sites do not have to change. */
/* Alpha blit out of the flat colour bank.
 *
 * Hard limits of that bank, enforced by the engine at SUBMIT:
 *     width  <= UI_TEX_STRIDE/2 = 960 px   (it rejects stride < w*2)
 *     height <= UI_TEX_BAND_H   = 224 rows (the band is only that tall)
 * Anything bigger has to be cut into pieces.  The source is one flat colour, so
 * every piece can read from the same origin - only the destination moves.
 * Getting this wrong is silent: the engine answers SLVERR, the task is dropped,
 * and the missing piece just never appears. */
#define A_MAX_W  ((int)(UI_TEX_STRIDE / 2))
#define A_MAX_H  ((int)UI_TEX_BAND_H)

static void alpha_bank(int band, int x, int y, int w, int h, int a)
{
    if (w <= 0 || h <= 0 || a <= 0) return;
    uint32_t base = UI_TEX_BASE + (uint32_t)band * UI_TEX_BAND_H * UI_TEX_STRIDE;

    for (int yy = 0; yy < h; yy += A_MAX_H) {
        int hh = (h - yy > A_MAX_H) ? A_MAX_H : (h - yy);
        for (int xx = 0; xx < w; xx += A_MAX_W) {
            int ww = (w - xx > A_MAX_W) ? A_MAX_W : (w - xx);
            bb_alpha(base, UI_TEX_STRIDE, x + xx, y + yy, ww, hh, (uint32_t)a, 0);
            ntask++;
        }
    }
}

static void panel(int band, int x, int y, int w, int h, int alpha)
{
    (void)band;
    (void)alpha;

    if (w <= 0 || h <= 0) return;
    bb_fill(x, y, w, h, 0x1082u, 0);
    if (h > 2) {
        bb_fill(x, y, w, 1, 0x2965u, 0);            /* top highlight */
        bb_fill(x, y + h - 1, w, 1, 0x0841u, 0);    /* bottom shadow  */
    }
    ntask += 3;
}

static char *s_num(char *p, int v, int digits)
{
    char t[12];
    int n = 0;
    if (v < 0) v = 0;
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v && n < 11);
    while (n < digits) t[n++] = '0';
    while (n) *p++ = t[--n];
    return p;
}

static char *s_str(char *p, const char *s)
{
    while (*s) *p++ = *s++;
    return p;
}

static void bg_fills(void)
{
    for (int i = 0; i < N_BG_BAND; i++)
        bb_fill(0, i * 15, SCREEN_W, 15, bg_band[i], 0);
    ntask += N_BG_BAND;
}

static void draw_bg(void)
{
    bg_fills();

    /* Furthest first: planet, then two nebula banks, then the star layers, then
     * the speed streaks.  Six depth cues out of six tasks plus the sprites that
     * were always there - the parallax is in the ordering, not in extra work. */
    blit(&G_PLANET, 940, 168);

    blit(&G_NEB[0], 30, (int)((g_frame * 1 + 120) % (SCREEN_H + 500)) - 500);
    blit(&G_NEB[1], 830, (int)((g_frame * 2 + 700) % (SCREEN_H + 500)) - 500);

    for (int i = 0; i < MAX_STAR; i++) {
        const Spr *s = &G_STAR[st[i].kind];
        blit(s, st[i].x, st[i].y);
    }
    for (int i = 0; i < MAX_STK; i++)
        blit(&G_STREAK[stk[i].k], stk[i].x, stk[i].y);
}

static void draw_world(void)
{
    int i;

    if (boss_alive)
        blit(&G_BOSS[(boss_phase == 2 && !boss_dying) ? 1 : 0],
             PXR(boss_x) - G_BOSS[0].w / 2 + sx, PXR(boss_y) - G_BOSS[0].h / 2 + sy);

    for (i = 0; i < nfo; i++) {
        const Spr *s = &G_FOE[fo[i].kind];
        blit(s, PX(fo[i].x) - s->w / 2 + sx, PX(fo[i].y) - s->h / 2 + sy);
    }

    for (i = 0; i < npb; i++) {
        const Spr *s = &G_PB[pb[i].kind];
        blit(s, PX(pb[i].x) - s->w / 2 + sx, PX(pb[i].y) - s->h / 2 + sy);
    }

    if (!pdead) {
        int bx = PX(px) + sx, by = PX(py) + sy;
        if (pinv == 0 || (pinv & 4)) {
            blit(&G_FLAME[(g_frame >> 2) % 3], bx - G_FLAME[0].w / 2, by + 20);
            const Spr *s = &G_SHIP[(pvx < -16) ? SHIP_LEFT : ((pvx > 16) ? SHIP_RIGHT : SHIP_IDLE)];
            if (pinv > 0 && (pinv & 2)) s = &G_SHIP[SHIP_HIT];
            blit(s, bx - s->w / 2, by - s->h / 2);
        }
    }

    for (i = 0; i < neb; i++) {
        const Spr *s = &G_BUL[eb[i].kind];
        blit(s, PX(eb[i].x) - s->w / 2 + sx, PX(eb[i].y) - s->h / 2 + sy);
    }

    for (i = 0; i < npt; i++) {
        Part *p = &pt[i];
        const Spr *s;
        if (p->kind == 0)      s = &G_BOOM[5 - (p->ttl * 6 / 16)];
        else if (p->kind == 1) s = &G_HIT[(6 - p->ttl) * 3 / 6];
        else                   s = &G_RING[2 - p->ttl * 3 / 15];
        blit(s, PX(p->x) - s->w / 2 + sx, PX(p->y) - s->h / 2 + sy);
    }
}

static void draw_ui(void)
{
    char b[40], *p;
    int hpw = 236;

    /* ---- left panel: hull, lives, graze, telemetry ----
     *
     * Starts at y = 68, not 12: the FPGA stamps "FPS: NN" over the picture at
     * (16,16)..(240,48) in pure black, after the framebuffer, so it always wins
     * over anything drawn there.  Nothing is painted in that band here - the
     * scenery shows through and the stamped digits sit on it the way an OSD
     * would, which is what the game should look like.  The frame rate is in the
     * panel below anyway, so the ghost text up here costs nothing. */

    panel(UI_BAND_BLACK, 16, 68, 300, 204, 155);   /* 68..272, last row ends 268 */
    hwt_text(28, 74, "HULL", PAL_CYAN);
    p = b; p = s_num(p, php, 3); *p = 0;
    hwt_text_right(304, 74, b, PAL_WHITE);

    bb_fill(28, 108, hpw, 20, 0x18C3u, 0);
    int fw = hpw * php / pmaxhp;
    if (fw > 0) {
        uint16_t c = (php > 60) ? 0x07E0u : ((php > 30) ? 0xFFE0u : 0xF800u);
        bb_fill(28, 108, fw, 20, c, 0);
        bb_fill(28, 108, fw, 5, 0xFFFFu, 0);
    }
    for (int i = 1; i < 5; i++) bb_fill(28 + hpw * i / 5, 108, 2, 20, 0x0000u, 0);

    hwt_text(28, 140, "LIVES", PAL_CYAN);
    p = b; p = s_num(p, plives, 1); *p = 0;
    hwt_text(172, 140, b, PAL_WHITE);

    hwt_text(28, 172, "GRAZE", PAL_CYAN);
    p = b; p = s_num(p, graze, 4); *p = 0;
    hwt_text(172, 172, b, PAL_YELLOW);

    if (frozen) {
        p = b; p = s_str(p, "FROZEN "); p = s_num(p, last_tasks, 4); *p = 0;
        hwt_text(28, 204, b, PAL_YELLOW);
    } else {
        p = b; p = s_str(p, "SPR "); p = s_num(p, last_tasks, 4); *p = 0;
        hwt_text(28, 204, b, PAL_GREEN);
    }

    p = b; p = s_num(p, (int)g_hw_fps, 2); p = s_str(p, " FPS"); *p = 0;
    hwt_text(28, 236, b, PAL_GREEN);

    /* ---- right panel: score ---- */
    panel(UI_BAND_BLACK, 916, 12, 348, 152, 155);
    hwt_text_right(1252, 18, "SCORE", PAL_CYAN);
    p = b; p = s_num(p, score, 7); *p = 0;
    hwt_text_big(952, 48, b, 1);
    hwt_text(946, 120, "HI", PAL_CYAN);
    p = b; p = s_num(p, hiscore, 7); *p = 0;
    hwt_text(1000, 120, b, PAL_YELLOW);

    /* ---- boss gauge, top centre ---- */
    if (boss_alive && !boss_dying) {
        int w = 520, x = 356;
        panel(UI_BAND_BLACK, x, 12, w, 96, 160);
        hwt_text(x + 16, 18, "SENTINEL", PAL_RED);
        p = b; p = s_str(p, "PHASE "); p = s_num(p, boss_phase + 1, 1); *p = 0;
        hwt_text_right(x + w - 16, 18, b, PAL_YELLOW);
        bb_fill(x + 16, 56, w - 32, 20, 0x18C3u, 0);
        int fw2 = (w - 32) * (boss_hp > 0 ? boss_hp : 0) / boss_maxhp;
        if (fw2 > 0) {
            bb_fill(x + 16, 56, fw2, 20, 0xF800u, 0);
            bb_fill(x + 16, 56, fw2, 5, 0xFFE0u, 0);
        }
        for (int i = 1; i < 3; i++) bb_fill(x + 16 + (w - 32) * i / 3, 56, 3, 20, 0x0000u, 0);
    }

    /* ---- mode and banners ---- */
    if (manual) hwt_text_right(1252, 682, "MANUAL CONTROL", PAL_YELLOW);
    else        hwt_text_right(1252, 682, "AUTO PILOT", PAL_GREEN);

    if (ready_t > 0) {
        panel(UI_BAND_BLACK, 400, 300, 480, 116, 170);
        p = b; p = s_str(p, "STAGE "); p = s_num(p, stage, 2); *p = 0;
        hwt_text_big(432, 316, b, 0);
        hwt_text(432, 386, "AUTO PILOT ENGAGED", PAL_GREEN);
    }
    if (gameover_t > 0) {
        panel(UI_BAND_BLACK, 340, 300, 600, 120, 190);
        hwt_text_big(372, 316, "GAME OVER", 0);
    }
    if (clear_t > 0) {
        /* STAGE CLEAR card.  Fade the panel in over the first 20 frames and the
         * text slightly after it, so it reads as a reward screen rather than a
         * hard cut.  11 glyphs at 44 px = 484 px wide. */
        int k  = (240 - clear_t);
        int on = (k < 20) ? 0 : 1;
        if (on) {
            panel(UI_BAND_BLACK, 380, 268, 520, 168, 200);
            hwt_text_big(398, 292, "STAGE CLEAR", 1);
            p = b; p = s_str(p, "SCORE "); p = s_num(p, score, 6); *p = 0;
            hwt_text(516, 372, b, PAL_YELLOW);
            if (k > 40) {
                p = b; p = s_str(p, "STAGE "); p = s_num(p, stage, 2); *p = 0;
                hwt_text(516, 396, b, PAL_WHITE);
            }
        }
    }
}

/* Submit one frame of BitBlt tasks.  This function must NOT commit: main.c's
 * frame() ends with the single FRAME_LAST task for every scene.  It used to
 * commit here as well, which made the engine commit twice per update - the
 * simulation advanced 30 times a second while the page changed 60 times, so
 * every state was shown twice and the boss visibly stuttered.  main.c now has
 * the only commit and the UART log prints sim vs frames so the two can never
 * silently drift apart again. */
void game_draw(void)
{
    ntask = 0;
    sx = sy = 0;
    pvx = 0;
    /* Smooth, deterministic shake.
     *
     * This used to call rnd() twice per frame, which did two bad things:
     * it advanced the gameplay random sequence from inside the renderer (so
     * bullet patterns and spawn positions changed every frame the screen
     * shivered), and the offset jumped randomly from frame to frame, which is
     * what made the boss look like it was teleporting.  Two sine waves at
     * different rates give a shiver that looks natural and costs nothing. */
    /* Impact shake: horizontal only.
     *
     * It used to move vertically as well, and because it is added to the world
     * layer - the boss, the foes and the bullets - but not to the background
     * bands, a hit made the boss hop 2..5 px up and down against a static sky.
     * That reads as the boss twitching, not as the screen being hit, and with
     * the autopilot taking a hit every couple of seconds it was nearly
     * continuous.  Horizontally the same offset is invisible against the
     * sideways weave, so the impact still lands without the judder. */
    if (shake > 0) {
        int ph = (int)(g_frame & 63u);
        sx = (icos(ph * 4) * shake) / 3072;
        sy = 0;
    }

    if (game_bisect_mode == 3) {
        /* full-page write, no sprites: the same 1.84 MB the normal frame
         * rewrites, but with nothing that could shimmer by itself */
        bb_fill(0, 0, SCREEN_W, SCREEN_H, 0x8410u, 0);
        last_tasks = 1;
        g_sprites = 1;

        return;
    }
    if (game_bisect_mode == 1) {
        bg_fills();                       /* 24 full-width fills, no sprites */
        last_tasks = ntask;
        g_sprites = (uint32_t)ntask;

        return;
    }
    if (game_bisect_mode == 2) {
        draw_world();                     /* sprites only, background untouched */
        draw_ui();
        last_tasks = ntask;
        g_sprites = (uint32_t)ntask;

        return;
    }

    draw_bg();
    draw_world();
    draw_ui();

    /* Damage flash: a translucent red frame around the edge of the screen.
     *
     * These four edges are 1280 px wide and 680 px tall, far beyond what the
     * colour bank can serve, so they go through alpha_bank() which cuts them
     * into legal pieces.  Before that they were rejected by the engine (err 1,
     * R_PARAM) and only the two side strips appeared. */
    if (flash > 0) {
        int a = 20 + flash * 8;
        if (a > 130) a = 130;

        alpha_bank(UI_BAND_RED, 0, 0, SCREEN_W, 20, a);
        alpha_bank(UI_BAND_RED, 0, SCREEN_H - 20, SCREEN_W, 20, a);
        alpha_bank(UI_BAND_RED, 0, 20, 14, SCREEN_H - 40, a);
        alpha_bank(UI_BAND_RED, SCREEN_W - 14, 20, 14, SCREEN_H - 40, a);
    }

    last_tasks = ntask;
    g_sprites = (uint32_t)ntask;
    /* no commit here - main.c's frame() owns the single FRAME_LAST task */
}

/* ------------------------------------------------------------- interface */
void game_key(int ch)
{
    if (ch == 'p' || ch == 'P') { autopilot = !autopilot; manual = 0; return; }
    if (ch == 'j' || ch == 'J') { manual = 1; autopilot = 0; px -= FX(26); return; }
    if (ch == 'l' || ch == 'L') { manual = 1; autopilot = 0; px += FX(26); return; }
    if (ch == 'i' || ch == 'I') { manual = 1; autopilot = 0; py -= FX(26); return; }
    if (ch == 'k' || ch == 'K') { manual = 1; autopilot = 0; py += FX(26); return; }
    /* 'f' freezes everything except the renderer.
     *
     * If the picture still shimmers with the simulation frozen, the problem is
     * in the display path (double buffering, DDR contention, HDMI), not in the
     * scene content.  If it goes rock steady, some per-frame content change is
     * responsible and the search narrows to what actually moves. */
    if (ch == 'f' || ch == 'F') { frozen = !frozen; return; }
    if (ch == 'r' || ch == 'R') { game_reset(); return; }
    /* display-path bisect, see game_bisect_mode above; pressing the same key
     * again returns to the normal frame */
    if (ch == 'x' || ch == 'X') { game_bisect_mode = (game_bisect_mode == 1) ? 0 : 1; return; }
    if (ch == 'z' || ch == 'Z') { game_bisect_mode = (game_bisect_mode == 2) ? 0 : 2; return; }
    if (ch == 'b' || ch == 'B') { game_bisect_mode = (game_bisect_mode == 3) ? 0 : 3; return; }
}

/* Objective status for the UART log.  The demo is driven by an autopilot, so
 * "is it actually fighting the boss" has to be measurable rather than judged by
 * eye: boss_hp only goes down while player bullets are landing on it.
 * boss_hp is -1 when no boss is on screen. */
/* One-shot: returns 1 the frame the STAGE CLEAR card has finished, so main.c
 * moves the demo on to the next scene instead of replaying the fight. */
int game_take_scene_request(void)
{
    int r = clear_req;
    clear_req = 0;
    return r;
}

void game_status(int *out)
{
    out[0] = php;
    out[1] = boss_alive ? boss_hp : -1;
    out[2] = deaths;
    out[3] = nfo;
}

void game_tick(void)
{
    /* Fixed density.
     *
     * Measured: the engine holds a rock steady 60 fps from 227 up to 337 tasks
     * per frame, so the LOD ramp had nothing to fix.  It only made the screen
     * change how busy it looked every second, which is the "faint flicker".
     * Start at full density and back off only on a genuine overload. */
    if (g_hw_fps > 0u && g_hw_fps < 50u) {
        density -= 8;
        if (density < 45) density = 45;
    }
}
