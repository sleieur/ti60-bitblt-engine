/******************************************************************************
*
* @file main.c: BitBlt engine demonstration program (contest brief, 2D engine)
*
*   boot -> build the DDR content (images, sprite sheets, glyph atlases)
*        -> put the display path in BitBlt mode
*        -> for ever: submit one frame of BitBlt tasks, publish the CPU-rendered
*           half if a new one is ready, commit the page, then spend the rest of
*           the frame time in the software rasteriser.
*
* Keys on the UART (115200 8N1):
*   1..8   jump to a scene          a  auto cycle on/off
*   + / -  more / fewer objects     r  reset the statistics
*   in the STARFALL scene: p = auto pilot on/off, j/l/i/k = move, r = restart
*
******************************************************************************/
#include <stdint.h>
#include "bbshow.h"
#include "bsp.h"
#include "fb_driver.h"
#include "bitblt_driver.h"
#include "game.h"
#include "art.h"

/* ------------------------------------------------------------------ timing */
/* Softwar rasterising budget per frame.
 *
 * The split-screen scenes submit two full half-screen blits every frame (one
 * for the engine half, one to publish the CPU half), which is 1440 row
 * descriptors.  At the measured 4.14 us per descriptor that already costs
 * 6.0 ms of the 16.7 ms budget, so spending 9 ms here pushed a frame past the
 * deadline and the engine half visibly dropped below 60 Hz.  6 ms leaves the
 * engine comfortable and, as a bonus, makes the CPU half lag further behind -
 * which is exactly what the scene is meant to show. */
#define SLICE_TICKS   (BSP_CLINT_HZ / 167)      /* ~6 ms of software rasterising per frame */
#define TICKS_PER_MS  (BSP_CLINT_HZ / 1000)

/* Auto cycle: dwell time per scene, in seconds, in presentation order.
 *
 * This is the recording timeline.  Auto cycle is on from boot, so a take needs
 * no key presses at all - start the camera and the sequence runs itself.
 *
 *   1 INTRO     7  read the menu
 *   2 FILL      7  squares, CPU vs hardware
 *   3 COPY      7  four pictures cycling, the CPU half takes ~12 display frames
 *                  per picture so a full round is under a second
 *   4 IMAGES    7  the strip loops about once a second
 *   5 ALPHA     7  the alpha sweep is a 2.1 s cycle, so this is three of them
 *   6 KEY       7  300 keyed sprites
 *   7 GAME     50  fail-safe only.  The fight needs 380 hits at ~0.6 bullet per
 *                  frame = 10.6 s of firing, plus 2 s ready, 2.2 s entry, 2.2 s
 *                  explosion and 4 s STAGE CLEAR = ~21 s; 50 s leaves room for
 *                  two deaths.  A won fight leaves the scene by itself.
 *   8 LIMIT    10  the probe hits the wall in ~5 s, the rest shows the settling
 *   9 CHECK     7  four operations verified, one row each
 *   0 SUMMARY   7  one card, the numbers stay on screen
 *
 * Every card except GAME and LIMIT gets seven seconds: the recording is cut
 * scene by scene and each clip is preceded by its own title page in the editor,
 * so the live dwell only has to cover one clean take of the effect, not the
 * narration.
 *
 * 56 + 50 + 30 = 136 s, so the whole round ends inside two minutes and a half
 * even if the GAME fail-safe has to fire.  The survivors scene is the one place
 * that needs longer than a card: six stages at 4.5 s each is 27 s, and the whole
 * point of the scene is to watch the load climb, so it cannot be cut short.
 * Then it wraps to scene 2 (the title card is skipped). */
static const uint8_t SCENE_SEC[N_SCENE] = { 7, 7, 7, 7, 7, 7, 50, 30, 7, 7 };

/* Board keys.
 *
 * DISABLED: this bitstream does not route them to the SoC.  The board schematic
 * ("TI60F225I3-V4 DEMO板软硬件设计说明" p34) puts the four keys on
 *     KEY0 GPIOL_07 / KEY1 GPIOR_22 / KEY2 GPIOR_21 / KEY3 GPIOL_03
 * while soc_gpio[0..3] in ddr_demo_ti60.peri.xml are assigned to
 *     GPIOR_27 / GPIOR_24 / GPIOL_P_12 / GPIOL_P_02
 * which are not connected to anything on this board - and with pull_option set
 * to "none" those pads float, so the port reads noise.  Reading them made the
 * demo jump to scene 3 on its own while pressing a key did nothing at all.
 *
 * Re-enable only after the pins have been reassigned to the four keys above and
 * the bitstream rebuilt; the register itself is readable at offset 0x00 of
 * SYSTEM_GPIO_0_IO_CTRL, the logic below is complete and debounced. */
