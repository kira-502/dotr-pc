#!/usr/bin/env python3
"""rcaudit.py - static crash audit of the 762 translated modules (build round, 18 Oct). Reads only the module asm
copy and the port's gen/out; runs nothing. Prints one section per check; every line it prints is a lead to read.

usage: rcaudit.py <port gen/out dir> [recomp input]
"""
import collections
import os
import re
import sys

import rcgen
import rcthunks

MAIN_LO, MAIN_HI = 0x00100000, 0x01E00000
GARGS = range(4, 12)
FARGS = range(12, 20)


def gpr_writes(x):
    op = x.op
    if op in ('jal',):
        return {31}
    if op == 'jalr':
        return {x.d}
    if op in ('addiu', 'daddiu', 'slti', 'sltiu', 'andi', 'ori', 'xori', 'lui', 'lb', 'lh', 'lw', 'lbu', 'lhu',
              'ld', 'lq'):
        return {x.t}
    if op == 'mfc1':
        return {x.t}
    if op in ('sw', 'sh', 'sb', 'sd', 'sq', 'swc1', 'lwc1', 'mtc1', 'jr', 'mult1', 'div', 'div1') or op is None:
        return set()
    if op in rcgen.BRANCHES or op in ('c.eq.s', 'c.lt.s', 'c.le.s'):
        return set()
    if op in ('add.s', 'sub.s', 'mul.s', 'div.s', 'mov.s', 'neg.s', 'adda.s', 'suba.s', 'mula.s', 'madd.s',
              'cvt.s.w'):
        return set()
    if op in ('mult', 'multu', 'maddu'):
        return {x.d}
    return {x.d}


def gpr_reads(x):
    op = x.op
    if op in ('lui', 'jal') or op is None:
        return set()
    if op in ('addiu', 'daddiu', 'slti', 'sltiu', 'andi', 'ori', 'xori', 'lb', 'lh', 'lw', 'lbu', 'lhu', 'ld', 'lq',
              'lwc1', 'swc1', 'jr', 'jalr', 'blez', 'bgtz', 'bltz', 'bgez', 'blezl', 'bgtzl', 'bltzl', 'bgezl'):
        return {x.s}
    if op in ('sw', 'sh', 'sb', 'sd', 'sq', 'beq', 'bne', 'beql', 'bnel'):
        return {x.s, x.t}
    if op == 'mtc1':
        return {x.t}
    if op in ('sll', 'srl', 'sra', 'dsll', 'dsll32', 'dsrl32', 'dsra32'):
        return {x.t}
    if op in ('mfhi', 'mflo', 'mfhi1', 'mflo1') or op.startswith(('bc1', 'c.')) or op.endswith('.s') or op == 'cvt.s.w':
        return set()
    return {x.s, x.t}


def fpr_reads(x):
    op = x.op
    if op in ('add.s', 'sub.s', 'mul.s', 'div.s', 'adda.s', 'suba.s', 'mula.s', 'madd.s', 'c.eq.s', 'c.lt.s',
              'c.le.s'):
        return {x.fs, x.ft}
    if op in ('mov.s', 'neg.s', 'cvt.s.w'):
        return {x.fs}
    if op == 'swc1':
        return {x.ft}
    if op == 'mfc1':
        return {x.fs}
    return set()


def fpr_writes(x):
    op = x.op
    if op in ('add.s', 'sub.s', 'mul.s', 'div.s', 'mov.s', 'neg.s', 'cvt.s.w', 'madd.s'):
        return {x.fd}
    if op == 'lwc1':
        return {x.ft}
    if op == 'mtc1':
        return {x.fs}
    return set()


def insns_of(f):
    return [rcgen.decode(w, f.addr + 4 * k) for k, (w, _) in enumerate(f.words)]


def labels_of(f, insns, jtargets):
    lab = {x.target for x in insns if x.op in rcgen.BRANCHES and x.target is not None}
    return lab | jtargets


def window_before(insns, k, labels):
    """Indices of straight-line code before the call at k (delay slot included), back to a label/call/branch."""
    out = [k + 1] if k + 1 < len(insns) else []
    j = k - 1
    while j >= 0:
        x = insns[j]
        if x.op in ('jal', 'jalr', 'jr') or x.op in rcgen.BRANCHES:
            break
        out.append(j)
        if x.pc in labels:
            break
        j -= 1
    return out


