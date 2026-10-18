/* iop_cpu.c - R3000A (MIPS I) interpreter for the IOP: branch and load delay slots, HI/LO, the syscall trap
 * the IRX loader plants in import stubs, and the IOP memory map. */
#include "iop.h"

u8 iop_ram[IOP_RAM_SIZE];
iop_regs iop;
u32 iop_gp;
int iop_halt;
u32 iop_trace_pc = 0xFFFFFFFFu;   /* debug: log registers when this pc runs (DOTR_IOP_TRACE) */

static u8 iop_spr[0x400];
static u64 icount;
static int depth;

u64 iop_instructions(void) { return icount; }

void iop_cpu_reset(void)
{
    memset(&iop, 0, sizeof iop);
    memset(iop_ram, 0, sizeof iop_ram);
    memset(iop_spr, 0, sizeof iop_spr);
    icount = 0;
    depth = 0;
}

/* ---- memory map ------------------------------------------------------------------------------------ */

static inline u32 phys(u32 a) { return a & 0x1FFFFFFFu; }

u32 iop_read32(u32 a)
{
    u32 p = phys(a), v;
    if (p < 0x00800000u) { memcpy(&v, iop_ram + (p & (IOP_RAM_SIZE - 1) & ~3u), 4); return v; }
    if (p >= 0x1F800000u && p < 0x1F800400u) { memcpy(&v, iop_spr + (p & 0x3FC), 4); return v; }
    if (p >= 0x1F900000u && p < 0x1F900800u) return spu2_read(p) | ((u32)spu2_read(p + 2) << 16);
    if (p >= 0x1F801000u && p < 0x1F810000u) return iop_hw_read(p, 4);
    return 0;
}

u16 iop_read16(u32 a)
{
    u32 p = phys(a);
    u16 v;
    if (p < 0x00800000u) { memcpy(&v, iop_ram + (p & (IOP_RAM_SIZE - 1) & ~1u), 2); return v; }
    if (p >= 0x1F800000u && p < 0x1F800400u) { memcpy(&v, iop_spr + (p & 0x3FE), 2); return v; }
    if (p >= 0x1F900000u && p < 0x1F900800u) return spu2_read(p);
    if (p >= 0x1F801000u && p < 0x1F810000u) return (u16)iop_hw_read(p, 2);
    return 0;
}

u8 iop_read8(u32 a)
{
    u32 p = phys(a);
    if (p < 0x00800000u) return iop_ram[p & (IOP_RAM_SIZE - 1)];
    if (p >= 0x1F800000u && p < 0x1F800400u) return iop_spr[p & 0x3FF];
    if (p >= 0x1F900000u && p < 0x1F900800u) return (u8)(spu2_read(p & ~1u) >> ((p & 1) * 8));
    if (p >= 0x1F801000u && p < 0x1F810000u) return (u8)iop_hw_read(p, 1);
    return 0;
}

void iop_write32(u32 a, u32 v)
{
    u32 p = phys(a);
    if (p < 0x00800000u) { memcpy(iop_ram + (p & (IOP_RAM_SIZE - 1) & ~3u), &v, 4); return; }
    if (p >= 0x1F800000u && p < 0x1F800400u) { memcpy(iop_spr + (p & 0x3FC), &v, 4); return; }
    if (p >= 0x1F900000u && p < 0x1F900800u) { spu2_write(p, (u16)v); spu2_write(p + 2, (u16)(v >> 16)); return; }
    if (p >= 0x1F801000u && p < 0x1F810000u) { iop_hw_write(p, v, 4); return; }
}

void iop_write16(u32 a, u16 v)
{
    u32 p = phys(a);
    if (p < 0x00800000u) { memcpy(iop_ram + (p & (IOP_RAM_SIZE - 1) & ~1u), &v, 2); return; }
    if (p >= 0x1F800000u && p < 0x1F800400u) { memcpy(iop_spr + (p & 0x3FE), &v, 2); return; }
    if (p >= 0x1F900000u && p < 0x1F900800u) { spu2_write(p, v); return; }
    if (p >= 0x1F801000u && p < 0x1F810000u) { iop_hw_write(p, v, 2); return; }
}