#define USE_BOARD_KEYS 0
#if USE_BOARD_KEYS
#define SW_REG      (*(volatile uint32_t *)(SYSTEM_GPIO_0_IO_CTRL))
#define SW_MASK     0x0Fu
#define SW_LOCKOUT  (BSP_CLINT_HZ / 5)          /* 200 ms contact debounce */
#define SW_SETTLE   (BSP_CLINT_HZ * 2)          /* ignore the first 2 s */
#endif

/* ------------------------------------------------------------------ state */
static int      cur_scene;
static int      auto_cycle = 1;
static int      cpu_band;
static int      cpu_ready;                      /* a finished CPU image is available */
static u64      cpu_accum;

/* The CPU half rotates through three buffers: the CPU draws into one while the
 * blit DMA reads another, so the DMA never samples a half-written image. */
static const uint32_t CPU_BUF[3] = { CPU_ADDR, CPU_ADDR2, CPU_ADDR3 };
static int      cpu_wr = 0;                     /* buffer the CPU is drawing into */
static int      cpu_rd = 1;                     /* buffer holding the last finished image */
static int      wipe;                           /* frames to clear to black after a scene change */
static int      hold;                           /* 'h': stop submitting frames, display freezes */
static uint32_t last_frames;
static uint32_t last_sim;
static uint32_t sw_last = 0xFu;                 /* GPIO idle level assumed high */
static uint64_t sw_t;                           /* time of the last accepted edge */
static uint32_t cpu_done, cpu_done_last;
static u64      t_sec, t_scene;

/* object count per scene, indexed by the scene numbers in bbshow.h:
 * SPLIT_FILL and COLOUR KEY want 300 sprites, everything else keeps its own */
static const int SCENE_COUNT[N_SCENE] = { 0, 300, 0, 0, 0, 300, 0, 0, 0, 0 };

/* ---- FPS plate for the scenes that do not draw it themselves ---------- */
static void quiet_plate(void) { bb_fill(8, 8, 240, 48, C_WHITE, 0); }

void bb_fail(const char *why)
{
    bsp_printf("\r\n*** BitBlt failure: %s  status=%x  error=%x ***\r\n",
               why, (int)bb_status(), (int)bb_error());
    for (;;) {
        bb_fill(0, 0, SCREEN_W, SCREEN_H, 0x0841u, 0);
        quiet_plate();
        hwt_text(288, 16, "BITBLT ENGINE ERROR", PAL_RED);
        hwt_text(288, 80, why, PAL_YELLOW);
        bb_fill(0, 0, 0, 0, 0, 1);            /* keep committing so the screen updates */
        g_frame++;
    }
}

/* ================================================================ scenes */

/* publish the buffer the CPU has just finished with */
static void cpu_publish(void)
{
    cpu_rd = cpu_wr;
    cpu_wr = (cpu_wr + 1) % 3;
}

static void prerender_cpu_half(void)
{
    u64 t0;

    cpu_band = 0;
    cpu_accum = 0;
    if (!scene_is_split(cur_scene)) return;

    swr_set_base(CPU_BUF[cpu_wr]);
    t0 = clint_getTime(BSP_CLINT);
    scene_cpu_begin_frame();
    for (int b = 0; b < N_BANDS; b++) scene_cpu_band(b);
    g_cpu_ms = (uint32_t)((clint_getTime(BSP_CLINT) - t0) / TICKS_PER_MS);
    cpu_publish();
    cpu_ready = 1;
}

static void scene_next(void);

static void goto_scene(int s)
{
    if (s < 0 || s >= N_SCENE) return;
    cur_scene = s;
    if (SCENE_COUNT[s] > 0) g_count = SCENE_COUNT[s];
    scene_enter(s);
    cpu_ready = 0;
    cpu_band  = 0;
    cpu_accum = 0;
    /* 10 blank frames: the engine drains whatever the old scene left queued and
     * both back pages get cleared, so no trace of the old scene can survive.
     * Going from the sprite limit probe to the game needs every one of them. */
    wipe      = 10;
    cpu_wr    = 0;
    cpu_rd    = 1;
    prerender_cpu_half();
    t_scene = clint_getTime(BSP_CLINT);
    last_frames = bb_frames();
    g_hw_fps = 60;
    bsp_printf("\r\n[scene %d] %s : %s\r\n", s, SCENE_NAME[s], scene_info(s));
}

/* ================================================================ frame */

