/* vif1.c - VIF1: VIFcode interpreter, every UNPACK form, MPG, DIRECT/DIRECTHL, double buffering. */
#include "gfx_internal.h"

vif1_state vif1;

void vif1_reset(void)
{
    memset(&vif1, 0, sizeof vif1);
    vif1.cycle_cl = 1;
    vif1.cycle_wl = 1;
}

enum {
    VIF_NOP = 0x00, VIF_STCYCL = 0x01, VIF_OFFSET = 0x02, VIF_BASE = 0x03, VIF_ITOP = 0x04, VIF_STMOD = 0x05,
    VIF_MSKPATH3 = 0x06, VIF_MARK = 0x07, VIF_FLUSHE = 0x10, VIF_FLUSH = 0x11, VIF_FLUSHA = 0x13,
    VIF_MSCAL = 0x14, VIF_MSCALF = 0x15, VIF_MSCNT = 0x17, VIF_STMASK = 0x20, VIF_STROW = 0x30, VIF_STCOL = 0x31,
    VIF_MPG = 0x4A, VIF_DIRECT = 0x50, VIF_DIRECTHL = 0x51
};

/* Start a VU1 program; swaps the double buffer as the hardware does on MSCAL/MSCALF/MSCNT. */
static void start_vu(int cont, u32 addr)
{
    vif1.itop = vif1.itops;
    vif1.top = vif1.tops;
    vif1.dbf ^= 1;
    vif1.tops = vif1.dbf ? vif1.base + vif1.ofst : vif1.base;
    if (gfx_no_draw) return;   /* its only output is GS primitives */
    if (cont) vu1_continue();
    else vu1_run(addr);
}

/* ---- UNPACK ---------------------------------------------------------------------------------------- */

static u32 unpack_vn(u32 cmd) { return (cmd >> 2) & 3; }
static u32 unpack_vl(u32 cmd) { return cmd & 3; }

/* Bits of input per vector for an UNPACK form. */
static u32 unpack_bits(u32 cmd)
{
    u32 vn = unpack_vn(cmd), vl = unpack_vl(cmd);
    if (vn == 3 && vl == 3) return 16;          /* V4-5 */
    return (vn + 1) * (32u >> vl);
}

/* Number of input words an UNPACK of num write-cycles needs. */
static u32 unpack_words(u32 cmd, u32 num)
{
    u32 cl = vif1.cycle_cl, wl = vif1.cycle_wl, vectors;
    if (wl <= cl || cl == 0) vectors = num;
    else vectors = (num / wl) * cl + ((num % wl) < cl ? (num % wl) : cl);
    return (vectors * unpack_bits(cmd) + 31) / 32;
}

