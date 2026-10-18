#!/usr/bin/env python3
"""rcthunks.py - the R4 thunk tables between translated modules and the native main program (rc_port.h).

Writes into <outdir>:
  rc_exits.c    rcx_<addr>(RcCpu *c) for every main-program function in ps2_functab that has a prototype in
                gen/out/include/functions.h (+ a few hand-written ones below), the sorted table rc_main_funcs[] used
                by rc_dispatch, and an rc_fatal stub for every module call target that has no thunk.
  rc_entries.c  rc_e_<addr>: one cdecl entry stub per address that starts a function in any module, and the sorted
                table rc_entry_stubs[] that rc_overlay_switch writes jumps to.
  rc_thunks.txt the report: targets without a thunk, prototypes that could not be mapped, functions too short for
                a jump, entries that read float arguments.

EE calling convention (Metrowerks and ee-gcc as used here): integer and pointer arguments in a0..a3, t0..t3
(r4..r11) in order; float arguments in f12..f19 in their own order; 64-bit integers and (soft) doubles take one GPR
each; results in v0 (sign-extended to 64 bits when 32-bit or narrower) or f0 for floats; doubles in v0 as bits.

usage: rcthunks.py <port gen/out dir> <outdir> [recomp input]
"""
import os
import re
import sys

import rcgen

INT_TYPES = {
    's8': 'int8_t', 'u8': 'uint8_t', 's16': 'int16_t', 'u16': 'uint16_t', 's32': 'int32_t', 'u32': 'uint32_t',
    'int': 'int32_t', 'unsigned int': 'uint32_t', 'char': 'int8_t', 'unsigned char': 'uint8_t',
    'short': 'int16_t', 'unsigned short': 'uint16_t', 'long': 'int64_t', 'unsigned long': 'uint64_t',
    'signed char': 'int8_t', 'unsigned': 'uint32_t', 'signed': 'int32_t', 'bool': 'int32_t', 'BOOL': 'int32_t',
    'size_t': 'uint32_t', 'u_int': 'uint32_t', 'u_char': 'uint8_t', 'u_short': 'uint16_t', 'u_long': 'uint64_t',
    'int8_t': 'int8_t', 'uint8_t': 'uint8_t', 'int16_t': 'int16_t', 'uint16_t': 'uint16_t',
    'int32_t': 'int32_t', 'uint32_t': 'uint32_t', 'int64_t': 'int64_t', 'uint64_t': 'uint64_t',
}
WIDE = {'s64': 'int64_t', 'u64': 'uint64_t', 'long long': 'int64_t', 'unsigned long long': 'uint64_t',
        'int64_t': 'int64_t', 'uint64_t': 'uint64_t', 'long': 'int64_t', 'unsigned long': 'uint64_t',
        'u_long': 'uint64_t'}
FLOAT_TYPES = {'f32': 'float', 'float': 'float'}
DOUBLE_TYPES = {'f64': 'double', 'double': 'double'}

