"""Argument-width check (port generator).

On the EE every argument is one 64-bit register, so a callee declared with s64 and a caller passing an int agree.
On x86 an s64 argument takes two stack slots: any place where the caller's view and the definition disagree on
which parameters are 64-bit shifts every later argument. Direct calls are remapped (remap.py); what is left:
  1. function-pointer types (typedefs, casts, parameters) with 64-bit parameters, called with unknown targets;
  2. definitions with 64-bit parameters whose address is taken (stored in tables, passed as callbacks), so callers
     reach them through a pointer type the generator cannot match.
report(texts, defs_params) lists both; porter.py writes it to out/widths.txt.
"""
import re

W64 = re.compile(r'\b(s64|u64|long\s+long|unsigned\s+long\s+long|signed\s+long\s+long|int64_t|uint64_t)\b')
FNPTR = re.compile(r'\(\s*\*\s*\w*\s*\)\s*\(([^()]*)\)')


PARAM_NAME = re.compile(r'\b\w+\s*$')


def wide_defs(defs, skip, fix_long, split_top):
    """defs: {name: (file, header, static)}. The definitions with a 64-bit (non-pointer) parameter that can get an
    EE-convention twin: {name: (file, header, static, [(type, is_wide)])}. Function-pointer and array parameters,
    varargs and K&R headers are left out (report() lists what stays wide)."""
    out = {}
    for n, (f, h, st) in defs.items():
        if n in skip or n == 'main': continue
        h2 = fix_long(h)
        m = re.search(r'\((.*)\)\s*$', h2)
        if not m or not m.group(1).strip() or m.group(1).strip() == 'void': continue
        ps = m.group(1)
        if '...' in ps or '(' in ps or '[' in ps: continue
        types = []
        for d in split_top(ps):
            d = ' '.join(d.split())
            ty = d if (' ' not in d or d.endswith('*')) else PARAM_NAME.sub('', d).strip()
            if not ty or ty in ('const', 'unsigned', 'signed'): types = None; break
            types.append((ty, '*' not in ty and bool(W64.search(ty))))
        if types and any(w for _, w in types):
            out[n] = (f, h2, st, types)
    return out


def thunk(name, header, types):
    """The EE-convention twin: every 64-bit parameter is one 32-bit slot, sign-extended like an EE register."""
    ret = re.match(r'^(.*?)\b%s\s*\(' % re.escape(name), header).group(1)
    ret = re.sub(r'\b(static|inline|__inline__|extern)\b', '', ret).strip()
    params, args = [], []
    for i, (ty, w) in enumerate(types):
        if w:
            params.append('s32 p%d' % i)
            args.append('(%s)(s32)p%d' % (ty, i))
        else:
            params.append('%s p%d' % (ty, i))
            args.append('p%d' % i)
    call = '%s(%s)' % (name, ', '.join(args))
    body = call + ';' if ret == 'void' else 'return ' + call + ';'
    proto = '%s ee_%s(%s)' % (ret, name, ', '.join(params))
    return proto, '%s { %s }\n' % (proto, body)


CAST_CALL = re.compile(r'\(\s*\(\s*[\w\s\*]+\(\s*\*\s*\)\s*\([^()]*\)\s*\)\s*(\w+)\s*\)\s*\(')


def uncast_calls(t, ok):
    """`((T (*)(P))name)(args)` with a known definition becomes a plain `name(args)`, so the call remap (remap.py)
    matches the arguments to the real parameters. ok(name) says which names qualify. Returns (text, count)."""
    n = [0]
    def sub(m):
        if not ok(m.group(1)): return m.group(0)
        n[0] += 1
        return m.group(1) + '('
    return CAST_CALL.sub(sub, t), n[0]


def rewrite_values(t, wide):
    """Uses of a wide function as a value (stored, passed as a callback, cast) take its EE twin instead, so whoever
    calls it through a pointer with one slot per argument reaches the right parameters. Returns (text, count)."""
    if not wide: return t, 0
    names = sorted(wide, key=len, reverse=True)
    pat = re.compile(r'(?<![\w.>])(%s)\b(?!\s*\()' % '|'.join(re.escape(n) for n in names))
    out, last, n = [], 0, 0
    for m in pat.finditer(t):
        before = t[:m.start()].rstrip()
        if not before: continue
        c = before[-1]
        if c in '=,(?:&){' or re.search(r'\breturn$', before):
            if c == ')' and re.search(r'\b(if|while|for|switch)\s*\([^()]*\)$', before): continue
            ln0 = t.rfind('\n', 0, m.start()) + 1
            if t[ln0:m.start()].lstrip().startswith('#'): continue      # #define name ... stays
            out.append(t[last:m.start()]); out.append('ee_' + m.group(1)); last = m.end(); n += 1
    out.append(t[last:])
    return ''.join(out), n


def report(texts, defs_params):
    """texts: {file: text}; defs_params: {name: [(kind, type)]} from remap.param_list. Returns lines."""
    out = []
    for f, t in sorted(texts.items()):
        for m in FNPTR.finditer(t):
            if W64.search(m.group(1)):
                line = t.count('\n', 0, m.start()) + 1
                out.append('fnptr %s:%d (%s)' % (f, line, ' '.join(m.group(1).split())))
    wide = {n for n, ps in defs_params.items() if ps and any(W64.search(ty) for _, ty in ps)}
    for f, t in sorted(texts.items()):
        for n in sorted(wide):
            # the name used as a value: not followed by '(' and not part of a declaration
            for m in re.finditer(r'\b%s\b(?!\s*\()' % re.escape(n), t):
                before = t[max(0, m.start() - 40):m.start()]
                if re.search(r'(\bextern\b|\bstatic\b|[\w\*]\s+)$', before) and not re.search(r'[=,(&]\s*$', before):
                    continue
                line = t.count('\n', 0, m.start()) + 1
                out.append('addr-taken %s:%d %s(%s)' % (f, line, n, ', '.join(ty for _, ty in defs_params[n])))
    return out