static void unpack_write_vector(u32 cmd, const u32 *in, int has_data)
{
    u32 vn = unpack_vn(cmd), vl = unpack_vl(cmd);
    int usn = (vif1.code >> 14) & 1;
    int masked = (cmd >> 4) & 1;
    u32 dst = vif1.addr & (VU1_MEM_BYTES - 1);
    u32 *out = (u32 *)(vu1.mem + dst);
    u32 v[4] = { 0, 0, 0, 0 };
    u32 cyc = vif1.cl < 3 ? vif1.cl : 3;
    int k;

    if (has_data) {
        u32 n = vn + 1, bits = 32u >> vl, i;
        if (vn == 3 && vl == 3) {
            u32 c = in[0] & 0xFFFF;
            v[0] = (c & 0x1F) << 3; v[1] = ((c >> 5) & 0x1F) << 3; v[2] = ((c >> 10) & 0x1F) << 3; v[3] = ((c >> 15) & 1) << 7;
        } else {
            u32 raw[4];
            for (i = 0; i < n; i++) {
                u32 bitpos = i * bits;
                u32 x = in[bitpos >> 5] >> (bitpos & 31);
                if (bits < 32) {
                    x &= (1u << bits) - 1;
                    if (!usn && (x >> (bits - 1)) & 1) x |= ~((1u << bits) - 1);
                }
                raw[i] = x;
            }
            switch (vn) {
            case 0: v[0] = v[1] = v[2] = v[3] = raw[0]; break;          /* S */
            case 1: v[0] = raw[0]; v[1] = raw[1]; v[2] = raw[0]; v[3] = raw[1]; break; /* V2: z,w repeat x,y */
            case 2: v[0] = raw[0]; v[1] = raw[1]; v[2] = raw[2];         /* V3: w is the next input element */
                {
                    u32 bitpos = 3 * bits, x = in[bitpos >> 5] >> (bitpos & 31);
                    if (bits < 32) { x &= (1u << bits) - 1; if (!usn && (x >> (bits - 1)) & 1) x |= ~((1u << bits) - 1); }
                    v[3] = x;
                }
                break;
            default: v[0] = raw[0]; v[1] = raw[1]; v[2] = raw[2]; v[3] = raw[3]; break;
            }
        }
    }
    for (k = 0; k < 4; k++) {
        u32 m = masked ? (vif1.mask >> (cyc * 8 + k * 2)) & 3 : 0;
        if (!has_data && m == 0) m = masked ? 3 : 1;   /* filling write without data: row value unless protected */
        switch (m) {
        case 0:
            switch (vif1.mode) {
            case 1: out[k] = v[k] + vif1.row[k]; break;
            case 2: vif1.row[k] += v[k]; out[k] = vif1.row[k]; break;
            default: out[k] = v[k]; break;
            }
            break;
        case 1: out[k] = vif1.row[k]; break;
        case 2: out[k] = vif1.col[cyc]; break;
        default: break;
        }
    }
}

/* Process one input word for an UNPACK in progress. Sub-word vectors are handled through a bit reservoir. */
static void unpack_word(u32 w)
{
    u32 cmd = (vif1.code >> 24) & 0x7F;
    u32 bits = unpack_bits(cmd);
    u32 cl = vif1.cycle_cl, wl = vif1.cycle_wl;
    /* reservoir: up to 4 words (V4-32 = 128 bits) */
    vif1.wbuf[vif1.wbuf_n++] = w;
    for (;;) {
        u32 have = vif1.wbuf_n * 32 - (u32)vif1.pend_nbits;  /* pend_nbits = bits already consumed in wbuf[0] */
        u32 in[5];
        u32 i, shift = (u32)vif1.pend_nbits;
        if (vif1.num == 0)
            return;
        /* filling writes (WL > CL) without input */
        if (wl > cl && cl != 0 && vif1.cl >= cl) {
            unpack_write_vector(cmd, NULL, 0);
            vif1.addr += 16;
            if (++vif1.cl >= wl) vif1.cl = 0;
            vif1.num--;
            continue;
        }
        if (have < bits)
            return;
        for (i = 0; i < 4; i++) {
            u32 lo = i < vif1.wbuf_n ? vif1.wbuf[i] : 0, hi = i + 1 < vif1.wbuf_n ? vif1.wbuf[i + 1] : 0;
            in[i] = shift ? (lo >> shift) | (hi << (32 - shift)) : lo;
        }
        unpack_write_vector(cmd, in, 1);
        vif1.num--;
        /* consume */
        shift += bits;
        while (shift >= 32 && vif1.wbuf_n) {
            for (i = 1; i < vif1.wbuf_n; i++) vif1.wbuf[i - 1] = vif1.wbuf[i];
            vif1.wbuf_n--;
            shift -= 32;
        }
        vif1.pend_nbits = (int)shift;
        /* advance the write cycle */
        vif1.addr += 16;
        vif1.cl++;
        if (wl <= cl || cl == 0) {
            if (vif1.cl >= wl) {          /* skipping write: jump over CL-WL qwords after WL writes */
                vif1.addr += (cl > wl ? cl - wl : 0) * 16;
                vif1.cl = 0;
            }
        } else if (vif1.cl >= wl) {
            vif1.cl = 0;
        }
    }
}