static void cpu_slice(void)
{
    u64 t0, tend;

    if (!scene_is_split(cur_scene)) return;

    if (cpu_band == 0) {
        swr_set_base(CPU_BUF[cpu_wr]);        /* a fresh image goes into the free buffer */
        scene_cpu_begin_frame();
    }

    t0   = clint_getTime(BSP_CLINT);
    tend = t0 + SLICE_TICKS;
    while (cpu_band < N_BANDS) {
        u64 b0 = clint_getTime(BSP_CLINT);
        scene_cpu_band(cpu_band);
        cpu_band++;
        cpu_accum += clint_getTime(BSP_CLINT) - b0;   /* only real rasterising time */
        if (clint_getTime(BSP_CLINT) >= tend) break;
    }

    if (cpu_band >= N_BANDS) {
        g_cpu_ms  = (uint32_t)(cpu_accum / TICKS_PER_MS);
        cpu_accum = 0;
        cpu_band  = 0;
        cpu_publish();
        cpu_ready = 1;
        cpu_done++;
    }
}

static void frame(void)
{
    /* 'h' / 'g' display-path probe: stop submitting frames entirely.  The HDMI
     * read master keeps scanning the last committed page at 60 Hz, so the
     * picture is perfectly static and the DDR write side is idle.  If the
     * screen still flickers in this state the cause is not bbShow at all - it
     * is the read path, the TMDS link, the cable or the monitor. */
    if (hold) { bsp_uDelay(1000); return; }

    scene_update();                           /* motion, g_frame++ */

    /* Scene change handling.
     *
     * The engine keeps working through its queue even while the CPU is already
     * in the next scene, and a heavy scene (the sprite limit probe runs into
     * thousands of queued descriptors) leaves it churning long after the switch.
     * That was why the tail of the previous scene stayed on screen.
     *
     * So for a few frames after a switch we do nothing but push a full screen
     * clear.  That is one descriptor-block per frame, so the engine drains the
     * backlog almost immediately, and both back pages end up black before the
     * new scene starts drawing. */
    if (wipe > 0) {
        wipe--;
        bb_fill(0, 0, SCREEN_W, SCREEN_H, C_BLACK, 0);
        bb_fill(0, 0, 0, 0, 0, 1);            /* commit this frame as well */
        return;
    }

    scene_hw_frame();

    /* The back page changes every frame, so the CPU half has to be blitted every
     * frame as well - from the buffer the CPU finished last, never from the one
     * being drawn into. */
    if (cpu_ready && scene_is_split(cur_scene)) {
        bb_fence();
        bb_copy(CPU_BUF[cpu_rd], CPU_STRIDE, 0, 0, CPU_REG_W, CPU_REG_H, 0);
    }

    bb_fill(0, 0, 0, 0, 0, 1);                /* empty frame-last task: commit */

    cpu_slice();
}

/* ================================================================ control */