def first_use_after(insns, k, labels, regs_g, regs_f):
    """After the call at k: which of regs read before written (straight line until a branch/call/label)."""
    rg, rf = set(), set()
    wg, wf = set(), set()
    j = k + 2
    while j < len(insns):
        x = insns[j]
        if x.pc in labels and j != k + 2:
            break
        for r in gpr_reads(x):
            if r in regs_g and r not in wg:
                rg.add(r)
        for r in fpr_reads(x):
            if r in regs_f and r not in wf:
                rf.add(r)
        if x.op in ('jal', 'jalr', 'jr') or x.op in rcgen.BRANCHES:
            # the delay slot runs too
            if j + 1 < len(insns):
                y = insns[j + 1]
                for r in gpr_reads(y):
                    if r in regs_g and r not in wg:
                        rg.add(r)
                for r in fpr_reads(y):
                    if r in regs_f and r not in wf:
                        rf.add(r)
            break
        wg |= gpr_writes(x)
        wf |= fpr_writes(x)
        j += 1
    return rg, rf


def consts(insns):
    """(pc, value) of lui/addiu|ori pairs (same register, straight line)."""
    out = []
    hi = {}
    for x in insns:
        if x.op == 'lui':
            hi[x.t] = (x.u << 16, x.pc)
            continue
        if x.op in ('addiu', 'daddiu') and x.s in hi:
            out.append((x.pc, (hi[x.s][0] + x.i) & 0xFFFFFFFF))
        elif x.op == 'ori' and x.s in hi:
            out.append((x.pc, hi[x.s][0] | x.u))
        for r in gpr_writes(x):
            hi.pop(r, None)
    return out


# ---- original EE code of a main-program function (the decomp's asm text): which argument registers it reads ------

ASM_RE = re.compile(r'\s*/\* [0-9A-F]+ ([0-9A-F]{8}) [0-9A-F]{8} \*/\s+(\S+)\s*(.*)')
GNAME = {f'$a{i}': 4 + i for i in range(4)}
GNAME.update({f'$t{i}': 8 + i for i in range(4)})
GNAME.update({'$v0': 2, '$v1': 3, '$ra': 31, '$sp': 29})
STORES = {'sb', 'sh', 'sw', 'sd', 'sq', 'swc1', 'sdc1', 'swl', 'swr', 'sdl', 'sdr', 'sqc2', 'usw', 'ush'}
NODEST = STORES | {'jr', 'jalr', 'jal', 'j', 'mtlo', 'mthi', 'mtlo1', 'mthi1', 'div', 'divu',
                   'div1', 'divu1', 'c.eq.s', 'c.lt.s', 'c.le.s', 'c.f.s', 'c.un.s', 'c.olt.s', 'c.ole.s', 'adda.s',
                   'suba.s', 'mula.s', 'madda.s', 'msuba.s', 'teq', 'tne', 'tge', 'tlt', 'mtsab', 'mtsah'}
SRC_FIRST = {'mtc1', 'ctc1', 'dmtc1', 'qmtc2', 'ctc2'}     # mtc1 $rt, $fs: reads rt, writes fs
NOREGS = {'nop', 'sync', 'sync.l', 'sync.p', 'break', 'syscall', 'ei', 'di', 'eret', 'vnop', 'vwaitq', 'vcallms'}


def regs_in(text):
    return re.findall(r'\$\w+', text)


def asm_insns(path):
    out = []
    for line in open(path):
        if line.startswith('endlabel'):
            break
        m = ASM_RE.match(line)
        if m:
            ops = [o.strip() for o in re.split(r',(?![^(]*\))', m.group(3)) if o.strip()]
            out.append((int(m.group(1), 16), m.group(2), ops))
    return out


def asm_reads_writes(op, ops):
    if op in NOREGS:
        return [], []
    if op in SRC_FIRST:
        return regs_in(ops[0]), regs_in(' '.join(ops[1:]))
    if (op in NODEST or op.startswith('b') or
            (op in ('mult', 'multu', 'mult1', 'multu1', 'madd', 'maddu', 'madd1', 'maddu1') and len(ops) == 2)):
        return regs_in(' '.join(ops)), []
    if op in ('qmfc2', 'cfc2', 'cfc1', 'mfc1', 'dmfc1', 'mfc0'):
        return regs_in(' '.join(ops[1:])), regs_in(ops[0])
    if op.startswith('v') or op == 'lqc2':
        return [r for r in regs_in(' '.join(ops)) if r[1] in 'astvz'], []
    if not ops:
        return [], []
    return regs_in(' '.join(ops[1:])), regs_in(ops[0])