/* After the last data word, the stream is padded to a word boundary; filling writes may remain. */
static void unpack_finish(void)
{
    u32 cmd = (vif1.code >> 24) & 0x7F;
    while (vif1.num) {
        if (vif1.cycle_wl > vif1.cycle_cl && vif1.cycle_cl && vif1.cl >= vif1.cycle_cl) {
            unpack_write_vector(cmd, NULL, 0);
            vif1.addr += 16;
            if (++vif1.cl >= vif1.cycle_wl) vif1.cl = 0;
            vif1.num--;
        } else {
            break;
        }
    }
    vif1.wbuf_n = 0;
    vif1.pend_nbits = 0;
}

/* ---- the command stream ---------------------------------------------------------------------------- */

static void begin_command(u32 code)
{
    u32 cmd = (code >> 24) & 0x7F, imm = code & 0xFFFF, num = (code >> 16) & 0xFF;
    vif1.code = code;
    vif1.remain = 0;
    if ((cmd & 0x60) == 0x60) {
        u32 n = num ? num : 256;
        vif1.num = n;
        vif1.addr = (imm & 0x3FF) * 16;
        if (imm & 0x8000) vif1.addr += vif1.tops * 16;
        vif1.cl = 0;
        vif1.wbuf_n = 0;
        vif1.pend_nbits = 0;
        vif1.remain = unpack_words(cmd, n);
        if (vif1.remain == 0) unpack_finish();
        return;
    }
    switch (cmd) {
    case VIF_NOP: break;
    case VIF_STCYCL: vif1.cycle_cl = imm & 0xFF; vif1.cycle_wl = (imm >> 8) & 0xFF; break;
    case VIF_OFFSET: vif1.ofst = imm & 0x3FF; vif1.dbf = 0; vif1.tops = vif1.base; break;
    case VIF_BASE: vif1.base = imm & 0x3FF; break;
    case VIF_ITOP: vif1.itops = imm & 0x3FF; break;
    case VIF_STMOD: vif1.mode = imm & 3; break;
    case VIF_MSKPATH3: vif1.mskpath3 = (imm >> 15) & 1; break;
    case VIF_MARK: vif1.mark = imm; break;
    case VIF_FLUSHE: case VIF_FLUSH: case VIF_FLUSHA: break;   /* everything is already finished */
    case VIF_MSCAL: case VIF_MSCALF: start_vu(0, imm); break;
    case VIF_MSCNT: start_vu(1, 0); break;
    case VIF_STMASK: vif1.remain = 1; break;
    case VIF_STROW: case VIF_STCOL: vif1.remain = 4; break;
    case VIF_MPG:
        vif1.addr = imm;
        vif1.remain = (num ? num : 256) * 2;
        break;
    case VIF_DIRECT: case VIF_DIRECTHL:
        vif1.remain = (imm ? imm : 65536) * 4;
        break;
    default:
        hw_log(1, "vif1: unknown VIFcode %08x", code);
        break;
    }
    vif1.wbuf_n = 0;
    if ((code >> 31) & 1)
        hw_raise_intc(HW_INTC_VIF1);   /* i-bit; the stall is not modelled (the game resumes it at once) */
}

static void data_word(u32 w)
{
    u32 cmd = (vif1.code >> 24) & 0x7F;
    if ((cmd & 0x60) == 0x60) {
        unpack_word(w);
        if (--vif1.remain == 0) unpack_finish();
        return;
    }
    switch (cmd) {
    case VIF_STMASK: vif1.mask = w; break;
    case VIF_STROW: vif1.row[4 - vif1.remain] = w; break;
    case VIF_STCOL: vif1.col[4 - vif1.remain] = w; break;
    case VIF_MPG:
        vif1.wbuf[vif1.wbuf_n++] = w;
        if (vif1.wbuf_n == 2) {
            u32 a = (vif1.addr & 0x7FF) * 8;
            memcpy(vu1.micro + a, vif1.wbuf, 8);
            vu1_micro_changed();
            vif1.addr++;
            vif1.wbuf_n = 0;
        }
        break;
    case VIF_DIRECT: case VIF_DIRECTHL:
        vif1.wbuf[vif1.wbuf_n++] = w;
        if (vif1.wbuf_n == 4) {
            gif_write(GIF_PATH2, (const qword *)vif1.wbuf, 1);
            vif1.wbuf_n = 0;
        }
        break;
    default: break;
    }
    vif1.remain--;
}

