/******************************************************************************
*
* @file bitblt_driver.h: BitBlt engine driver (registers: bitblt_regs.h, RTL: rtl/bitblt)
*
* BitBlt mode (bb_init): the engine owns the double buffer. The CPU only submits tasks;
* each frame ends with a task flagged last (BB_F_LAST): when it and every earlier task
* have completed (all DDR writes acknowledged), the engine commits the page and takes
* the next back page by itself.
* Relative destinations (x, y) always land in the back page of the frame being drawn.
* Tasks are clipped to the screen here. Sources must not overlap the destination.
*
* This copy adds bb_fence() (publish CPU stores to DDR before a task reads them) and
* a submit timeout so a broken engine can never hang the CPU forever.
*
******************************************************************************/
#ifndef BITBLT_DRIVER_H
#define BITBLT_DRIVER_H

#include <stdint.h>
#include "bitblt_regs.h"

#define BB_W 1280
#define BB_H 720

void     bb_init(void);                         /* RENDER_MODE = 1 (before the display start) */
uint32_t bb_status(void);
uint32_t bb_error(void);                        /* ERROR_CODE */
uint32_t bb_frames(void);                       /* frames committed by the engine */
uint32_t bb_capability(void);

/* make earlier CPU stores visible to the BitBlt DMA (call before submitting a task
 * whose source was just written by the CPU) */
void     bb_fence(void);

/* relative destination (back page), clipped; src = top-left source pixel, sstride in bytes.
 * Returns 0 on success, -1 if the engine stopped accepting tasks (timeout / error). */
int bb_fill (int x, int y, int w, int h, uint16_t color, uint32_t last);
int bb_copy (uint32_t src, uint32_t sstride, int x, int y, int w, int h, uint32_t last);
int bb_key  (uint32_t src, uint32_t sstride, int x, int y, int w, int h, uint16_t key, uint32_t last);
int bb_alpha(uint32_t src, uint32_t sstride, int x, int y, int w, int h, uint32_t alpha, uint32_t last);

/* absolute destination (e.g. image buffers), no clipping */
int bb_fill_abs(uint32_t dst, uint32_t dstride, int w, int h, uint16_t color);
int bb_copy_abs(uint32_t src, uint32_t sstride, uint32_t dst, uint32_t dstride, int w, int h);
int bb_key_abs  (uint32_t src, uint32_t sstride, uint32_t dst, uint32_t dstride,
                 int w, int h, uint16_t key);
int bb_alpha_abs(uint32_t src, uint32_t sstride, uint32_t dst, uint32_t dstride,
                 int w, int h, uint32_t alpha);

/* 1 when the task queue is empty and nothing is in flight, i.e. every write the
 * engine issued has been acknowledged.  Absolute tasks have no FRAME_LAST, so
 * this is how the self-check knows the result is in DDR before it compares. */
int bb_idle(void);

#endif
