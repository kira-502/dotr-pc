#!/usr/bin/env python3
"""rctest_gen.py - R1 instruction tests for the module recompiler.

Builds small test "modules" out of synthetic EE code: every instruction kind the modules use, with random
registers and immediates, plus control-flow patterns (every branch kind forward and backward, branch-likely,
branches into a delay slot, jal inside and outside the module, jalr, switch jumps, computed tail calls).
Each one is translated by rcgen.py; rc_unit.c runs the reference interpreter and the translation from the same
random states and compares the whole CPU state and memory.

usage: rctest_gen.py <out.c> [seed]
"""
import random
import sys

import rcgen

BASE = rcgen.MOD_BASE
EXT = 0x00104520          # a main-program address (a real module callee; the tests stub it)

PLAIN = {
    # kind: (opcode, fixed low bits, dest field, uses sa)
    'sll': (0, 0x00, 'd', 1), 'srl': (0, 0x02, 'd', 1), 'sra': (0, 0x03, 'd', 1),
    'sllv': (0, 0x04, 'd', 0), 'srlv': (0, 0x06, 'd', 0), 'srav': (0, 0x07, 'd', 0),
    'movz': (0, 0x0A, 'd', 0), 'movn': (0, 0x0B, 'd', 0), 'mfhi': (0, 0x10, 'd', 0), 'mflo': (0, 0x12, 'd', 0),
    'mult': (0, 0x18, 'd', 0), 'multu': (0, 0x19, 'd', 0), 'div': (0, 0x1A, None, 0),
    'addu': (0, 0x21, 'd', 0), 'subu': (0, 0x23, 'd', 0), 'and': (0, 0x24, 'd', 0), 'or': (0, 0x25, 'd', 0),
    'nor': (0, 0x27, 'd', 0), 'slt': (0, 0x2A, 'd', 0), 'sltu': (0, 0x2B, 'd', 0),
    'dsll': (0, 0x38, 'd', 1), 'dsll32': (0, 0x3C, 'd', 1), 'dsrl32': (0, 0x3E, 'd', 1), 'dsra32': (0, 0x3F, 'd', 1),
    'addiu': (9, None, 't', 0), 'slti': (0xA, None, 't', 0), 'sltiu': (0xB, None, 't', 0),
    'andi': (0xC, None, 't', 0), 'ori': (0xD, None, 't', 0), 'xori': (0xE, None, 't', 0),
    'lui': (0xF, None, 't', 0), 'daddiu': (0x19, None, 't', 0),
    'lb': (0x20, None, 't', 0), 'lh': (0x21, None, 't', 0), 'lw': (0x23, None, 't', 0), 'lbu': (0x24, None, 't', 0),
    'lhu': (0x25, None, 't', 0), 'ld': (0x37, None, 't', 0), 'lq': (0x1E, None, 't', 0),
    'sb': (0x28, None, None, 0), 'sh': (0x29, None, None, 0), 'sw': (0x2B, None, None, 0),
    'sd': (0x3F, None, None, 0), 'sq': (0x1F, None, None, 0),
    'lwc1': (0x31, None, None, 0), 'swc1': (0x39, None, None, 0),
    'maddu': (0x1C, 0x01, 'd', 0), 'mfhi1': (0x1C, 0x10, 'd', 0), 'mflo1': (0x1C, 0x12, 'd', 0),
    'mult1': (0x1C, 0x18, 'd', 0), 'div1': (0x1C, 0x1A, None, 0), 'paddub': (0x1C, 0x28 | (0x18 << 6), 'd', 0),
}
COP1 = {   # kind: (fmt, funct)
    'add.s': (0x10, 0), 'sub.s': (0x10, 1), 'mul.s': (0x10, 2), 'div.s': (0x10, 3), 'mov.s': (0x10, 6),
    'neg.s': (0x10, 7), 'adda.s': (0x10, 0x18), 'suba.s': (0x10, 0x19), 'mula.s': (0x10, 0x1A),
    'madd.s': (0x10, 0x1C), 'c.eq.s': (0x10, 0x32), 'c.lt.s': (0x10, 0x34), 'c.le.s': (0x10, 0x36),
    'cvt.s.w': (0x14, 0x20),
}
BRANCH = {   # kind: word without the offset (registers filled in later)
    'beq': 4 << 26, 'bne': 5 << 26, 'blez': 6 << 26, 'bgtz': 7 << 26,
    'beql': 0x14 << 26, 'bnel': 0x15 << 26, 'blezl': 0x16 << 26, 'bgtzl': 0x17 << 26,
    'bltz': (1 << 26) | (0 << 16), 'bgez': (1 << 26) | (1 << 16),
    'bltzl': (1 << 26) | (2 << 16), 'bgezl': (1 << 26) | (3 << 16),
    'bc1f': (0x11 << 26) | (8 << 21) | (0 << 16), 'bc1t': (0x11 << 26) | (8 << 21) | (1 << 16),
    'bc1fl': (0x11 << 26) | (8 << 21) | (2 << 16), 'bc1tl': (0x11 << 26) | (8 << 21) | (3 << 16),
}
SAFE_DEST = [r for r in range(32) if r not in (29, 31, 25)]   # never sp, ra, or t9 (the loop counter)


