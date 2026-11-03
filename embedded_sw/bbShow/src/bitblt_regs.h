/* BitBlt register map on axiA (fb_regs_axil.v, 0xE1000100..0xE1000158). */
#ifndef BITBLT_REGS_H
#define BITBLT_REGS_H

#define BB_BASE             0xE1000000u
#define BB_RENDER_MODE      0x100u   /* bit0: 0 CPU, 1 BitBlt (before the display start only) */
#define BB_OP_FLAGS         0x104u   /* [1:0] op, bit2 DST_REL, bit3 FRAME_LAST */
#define BB_SRC_ADDR         0x108u
#define BB_DST_ADDR         0x10Cu   /* absolute destination */
#define BB_SIZE             0x110u   /* width[15:0] height[31:16] in pixels */
#define BB_SRC_STRIDE       0x114u   /* bytes */
#define BB_DST_STRIDE       0x118u   /* bytes, absolute destination only */
#define BB_DST_XY           0x11Cu   /* x[15:0] y[31:16], relative destination */
#define BB_FILL_COLOR       0x120u   /* RGB565 */
#define BB_KEY_COLOR        0x124u   /* RGB565 */
#define BB_ALPHA            0x128u   /* 0..255 */
#define BB_SUBMIT           0x12Cu   /* write 1 */
#define BB_QUEUE_LEVEL      0x130u   /* [7:0] queued, bit8 FULL */
#define BB_STATUS           0x134u
#define BB_ERROR_CODE       0x138u   /* [3:0] submit reject, [11:8] execution error */
#define BB_LAST_ACCEPTED_ID 0x13Cu
#define BB_LAST_DONE_ID     0x140u
#define BB_ERROR_TASK       0x144u
#define BB_ERROR_ROW        0x148u
#define BB_COMPLETED_COUNT  0x14Cu
#define BB_FRAME_COUNT      0x150u
#define BB_ERROR_CLEAR      0x154u
#define BB_CAPABILITY       0x158u   /* 0x00010565 */

#define BB_OP_COPY   0u
#define BB_OP_FILL   1u
#define BB_OP_KEY    2u
#define BB_OP_ALPHA  3u
#define BB_F_REL     (1u << 2)
#define BB_F_LAST    (1u << 3)

#define BB_ST_BUSY       (1u << 0)
#define BB_ST_FRAME_WAIT (1u << 1)
#define BB_ST_ERROR      (1u << 2)
#define BB_ST_CAN_SUBMIT (1u << 3)
#define BB_ST_HW_MODE    (1u << 4)

#define BB_CAP_VALUE 0x00010565u

/* ERROR_CODE[3:0] submit reject */
#define BB_REJ_PARAM 1u
#define BB_REJ_FULL  2u
#define BB_REJ_MODE  3u
#define BB_REJ_EXEC  4u
/* ERROR_CODE[11:8] execution error */
#define BB_ERR_RD    1u
#define BB_ERR_WR    2u
#define BB_ERR_LAST  3u

/* address windows checked at SUBMIT */
#define BB_RD_LO   0x01000000u
#define BB_WR_LO   0x02000000u
#define BB_DDR_TOP 0x10000000u

#endif
