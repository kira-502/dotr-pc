"""Stack slack for local aggregates (port generator).

Metrowerks lays every EE stack frame out in 16-byte steps, so a local struct or array whose address goes to a
callee has slack behind it: GGet_ModelPosZSign's 12-byte Vec3 gets 16 bytes written into it (arg1[3] = 1.0f),
SzFade_Draw_Pipe's 0x28-byte packet gets 8 bytes past its end (ExAssignAlpha). Harmless on the PS2; on x86 it
overwrites the next local or the return address. Every local array, and every local struct/union whose address is
taken, is wrapped with 16 bytes of tail slack and 16-byte alignment:
    T name[N];   ->   struct { T v[N]; u8 slack_[16]; } __attribute__((aligned(16))) name__s;
and the function's later uses of `name` become `name__s.v`.

A local struct the function copies out whole (`*dest = name;`) while setting only some of its fields starts at zero:
its other bytes went out as x86 stack garbage (SzDuel_PackDeck's padding byte, lSzView_CalcLook's w). The EE copies
its own stack leftovers there, which no port can reproduce; zero is what lane R's interpreter sees on a fresh stack.
The same holds for a local whose address goes to a function that copies it whole (COPY_SINKS, rules.json
"copy_whole_sinks": SzView_SetLookSP keeps all 4 floats of a vector whose w its callers never set).
"""
import re

COPY_SINKS = set()     # set by porter from rules.json "copy_whole_sinks"

DECL = re.compile(r'(?m)^([ \t]+)((?:const\s+|volatile\s+)*(?:struct\s+|union\s+)?\w+)\s+(\w+)\s*((?:\[[^\]\n]*\])*)'
                  r'(?P<attr>)\s*(?P<init>=\s*[^;]*?)?;')
# PORTER_SLACK_MARK=1 (porter): the marker build for finding stack packets read past their end
# (a packet whose reader takes more words than its C type holds). Locals with __attribute__((aligned(N))) are wrapped too,
# and every slack starts as 0xA5 bytes; the packet readers and the DMA check (runtime/slackmark.c) log any marker they meet.
MARK = False
DECL_MARK = re.compile(r'(?m)^([ \t]+)((?:const\s+|volatile\s+)*(?:struct\s+|union\s+)?\w+)\s+(\w+)\s*((?:\[[^\]\n]*\])*)'
                       r'(?P<attr>\s*__attribute__\(\(aligned\(\d+\)\)\))?\s*(?P<init>=\s*[^;]*?)?;')
zeroed = [0]       # locals copied out whole that now start at zero (reported by porter)
KEYWORDS = {'return', 'goto', 'case', 'else', 'typedef', 'extern', 'static', 'register', 'sizeof', 'if', 'while',
            'for', 'switch', 'do', 'break', 'continue'}


def _uses(body, name, start):
    """positions of `name` as an identifier (not a member after . or ->) from start on"""
    out = []
    for m in re.finditer(r'(?<![\w.])(?<!->)\b%s\b' % re.escape(name), body[start:]):
        out.append(start + m.start())
    return out


def wrap(t, functions, aggregate):
    """functions: [(name, header, body_start, body_end, static)]; aggregate(type) -> True for struct/union types.
    Returns (text, locals wrapped)."""
    edits = []          # (start, end, replacement) on t
    n = 0
    # names a #define body uses can't be renamed in the function (the macro expands there): those stay as they are
    in_macros = set(re.findall(r'\w+', ' '.join(
        m.group(0) for m in re.finditer(r'(?m)^[ \t]*#[ \t]*define\b(?:[^\n]*\\\n)*[^\n]*', t))))
    for fname, h, b0, b1, st in functions:
        body = t[b0:b1]
        for m in (DECL_MARK if MARK else DECL).finditer(body):
            ind, ty, name, arr, init = m.group(1), m.group(2).strip(), m.group(3), m.group(4), m.group('init')
            first = ty.split()[0]
            if first in KEYWORDS or name in KEYWORDS or ty in ('return',) or name in in_macros: continue
            # a member of a struct/union defined inside the function is not a local
            depth, k = 0, m.start() - 1
            while k >= 0:
                if body[k] == '}': depth += 1
                elif body[k] == '{':
                    if depth == 0: break
                    depth -= 1
                k -= 1
            if k >= 0 and re.search(r'\b(struct|union)\s*\w*\s*$', body[:k]): continue
            is_struct = aggregate(ty)
            if not arr and not is_struct: continue
            uses = _uses(body, name, m.end())
            zero = (not arr and not init and
                    any(re.search(r'(?<![=!<>])=\s*$', body[:p]) and re.match(r'\s*;', body[p + len(name):])
                        for p in uses))                       # copied out whole
            if not init and COPY_SINKS and any(
                    re.search(r'\b(%s)\s*\([^;]*$' % '|'.join(map(re.escape, COPY_SINKS)), body[body.rfind(';', 0, p) + 1:p])
                    for p in uses):
                zero = True                                   # handed to a function that copies it whole
            if not arr and not any(body[max(0, p - 1)] == '&' or body[max(0, p - 2):p].rstrip().endswith('&')
                                   for p in uses):
                if zero:                                      # a struct whose address is never taken
                    edits.append((b0 + m.end() - 1, b0 + m.end() - 1, ' = { 0 }'))
                    zeroed[0] += 1
                continue
            init_s = ''
            if init:
                init_s = ' = { %s }' % init.lstrip('=').strip()
            elif zero:
                init_s = ' = { { 0 } }'
                zeroed[0] += 1
            decl = '%sstruct { %s v%s; u8 slack_[16]; } __attribute__((aligned(16))) %s__s%s;' % (
                ind, ty, arr, name, init_s)
            if MARK:
                decl += ' __builtin_memset(%s__s.slack_, 0xA5, 16);' % name
            edits.append((b0 + m.start(), b0 + m.end(), decl))
            for p in uses:
                edits.append((b0 + p, b0 + p + len(name), '%s__s.v' % name))
            n += 1
    edits.sort()
    out, last = [], 0
    for s, e, r in edits:
        if s < last: continue
        out.append(t[last:s]); out.append(r); last = e
    out.append(t[last:])
    return ''.join(out), n
