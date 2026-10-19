"""Argument registers passed straight through (port generator).

A matching function can leave an argument register untouched and call another function that reads it, e.g.
newlib's bcopy(src, dst) { memmove(dst, src); } relies on len staying in $a2. On the EE that works; on x86
the argument is lost. From the function's matched assembly: before its first call (on any path), which argument registers
beyond the C definition's own parameters does the callee need and nobody wrote? Those become extra
parameters of the definition, and the call inside passes them on (remap.py fills them in).
"""
import os, re

INT_ARGS = ['$a0', '$a1', '$a2', '$a3', '$t0', '$t1', '$t2', '$t3']
LINE = re.compile(r'/\*\s*[0-9A-F]+\s+[0-9A-F]{8}\s+[0-9A-F]{8}\s*\*/\s+(\S+)\s*(.*)$')
NO_DEST = re.compile(r'^(s[bhwdq]|sw[lr]|sd[lr]|swc1|sdc1|sqc2|b\w*|j|jr|jal|nop|sync.*|syscall|break|mt(hi|lo)\w*|'
                     r'mtc1|ctc1|c\.\w+|teq\w*|cache|pref)$')


_ASM = {}


def _asm_path(asm_root, name):
    """a function's matched asm: asm/matchings/<file>/<name>.s, or <area>/<file>/<name>.s since the decomp's
    Konami file layout (32a9475b)"""
    if asm_root not in _ASM:
        idx = {}
        for dp, _, fs in os.walk(asm_root):
            for f in fs:
                if f.endswith('.s'): idx.setdefault(f[:-2], os.path.join(dp, f))
        _ASM[asm_root] = idx
    return _ASM[asm_root].get(name)


def parse(path):
    out = []
    for ln in open(path, errors='replace'):
        m = LINE.search(ln)
        if m: out.append((m.group(1), m.group(2)))
    return out


def find(asm_root, name, n_int_params, callee_int_count):
    """returns (callee, [register indices passed through]) or None"""
    path = _asm_path(asm_root, name)
    if not path: return None
    written = set()
    ins = parse(path)
    for i, (mn, ops) in enumerate(ins):
        if mn in ('jal', 'j'):
            target = ops.strip()
            n = callee_int_count(target)
            if n is None: return None
            # the delay slot runs before the callee: count its write too
            if i + 1 < len(ins):
                dmn, dops = ins[i + 1]
                if not NO_DEST.match(dmn):
                    d = dops.split(',')[0].strip()
                    if d.startswith('$'): written.add(d)
            missing = [k for k in range(n_int_params, min(n, 8)) if INT_ARGS[k] not in written]
            return (target, missing) if missing else None
        if mn == 'jalr':
            return None            # a call through a register: its needs are unknown
        # branches and early returns: the scan goes on in address order, so a register counts as written when
        # any path before the call writes it (the matching compilers only branch forward here)
        if not NO_DEST.match(mn):
            d = ops.split(',')[0].strip()
            if d.startswith('$'): written.add(d)
    return None


def v0_from_last_call(asm_root, name):
    """True when the matched asm leaves $v0 alone after its last call: the function returns that call's result
    even where its C has no return (loops.fall_through_returns)."""
    path = _asm_path(asm_root, name)
    if not path: return False
    ins = parse(path)
    last = max((i for i, (mn, _) in enumerate(ins) if mn == 'jal'), default=-1)
    if last < 0: return False
    for mn, ops in ins[last + 2:]:
        if not NO_DEST.match(mn) and ops.split(',')[0].strip() in ('$v0', '$2'): return False
    return True


def own_untouched(asm_root, name, n_int_params):
    """The definition's own integer parameters still in their argument registers at its first call (any path):
    a call the C writes with fewer arguments passes them on there (Ta_ManageHandEffect hands its arg0 on)."""
    path = _asm_path(asm_root, name)
    if not path: return []
    written = set()
    ins = parse(path)
    for i, (mn, ops) in enumerate(ins):
        if mn in ('jal', 'j'):
            if i + 1 < len(ins):
                dmn, dops = ins[i + 1]
                if not NO_DEST.match(dmn):
                    d = dops.split(',')[0].strip()
                    if d.startswith('$'): written.add(d)
            return [k for k in range(min(n_int_params, 8)) if INT_ARGS[k] not in written]
        if mn == 'jalr': return []
        if not NO_DEST.match(mn):
            d = ops.split(',')[0].strip()
            if d.startswith('$'): written.add(d)
    return []