def ee_arg_reads(path):
    """(highest integer argument read 0..8, float argument regs read, v0 written, f0 written) of the original code:
    must-written dataflow over the function's own branches; a call clobbers the argument registers."""
    ins = asm_insns(path)
    if not ins:
        return None
    pcs = {pc: k for k, (pc, _, _) in enumerate(ins)}
    args = set(range(4, 12)) | {f'f{i}' for i in range(12, 20)}

    def reg_id(r):
        if r in GNAME:
            return GNAME[r]
        m = re.match(r'\$f(\d+)$', r)
        return f'f{m.group(1)}' if m else None

    def succ(k):
        _, op, ops = ins[k]
        if op == 'jr':
            return [k + 1] if k + 1 < len(ins) else []   # the delay slot, then out
        if k > 0 and ins[k - 1][1] == 'jr':
            return []
        out = []
        tgt = None
        if (op.startswith('b') or op == 'j') and ops:
            m = re.search(r'\.L([0-9A-F]{8})', ops[-1]) or re.search(r'([0-9A-F]{8})', ops[-1])
            tgt = int(m.group(1), 16) if m else None
        if op.startswith('b') or op == 'j':
            # the delay slot runs first: model it as branch -> slot -> {target, next}
            nxt = k + 1
            if nxt < len(ins):
                out.append(nxt)
                if tgt in pcs:
                    extra[nxt] = extra.get(nxt, set()) | {pcs[tgt]}
                if op in ('b', 'j'):
                    nofall.add(nxt)
            return out
        if k in nofall:
            return sorted(extra.get(k, set()))
        out = sorted(extra.get(k, set()))
        if k + 1 < len(ins):
            out.append(k + 1)
        return out

    extra, nofall = {}, set()
    for k in range(len(ins)):           # first pass fills extra/nofall
        _, op, _ = ins[k]
        if op.startswith('b') or op == 'j':
            succ(k)
    written_in = [None] * len(ins)
    written_in[0] = frozenset()
    reads = set()
    work = [0]
    wv0 = wf0 = False
    while work:
        k = work.pop()
        w = set(written_in[k])
        _, op, ops = ins[k]
        rr, ww = asm_reads_writes(op, ops)
        for r in rr:
            i = reg_id(r)
            if i in args and i not in w:
                reads.add(i)
        for r in ww:
            i = reg_id(r)
            if i is not None:
                w.add(i)
            wv0 |= i == 2
            wf0 |= i == 'f0'
        if k > 0 and ins[k - 1][1] in ('jal', 'jalr'):
            w |= args                   # the call happens after its delay slot
        fw = frozenset(w)
        for s2 in succ(k):
            old = written_in[s2]
            new = fw if old is None else old & fw
            if new != old:
                written_in[s2] = new
                work.append(s2)
    gi = max((i - 3 for i in reads if isinstance(i, int)), default=0)
    fr = {int(i[1:]) for i in reads if isinstance(i, str)}
    return gi, fr, wv0, wf0


# ---- main -> module: the effect tables (D_00292FF0) against the module each entry loads ----------------------------

def elf_reader(path):
    import struct
    d = open(path, 'rb').read()
    phoff, = struct.unpack_from('<I', d, 0x1C)
    phnum, = struct.unpack_from('<H', d, 0x2C)
    segs = []
    for i in range(phnum):
        t, off, va, _, fs, _, _, _ = struct.unpack_from('<8I', d, phoff + 32 * i)
        if t == 1:
            segs.append((va, off, fs))

    def rd(a, n=4):
        for va, off, fs in segs:
            if va <= a < va + fs:
                return int.from_bytes(d[off + a - va:off + a - va + n], 'little')
        return None
    return rd