void iop_write8(u32 a, u8 v)
{
    u32 p = phys(a);
    if (p < 0x00800000u) { iop_ram[p & (IOP_RAM_SIZE - 1)] = v; return; }
    if (p >= 0x1F800000u && p < 0x1F800400u) { iop_spr[p & 0x3FF] = v; return; }
    if (p >= 0x1F801000u && p < 0x1F810000u) { iop_hw_write(p, v, 1); return; }
}

/* ---- interpreter ----------------------------------------------------------------------------------- */

#define RS(w) (((w) >> 21) & 31)
#define RT(w) (((w) >> 16) & 31)
#define RD(w) (((w) >> 11) & 31)
#define SA(w) (((w) >> 6) & 31)
#define IMM(w) ((u32)(s32)(s16)(w))
#define IMMU(w) ((w) & 0xFFFFu)

/* The general register an instruction writes (0 = none); loads are tracked separately. */
static u32 dest_reg(u32 w)
{
    u32 op = w >> 26;
    if (op == 0) {
        switch (w & 0x3F) {
        case 0x08: case 0x0C: case 0x0D: case 0x11: case 0x13: case 0x18: case 0x19: case 0x1A: case 0x1B: return 0;
        default: return RD(w);
        }
    }
    if (op == 0x01) return (RT(w) & 0x10) ? 31 : 0;
    if (op == 0x03) return 31;
    if (op >= 0x08 && op <= 0x0F) return RT(w);
    return 0;
}