def plain(rng, kind=None):
    kind = kind or rng.choice(list(PLAIN) + list(COP1) + ['mtc1'])
    while True:
        s, t, d, sa = (rng.randrange(32) for _ in range(4))
        if kind in COP1:
            fmt, fn = COP1[kind]
            w = (0x11 << 26) | (fmt << 21) | (t << 16) | (d << 11) | (sa << 6) | fn
        elif kind == 'mtc1':
            w = (0x11 << 26) | (4 << 21) | (t << 16) | (d << 11)
        else:
            opc, low, dest, usa = PLAIN[kind]
            if low is None:
                imm = rng.choice([rng.randrange(0x10000), rng.randrange(16), 0xFFFF - rng.randrange(16), 0x8000])
                w = (opc << 26) | (s << 21) | (t << 16) | imm
            else:
                w = (opc << 26) | (s << 21) | (t << 16) | (d << 11) | ((sa << 6) if usa else 0) | low
            r = {'d': d, 't': t}.get(dest)
            if r is not None and r not in SAFE_DEST and r != 0:
                continue
        x = rcgen.decode(w, 0)
        assert x.op == kind, (kind, x.op, hex(w))
        return w


def br(kind, rng, off):
    w = BRANCH[kind]
    if not kind.startswith('bc1'):
        s = rng.choice([0, rng.randrange(32), 2, 4])
        w |= s << 21
        if kind.startswith(('beq', 'bne')):
            w |= rng.choice([s, rng.randrange(32), 0]) << 16
    return w | (off & 0xFFFF)


def jal(target):
    return (3 << 26) | ((target >> 2) & 0x3FFFFFF)


def addiu(t, s, imm):
    return (9 << 26) | (s << 21) | (t << 16) | (imm & 0xFFFF)


def lui(t, imm):
    return (0xF << 26) | (t << 16) | (imm & 0xFFFF)


def ori(t, s, imm):
    return (0xD << 26) | (s << 21) | (t << 16) | (imm & 0xFFFF)


JR_RA = 0x03E00008
NOP = 0
V1 = 3
T9 = 25


def jr(r):
    return (r << 21) | 8


def jalr(r):
    return (r << 21) | (31 << 11) | 9


