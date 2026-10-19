#!/usr/bin/env python3
"""rcgen.py - the module recompiler: EE (R5900) module code -> C (lane R).

Input is the decomp's own split (a read-only copy in recomp/input, see README.md): one .s file per function with
the instruction words in its comments, and the module's data files (jump tables `jtbl_*`). The instruction words are
decoded here; the disassembly text is only used to cross-check the decoder.

Each EE instruction becomes one C statement on an RcCpu (rc.h). Branches become gotos with the delay slot placed
where the EE runs it; jal inside the module is a direct C call, jal outside it goes through RCX() (the main-program
thunks), jalr and computed tail calls go through rc_dispatch, switch jumps become a C switch over the jump table.

usage:
  rcgen.py check  [input]          decode every function, compare with the disassembly, report what is unsupported
  rcgen.py module <name> [input]   print one translated module
  rcgen.py all    <outdir> [input] translate every module into <outdir> (one .c per module + rc_modules.c)
  rcgen.py images <outdir> [input] write each module's code + data image (<module>.bin) for the R3 tests
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_INPUT = os.path.join(HERE, 'input')
MOD_BASE = 0x01E00000
MOD_LIMIT = 0x01E08000          # modules run from the 32 KB window at 0x01E00000
SENTINEL = 0xFFFF0000
TRACE = os.environ.get('RCGEN_TRACE', '')   # debugging: RC_TRACE(pc) before each instruction

LINE_RE = re.compile(r'\s*/\* ([0-9A-F]+) ([0-9A-F]{8}) ([0-9A-F]{8}) \*/\s+(\S+)')
GLABEL_RE = re.compile(r'glabel (func_([0-9A-F]{8})_\w+)')


class Fn:
    def __init__(self, name, addr):
        self.name, self.addr, self.words = name, addr, []
        self.after = None       # first word after endlabel (a delay slot the split left outside the function)

    @property
    def end(self):
        return self.addr + 4 * len(self.words)


def word_of(hexbytes):
    return int.from_bytes(bytes.fromhex(hexbytes), 'little')


def load_functions(inp, mod):
    """All functions of a module, sorted by address; each with its words and spimdisasm mnemonics."""
    fns = []
    d = os.path.join(inp, 'func', mod)
    for f in os.listdir(d):
        fn = None
        ended = False
        with open(os.path.join(d, f)) as fh:
            for line in fh:
                if fn is None:
                    m = GLABEL_RE.search(line)
                    if m:
                        fn = Fn(m.group(1), int(m.group(2), 16))
                    continue
                if line.startswith('endlabel'):
                    ended = True
                    continue
                m = LINE_RE.match(line)
                if m and ended:
                    if int(m.group(2), 16) == fn.end:
                        fn.after = word_of(m.group(3))
                    break
                if m:
                    addr = int(m.group(2), 16)
                    assert addr == fn.end, (f, hex(addr))
                    fn.words.append((word_of(m.group(3)), m.group(4)))
        assert fn and fn.words, f
        fns.append(fn)
    fns.sort(key=lambda x: x.addr)
    return fns


def load_data(inp, mod):
    """Jump tables (address -> targets) and the data image (address -> word) of a module."""
    jtbls, image = {}, {}
    for f in (f'{mod}.data.s', f'{mod}_pool.rodata.s'):
        p = os.path.join(inp, 'data', f)
        if not os.path.exists(p):
            continue
        cur = None
        pending = None
        with open(p) as fh:
            for line in fh:
                s = line.strip()
                if s.startswith('dlabel '):
                    name = s.split()[1]
                    cur = [] if name.startswith('jtbl_') else None
                    if cur is not None:
                        jtbls[int(name[5:13], 16)] = cur
                    continue
                m = re.match(r'/\* [0-9A-F]+ ([0-9A-F]{8}) ([0-9A-F]{8}) \*/ \.(word|float)', s)
                if m:
                    a, w = int(m.group(1), 16), word_of(m.group(2))
                    image[a] = w
                    if cur is not None:
                        cur.append(w)
                    continue
                m = re.match(r'/\* [0-9A-F]+ ([0-9A-F]{8}) \*/ \.(byte|short|asciz) (.*)', s)
                if m:
                    a, kind, val = int(m.group(1), 16), m.group(2), m.group(3)
                    if kind == 'asciz':
                        pending = a
                    else:
                        n = 1 if kind == 'byte' else 2
                        put_bytes(image, a, int(val.split()[0].rstrip(','), 0).to_bytes(n, 'little'))
                    continue
                m = re.match(r'/\* ([0-9A-F]+) \*/$', s)
                if m and pending is not None:
                    put_bytes(image, pending, bytes.fromhex(m.group(1)))
                    pending = None
    return jtbls, image


def put_bytes(image, a, data):
    for i, b in enumerate(data):
        wa = (a + i) & ~3
        sh = ((a + i) & 3) * 8
        image[wa] = (image.get(wa, 0) & ~(0xFF << sh)) | (b << sh)


def module_names(inp):
    return sorted(os.listdir(os.path.join(inp, 'func')))


# ---- decoder ---------------------------------------------------------------------------------------------------

class Insn:
    __slots__ = ('op', 's', 't', 'd', 'sa', 'i', 'u', 'target', 'fs', 'ft', 'fd', 'pc', 'w')


def decode(w, pc):
    """Decode one word into an Insn; op is the base mnemonic (pseudo-ops such as b/beqz/nop are not used here)."""
    x = Insn()
    x.w, x.pc = w, pc
    opc = w >> 26
    x.s, x.t, x.d, x.sa = (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31, (w >> 6) & 31
    x.u = w & 0xFFFF
    x.i = x.u - 0x10000 if x.u & 0x8000 else x.u
    x.fs, x.ft, x.fd = x.d, x.t, x.sa
    x.target = None
    op = None
    if opc == 0:
        op = {0: 'sll', 2: 'srl', 3: 'sra', 4: 'sllv', 6: 'srlv', 7: 'srav', 8: 'jr', 9: 'jalr', 0xA: 'movz',
              0xB: 'movn', 0x10: 'mfhi', 0x12: 'mflo', 0x18: 'mult', 0x19: 'multu', 0x1A: 'div', 0x21: 'addu',
              0x23: 'subu', 0x24: 'and', 0x25: 'or', 0x26: 'xor', 0x27: 'nor', 0x2A: 'slt', 0x2B: 'sltu',
              0x38: 'dsll', 0x3C: 'dsll32', 0x3E: 'dsrl32', 0x3F: 'dsra32'}.get(w & 63)
    elif opc == 1:
        op = {0: 'bltz', 1: 'bgez', 2: 'bltzl', 3: 'bgezl'}.get(x.t)
    elif opc == 3:
        op = 'jal'
        x.target = ((pc + 4) & 0xF0000000) | ((w & 0x3FFFFFF) << 2)
    elif opc == 0x11:
        if x.s == 4:
            op = 'mtc1'
        elif x.s == 8:
            op = {0: 'bc1f', 1: 'bc1t', 2: 'bc1fl', 3: 'bc1tl'}.get(x.t)
        elif x.s == 0x10:
            op = {0: 'add.s', 1: 'sub.s', 2: 'mul.s', 3: 'div.s', 6: 'mov.s', 7: 'neg.s', 0x18: 'adda.s',
                  0x19: 'suba.s', 0x1A: 'mula.s', 0x1C: 'madd.s', 0x32: 'c.eq.s', 0x34: 'c.lt.s',
                  0x36: 'c.le.s'}.get(w & 63)
        elif x.s == 0x14 and (w & 63) == 0x20:
            op = 'cvt.s.w'
    elif opc == 0x1C:
        f = w & 63
        if f == 0x28 and x.sa == 0x18:
            op = 'paddub'
        else:
            op = {0x01: 'maddu', 0x10: 'mfhi1', 0x12: 'mflo1', 0x18: 'mult1', 0x1A: 'div1'}.get(f)
    else:
        op = {4: 'beq', 5: 'bne', 6: 'blez', 7: 'bgtz', 9: 'addiu', 0xA: 'slti', 0xB: 'sltiu', 0xC: 'andi',
              0xD: 'ori', 0xE: 'xori', 0xF: 'lui', 0x14: 'beql', 0x15: 'bnel', 0x16: 'blezl', 0x17: 'bgtzl',
              0x19: 'daddiu', 0x1E: 'lq', 0x1F: 'sq', 0x20: 'lb', 0x21: 'lh', 0x23: 'lw', 0x24: 'lbu',
              0x25: 'lhu', 0x28: 'sb', 0x29: 'sh', 0x2B: 'sw', 0x31: 'lwc1', 0x37: 'ld', 0x39: 'swc1',
              0x3F: 'sd'}.get(opc)
    if op in BRANCHES:
        x.target = (pc + 4 + (x.i << 2)) & 0xFFFFFFFF
    x.op = op
    return x


BRANCHES = {'beq', 'bne', 'blez', 'bgtz', 'bltz', 'bgez', 'beql', 'bnel', 'blezl', 'bgtzl', 'bltzl', 'bgezl',
            'bc1f', 'bc1t', 'bc1fl', 'bc1tl'}
LIKELY = {'beql', 'bnel', 'blezl', 'bgtzl', 'bltzl', 'bgezl', 'bc1fl', 'bc1tl'}
JUMPS = {'jal', 'jalr', 'jr'}
PSEUDO = {'nop': 'sll', 'b': 'beq', 'beqz': 'beq', 'bnez': 'bne', 'negu': 'subu', 'not': 'nor', 'move': 'or'}


# ---- C emission ------------------------------------------------------------------------------------------------

def R(n):
    return '0' if n == 0 else f'R({n})'


def setr(n, expr):
    return '' if n == 0 else f'R({n}) = {expr};'


def hexc(v):
    return f'0x{v & 0xFFFFFFFF:X}u'


def ea(x):
    return f'(uint32_t){R(x.s)} + {x.i}' if x.i else f'(uint32_t){R(x.s)}'


def emit_plain(x):
    """C for a non-control-flow instruction."""
    op, s, t, d, sa = x.op, x.s, x.t, x.d, x.sa
    if op == 'sll':
        return '' if d == 0 else setr(d, f'SX((uint32_t){R(t)} << {sa})')
    if op == 'srl':
        return setr(d, f'SX((uint32_t){R(t)} >> {sa})')
    if op == 'sra':
        return setr(d, f'SX((int32_t){R(t)} >> {sa})')
    if op == 'sllv':
        return setr(d, f'SX((uint32_t){R(t)} << ({R(s)} & 31))')
    if op == 'srlv':
        return setr(d, f'SX((uint32_t){R(t)} >> ({R(s)} & 31))')
    if op == 'srav':
        return setr(d, f'SX((int32_t){R(t)} >> ({R(s)} & 31))')
    if op == 'movz':
        return '' if d == 0 else f'if ({R(t)} == 0) R({d}) = {R(s)};'
    if op == 'movn':
        return '' if d == 0 else f'if ({R(t)} != 0) R({d}) = {R(s)};'
    if op == 'mfhi':
        return setr(d, 'c->hi')
    if op == 'mflo':
        return setr(d, 'c->lo')
    if op == 'mfhi1':
        return setr(d, 'c->hi1')
    if op == 'mflo1':
        return setr(d, 'c->lo1')
    if op in ('mult', 'multu', 'mult1', 'maddu'):
        fn = {'mult': 'rc_mult', 'multu': 'rc_multu', 'mult1': 'rc_mult', 'maddu': 'rc_maddu'}[op]
        p = '1' if op == 'mult1' else ''
        call = f'{fn}(&c->lo{p}, &c->hi{p}, {R(s)}, {R(t)})'
        return f'{call};' if d == 0 else setr(d, call)
    if op == 'div':
        return f'rc_div(&c->lo, &c->hi, {R(s)}, {R(t)});'
    if op == 'div1':
        return f'rc_div(&c->lo1, &c->hi1, {R(s)}, {R(t)});'
    if op == 'addu':
        return setr(d, f'SX((uint32_t){R(s)} + (uint32_t){R(t)})')
    if op == 'subu':
        return setr(d, f'SX((uint32_t){R(s)} - (uint32_t){R(t)})')
    if op == 'and':
        return setr(d, f'{R(s)} & {R(t)}')
    if op == 'or':
        return setr(d, f'{R(s)} | {R(t)}')
    if op == 'xor':
        return setr(d, f'{R(s)} ^ {R(t)}')
    if op == 'nor':
        return setr(d, f'~({R(s)} | {R(t)})')
    if op == 'slt':
        return setr(d, f'(int64_t){R(s)} < (int64_t){R(t)}')
    if op == 'sltu':
        return setr(d, f'(uint64_t){R(s)} < (uint64_t){R(t)}')
    if op == 'dsll':
        return setr(d, f'{R(t)} << {sa}' if sa else R(t))
    if op == 'dsll32':
        return setr(d, f'(uint64_t){R(t)} << {sa + 32}')
    if op == 'dsrl32':
        return setr(d, f'(uint64_t){R(t)} >> {sa + 32}')
    if op == 'dsra32':
        return setr(d, f'(uint64_t)((int64_t){R(t)} >> {sa + 32})')
    if op == 'addiu':
        return setr(t, f'SX((uint32_t){R(s)} + {hexc(x.i)})' if s else lit64(x.i))
    if op == 'daddiu':
        return setr(t, f'{R(s)} + {lit64(x.i)}' if s else lit64(x.i))
    if op == 'slti':
        return setr(t, f'(int64_t){R(s)} < {x.i}')
    if op == 'sltiu':
        return setr(t, f'(uint64_t){R(s)} < {lit64(x.i)}')
    if op == 'andi':
        return setr(t, f'{R(s)} & 0x{x.u:X}u')
    if op == 'ori':
        return setr(t, f'{R(s)} | 0x{x.u:X}u' if s else f'0x{x.u:X}u')
    if op == 'xori':
        return setr(t, f'{R(s)} ^ 0x{x.u:X}u')
    if op == 'lui':
        return setr(t, lit64(x.i << 16))
    if op in ('lb', 'lbu', 'lh', 'lhu', 'lw', 'ld'):
        acc = {'lb': 'RC_S8', 'lbu': 'RC_U8', 'lh': 'RC_S16', 'lhu': 'RC_U16', 'lw': 'RC_S32', 'ld': 'RC_U64'}[op]
        if t == 0:
            return ''
        if op in ('lb', 'lh', 'lw'):
            return setr(t, f'(uint64_t)(int64_t){acc}({ea(x)})')
        return setr(t, f'{acc}({ea(x)})')
    if op == 'lq':
        if t == 0:
            return ''
        return f'{{ uint32_t a_ = ({ea(x)}) & ~15u; R({t}) = RC_U64(a_); c->rh[{t}] = RC_U64(a_ + 8); }}'
    if op in ('sb', 'sh', 'sw', 'sd'):
        acc, ty = {'sb': ('RC_U8', 'uint8_t'), 'sh': ('RC_U16', 'uint16_t'), 'sw': ('RC_U32', 'uint32_t'),
                   'sd': ('RC_U64', 'uint64_t')}[op]
        return f'{acc}({ea(x)}) = ({ty}){R(t)};'
    if op == 'sq':
        hi = f'c->rh[{t}]' if t else '0'
        return f'{{ uint32_t a_ = ({ea(x)}) & ~15u; RC_U64(a_) = {R(t)}; RC_U64(a_ + 8) = {hi}; }}'
    if op == 'paddub':
        if d == 0:
            return ''
        if t == 0:
            return f'R({d}) = {R(s)}; c->rh[{d}] = {"c->rh[%d]" % s if s else "0"};'
        return f'rc_paddub(c, {d}, {s}, {t});'
    if op == 'lwc1':
        return f'FU({x.ft}) = RC_U32({ea(x)});'
    if op == 'swc1':
        return f'RC_U32({ea(x)}) = FU({x.ft});'
    if op == 'mtc1':
        return f'FU({x.fs}) = (uint32_t){R(t)};'
    fs, ft, fd = x.fs, x.ft, x.fd
    if op in ('add.s', 'sub.s', 'mul.s', 'div.s'):
        return f'F({fd}) = RC_FNAN(F({fs}) {"+-*/"[["add.s", "sub.s", "mul.s", "div.s"].index(op)]} F({ft}));'
    if op == 'mov.s':
        return f'FU({fd}) = FU({fs});'
    if op == 'neg.s':
        return f'FU({fd}) = FU({fs}) ^ 0x80000000u;'
    if op == 'cvt.s.w':
        return f'F({fd}) = (float)c->f[{fs}].i;'
    if op == 'adda.s':
        return f'c->acc.f = RC_FNAN(F({fs}) + F({ft}));'
    if op == 'suba.s':
        return f'c->acc.f = RC_FNAN(F({fs}) - F({ft}));'
    if op == 'mula.s':
        return f'c->acc.f = RC_FNAN(F({fs}) * F({ft}));'
    if op == 'madd.s':
        return f'{{ float m_ = RC_FNAN(F({fs}) * F({ft})); F({fd}) = RC_FNAN(c->acc.f + m_); }}'
    if op in ('c.eq.s', 'c.lt.s', 'c.le.s'):
        return f'c->fcc = F({fs}) {({"c.eq.s": "==", "c.lt.s": "<", "c.le.s": "<="})[op]} F({ft});'
    raise Unsupported(op or f'word {x.w:08X}')


def lit64(v):
    v &= 0xFFFFFFFFFFFFFFFF
    return f'0x{v:X}ull' if v > 9 else str(v)


def cond(x):
    op, s, t = x.op.rstrip('l') if x.op not in ('bc1fl', 'bc1tl') else x.op[:-1], x.s, x.t
    if op == 'beq':
        return '1' if s == t else f'{R(s)} == {R(t)}'
    if op == 'bne':
        return f'{R(s)} != {R(t)}'
    if op == 'blez':
        return f'(int64_t){R(s)} <= 0'
    if op == 'bgtz':
        return f'(int64_t){R(s)} > 0'
    if op == 'bltz':
        return f'(int64_t){R(s)} < 0'
    if op == 'bgez':
        return f'(int64_t){R(s)} >= 0'
    if op == 'bc1t':
        return 'c->fcc'
    if op == 'bc1f':
        return '!c->fcc'
    raise Unsupported(x.op)


class Unsupported(Exception):
    pass


class ModCtx:
    """What one module's translation needs to know: its functions and jump tables."""

    def __init__(self, mod, fns, jtbls):
        self.mod, self.fns, self.jtbls = mod, fns, jtbls
        self.starts = {f.addr: f for f in fns}
        self.lo = fns[0].addr if fns else MOD_BASE
        self.hi = max(f.end for f in fns) if fns else MOD_BASE
        self.ext = set()          # main-program functions called
        self.notes = []

    def cname(self, addr):
        return f'rc_{self.mod}_{addr:08X}'

    def in_module(self, a):
        return MOD_BASE <= a < MOD_LIMIT


