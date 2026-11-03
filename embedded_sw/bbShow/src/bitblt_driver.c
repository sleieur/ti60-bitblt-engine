/******************************************************************************
*
* @file bitblt_driver.c: BitBlt engine driver
*
******************************************************************************/
#include <stdint.h>
#include "bitblt_driver.h"
#include "bsp.h"
#include "io.h"
#include "clint.h"

/* A full 720p frame takes at most ~5 ms of engine time; 200 ms means the engine
 * is wedged (execution error, or the display never releases the page). */
#define BB_TIMEOUT_TICKS 20000000u

static int bb_dead;

static inline void bb_wr(uint32_t off, uint32_t v) { write_u32(v, BB_BASE + off); }
static inline uint32_t bb_rd(uint32_t off) { return read_u32(BB_BASE + off); }

void bb_init(void)      { bb_dead = 0; bb_wr(BB_RENDER_MODE, 1); }
uint32_t bb_status(void) { return bb_rd(BB_STATUS); }
uint32_t bb_error(void)  { return bb_rd(BB_ERROR_CODE); }
uint32_t bb_frames(void) { return bb_rd(BB_FRAME_COUNT); }
uint32_t bb_capability(void) { return bb_rd(BB_CAPABILITY); }

void bb_fence(void) { asm volatile ("fence iorw,iorw" ::: "memory"); }

/* Wait until the task FIFO has room.  Returns -1 if the engine stopped accepting. */
static int wait_slot(void)
{
    u64 t0;
    u32 spins = 0;

    if (bb_dead) return -1;
    t0 = clint_getTime(SYSTEM_CLINT_CTRL);
    for (;;) {
        if (bb_rd(BB_STATUS) & BB_ST_CAN_SUBMIT) return 0;
        if ((++spins & 0x3FFu) == 0) {
            if (clint_getTime(SYSTEM_CLINT_CTRL) - t0 > BB_TIMEOUT_TICKS) {
                bb_dead = 1;
                return -1;
            }
        }
    }
}

/* Replicate fb_regs_axil.v's SUBMIT-time parameter checks in software.
 *
 * The engine answers SLVERR and silently drops the task when a parameter is
 * illegal, and the 4 bit reason code in ERROR_CODE is sticky, so one bad task
 * poisons the counter for the rest of the run.  Without this the symptom is
 * "something is missing from the screen and ERR is 1" with no way to tell which
 * task.  Only the first few offenders are printed so it cannot flood. */
static int bad_dumped;

static void check_params(uint32_t opf, uint32_t src, uint32_t ss,
                         uint32_t xy, int w, int h)
{
    int bad = 0;
    int x = (int)(xy & 0xFFFFu);
    int y = (int)(xy >> 16);

    if (bad_dumped >= 8) return;

    if (w <= 0 || h <= 0) bad |= 0x100;

    if ((opf & 3u) != BB_OP_FILL) {              /* source has to be readable */
        uint64_t rbytes  = (uint64_t)w * 2u;
        uint64_t src_end = (uint64_t)src + (uint64_t)(h - 1) * ss + rbytes;
        if (src & 1u)                        bad |= 0x01;
        if (ss & 1u)                         bad |= 0x02;
        if (ss > 0xFFFFu)                    bad |= 0x04;
        if ((uint64_t)ss < rbytes)           bad |= 0x08;
        if (src < BB_RD_LO || src_end > BB_DDR_TOP) bad |= 0x10;
    }

    if (x + w > BB_W || y + h > BB_H) bad |= 0x20;

    if (bad) {
        bad_dumped++;
        bsp_printf("BADTASK op=%x src=%x ss=%u xy=%d,%d w=%d h=%d why=%x\r\n",
                   (int)opf, (int)src, (int)ss, x, y, w, h, bad);
    }
}

static int submit(uint32_t opf, uint32_t src, uint32_t sstride, uint32_t dst, uint32_t dstride,
                  uint32_t xy, int w, int h, uint32_t color, uint32_t alpha)
{
    check_params(opf, src, sstride, xy, w, h);
    if (wait_slot()) return -1;
    bb_wr(BB_OP_FLAGS, opf);
    bb_wr(BB_SRC_ADDR, src);   bb_wr(BB_SRC_STRIDE, sstride);
    bb_wr(BB_DST_ADDR, dst);   bb_wr(BB_DST_STRIDE, dstride);
    bb_wr(BB_DST_XY, xy);
    bb_wr(BB_SIZE, ((uint32_t)h << 16) | (uint32_t)w);
    bb_wr(BB_FILL_COLOR, color); bb_wr(BB_KEY_COLOR, color); bb_wr(BB_ALPHA, alpha);
    bb_wr(BB_SUBMIT, 1);
    return 0;
}

/* clip a relative rectangle; the source start moves with the clipped edges */
static int rel(uint32_t op, uint32_t src, uint32_t sstride, int x, int y, int w, int h,
               uint32_t color, uint32_t alpha, uint32_t last)
{
    if (x < 0) { src -= (uint32_t)(2 * x); w += x; x = 0; }
    if (y < 0) { src -= (uint32_t)y * sstride; h += y; y = 0; }
    if (x + w > BB_W) w = BB_W - x;
    if (y + h > BB_H) h = BB_H - y;
    if (w <= 0 || h <= 0) {
        if (!last) return 0;
        w = 0; h = 0; x = 0; y = 0;
    }
    return submit(op | BB_F_REL | (last ? BB_F_LAST : 0), src, sstride, 0, 0,
                  ((uint32_t)y << 16) | (uint32_t)x, w, h, color, alpha);
}

int bb_fill(int x, int y, int w, int h, uint16_t color, uint32_t last)
{ return rel(BB_OP_FILL, 0, 0, x, y, w, h, color, 0, last); }

int bb_copy(uint32_t src, uint32_t sstride, int x, int y, int w, int h, uint32_t last)
{ return rel(BB_OP_COPY, src, sstride, x, y, w, h, 0, 0, last); }

int bb_key(uint32_t src, uint32_t sstride, int x, int y, int w, int h, uint16_t key, uint32_t last)
{ return rel(BB_OP_KEY, src, sstride, x, y, w, h, key, 0, last); }

int bb_alpha(uint32_t src, uint32_t sstride, int x, int y, int w, int h, uint32_t alpha, uint32_t last)
{ return rel(BB_OP_ALPHA, src, sstride, x, y, w, h, 0, alpha, last); }

int bb_fill_abs(uint32_t dst, uint32_t dstride, int w, int h, uint16_t color)
{ return submit(BB_OP_FILL, 0, 0, dst, dstride, 0, w, h, color, 0); }

int bb_copy_abs(uint32_t src, uint32_t sstride, uint32_t dst, uint32_t dstride, int w, int h)
{ return submit(BB_OP_COPY, src, sstride, dst, dstride, 0, w, h, 0, 0); }

int bb_key_abs(uint32_t src, uint32_t sstride, uint32_t dst, uint32_t dstride,
               int w, int h, uint16_t key)
{ return submit(BB_OP_KEY, src, sstride, dst, dstride, 0, w, h, key, 0); }

int bb_alpha_abs(uint32_t src, uint32_t sstride, uint32_t dst, uint32_t dstride,
                 int w, int h, uint32_t alpha)
{ return submit(BB_OP_ALPHA, src, sstride, dst, dstride, 0, w, h, 0, alpha); }

int bb_idle(void)
{
    return (bb_status() & BB_ST_BUSY) == 0u;
}