def tests(rng):
    """Yield (name, words, [function starts], {jtbl addr: targets})."""
    for kind in list(PLAIN) + list(COP1) + ['mtc1']:
        for i in range(40):
            yield f'{kind}#{i}', [plain(rng, kind), plain(rng, kind), JR_RA, plain(rng, kind)], [0], {}
    for kind in BRANCH:
        for i in range(30):
            # forward: taken -> w5, delay slot w1
            yield f'{kind}/fwd#{i}', [br(kind, rng, 4), plain(rng), addiu(V1, V1, 7), JR_RA, NOP,
                                      addiu(V1, V1, 3), plain(rng), JR_RA, plain(rng)], [0], {}
            # backward: a loop counted down in t9 (3 trips), the random body may or may not take the branch
            yield f'{kind}/back#{i}', [addiu(T9, 0, 3), addiu(T9, T9, -1), plain(rng), plain(rng),
                                       (7 << 26) | (T9 << 21) | (-4 & 0xFFFF),   # bgtz t9, w1
                                       plain(rng), br(kind, rng, -5), plain(rng), JR_RA, NOP], [0], {}
            # into a delay slot: w0 branches to w3, which is the delay slot of w2
            yield f'{kind}/ds#{i}', [br(kind, rng, 2), plain(rng), (4 << 26) | 4, plain(rng),
                                     addiu(V1, V1, 1), JR_RA, NOP, addiu(V1, V1, 9), JR_RA, plain(rng)], [0], {}
    for i in range(60):
        # calls: main program, own function, jalr into the module, jalr to the main program
        f2 = 18
        yield f'calls#{i}', [addiu(29, 29, -16), (0x3F << 26) | (29 << 21) | (31 << 16),     # sd ra, 0(sp)
                             jal(EXT), plain(rng),
                             jal(BASE + 4 * f2), plain(rng),
                             lui(T9, BASE >> 16), ori(T9, T9, (BASE + 4 * f2) & 0xFFFF), jalr(T9), plain(rng),
                             lui(T9, EXT >> 16), ori(T9, T9, EXT & 0xFFFF), jalr(T9), plain(rng),
                             (0x37 << 26) | (29 << 21) | (31 << 16),                           # ld ra, 0(sp)
                             JR_RA, addiu(29, 29, 16), NOP,
                             addiu(2, 2, 1), plain(rng), JR_RA, plain(rng)], [0, 4 * f2], {}
        # tail calls: branch to another function, jr to the main program
        yield f'tail#{i}', [br('beq', rng, 5), plain(rng), lui(T9, EXT >> 16), ori(T9, T9, EXT & 0xFFFF),
                            jr(T9), plain(rng),
                            addiu(2, 2, 5), JR_RA, plain(rng)], [0, 24], {}
        # switch: a0 & 3 picks one of four cases through a jump table at BASE + 0x100
        tbl = BASE + 0x100
        cases = [10, 13, 16, 19]
        words = [(0xC << 26) | (4 << 21) | (8 << 16) | 3,                  # andi t0, a0, 3
                 (8 << 16) | (8 << 11) | (2 << 6),                          # sll t0, t0, 2
                 lui(9, tbl >> 16),
                 (9 << 21) | (8 << 16) | (9 << 11) | 0x21,                  # addu t1, t1, t0
                 (0x23 << 26) | (9 << 21) | (9 << 16) | (tbl & 0xFFFF),     # lw t1, lo(tbl)(t1)
                 jr(9), plain(rng), NOP, NOP, NOP]
        for k in range(4):
            words += [addiu(V1, V1, k + 1), JR_RA, plain(rng)]
        words += [NOP] * (0x40 - len(words))
        words += [BASE + 4 * c for c in cases]
        yield f'switch#{i}', words, [0], {tbl: [BASE + 4 * c for c in cases]}


def main(argv):
    out = argv[1]
    rng = random.Random(int(argv[2]) if len(argv) > 2 else 1)
    parts = ['/* Generated by recomp/rctest_gen.py. Do not edit. */', '#include "rc_test.h"',
             '#define R(n) c->r[n]', '#define F(n) c->f[n].f', '#define FU(n) c->f[n].u',
             '#define SX(x) ((uint64_t)(int64_t)(int32_t)(x))',
             '#pragma clang diagnostic ignored "-Wunused-label"', '#pragma clang diagnostic ignored "-Wunused-variable"',
             '#pragma clang diagnostic ignored "-Wparentheses-equality"']
    table = []
    for n, (name, words, starts, jt) in enumerate(tests(rng)):
        mod = f't{n:05d}'
        fns = []
        ends = starts[1:] + [None]
        for st, en in zip(starts, ends):
            f = rcgen.Fn(f'func_{BASE + st:08X}', BASE + st)
            f.words = [(w, '') for w in (words[st // 4:en // 4] if en else words[st // 4:])]
            if name.startswith('switch'):
                f.words = f.words[:22]
            fns.append(f)
        ctx = rcgen.ModCtx(mod, fns, jt)
        parts += [f'void {ctx.cname(f.addr)}(RcCpu *c);' for f in fns]
        parts += [rcgen.translate_function(ctx, f) for f in fns]
        parts.append(f'static const uint32_t {mod}_w[] = {{' + ','.join(f'0x{w:08X}u' for w in words) + '};')
        parts.append(f'static const RcFunc {mod}_f[] = {{' +
                     ','.join(f'{{0x{f.addr:08X}u, {ctx.cname(f.addr)}}}' for f in fns) + '};')
        table.append(f'{{"{name}", {mod}_w, {len(words)}, {mod}_f, {len(fns)}}}')
    parts.append('const RcUnitTest rc_unit_tests[] = {\n    ' + ',\n    '.join(table) + '\n};')
    parts.append(f'const unsigned rc_unit_ntests = {len(table)};')
    with open(out, 'w', newline='\n') as fh:
        fh.write('\n'.join(parts) + '\n')
    print(f'{len(table)} tests -> {out}')


if __name__ == '__main__':
    main(sys.argv)