def translate_function(ctx, fn):
    """C source of one function."""
    insns = [decode(w, fn.addr + 4 * k) for k, (w, _) in enumerate(fn.words)]
    lo, hi = fn.addr, fn.end
    labels = set()
    for x in insns:
        if x.op in BRANCHES and lo <= x.target < hi:
            labels.add(x.target)
    switch_targets = sorted({a for tbl in ctx.jtbls.values() for a in tbl if lo <= a < hi})
    labels.update(switch_targets)
    out = [f'void {ctx.cname(fn.addr)}(RcCpu *c)', '{', '    RC_ENTER();']
    k = 0
    n = len(insns)

    def lab(a):
        return f'L{a:08X}'

    def body(x):
        if x.op in BRANCHES or x.op in JUMPS:
            raise Unsupported(f'control flow in a delay slot at {x.pc:08X}')
        return emit_plain(x)

    def goto(target, pc):
        """Code that runs the delay slot (already emitted) and transfers control to target."""
        if lo <= target < hi:
            tick = 'RC_TICK(); ' if target <= pc else ''
            return f'{tick}goto {lab(target)};'
        if target in ctx.starts:              # branch into another function = tail call
            return f'{ctx.cname(target)}(c); return;'
        raise Unsupported(f'branch from {pc:08X} to {target:08X} outside any function')

    ends_open = True    # can control run off the end of what has been emitted so far?
    while k < n:
        x = insns[k]
        pc = x.pc
        if pc in labels:
            out.append(f'{lab(pc)}:;')
            ends_open = True
        tr = f'RC_TRACE(0x{pc:08X}u); ' if TRACE and (TRACE != 'calls' or x.op in ('jal', 'jalr')) else ''
        if x.op not in BRANCHES and x.op not in JUMPS:
            out.append('    ' + tr + emit_plain(x))
            ends_open = True
            k += 1
            continue
        if k + 1 >= n:
            # the split ends on a branch: its delay slot lies past the module's text (4 functions, all past
            # the module end in config/modules.tsv). Take the word after endlabel if there is one, else a nop.
            insns.append(decode(fn.after or 0, pc + 4))
            ctx.notes.append(f'{fn.name}: delay slot at {pc + 4:08X} past the split, taken as {fn.after or 0:08X}')
            n += 1
        ds = insns[k + 1]
        dcode = body(ds)
        ds_label = ds.pc in labels
        ends_open = not (x.op == 'jr' or (x.op in ('beq', 'beql') and x.s == x.t))
        if tr:
            out.append('    ' + tr)
        if x.op in BRANCHES:
            cnd = cond(x)
            if x.op in LIKELY:
                out.append(f'    if ({cnd}) {{ {dcode} {goto(x.target, pc)} }}')
            elif cnd == '1':
                out.append(f'    {dcode} {goto(x.target, pc)}')
            else:
                out.append(f'    bc = {cnd}; {dcode} if (bc) {{ {goto(x.target, pc)} }}')
        elif x.op == 'jal':
            t = x.target
            call = ctx.cname(t) + '(c);' if t in ctx.starts else None
            if call is None:
                if ctx.in_module(t):
                    raise Unsupported(f'jal from {pc:08X} into the middle of module code {t:08X}')
                ctx.ext.add(t)
                call = f'RCX({t:08X});'
            out.append(f'    R(31) = {hexc(pc + 8)}; {dcode} {call}')
        elif x.op == 'jalr':
            rd = x.d
            out.append(f'    {{ uint32_t t_ = (uint32_t){R(x.s)}; {setr(rd, hexc(pc + 8))} {dcode} '
                       f'rc_dispatch(c, t_); }}')
        elif x.op == 'jr':
            if x.s == 31:
                out.append(f'    {dcode} return;')
            else:
                cases = ' '.join(f'case {hexc(a)}: {"RC_TICK(); " if a <= pc else ""}goto {lab(a)};'
                                 for a in switch_targets)
                out.append(f'    {{ uint32_t t_ = (uint32_t){R(x.s)}; {dcode} switch (t_) {{ {cases} '
                           f'default: rc_dispatch(c, t_); return; }} }}')
        if ds_label:
            # someone branches straight into this delay slot: run it on its own, then fall through past the branch
            out.append(f'    goto {lab(ds.pc + 4)};')
            out.append(f'{lab(ds.pc)}:;')
            out.append(f'    {dcode}')
            labels.add(ds.pc + 4)
            ends_open = True
        k += 2
    falls = ends_open
    if hi in labels:
        out.append(f'{lab(hi)}:;')
        falls = True
    if falls:
        if hi in ctx.starts:
            out.append(f'    {ctx.cname(hi)}(c);')
        else:
            out.append(f'    rc_bad_jump(c, {hexc(hi - 4)}, {hexc(hi)});')
    out.append('}')
    text = '\n'.join(out)
    if 'bc = ' in text:
        text = text.replace('{\n', '{\n    int bc;\n', 1)
    return text + '\n'


