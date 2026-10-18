/* gif.c - GIF: GIFtag parsing for PATH1/2/3; PACKED, REGLIST and IMAGE modes into the GS. */
#include "gfx_internal.h"

typedef struct gif_path {
    int in_tag;          /* 1 = data of the current tag still expected */
    u32 nloop, loop;     /* loops in the tag, loops done */
    u32 nreg, reg;       /* registers per loop, current register */
    u32 flg;
    int eop;
    u64 regs;
    float q_latch;
} gif_path;

static gif_path paths[3];

void gif_reset(void) { memset(paths, 0, sizeof paths); }

int gif_path_idle(int path) { return !paths[path].in_tag; }
int gif_path_ended(int path) { return !paths[path].in_tag && paths[path].eop; }

static u64 q64(const qword *q, int hi) { return hi ? (q->w[2] | ((u64)q->w[3] << 32)) : (q->w[0] | ((u64)q->w[1] << 32)); }

static void packed_write(gif_path *p, u32 r, const qword *q)
{
    u64 v;
    switch (r) {
    case 0x0: gs_write_reg(0x00, q->w[0] & 0x7FF); break;
    case 0x1:
        v = (q->w[0] & 0xFF) | ((u64)(q->w[1] & 0xFF) << 8) | ((u64)(q->w[2] & 0xFF) << 16) | ((u64)(q->w[3] & 0xFF) << 24);
        { u32 qb; memcpy(&qb, &p->q_latch, 4); v |= (u64)qb << 32; }
        gs_write_reg(0x01, v);
        break;
    case 0x2:
        memcpy(&p->q_latch, &q->w[2], 4);
        gs_write_reg(0x02, q->w[0] | ((u64)q->w[1] << 32));
        break;
    case 0x3:
        gs_write_reg(0x03, (q->w[0] & 0x3FFF) | ((u64)(q->w[1] & 0x3FFF) << 16));
        break;
    case 0x4:
        v = (q->w[0] & 0xFFFF) | ((u64)(q->w[1] & 0xFFFF) << 16) | ((u64)((q->w[2] >> 4) & 0xFFFFFF) << 32) |
            ((u64)((q->w[3] >> 4) & 0xFF) << 56);
        gs_write_reg((q->w[3] & 0x8000) ? 0x0C : 0x04, v);
        break;
    case 0x5:
        v = (q->w[0] & 0xFFFF) | ((u64)(q->w[1] & 0xFFFF) << 16) | ((u64)q->w[2] << 32);
        gs_write_reg((q->w[3] & 0x8000) ? 0x0D : 0x05, v);
        break;
    case 0xA: gs_write_reg(0x0A, (u64)((q->w[3] >> 4) & 0xFF) << 56); break;
    case 0xE: gs_write_reg(q->w[2] & 0xFF, q64(q, 0)); break;
    case 0xF: break;
    default: gs_write_reg(r, q64(q, 0)); break;
    }
}

static u32 gif_write_inner(int path, const qword *q, u32 nq);

u32 gif_write(int path, const qword *q, u32 nq)
{
    u32 n;
    gs_in_dma++;
    n = gif_write_inner(path, q, nq);
    gs_in_dma--;
    if (!gs_in_dma) gs_flush();
    return n;
}

static u32 gif_write_inner(int path, const qword *q, u32 nq)
{
    gif_path *p = &paths[path];
    u32 i = 0;
    while (i < nq) {
        if (!p->in_tag) {
            const qword *t = &q[i++];
            u64 lo = q64(t, 0);
            p->nloop = (u32)(lo & 0x7FFF);
            p->eop = (int)((lo >> 15) & 1);
            p->flg = (u32)((lo >> 58) & 3);
            p->nreg = (u32)((lo >> 60) & 0xF);
            if (p->nreg == 0) p->nreg = 16;
            p->regs = q64(t, 1);
            p->loop = 0;
            p->reg = 0;
            p->q_latch = 1.0f;
            if (p->flg == 0 && ((lo >> 46) & 1))
                gs_write_reg(0x00, (lo >> 47) & 0x7FF);
            p->in_tag = p->nloop != 0;
            if (!p->in_tag && p->eop && path == GIF_PATH1)
                return i;
            continue;
        }
        switch (p->flg) {
        case 0: { /* PACKED */
            u32 r = (u32)((p->regs >> (p->reg * 4)) & 0xF);
            packed_write(p, r, &q[i++]);
            if (++p->reg >= p->nreg) { p->reg = 0; p->loop++; }
            break;
        }
        case 1: { /* REGLIST: two 64-bit registers per qword */
            int half;
            for (half = 0; half < 2 && p->loop < p->nloop; half++) {
                u32 r = (u32)((p->regs >> (p->reg * 4)) & 0xF);
                if (r != 0xE && r != 0xF) gs_write_reg(r, q64(&q[i], half));
                if (++p->reg >= p->nreg) { p->reg = 0; p->loop++; }
            }
            i++;
            break;
        }
        default: { /* IMAGE: the rest of the run in one go */
            u32 n = nq - i;
            if (n > p->nloop - p->loop) n = p->nloop - p->loop;
            gs_write_hwreg(q[i].w, 4 * n);
            i += n;
            p->loop += n;
            break;
        }
        }
        if (p->loop >= p->nloop) {
            p->in_tag = 0;
            if (p->eop && path == GIF_PATH1)
                return i;
        }
    }
    return i;
}

void *gif_state_ptr(void) { return paths; }
u32 gif_state_size(void) { return (u32)sizeof paths; }