static void poll_uart(void)
{
    while (uart_readOccupancy(BSP_UART_TERMINAL)) {
        int c = (int)(read_u32(BSP_UART_TERMINAL + UART_DATA) & 0xFFu);

        if (c == 'h' || c == 'H') {
            hold = !hold;
            bsp_printf("display hold %s (frames %s)\r\n", hold ? "ON" : "OFF",
                       hold ? "stopped" : "running");
            continue;
        }
        if (c == 'g' || c == 'G') {
            /* one flat mid-grey page, then stop: a uniform field makes a faint
             * brightness modulation obvious and removes all content from the
             * picture */
            bb_fill(0, 0, SCREEN_W, SCREEN_H, 0x8410u, 0);
            bb_fill(0, 0, 0, 0, 0, 1);
            hold = 1;
            bsp_printf("flat grey page committed, frames stopped\r\n");
            continue;
        }
        if (cur_scene == SCENE_DANMAKU && (c == 'p' || c == 'P' || c == 'j' || c == 'J' ||
                                           c == 'l' || c == 'L' || c == 'i' || c == 'I' ||
                                           c == 'k' || c == 'K' || c == 'f' || c == 'F' ||
                                           c == 'x' || c == 'X' || c == 'z' || c == 'Z' ||
                                           c == 'b' || c == 'B')) {
            game_key(c);
            bsp_printf("game key '%c'  bisect=%d\r\n", c, game_bisect_mode);
            continue;
        }
        if (cur_scene == SCENE_IMAGES) {
            /* inside the picture strip the keys mean playback, not scene jumps.
             * The log line is deliberate: if the screen does not react, the
             * terminal is the first thing to rule out. */
            if (c == ' ' || c == 'p' || c == 'P') {
                images_toggle_play();
                bsp_printf("images: %s\r\n", "toggle");
                continue;
            }
            if (c == '+' || c == '=') {
                images_fps_step(+3);
                bsp_printf("images: speed up\r\n");
                continue;
            }
            if (c == '-' || c == '_') {
                images_fps_step(-3);
                bsp_printf("images: slow down\r\n");
                continue;
            }
        }
        if (c >= '1' && c <= '9') {
            auto_cycle = 0;
            goto_scene(c - '1');
        } else if (c == '0') {
            /* the summary sits after the self check, so it needs the tenth key */
            auto_cycle = 0;
            goto_scene(SCENE_SUMMARY);
        } else if (c == 'a' || c == 'A') {
            auto_cycle = !auto_cycle;
            t_scene = clint_getTime(BSP_CLINT);
            bsp_printf("auto cycle %s\r\n", auto_cycle ? "ON" : "OFF");
        } else if (c == '+' || c == '=') {
            g_count += 100;
            if (g_count > 1024) g_count = 1024;
            bsp_printf("objects = %d\r\n", g_count);
            goto_scene(cur_scene);
        } else if (c == '-' || c == '_') {
            g_count -= 100;
            if (g_count < 100) g_count = 100;
            bsp_printf("objects = %d\r\n", g_count);
            goto_scene(cur_scene);
        } else if (c == 'r' || c == 'R') {
            if (cur_scene == SCENE_DANMAKU) game_key(c);
            last_frames = bb_frames();
        } else if (c == ' ' || c == '\r' || c == '\n' || c == 'n' || c == 'N' ||
                   c == '.' || c == 0x1Bu) {
            /* any "next" key: no need to remember the scene numbers while filming.
             * In the IMAGES scene the same key advances the picture instead. */
            if (cur_scene == SCENE_IMAGES) { images_next(); continue; }
            auto_cycle = 0;
            scene_next();                  /* same order as the auto cycle */
        }
    }
}

/* Presentation order, independent of the scene ids.
 *
 * The survivors stress test opens the take and STARFALL follows it, because those
 * are the two scenes worth watching first: the horde shows what the engine can
 * carry, the boss fight shows what it is for.  Everything else follows.  Keeping
 * the order in a table instead of renumbering the scenes means the on-screen menu
 * numbers stay the ones people already know.
 *
 * Wrapping returns to the survivors scene, so a looping demo never lands on the
 * title card. */
static const uint8_t SCENE_ORDER[N_SCENE] = {
    SCENE_LIMIT,        /* 8  survivors horde  */
    SCENE_DANMAKU,      /* 7  STARFALL         */
    SCENE_INTRO,        /* 1  title menu       */
    SCENE_SPLIT_FILL,   /* 2                   */
    SCENE_SPLIT_COPY,   /* 3                   */
    SCENE_IMAGES,       /* 4                   */
    SCENE_ALPHA,        /* 5                   */
    SCENE_KEY,          /* 6                   */
    SCENE_CHECK,        /* 9                   */
    SCENE_SUMMARY       /* 0                   */
};

/* Advance to the next scene in the presentation order.  Shared by the auto cycle,
 * the board switches and the "boss down" hand-off at the end of STARFALL, so all
 * three follow the same order. */
static void scene_next(void)
{
    int i = 0;
    while (i < N_SCENE - 1 && SCENE_ORDER[i] != cur_scene) i++;
    goto_scene(SCENE_ORDER[(i + 1) % N_SCENE]);
}

/* =================================================================== main */

