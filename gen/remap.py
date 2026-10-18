"""EE-ABI call remapping for the port generator.

Matching C often declares a callee locally with a different parameter list than its definition (extra or missing
arguments, float and integer arguments in another order). On the EE that works by accident of the ABI: integer
arguments go to a0..a3/t0..t3 and float arguments to f12..f19, each counted separately, and unused registers are
simply ignored. On x86 every argument goes to the stack in order, so the calls must be rewritten: the caller's
integer arguments feed the definition's integer parameters in order, its float arguments the float parameters;
missing ones become 0, extra ones are dropped (the PS2 ignored them too).
"""
import re

FLOAT = re.compile(r'^\s*(const\s+)?(f32|f64|float|double)\s*\w*\s*$')


def split_top(s, sep=','):
    out, depth, cur, q = [], 0, [], None
    i = 0
    while i < len(s):
        c = s[i]
        if q:
            cur.append(c)
            if c == '\\':
                cur.append(s[i + 1]); i += 2; continue
            if c == q: q = None
        elif c in '"\'':
            q = c; cur.append(c)
        elif c in '([{':
            depth += 1; cur.append(c)
        elif c in ')]}':
            depth -= 1; cur.append(c)
        elif c == sep and depth == 0:
            out.append(''.join(cur)); cur = []
        else:
            cur.append(c)
        i += 1
    out.append(''.join(cur))
    return out


def param_list(params):
    """'s32 a, f32 b' -> [('I','s32'), ('F','f32')]; '' -> None (no prototype); 'void' -> []"""
    p = params.strip()
    if p == '': return None
    if p == 'void': return []
    out = []
    for x in split_top(p):
        x = x.strip()
        if x == '...': out.append(('V', '...')); continue
        typ = param_type(x)
        out.append(('F' if FLOAT.match(typ) else 'I', typ))
    return out


def param_type(decl):
    d = decl.strip()
    d = re.sub(r'\(\s*\*\s*\w+\s*\)', '(*)', d)            # function pointer name
    d = re.sub(r'\b\w+\s*(\[[^\]]*\])+\s*$', '*', d)          # array parameter -> pointer
    m = re.match(r'^(.*?[\w\*\)\]])\s*\b(\w+)\s*$', d)
    if m and m.group(1).strip() not in ('', 'const', 'unsigned', 'signed', 'struct', 'union', 'enum') \
            and not m.group(1).strip().endswith(('struct', 'union', 'enum', 'unsigned', 'signed')):
        return m.group(1).strip()
    return d


def find_call_end(t, i):
    """t[i] == '(' -> index after the matching ')'"""
    depth, q = 0, None
    while i < len(t):
        c = t[i]
        if q:
            if c == '\\': i += 2; continue
            if c == q: q = None
        elif c in '"\'':
            q = c
        elif c == '(':
            depth += 1
        elif c == ')':
            depth -= 1
            if depth == 0: return i + 1
        i += 1
    return -1


def remap_args(args, caller, callee, int_fill=None):
    """args: list of expression strings. caller/callee: param lists (None = unknown). Returns new list or None.
    int_fill: {integer argument index: expression} for missing integer arguments the caller passes through
    untouched registers (passthrough.py); other missing arguments become 0."""
    if callee is None or any(k == 'V' for k, _ in callee): return None
    args = [a for a in args if a.strip() != ''] if len(args) == 1 and args[0].strip() == '' else args
    if caller is None or any(k == 'V' for k, _ in caller) or len(caller) != len(args):
        caller = callee[:len(args)] + [('I', '')] * max(0, len(args) - len(callee))
    if [k for k, _ in caller] == [k for k, _ in callee] and len(args) == len(callee):
        out = []
        for a, (ck, ct), (dk, dt) in zip(args, caller, callee):
            out.append(cast(a, ct, dt))
        return out if out != args else None
    ints = [(a, t) for a, (k, t) in zip(args, caller) if k == 'I']
    flts = [(a, t) for a, (k, t) in zip(args, caller) if k == 'F']
    out = []
    ni = 0
    for k, t in callee:
        pool = flts if k == 'F' else ints
        if pool:
            a, ct = pool.pop(0)
            out.append(cast(a, ct, t))
        elif k == 'I' and int_fill and ni in int_fill:
            out.append(cast(int_fill[ni], 's32', t))
        else:
            out.append('0')
        if k == 'I': ni += 1
    return out


SCALAR = re.compile(r'^(const\s+)?(volatile\s+)?(s8|u8|s16|u16|s32|u32|s64|u64|f32|f64|int|char|short|long|'
                    r'long long|float|double|unsigned|signed|unsigned int|unsigned char|unsigned short|'
                    r'signed char|unsigned long long)$')


def cast(expr, from_t, to_t):
    """Only pointers and plain scalars are cast (a struct passed by value keeps its expression)."""
    if not to_t or from_t == to_t or '[' in to_t: return expr
    if '*' not in to_t and not SCALAR.match(to_t.strip()): return expr
    return '(%s)(%s)' % (to_t, expr.strip())