# Main-program functions the module code calls that are not plain C in the port (assembly helpers). Each body
# works on RcCpu directly. Checked against the module call sites (b053/b056: doubles in a0/a1 as 64-bit GPRs,
# result tested with beqz; _f_ulltof: u64 in a0, float in f0).
HAND = {
    0x0017B340: ('_f_ulltof', 'c->f[0].f = (float)(uint64_t)c->r[4];'),
    0x0017B3D0: ('_dpflt', '{ double a, b; memcpy(&a, &c->r[4], 8); memcpy(&b, &c->r[5], 8); c->r[2] = a < b; }'),
    0x0017B3F0: ('_dpfgt', '{ double a, b; memcpy(&a, &c->r[4], 8); memcpy(&b, &c->r[5], 8); c->r[2] = a > b; }'),
    # Declared void in the port, but on the EE the value of their last call stays in v0/f0 and the module code uses
    # it (rcaudit.py V0_USED_BUT_VOID_OR_FLOAT / F0_USED_NOT_FLOAT_RET). Each body is the original's, keeping that value.
    0x0017B380: ('_d_utod', '{ double d_ = (double)(uint32_t)c->r[4]; memcpy(&c->r[2], &d_, 8); }'),
    0x001F5850: ('SzEffect_GetRadius', 'c->f[0].f = ((float (*)(int32_t))0x001F5860u)(RC_S32(0x0031BE98u));'),
    0x00100B00: ('GRotTransPers_3', '{ struct { float v[4][4]; } __attribute__((aligned(16))) s_; '
                 'memcpy(s_.v[0], RC_ADDR(c->r[4]), 16); memcpy(s_.v[1], RC_ADDR(c->r[5]), 16); '
                 'memcpy(s_.v[2], RC_ADDR(c->r[6]), 16); '
                 'c->r[2] = (uint64_t)(int64_t)((int32_t (*)(void *, float *, int32_t, int32_t, int32_t))0x002058D0u)'
                 '(s_.v, (float *)(uintptr_t)0x0030FF00u, (int32_t)c->r[7], (int32_t)c->r[8], (int32_t)c->r[9]); }'),
    0x00100BA0: ('GRotTransPers_4', '{ struct { float v[5][4]; } __attribute__((aligned(16))) s_; '
                 'memcpy(s_.v[0], RC_ADDR(c->r[4]), 16); memcpy(s_.v[1], RC_ADDR(c->r[5]), 16); '
                 'memcpy(s_.v[2], RC_ADDR(c->r[6]), 16); memcpy(s_.v[3], RC_ADDR(c->r[7]), 16); '
                 'c->r[2] = (uint64_t)(int64_t)((int32_t (*)(void *, float *, int32_t, int32_t, int32_t))0x002058D0u)'
                 '(s_.v, (float *)(uintptr_t)0x0030FF00u, (int32_t)c->r[8], (int32_t)c->r[9], (int32_t)c->r[10]); }'),
    0x00103760: ('LkSetAngleXY_m90', '{ const float *v_ = (const float *)RC_ADDR(c->r[4]); '
                 'float a_ = ((float (*)(float, float))0x001A6C18u)(v_[1], v_[0]); '
                 'c->r[2] = (uint64_t)((int64_t (*)(float))0x001A22C0u)(90.0f + (360.0f / (6.2831855f / a_))); }'),
}


def norm_type(t):
    t = re.sub(r'\b(const|volatile|register|struct|union|enum|signed(?= +(char|short|int|long)))\b', ' ', t)
    t = re.sub(r'MW_ALIGN\([^)]*\)', ' ', t)
    return ' '.join(t.split())


def classify(ctype):
    """'void', ('int', c), ('wide', c), ('float',), ('double',), ('ptr',) or None if unknown."""
    if '(' in ctype or '*' in ctype or '[' in ctype:
        return ('ptr',)
    t = norm_type(ctype)
    if t == 'void':
        return 'void'
    if t in WIDE:
        return ('wide', WIDE[t])
    if t in INT_TYPES:
        return ('int', INT_TYPES[t])
    if t in FLOAT_TYPES:
        return ('float',)
    if t in DOUBLE_TYPES:
        return ('double',)
    return None


def split_params(s):
    out, depth, cur = [], 0, ''
    for ch in s:
        if ch == ',' and depth == 0:
            out.append(cur)
            cur = ''
            continue
        depth += ch == '('
        depth -= ch == ')'
        cur += ch
    if cur.strip():
        out.append(cur)
    return [p.strip() for p in out]


def param_type(p):
    p = re.sub(r'MW_ALIGN\([^)]*\)', '', p).strip()
    if p in ('void', '...'):
        return p
    if '(' in p:
        return p                       # function pointer
    m = re.match(r'(.*?)(\w+)\s*(\[[^\]]*\])?$', p)
    if m and m.group(1).strip():
        return m.group(1).strip() + ('*' if m.group(3) else '')
    return p                           # unnamed parameter: the whole thing is the type