def iso_files(path):
    """name -> (byte offset, size) of every file on the disc."""
    fh = open(path, 'rb')

    def sector(n, k=1):
        fh.seek(n * 2048)
        return fh.read(2048 * k)
    pvd = sector(16)
    out = {}

    def walk(lba, size, prefix):
        data = sector(lba, (size + 2047) // 2048)
        i = 0
        while i < len(data):
            ln = data[i]
            if ln == 0:
                i = (i // 2048 + 1) * 2048
                continue
            rec = data[i:i + ln]
            elba = int.from_bytes(rec[2:6], 'little')
            esz = int.from_bytes(rec[10:14], 'little')
            flags = rec[25]
            name = rec[33:33 + rec[32]].decode('latin-1').split(';')[0]
            if name not in ('\x00', '\x01'):
                if flags & 2:
                    walk(elba, esz, prefix + name + '/')
                else:
                    out[(prefix + name).upper()] = (elba * 2048, esz)
            i += ln
    root = pvd[156:156 + 34]
    walk(int.from_bytes(root[2:6], 'little'), int.from_bytes(root[10:14], 'little'), '')
    return out


def check_effect_tables(elf, iso, inp, sec):
    rd = elf_reader(elf)
    files = iso_files(iso)
    fh = open(iso, 'rb')
    starts = {m: {f.addr for f in rcgen.load_functions(inp, m)} for m in rcgen.module_names(inp)}
    mrg = {0: 'CMODULE.MRG', 1: 'BMODULE.MRG', 2: 'MMODULE.MRG'}
    entries = collections.defaultdict(set)
    n = 0
    for kind in range(3):
        vals = rd(0x00292FF0 + 12 * kind)
        path = next((k for k in files if k.endswith(mrg[kind])), None)
        card = 0
        while True:
            entry = rd(vals + 4 * card)
            if not entry:
                break
            if kind == 0:
                file = card
            else:
                file = rd(0x002CD8A0 + 2 * card, 2) if card < 0x356 else 0x3E7
                if file == 0x29F or file >= 0x356:
                    file = 0x3E7
            if kind and file >= 0x2AB:
                card += 1
                continue                        # the loader refuses it (SzEffect_LoadModule)
            off = files[path][0] + (file << 15)
            fh.seek(off + 0x20)
            name = fh.read(16).split(b'.')[0].decode('latin-1', 'replace')
            n += 1
            if not name.strip(chr(0)):
                sec['EFFECT_ENTRY_EMPTY_SLOT (no module on the disc)'].append(f'kind {kind} card {card}')
            elif name not in starts:
                sec['EFFECT_ENTRY_UNKNOWN_MODULE'].append(f'kind {kind} card {card} file {file}: {name!r}')
            elif entry not in starts[name]:
                sec['EFFECT_ENTRY_NOT_A_FUNCTION'].append(f'kind {kind} card {card} -> {name} {entry:08X}')
            else:
                entries[name].add(entry)
            card += 1
    sec['EFFECT_ENTRIES_CHECKED'].append(str(n))
    # every module function the main program can call (effect entries, data pointers, built addresses) must read
    # integer arguments only, at most 8: the entry stubs (rcthunks.py rc_e_) pass a0..t3 and nothing else
    for m in rcgen.module_names(inp):
        fns = rcgen.load_functions(inp, m)
        st = {f.addr: f for f in fns}
        _, img = rcgen.load_data(inp, m)
        taken = {w for w in img.values() if w in st} | entries[m]
        for f in fns:
            taken |= {v for _, v in consts(insns_of(f)) if v in st}
        for a in sorted(taken):
            r = ee_arg_reads(os.path.join(inp, 'func', m, st[a].name + '.s'))
            if r and r[1]:
                sec['CALLABLE_FROM_MAIN_READS_FLOAT'].append(f'{m} {a:08X} f{sorted(r[1])}'
                                                             f'{" (effect entry)" if a in entries[m] else ""}')
            if r and r[0] > 8:
                sec['CALLABLE_FROM_MAIN_READS_9+_ARGS'].append(f'{m} {a:08X} {r[0]}')


def main(argv):
    gen_out = argv[1]
    inp = argv[2] if len(argv) > 2 else rcgen.DEFAULT_INPUT
    protos = rcthunks.parse_prototypes(open(os.path.join(gen_out, 'include', 'functions.h'), errors='replace').read())
    functab = rcthunks.load_functab(os.path.join(gen_out, 'src', 'zz_functab.c'))
    fstarts = {a: (native, name) for a, native, name in functab}
    thunk = {}
    for a, native, name in functab:
        p = protos.get(native) or protos.get(name)
        if p and a not in thunk:
            body, why = rcthunks.thunk_body(a, *p)
            if body:
                thunk[a] = (native, p)
    for a in rcthunks.HAND:
        thunk[a] = (rcthunks.HAND[a][0], None)

    sec = collections.defaultdict(list)
    calls = collections.defaultdict(list)          # main target -> [(mod, pc, gargs set, fargs set, uses)]
    taken = collections.defaultdict(set)           # mod -> module addresses whose value the code/data builds
    frames = {}
    nmods = nfns = 0
    for mod in rcgen.module_names(inp):
        nmods += 1
        fns = rcgen.load_functions(inp, mod)
        jtbls, image = rcgen.load_data(inp, mod)
        starts = {f.addr: f for f in fns}
        jt_all = {a for t in jtbls.values() for a in t}
        text_lo, text_hi = fns[0].addr, max(f.end for f in fns)
        # jump tables: every entry inside one function, and that function has a jr through a register
        for ta, tbl in jtbls.items():
            own = [next((f.addr for f in fns if f.addr <= a < f.end), None) for a in tbl]
            if None in own or len(set(own)) != 1:
                sec['JTBL_TRAILING_WORDS'].append(f'{mod} jtbl_{ta:08X}: ' + ' '.join(
                    f'{a:08X}@{o:08X}' if o else f'{a:08X}@NONE' for a, o in zip(tbl, own)))
        for f in fns:
            nfns += 1
            ins = insns_of(f)
            jt_here = {a for a in jt_all if f.addr <= a < f.end}
            labels = labels_of(f, ins, jt_here)
            # frame size
            fr = 0
            for x in ins:
                if x.op in ('addiu', 'daddiu') and x.s == 29 and x.t == 29 and x.i < 0:
                    fr = max(fr, -x.i)
            frames[(mod, f.addr)] = fr
            for k, x in enumerate(ins):
                if x.op is None:
                    sec['UNDECODED'].append(f'{mod} {x.pc:08X} {x.w:08X}')
                if x.op == 'jr' and x.s != 31 and not jt_here:
                    sec['JR_NO_TABLE'].append(f'{mod} {f.addr:08X} at {x.pc:08X} jr ${x.s}')
                if x.op == 'jal' and x.target not in starts:
                    win = window_before(ins, k, labels)
                    g = {r for j in win for r in gpr_writes(ins[j]) if r in GARGS}
                    fl = {r for j in win for r in fpr_writes(ins[j]) if r in FARGS}
                    ug, uf = first_use_after(ins, k, labels, {2, 3}, {0})
                    calls[x.target].append((mod, x.pc, g, fl, ug, uf))
                if x.op == 'jal' and x.target in starts:
                    pass
            for pc, v in consts(ins):
                if text_lo <= v < text_hi:
                    taken[mod].add(v)
                    if v not in starts:
                        sec['CODE_CONST_NOT_START'].append(f'{mod} {pc:08X} builds {v:08X} (inside {f.addr:08X}?)')
                elif v in fstarts and v not in thunk:
                    sec['CODE_CONST_MAIN_NO_THUNK'].append(f'{mod} {pc:08X} builds {v:08X} {fstarts[v][1]}')
        for a, w in sorted(image.items()):
            if text_lo <= w < text_hi:
                if w in starts:
                    taken[mod].add(w)
                else:
                    own = next((f for f in fns if f.addr <= w < f.end), None)
                    jr = own and any(x.op == 'jr' and x.s != 31 for x in insns_of(own))
                    if not jr:
                        sec['DATA_PTR_NOT_START'].append(f'{mod} data {a:08X} = {w:08X} (in {own.addr if own else 0:08X})')
            elif w in fstarts and w not in thunk:
                sec['DATA_PTR_MAIN_NO_THUNK'].append(f'{mod} data {a:08X} = {w:08X} {fstarts[w][1]}')
        # address-taken functions: float argument reads (first touch, straight line) and float returns
        for a in sorted(taken[mod]):
            f = starts.get(a)
            if not f:
                continue
            if rcthunks.reads_float_arg(f):
                sec['TAKEN_READS_FLOAT'].append(f'{mod} {a:08X}')
            ins = insns_of(f)
            if any(0 in fpr_writes(x) for x in ins) and not any(2 in gpr_writes(x) for x in ins):
                sec['TAKEN_FLOAT_RETURN?'].append(f'{mod} {a:08X} writes f0, never v0')
        # module entry (+0x80) too
        f0 = starts.get(rcgen.MOD_BASE + 0x80)
        if f0 and rcthunks.reads_float_arg(f0):
            sec['ENTRY_READS_FLOAT'].append(f'{mod} {f0.addr:08X}')

    # module -> main calls vs prototypes (and vs the original code's own argument reads)
    asm_path = {}
    for dp, _, fs in os.walk(os.path.join(gen_out, '..', 'decomp', 'asm')):
        for fn in fs:
            if fn.endswith('.s'):
                asm_path.setdefault(fn[:-2], os.path.join(dp, fn))
    for t, sites in sorted(calls.items()):
        if t not in thunk:
            sec['CALL_NO_THUNK'].append(f'{t:08X} {fstarts.get(t, ("?",))[0]} from {len(sites)} sites')
            continue
        native, p = thunk[t]
        if p is None:
            continue
        ret, params = p
        kinds = [rcthunks.classify(q) for q in params if q not in ('void', '...')]
        ni = sum(1 for q in kinds if q[0] != 'float')
        nf = sum(1 for q in kinds if q[0] == 'float')
        variadic = '...' in params
        rk = rcthunks.classify(ret)
        maxg = max((max(s[2]) - 3 if s[2] else 0) for s in sites)
        maxf = max((max(s[3]) - 11 if s[3] else 0) for s in sites)
        usev0 = sum(1 for s in sites if 2 in s[4])
        usev1 = sum(1 for s in sites if 3 in s[4])
        usef0 = sum(1 for s in sites if 0 in s[5])
        tag = f'{t:08X} {native}({", ".join(params)}) -> {ret} [{len(sites)} sites]'
        ap = asm_path.get(fstarts[t][1]) or asm_path.get(native)
        er = ee_arg_reads(ap) if ap else None
        if er is None:
            sec['NO_EE_ASM'].append(tag)
        else:
            gi, fr, wv0, wf0 = er
            nfr = max(fr) - 11 if fr else 0
            if not variadic and gi > ni:
                sec['EE_READS_MORE_GPR_ARGS'].append(f'{tag}: original reads {gi} integer args, proto {ni} '
                                                     f'(sites set up to {maxg})')
            if nfr > nf:
                sec['EE_READS_MORE_FLOAT_ARGS'].append(f'{tag}: original reads f12..f{11 + nfr}, proto {nf} '
                                                       f'(sites set up to {maxf})')
            if gi < ni or nfr < nf:
                sec['PROTO_MORE_ARGS_THAN_EE_READS'].append(f'{tag}: original reads {gi} int / {nfr} float')
        if nf and maxf == 0:
            sec['PROTO_FLOAT_NOT_SET'].append(f'{tag}: no site sets f12.. in its block')
        if usef0 and (rk == 'void' or rk[0] != 'float'):
            sec['F0_USED_NOT_FLOAT_RET'].append(f'{tag}: {usef0} sites read f0 after')
        if usev0 and (rk == 'void' or rk[0] == 'float'):
            sec['V0_USED_BUT_VOID_OR_FLOAT'].append(f'{tag}: {usev0} sites read v0 after')
        if usev1 and (rk == 'void' or rk[0] not in ('wide', 'double')):
            sec['V1_USED'].append(f'{tag}: {usev1} sites read v1 after')
        if variadic or any(q[0] in ('wide', 'double') for q in kinds) or (rk != 'void' and rk[0] in ('wide', 'double')):
            sec['WIDE_OR_VARIADIC'].append(tag)

    # stack: deepest static chain inside each module (direct calls only)
    for mod in rcgen.module_names(inp):
        fns = rcgen.load_functions(inp, mod)
        starts = {f.addr: f for f in fns}
        callees = {f.addr: {x.target for x in insns_of(f) if x.op == 'jal' and x.target in starts} |
                   {x.target for x in insns_of(f) if x.op in rcgen.BRANCHES and x.target in starts and x.target != f.addr
                    and not (f.addr <= x.target < f.end)} for f in fns}
        memo, onstack = {}, set()

        def depth(a):
            if a in memo:
                return memo[a]
            if a in onstack:
                sec['RECURSION'].append(f'{mod} {a:08X}')
                return 0
            onstack.add(a)
            d = frames[(mod, a)] + max((depth(b) for b in callees[a]), default=0)
            onstack.discard(a)
            memo[a] = d
            return d
        worst = max((depth(f.addr), f.addr) for f in fns)
        sec['STACK_DEEPEST'].append((worst[0], f'{mod} {worst[1]:08X} {worst[0]:#x} bytes'))
    sec['STACK_DEEPEST'] = [s for _, s in sorted(sec['STACK_DEEPEST'], reverse=True)[:5]]

    if len(argv) > 4:
        check_effect_tables(argv[3], argv[4], inp, sec)
    print(f'{nmods} modules, {nfns} functions, {len(calls)} main targets')
    for k in sorted(sec):
        v = sec[k]
        print(f'== {k}: {len(v)}')
        for line in v[:60]:
            print('  ', line)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