PRELUDE = '''/* Generated by recomp/rcgen.py from the decomp's split of module {mod}. Do not edit. */
#include "rc.h"
#define R(n) c->r[n]
#define F(n) c->f[n].f
#define FU(n) c->f[n].u
#define SX(x) ((uint64_t)(int64_t)(int32_t)(x))
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wunused-label"
#pragma clang diagnostic ignored "-Wparentheses-equality"
#endif
'''


def translate_module(inp, mod):
    fns = load_functions(inp, mod)
    jtbls, _ = load_data(inp, mod)
    ctx = ModCtx(mod, fns, jtbls)
    bodies = [translate_function(ctx, f) for f in fns]
    head = [PRELUDE.format(mod=mod)]
    if ctx.ext:
        head.append('#ifndef RC_GENERIC_EXT')
        head += [f'void rcx_{a:08X}(RcCpu *c);' for a in sorted(ctx.ext)]
        head.append('#endif')
    head += [f'void {ctx.cname(f.addr)}(RcCpu *c);' for f in fns]
    tail = [f'const RcFunc rc_{mod}_funcs[] = {{']
    tail += [f'    {{0x{f.addr:08X}u, {ctx.cname(f.addr)}}},' for f in fns]
    tail.append('};')
    tail.append(f'const RcModule rc_mod_{mod} = {{"{mod}", rc_{mod}_funcs, {len(fns)}}};')
    return '\n'.join(head) + '\n\n' + '\n'.join(bodies) + '\n' + '\n'.join(tail) + '\n', ctx