void main(void)
{
    u64 t0;

    bsp_init();
    bsp_printf("\r\n\r\n");
    bsp_printf("*** TI60 BitBlt engine demo: HDMI 1280x720p60 RGB565 ***\r\n");
    bsp_printf("keys: SPACE/ENTER = next scene, 1-9 jump, 0 summary, a auto\r\n");
    bsp_printf("      +/- objects, r reset, h freeze frames, g flat grey page\r\n");

    fb_set_base(0, FB0_ADDR);
    fb_set_base(1, FB1_ADDR);
    bb_init();

    if ((bb_status() & BB_ST_HW_MODE) == 0) {
        bsp_printf("\r\n!!! cannot enter BitBlt mode (status=%x) !!!\r\n", (int)bb_status());
        bsp_printf("The display logic of the FPGA is already running, so RENDER_MODE is\r\n");
        bsp_printf("frozen.  Reload ddr_demo_ti60.bit with the Efinity Programmer and\r\n");
        bsp_printf("start this program again.\r\n");
        for (;;) bsp_uDelay(500000);
    }
    if (bb_capability() != BB_CAP_VALUE) {
        bsp_printf("unexpected capability %x\r\n", (int)bb_capability());
    }

    bsp_printf("building DDR content ...\r\n");
    t0 = clint_getTime(BSP_CLINT);
    content_init();
    bsp_printf("  images, sprites and glyphs done (%d ms)\r\n",
               (int)((clint_getTime(BSP_CLINT) - t0) / TICKS_PER_MS));
    bsp_printf("frame size %x  stride %d\r\n", (int)fb_frame_size(), (int)fb_stride());
    /* Reuses main's printf instead of pulling a second one into art.c: adding the
     * call there cost 1.2 KB of the 124 KB region. */
    bsp_printf("art source: %s\r\n", art_pack_loaded ? "hand-painted pack" : "procedural");

    game_init();

    /* recording timeline: nothing has to be pressed, the sequence runs itself */
    {
        int total = 0;
        for (int i = 0; i < N_SCENE; i++) total += SCENE_SEC[i];
        bsp_printf("auto cycle ON: ");
        for (int i = 0; i < N_SCENE; i++)
            bsp_printf("%d%s ", SCENE_SEC[i], (i + 1 < N_SCENE) ? "" : "s\r\n");
        bsp_printf("  total %d s, wraps to scene 2 after the summary\r\n", total);
    }

    g_count = SCENE_COUNT[SCENE_INTRO];
    goto_scene(SCENE_INTRO);

    t_sec = clint_getTime(BSP_CLINT);
    last_frames = bb_frames();
    last_sim = g_frame;

    for (;;) {
        frame();
        poll_uart();

        /* the game scene ends itself once the boss is down: the STAGE CLEAR
         * card has been shown, so move on to the picture strip and stay there
         * instead of replaying the same fight forever */
        if (cur_scene == SCENE_DANMAKU && game_take_scene_request()) {
            bsp_printf("STARFALL cleared, next scene\r\n");
            scene_next();
        }

        u64 t = clint_getTime(BSP_CLINT);

#if USE_BOARD_KEYS
        /* board keys: one falling edge = next scene, debounced.  The idle level
         * is latched from the first read instead of being assumed, so a pad that
         * settles low cannot look like a keypress. */
        {
            uint32_t v = SW_REG & SW_MASK;
            if (t < SW_SETTLE) {
                sw_last = v;                       /* settle the level first */
            } else if (((~v) & sw_last & SW_MASK) && (t - sw_t) > SW_LOCKOUT) {
                sw_t = t;
                scene_next();
                bsp_printf("KEY -> next scene\r\n");
            } else {
                sw_last = v;
            }
        }
#endif

        if (t - t_sec >= (u64)BSP_CLINT_HZ) {
            uint32_t f = bb_frames();
            uint32_t e = bb_error();

            g_hw_fps     = f - last_frames;
            g_cpu_hz     = cpu_done - cpu_done_last;
            last_frames  = f;
            cpu_done_last = cpu_done;
            t_sec        = t;

            /* sim = how often the simulation advanced in this second, frames =
             * how often a frame was actually committed and shown.  They have to
             * match: if sim is higher the loop is producing states that never
             * reach the screen, which reads as motion that stutters or stumbles
             * even though every position in its own timeline is smooth. */
            uint32_t sim = g_frame - last_sim;
            last_sim = g_frame;

            bsp_printf("[%s] hw %d fps  cpu %d ms  cpu %d hz  sprites %d  frames %d  sim %d  err %x  uf %d",
                       SCENE_NAME[cur_scene], (int)g_hw_fps, (int)g_cpu_ms, (int)g_cpu_hz,
                       (int)g_sprites, (int)f, (int)sim, (int)e, (int)fb_underflow());
            /* the game is driven by an autopilot - boss hull only drops while
             * player bullets are actually landing on it, so this is the only
             * objective check that the auto player is fighting and not hiding */
            if (cur_scene == SCENE_DANMAKU) {
                int st[4];
                game_status(st);
                bsp_printf("  hull %d boss %d deaths %d foes %d\r\n",
                           st[0], st[1], st[2], st[3]);
            } else {
                bsp_printf("\r\n");
            }

            if (e & 0xF00u) bb_fail("execution error");

            scene_tick();
        }

        if (auto_cycle) {
            u64 lim = (u64)BSP_CLINT_HZ * SCENE_SEC[cur_scene];
            if ((t - t_scene) >= lim) {
                bsp_printf("auto: %s done, next\r\n", SCENE_NAME[cur_scene]);
                scene_next();
            }
        }
    }
}