def parse_prototypes(header_text):
    protos = {}
    text = re.sub(r'/\*.*?\*/', ' ', header_text, flags=re.S)
    text = re.sub(r'//[^\n]*', ' ', text)
    for m in re.finditer(r'^\s*([A-Za-z_][\w \*]*?)\s*\b(\w+)\s*\(([^;{]*)\)\s*;', text, re.M):
        ret, name, params = m.group(1), m.group(2), m.group(3)
        if ret.startswith(('typedef', 'return', '#')) or name in ('if', 'while', 'for', 'switch'):
            continue
        protos[name] = (ret.strip(), [param_type(p) for p in split_params(params)])
    return protos


def thunk_body(addr, ret, params, callee=None):
    """C body of rcx_<addr>, or None with a reason. callee: call this native function by name instead of the PS2
    address (whose jump reaches an ee_ twin with narrower parameters)."""
    gi, fi = 4, 12
    ctypes, args = [], []
    variadic = False
    for p in params:
        if p == 'void':
            continue
        if p == '...':
            variadic = True
            continue
        k = classify(p)
        if k is None:
            return None, f'parameter type {p!r}'
        if k == 'void':
            return None, 'void parameter'
        if k[0] == 'float':
            if fi > 19:
                return None, 'more than 8 float arguments'
            ctypes.append('float')
            args.append(f'c->f[{fi}].f')
            fi += 1
            continue
        # the 9th integer argument on: 8-byte slots at the caller's $sp + 0, + 8, ... (checked in the original code:
        # lSzDuel_CalcMapMovable reads its 9th and 10th at 0xF0/0xF8($sp) after a 0xF0 frame)
        reg = f'c->r[{gi}]' if gi <= 11 else f'RC_U64((uint32_t)c->r[29] + {8 * (gi - 12)})'
        if k[0] == 'ptr':
            ctypes.append('void *')
            args.append(f'(void *)(uintptr_t)(uint32_t){reg}')
        elif k[0] == 'int':
            ctypes.append(k[1])
            args.append(f'({k[1]}){reg}')
        elif k[0] == 'wide':
            ctypes.append(k[1])
            args.append(f'({k[1]}){reg}')
        elif k[0] == 'double':
            ctypes.append('double')
            args.append(f'rc_dbl({reg})')
        gi += 1
    if variadic:
        # the module passes the rest in the remaining GPRs; hand them on as 32-bit values
        while gi <= 11:
            ctypes.append('uint32_t')
            args.append(f'(uint32_t)c->r[{gi}]')
            gi += 1
    rk = classify(ret)
    if rk is None:
        return None, f'return type {ret!r}'
    if rk == 'void':
        rtype = 'void'
    else:
        rtype = {'ptr': 'void *', 'float': 'float', 'double': 'double'}.get(rk[0]) or rk[1]
    fn = f'&{callee}' if callee else f'0x{addr:08X}u'
    fp = f'(({rtype} (*)({", ".join(ctypes) or "void"})){fn})({", ".join(args)})'
    if rk == 'void':
        return f'{fp};', None
    if rk[0] == 'float':
        return f'c->f[0].f = {fp};', None
    if rk[0] == 'double':
        return f'{{ double r_ = {fp}; memcpy(&c->r[2], &r_, 8); }}', None
    if rk[0] == 'ptr':
        return f'c->r[2] = (uint64_t)(int64_t)(int32_t)(uint32_t)(uintptr_t){fp};', None
    if rk[0] == 'wide':
        return f'c->r[2] = (uint64_t){fp};', None
    return f'c->r[2] = (uint64_t)(int64_t)(int32_t)({rk[1]}){fp};', None