def check(inp):
    """Decode everything; compare the base mnemonic with spimdisasm's; translate every function."""
    mism, unsup, nf, ni = [], [], 0, 0
    ext = set()
    for mod in module_names(inp):
        fns = load_functions(inp, mod)
        jtbls, _ = load_data(inp, mod)
        ctx = ModCtx(mod, fns, jtbls)
        for f in fns:
            nf += 1
            for k, (w, mn) in enumerate(f.words):
                ni += 1
                x = decode(w, f.addr + 4 * k)
                if x.op != PSEUDO.get(mn, mn):
                    mism.append((mod, f.name, hex(x.pc), mn, x.op))
            try:
                translate_function(ctx, f)
            except Unsupported as e:
                unsup.append((mod, f.name, str(e)))
        ext |= ctx.ext
    print(f'{nf} functions, {ni} instructions, {len(mism)} decoder mismatches, {len(unsup)} untranslatable, '
          f'{len(ext)} distinct main-program targets')
    for m in mism[:20]:
        print('  MISMATCH', *m)
    for u in unsup[:40]:
        print('  UNSUPPORTED', *u)
    return not mism and not unsup


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    cmd = argv[1]
    if cmd == 'check':
        return 0 if check(argv[2] if len(argv) > 2 else DEFAULT_INPUT) else 1
    if cmd == 'module':
        src, _ = translate_module(argv[3] if len(argv) > 3 else DEFAULT_INPUT, argv[2])
        sys.stdout.write(src)
        return 0
    if cmd == 'images':
        # module images for the differential tests (derived from the game: build tree only)
        outdir = argv[2]
        inp = argv[3] if len(argv) > 3 else DEFAULT_INPUT
        os.makedirs(outdir, exist_ok=True)
        for mod in module_names(inp):
            words = {}
            for f in load_functions(inp, mod):
                for k, (w, _) in enumerate(f.words):
                    words[f.addr + 4 * k] = w
            _, image = load_data(inp, mod)
            words.update(image)
            top = max(words) + 4
            buf = bytearray(top - MOD_BASE)
            for a, w in words.items():
                buf[a - MOD_BASE:a - MOD_BASE + 4] = w.to_bytes(4, 'little')
            with open(os.path.join(outdir, f'{mod}.bin'), 'wb') as fh:
                fh.write(buf)
        print(f'images -> {outdir}')
        return 0
    if cmd == 'all':
        outdir = argv[2]
        inp = argv[3] if len(argv) > 3 else DEFAULT_INPUT
        os.makedirs(outdir, exist_ok=True)
        mods = module_names(inp)
        for mod in mods:
            src, _ = translate_module(inp, mod)
            with open(os.path.join(outdir, f'mod_{mod}.c'), 'w', newline='\n') as fh:
                fh.write(src)
        with open(os.path.join(outdir, 'rc_modules.c'), 'w', newline='\n') as fh:
            fh.write('/* Generated by recomp/rcgen.py. Do not edit. */\n#include "rc.h"\n')
            fh.writelines(f'extern const RcModule rc_mod_{m};\n' for m in mods)
            fh.write('const RcModule *const rc_modules[] = {\n')
            fh.writelines(f'    &rc_mod_{m},\n' for m in mods)
            fh.write(f'}};\nconst unsigned rc_nmodules = {len(mods)};\n')
        print(f'{len(mods)} modules -> {outdir}')
        return 0
    print(__doc__)
    return 2


if __name__ == '__main__':
    sys.exit(main(sys.argv))