/* UNPACKs of 32-bit elements or V3-16, unmasked, without filling writes: whole vectors straight from the input,
 * given to unpack_write_vector exactly as the reservoir in unpack_word would (zeros past the vector). Returns the
 * words used; 0 when this UNPACK or its position needs the word-by-word path. */
static u32 unpack_whole(u32 cmd, const u32 *w, u32 avail)
{
    u32 vw = unpack_vn(cmd) + 1, cl = vif1.cycle_cl, wl = vif1.cycle_wl, used = 0;
    if (vif1.wbuf_n || vif1.pend_nbits || ((cmd >> 4) & 1) || !(wl <= cl || cl == 0))
        return 0;
    if (unpack_vn(cmd) == 2 && unpack_vl(cmd) == 1) {
        /* V3-16: two 48-bit vectors per three words; the reservoir would hold [a b] for the first (its w is the
         * next vector's x) and [b c] shifted by 16 for the second (w past the data: 0) */
        int usn = (vif1.code >> 14) & 1;
#define E16(x) (usn ? (u32)(x) & 0xFFFFu : (u32)(s32)(s16)(x))
        while (vif1.num >= 2 && avail - used >= 3 && vif1.remain >= 3) {
            u32 a = w[used], b = w[used + 1], c = w[used + 2], k, j;
            u32 v[2][4] = { { E16(a), E16(a >> 16), E16(b), E16(b >> 16) }, { E16(b >> 16), E16(c), E16(c >> 16), 0 } };
            for (k = 0; k < 2; k++) {
                u32 *out = (u32 *)(vu1.mem + (vif1.addr & (VU1_MEM_BYTES - 1)));
                switch (vif1.mode) {   /* unpack_write_vector's unmasked writes */
                case 1: for (j = 0; j < 4; j++) out[j] = v[k][j] + vif1.row[j]; break;
                case 2: for (j = 0; j < 4; j++) { vif1.row[j] += v[k][j]; out[j] = vif1.row[j]; } break;
                default: for (j = 0; j < 4; j++) out[j] = v[k][j]; break;
                }
                vif1.addr += 16;
                if (++vif1.cl >= wl) { vif1.addr += (cl > wl ? cl - wl : 0) * 16; vif1.cl = 0; }
            }
            vif1.num -= 2;
            used += 3;
            vif1.remain -= 3;
        }
#undef E16
        if (used && vif1.remain == 0) unpack_finish();
        return used;
    }
    if (unpack_vl(cmd) != 0) return 0;
    while (vif1.num && avail - used >= vw && vif1.remain >= vw) {
        u32 in[5] = { 0, 0, 0, 0, 0 }, k;
        for (k = 0; k < vw; k++) in[k] = w[used + k];
        unpack_write_vector(cmd, in, 1);
        vif1.num--;
        used += vw;
        vif1.remain -= vw;
        vif1.addr += 16;
        if (++vif1.cl >= wl) {   /* skipping write: jump over CL-WL qwords after WL writes */
            vif1.addr += (cl > wl ? cl - wl : 0) * 16;
            vif1.cl = 0;
        }
    }
    if (used && vif1.remain == 0) unpack_finish();
    return used;
}

void vif1_write(const u32 *words, u32 n)
{
    u32 i;
    for (i = 0; i < n; i++) {
        u32 cmd = (vif1.code >> 24) & 0x7F;
        if (vif1.remain && vif1.wbuf_n == 0 && (cmd == VIF_DIRECT || cmd == VIF_DIRECTHL) && n - i >= 8) {
            /* DIRECT data: whole qwords go to the GIF as one run */
            u32 nq = (n - i < vif1.remain ? n - i : vif1.remain) / 4;
            gif_write(GIF_PATH2, (const qword *)(words + i), nq);
            vif1.remain -= nq * 4;
            i += nq * 4 - 1;
            continue;
        }
        if (vif1.remain && (cmd & 0x60) == 0x60) {
            u32 used = unpack_whole(cmd, words + i, n - i);
            if (used) { i += used - 1; continue; }
        }
        if (vif1.remain == 0) begin_command(words[i]);
        else data_word(words[i]);
    }
}