def load_functab(path):
    tab = []
    for m in re.finditer(r'\{\s*0x([0-9A-Fa-f]{8}),\s*\(void \*\)(\w+),\s*"(\w+)"\s*\}', open(path).read()):
        tab.append((int(m.group(1), 16), m.group(2), m.group(3)))
    return tab


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 2
    gen_out, outdir = argv[1], argv[2]
    inp = argv[3] if len(argv) > 3 else rcgen.DEFAULT_INPUT
    os.makedirs(outdir, exist_ok=True)
    protos = parse_prototypes(open(os.path.join(gen_out, 'include', 'functions.h'), errors='replace').read())
    functab = load_functab(os.path.join(gen_out, 'src', 'zz_functab.c'))
    # C-library names the port renames in its objects (gen/porter.py renames.txt, decorated: _name _ps2_name)
    renamed = {}
    rn = os.path.join(gen_out, 'renames.txt')
    if os.path.exists(rn):
        for line in open(rn).read().splitlines():
            f = line.split()
            if len(f) == 2:
                renamed[f[0][1:]] = f[1][1:]

    # what the modules need: call targets, function starts, entries that read floats
    targets, starts, short, float_entries = set(), set(), [], []
    for mod in rcgen.module_names(inp):
        fns = rcgen.load_functions(inp, mod)
        ctx = rcgen.ModCtx(mod, fns, {})
        for i, f in enumerate(fns):
            starts.add(f.addr)
            room = (fns[i + 1].addr if i + 1 < len(fns) else f.end) - f.addr
            if room < 5:
                short.append(f'{mod} {f.addr:08X} ({room} bytes)')
            for k, (w, _) in enumerate(f.words):
                x = rcgen.decode(w, f.addr + 4 * k)
                if x.op == 'jal' and x.target not in ctx.starts:
                    targets.add(x.target)
        f0 = ctx.starts.get(rcgen.MOD_BASE + 0x80)
        if f0 and reads_float_arg(f0):
            float_entries.append(f'{mod} {f0.addr:08X}')

    report = []
    exits, table = [], []
    wide_twins = []
    have = set()
    nargs = {}                      # addr -> integer args (low 4 bits) | float args << 4 (the oracle compares only those)
    rets = {}                       # addr -> 0 void, 1 int/pointer (v0), 2 float (f0), 3 64-bit/double (v0, 64 bits)
    for addr, native, name in sorted(functab):
        if addr in HAND or addr in have:
            continue
        proto = protos.get(native) or protos.get(name)
        callee = None
        if native.startswith('ee_') and name in protos and any(
                (classify(q) or ('?',))[0] in ('wide', 'double') for q in protos[name][1]):
            # the EE passes a full 64-bit register; the ee_ twin at the PS2 address takes 32 bits of it
            proto, callee = protos[name], renamed.get(name, name)
            wide_twins.append(callee)
        if not proto:
            if addr in targets:
                report.append(f'NO PROTOTYPE {addr:08X} {native}')
            continue
        body, why = thunk_body(addr, *proto, callee=callee)
        if body is None:
            if addr in targets:
                report.append(f'UNMAPPED {addr:08X} {native}: {why}')
            continue
        exits.append(f'void rcx_{addr:08X}(RcCpu *c) {{ RR_PRE(0x{addr:08X}u) {body} RR_POST(0x{addr:08X}u) }}  /* {native} */')
        table.append(addr)
        have.add(addr)
        rk = classify(proto[0])
        rets[addr] = 0 if rk == 'void' else {'float': 2, 'double': 3, 'wide': 3}.get(rk[0], 1)
        kinds = [classify(q) for q in proto[1] if q not in ('void', '...')]
        nargs[addr] = (min(sum(1 for q in kinds if q and q != 'void' and q[0] != 'float'), 15) |
                       min(sum(1 for q in kinds if q and q != 'void' and q[0] == 'float'), 15) << 4)
    for addr, (name, body) in sorted(HAND.items()):
        exits.append(f'void rcx_{addr:08X}(RcCpu *c) {{ RR_PRE(0x{addr:08X}u) {body} RR_POST(0x{addr:08X}u) }}  /* {name}, hand-written */')
        table.append(addr)
        have.add(addr)
    for addr in sorted(targets - have):
        exits.append(f'void rcx_{addr:08X}(RcCpu *c) {{ RR_PRE(0x{addr:08X}u) rc_interp_main(c, 0x{addr:08X}u); RR_POST(0x{addr:08X}u) }}  '
                     f'/* inside a main-program function: its original code, interpreted */')
        report.append(f'INTERPRETED (no native entry: the original code runs on the interpreter) {addr:08X}')
    table.sort()
    with open(os.path.join(outdir, 'rc_exits.c'), 'w', newline='\n') as fh:
        fh.write('/* Generated by recomp/rcthunks.py from the port\'s functions.h. Do not edit. */\n'
                 '#include <string.h>\n#include "rc_port.h"\n'
                 'static double rc_dbl(uint64_t b) { double d; memcpy(&d, &b, 8); return d; }\n'
                 'void rc_interp_main(RcCpu *c, uint32_t addr);\n'
                 '/* module oracle record/replay (rc_oracle_mod.h): off unless a check is running */\n'
                 'extern int rc_rr_on;\n'
                 'int rc_rr_pre(RcCpu *c, uint32_t addr);\n'
                 'void rc_rr_post(RcCpu *c, uint32_t addr);\n'
                 '#define RR_PRE(a) if (rc_rr_on && rc_rr_pre(c, a)) return;\n'
                 '#define RR_POST(a) if (rc_rr_on) rc_rr_post(c, a);\n'
                 '#if defined(__clang__)\n#pragma clang diagnostic ignored "-Wunused-function"\n#endif\n\n')
        fh.writelines(f'void {n}(void);   /* real prototype in the port; called through a cast */\n' for n in wide_twins)
        fh.write('\n'.join(exits) + '\n\n')
        fh.write('const RcFunc rc_main_funcs[] = {\n')
        fh.writelines(f'    {{0x{a:08X}u, rcx_{a:08X}}},\n' for a in table)
        fh.write(f'}};\nconst unsigned rc_nmain_funcs = {len(table)};\n')
        fh.write('/* return kind of each, same order: 0 void, 1 v0 (32-bit), 2 f0, 3 v0 (64-bit) */\n')
        fh.write('const unsigned char rc_main_rets[] = {' + ','.join(str(rets.get(a, 1)) for a in table) + '};\n')
        fh.write('/* declared arguments of each, same order: integers (low 4 bits) | floats << 4 */\n')
        fh.write('const unsigned char rc_main_args[] = {' + ','.join(str(nargs.get(a, 0x48)) for a in table) + '};\n')

    with open(os.path.join(outdir, 'rc_entries.c'), 'w', newline='\n') as fh:
        fh.write('/* Generated by recomp/rcthunks.py. Do not edit. */\n#include "rc_port.h"\n'
                 'uint64_t rc_enter(uint32_t addr, const uint32_t *args);\n'
                 'typedef struct RcEntry { uint32_t addr; void *stub; } RcEntry;\n'
                 'typedef struct RcDiscModule { const char *name, *mrg; uint32_t offset; } RcDiscModule;\n'
                 '#define E(a) static uint64_t __cdecl rc_e_##a(uint32_t a0, uint32_t a1, uint32_t a2, '
                 'uint32_t a3, uint32_t a4, uint32_t a5, uint32_t a6, uint32_t a7) '
                 '{ uint32_t v[8] = {a0, a1, a2, a3, a4, a5, a6, a7}; return rc_enter(0x##a##u, v); }\n')
        for a in sorted(starts):
            fh.write(f'E({a:08X})\n')
        fh.write('const RcEntry rc_entry_stubs[] = {\n')
        fh.writelines(f'    {{0x{a:08X}u, (void *)rc_e_{a:08X}}},\n' for a in sorted(starts))
        fh.write(f'}};\nconst unsigned rc_nentry_stubs = {len(starts)};\n')
        # where each module sits on the disc (for rc_check.c), from the decomp's config/modules.tsv
        tsv = os.path.join(gen_out, '..', 'decomp', 'config', 'modules.tsv')
        rows = []
        if os.path.exists(tsv):
            for line in open(tsv).read().splitlines()[1:]:
                f = line.split('\t')
                if len(f) > 2:
                    rows.append((f[0], f[1], int(f[2], 16)))
        fh.write('const RcDiscModule rc_disc_modules[] = {\n')
        fh.writelines(f'    {{"{n}", "{m}", 0x{o:X}u}},\n' for n, m, o in rows)
        fh.write(f'    {{0, 0, 0}}\n}};\nconst unsigned rc_ndisc_modules = {len(rows)};\n')
        # main-program function extents (for rc_oracle.c), from the decomp's symbol list (nm -S)
        funcs = os.path.join(gen_out, '..', 'decomp', 'main.funcs')
        ext = {}
        if os.path.exists(funcs):
            for line in open(funcs).read().splitlines():
                f = line.split()
                if len(f) == 4 and f[2] in ('T', 't'):
                    a, sz = int(f[0], 16), int(f[1], 16)
                    if sz and (a not in ext or not f[3].startswith('func_')):
                        ext[a] = (sz, f[3])
        # functions the port replaces or stubs by design (lane P's generator): the oracle's deep mode calls them natively
        nat = os.path.join(gen_out, 'native_only.txt')
        only = []
        if os.path.exists(nat):
            for line in open(nat).read().splitlines():
                f = line.split()
                if len(f) >= 2 and f[1].startswith('0x'):
                    only.append(int(f[1], 16))
        # EE twins (ee_*): the PS2 address reaches a wrapper with a narrower parameter than the native function the
        # game C calls, so the oracle has no exact native counterpart to run: skip them too
        only += [a for a, native, _ in functab if native.startswith('ee_')]
        # runtime-owned state the oracle cannot snapshot (the heap break)
        only += [a for a, _, name in functab if name in ('sbrk', '_sbrk_r')]
        fh.write('const uint32_t rc_native_only[] = {' + ','.join(f'0x{a:08X}u' for a in sorted(set(only))) +
                 ('' if only else '0') + '};\n')
        fh.write(f'const unsigned rc_nnative_only = {len(set(only))};\n')
        fh.write('typedef struct RcMainFunc { uint32_t addr, size; const char *name; } RcMainFunc;\n')
        fh.write('const RcMainFunc rc_main_extents[] = {\n')
        fh.writelines(f'    {{0x{a:08X}u, 0x{sz:X}u, "{n}"}},\n' for a, (sz, n) in sorted(ext.items()))
        fh.write(f'}};\nconst unsigned rc_nmain_extents = {len(ext)};\n')

    report.append(f'{len(table)} main-program thunks; {len(targets)} module call targets, '
                  f'{len(targets - have)} without a thunk; {len(starts)} entry stubs')
    report += [f'TOO SHORT FOR A JUMP {s}' for s in short]
    report += [f'ENTRY READS A FLOAT ARGUMENT (stub passes integers only) {s}' for s in float_entries]
    with open(os.path.join(outdir, 'rc_thunks.txt'), 'w', newline='\n') as fh:
        fh.write('\n'.join(report) + '\n')
    print(report[-1 - len(short) - len(float_entries)] if (short or float_entries) else report[-1])
    return 0


def reads_float_arg(fn):
    """Does the function read one of f12..f19 before writing it (straight-line, first touch)?"""
    seen = set()
    for k, (w, _) in enumerate(fn.words):
        x = rcgen.decode(w, fn.addr + 4 * k)
        op = x.op
        reads = set()
        if op in ('add.s', 'sub.s', 'mul.s', 'div.s', 'adda.s', 'suba.s', 'mula.s', 'madd.s',
                  'c.eq.s', 'c.lt.s', 'c.le.s'):
            reads = {x.fs, x.ft}
        elif op in ('mov.s', 'neg.s', 'cvt.s.w'):
            reads = {x.fs}
        elif op == 'swc1':
            reads = {x.ft}
        if any(12 <= r <= 19 and r not in seen for r in reads):
            return True
        seen |= reads
        if op in ('add.s', 'sub.s', 'mul.s', 'div.s', 'mov.s', 'neg.s', 'cvt.s.w', 'madd.s'):
            seen.add(x.fd)
        elif op == 'lwc1':
            seen.add(x.ft)
        elif op == 'mtc1':
            seen.add(x.fs)
        elif op in ('jal', 'jalr'):
            seen |= set(range(20))
    return False


if __name__ == '__main__':
    sys.exit(main(sys.argv))
