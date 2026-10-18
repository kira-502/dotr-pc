"""Oracle hooks (port generator; lane R's differential, recomp/rc_port.h rc_oracle_hook).

Under #ifdef DOTR_ORACLE every native main-program function with a PS2 address starts with
    { u64 a_[] = {integer/pointer args as EE registers}; f32 f_[] = {float args}; rc_oracle_hook(addr, ...); }
so lane R can run the original EE code on its interpreter from the same state and compare. Normal builds don't
define DOTR_ORACLE: the hook is not compiled.
"""
import re

FLOAT = re.compile(r'^(const\s+)?(f32|float)$')
DOUBLE = re.compile(r'^(const\s+)?(f64|double)$')
WIDE = re.compile(r'\b(s64|u64|long\s+long)\b')


def arg_exprs(params, split_top):
    """params text -> (int exprs, float exprs) or None when a parameter can't be passed as an EE register."""
    ps = params.strip()
    if ps in ('', 'void'): return [], []
    if '...' in ps: return None
    ints, flts = [], []
    for d in split_top(ps):
        d = ' '.join(d.split())
        fm = re.search(r'\(\s*\*\s*(\w+)\s*\)', d)
        if fm:                                              # function pointer
            ints.append('(u64)(u32)%s' % fm.group(1)); continue
        m = re.match(r'^(.*?)\b(\w+)\s*((?:\[[^\]]*\])*)$', d)
        if not m: return None
        ty, name, arr = m.group(1).strip(), m.group(2), m.group(3)
        if not ty: return None
        pt = re.match(r'^pt_r(\d+)$', name)
        while pt and len(ints) < int(pt.group(1)):          # a pass-through register lands in its own slot: pad gaps
            ints.append('0')
        if '*' in ty or arr:
            ints.append('(u64)(u32)%s' % name)
        elif FLOAT.match(ty):
            flts.append(name)
        elif DOUBLE.match(ty):
            ints.append('({ union { double d; u64 u; } u_; u_.d = %s; u_.u; })' % name)
        elif WIDE.search(ty):
            ints.append('(u64)%s' % name)
        elif re.search(r'\b(struct|union)\b', ty) and '*' not in ty:
            return None                                     # passed by value: no EE register picture
        else:
            ints.append('(u64)(s64)(s32)%s' % name)
    return ints, flts


def hook(addr, ints, flts):
    a = ', '.join(ints) if ints else '0'
    f = ', '.join(flts) if flts else '0'
    return ('\n#ifdef DOTR_ORACLE\n    { u64 a_[] = { %s }; f32 f_[] = { %s }; rc_oracle_hook(0x%08X, %d, a_, %d, f_); }\n#endif\n'
            % (a, f, addr, len(ints), len(flts)))


def insert(t, functions, addr_of, params_of, split_top):
    """functions: [(name, header, body_start, body_end, static)]. Returns (text, hooks)."""
    edits = []
    for name, h, b0, b1, st in functions:
        a = addr_of.get(name)
        if a is None: continue
        r = arg_exprs(params_of(h), split_top)
        if r is None: continue
        edits.append((b0 + 1, hook(a, r[0], r[1])))
    for pos, s in sorted(edits, reverse=True):
        t = t[:pos] + s + t[pos:]
    return t, len(edits)