static void run(void)
{
    u32 *r = iop.r;
    u32 pc = iop.pc, npc = pc + 4;
    int ld_reg = 0;          /* load delay: register and value that land after the next instruction */
    u32 ld_val = 0;
    u64 guard = 0;
    for (;;) {
        u32 w, op, target = 0;
        int taken = 0, nld_reg = 0;
        u32 nld_val = 0;
        if (pc == IOP_RET_MAGIC) break;
        if (pc == iop_trace_pc)
            iop_log(2, "iop trace %08x: a0=%08x a1=%08x a2=%08x a3=%08x v0=%08x v1=%08x sp=%08x ra=%08x", pc,
                    r[4], r[5], r[6], r[7], r[2], r[3], r[29], r[31]);
        w = iop_read32(pc);
        op = w >> 26;
        icount++;
        switch (op) {
        case 0x00: /* SPECIAL */
            switch (w & 0x3F) {
            case 0x00: if (RD(w)) r[RD(w)] = r[RT(w)] << SA(w); break;
            case 0x02: if (RD(w)) r[RD(w)] = r[RT(w)] >> SA(w); break;
            case 0x03: if (RD(w)) r[RD(w)] = (u32)((s32)r[RT(w)] >> SA(w)); break;
            case 0x04: if (RD(w)) r[RD(w)] = r[RT(w)] << (r[RS(w)] & 31); break;
            case 0x06: if (RD(w)) r[RD(w)] = r[RT(w)] >> (r[RS(w)] & 31); break;
            case 0x07: if (RD(w)) r[RD(w)] = (u32)((s32)r[RT(w)] >> (r[RS(w)] & 31)); break;
            case 0x08: target = r[RS(w)]; taken = 1; break;                           /* jr */
            case 0x09: target = r[RS(w)]; taken = 1; if (RD(w)) r[RD(w)] = pc + 8; break; /* jalr */
            case 0x0C: {                                                               /* syscall */
                u32 code = (w >> 6) & 0xFFFFF;
                if (ld_reg) { r[ld_reg] = ld_val; ld_reg = 0; }
                iop.pc = pc;
                iop_hle_call(code);
                /* HLE stubs return straight to the caller */
                pc = r[31]; npc = pc + 4;
                if (iop_halt) { iop_halt = 0; goto out; }   /* the thread blocks here for good: the call ends */
                continue;
            }
            case 0x0D: iop_log(1, "iop: break at %08x", pc); break;
            case 0x10: if (RD(w)) r[RD(w)] = iop.hi; break;
            case 0x11: iop.hi = r[RS(w)]; break;
            case 0x12: if (RD(w)) r[RD(w)] = iop.lo; break;
            case 0x13: iop.lo = r[RS(w)]; break;
            case 0x18: { s64 m = (s64)(s32)r[RS(w)] * (s32)r[RT(w)]; iop.lo = (u32)m; iop.hi = (u32)((u64)m >> 32); break; }
            case 0x19: { u64 m = (u64)r[RS(w)] * r[RT(w)]; iop.lo = (u32)m; iop.hi = (u32)(m >> 32); break; }
            case 0x1A: {
                s32 n = (s32)r[RS(w)], d = (s32)r[RT(w)];
                if (d == 0) { iop.lo = n >= 0 ? 0xFFFFFFFFu : 1; iop.hi = (u32)n; }
                else if (n == (s32)0x80000000 && d == -1) { iop.lo = 0x80000000u; iop.hi = 0; }
                else { iop.lo = (u32)(n / d); iop.hi = (u32)(n % d); }
                break;
            }
            case 0x1B: {
                u32 n = r[RS(w)], d = r[RT(w)];
                if (d == 0) { iop.lo = 0xFFFFFFFFu; iop.hi = n; } else { iop.lo = n / d; iop.hi = n % d; }
                break;
            }
            case 0x20: case 0x21: if (RD(w)) r[RD(w)] = r[RS(w)] + r[RT(w)]; break;
            case 0x22: case 0x23: if (RD(w)) r[RD(w)] = r[RS(w)] - r[RT(w)]; break;
            case 0x24: if (RD(w)) r[RD(w)] = r[RS(w)] & r[RT(w)]; break;
            case 0x25: if (RD(w)) r[RD(w)] = r[RS(w)] | r[RT(w)]; break;
            case 0x26: if (RD(w)) r[RD(w)] = r[RS(w)] ^ r[RT(w)]; break;
            case 0x27: if (RD(w)) r[RD(w)] = ~(r[RS(w)] | r[RT(w)]); break;
            case 0x2A: if (RD(w)) r[RD(w)] = (s32)r[RS(w)] < (s32)r[RT(w)]; break;
            case 0x2B: if (RD(w)) r[RD(w)] = r[RS(w)] < r[RT(w)]; break;
            default: goto bad;
            }
            break;
        case 0x01: { /* REGIMM */
            s32 v = (s32)r[RS(w)];
            u32 rt = RT(w);
            int cond = (rt & 1) ? v >= 0 : v < 0;
            if (rt & 0x10) r[31] = pc + 8;
            if (cond) { target = pc + 4 + (IMM(w) << 2); taken = 1; }
            break;
        }
        case 0x02: target = (pc & 0xF0000000u) | ((w & 0x3FFFFFF) << 2); taken = 1; break;
        case 0x03: target = (pc & 0xF0000000u) | ((w & 0x3FFFFFF) << 2); taken = 1; r[31] = pc + 8; break;
        case 0x04: if (r[RS(w)] == r[RT(w)]) { target = pc + 4 + (IMM(w) << 2); taken = 1; } break;
        case 0x05: if (r[RS(w)] != r[RT(w)]) { target = pc + 4 + (IMM(w) << 2); taken = 1; } break;
        case 0x06: if ((s32)r[RS(w)] <= 0) { target = pc + 4 + (IMM(w) << 2); taken = 1; } break;
        case 0x07: if ((s32)r[RS(w)] > 0) { target = pc + 4 + (IMM(w) << 2); taken = 1; } break;
        case 0x08: case 0x09: if (RT(w)) r[RT(w)] = r[RS(w)] + IMM(w); break;
        case 0x0A: if (RT(w)) r[RT(w)] = (s32)r[RS(w)] < (s32)IMM(w); break;
        case 0x0B: if (RT(w)) r[RT(w)] = r[RS(w)] < IMM(w); break;
        case 0x0C: if (RT(w)) r[RT(w)] = r[RS(w)] & IMMU(w); break;
        case 0x0D: if (RT(w)) r[RT(w)] = r[RS(w)] | IMMU(w); break;
        case 0x0E: if (RT(w)) r[RT(w)] = r[RS(w)] ^ IMMU(w); break;
        case 0x0F: if (RT(w)) r[RT(w)] = IMMU(w) << 16; break;
        case 0x10: /* COP0: mfc0 / mtc0 / rfe; the kernel is HLE, so status and cause read as quiet */
            if (RS(w) == 0 && RT(w)) { nld_reg = (int)RT(w); nld_val = 0; }
            break;
        case 0x20: nld_reg = (int)RT(w); nld_val = (u32)(s32)(s8)iop_read8(r[RS(w)] + IMM(w)); break;
        case 0x21: nld_reg = (int)RT(w); nld_val = (u32)(s32)(s16)iop_read16(r[RS(w)] + IMM(w)); break;
        case 0x22: case 0x26: { /* lwl / lwr: work on the value still in flight */
            u32 a = r[RS(w)] + IMM(w), m = iop_read32(a & ~3u), sh = (a & 3) * 8;
            u32 cur = (ld_reg == (int)RT(w)) ? ld_val : r[RT(w)];
            if (op == 0x22) nld_val = (cur & (0x00FFFFFFu >> sh)) | (m << (24 - sh));
            else nld_val = (cur & ~(0xFFFFFFFFu >> sh)) | (m >> sh);
            nld_reg = (int)RT(w);
            break;
        }
        case 0x23: nld_reg = (int)RT(w); nld_val = iop_read32(r[RS(w)] + IMM(w)); break;
        case 0x24: nld_reg = (int)RT(w); nld_val = iop_read8(r[RS(w)] + IMM(w)); break;
        case 0x25: nld_reg = (int)RT(w); nld_val = iop_read16(r[RS(w)] + IMM(w)); break;
        case 0x28: iop_write8(r[RS(w)] + IMM(w), (u8)r[RT(w)]); break;
        case 0x29: iop_write16(r[RS(w)] + IMM(w), (u16)r[RT(w)]); break;
        case 0x2A: case 0x2E: { /* swl / swr */
            u32 a = r[RS(w)] + IMM(w), m = iop_read32(a & ~3u), sh = (a & 3) * 8, v = r[RT(w)];
            if (op == 0x2A) m = (m & ~(0xFFFFFFFFu >> (24 - sh))) | (v >> (24 - sh));
            else m = (m & ~(0xFFFFFFFFu << sh)) | (v << sh);
            iop_write32(a & ~3u, m);
            break;
        }
        case 0x2B: iop_write32(r[RS(w)] + IMM(w), r[RT(w)]); break;
        case 0x12: case 0x32: case 0x3A: break; /* COP2 / LWC2 / SWC2: no GTE on the PS2 IOP */
        default:
        bad:
            /* not code: the driver jumped somewhere wrong. Stop this call (one line, no flood, no freeze) */
            iop_log(0, "iop: not an instruction %08x at %08x (ra %08x): the call is stopped", w, pc, r[31]);
            goto out;
        }
        /* the previous load lands now (after this instruction read its operands), unless this instruction
         * wrote that register itself (then the loaded value is lost, as on the R3000) */
        if (ld_reg && ld_reg != nld_reg && ld_reg != (int)dest_reg(w)) r[ld_reg] = ld_val;
        ld_reg = nld_reg; ld_val = nld_val;
        r[0] = 0;
        pc = npc;
        npc = taken ? target : npc + 4;
        /* a call that never returns (a wait the emulation cannot answer) must not freeze the game: the longest real
         * one, a sound bank copy, is well under a million instructions */
        if (++guard > 50000000ull) { iop_log(0, "iop: call did not return (pc %08x, ra %08x)", pc, r[31]); break; }
        if ((guard & 63) == 0) { iop.pc = pc; iop_check_intr(); }
    }
out:
    if (ld_reg) r[ld_reg] = ld_val;
    r[0] = 0;
    iop.pc = pc;
}

u32 iop_call(u32 func, u32 a0, u32 a1, u32 a2, u32 a3, u32 sp)
{
    iop_regs saved = iop;
    u32 v0;
    iop.r[4] = a0; iop.r[5] = a1; iop.r[6] = a2; iop.r[7] = a3;
    iop.r[29] = sp;
    iop.r[31] = IOP_RET_MAGIC;
    iop.r[28] = iop_gp;
    iop.pc = func;
    depth++;
    run();
    depth--;
    v0 = iop.r[2];
    iop = saved;
    return v0;
}
