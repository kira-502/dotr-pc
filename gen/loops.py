"""Loops and fall-through returns (port generator).

Busy waits. On the PS2 a loop that spins on a flag (a disc read done, a DMA finished) ends because an interrupt
sets the flag. The PC threads are cooperative and have no interrupts, so every game loop gets an interrupt point:
RT_POLL() in each while/for condition and before each backward goto. It costs one increment per turn; every 4096
turns the runtime looks whether the next vblank is due and, if so, runs it right there (runtime/threads.c).

Fall-through returns. Matching C has functions that end with `callee(...);` and no return: on the EE the
callee's result is still in $v0, so the caller returns it. The PC needs the return written out.
"""
import re

KW = re.compile(r'\b(while|for)\s*\(')
GOTO = re.compile(r'\bgoto\s+(\w+)\s*;')


def _code_mask(t):
    """True for each character that is code (not inside a string, character, comment or preprocessor line)."""
    n = len(t)
    m = bytearray(b'\x01') * n
    i = 0
    line_start = True
    while i < n:
        c = t[i]
        if line_start and c == '#':
            j = i
            while True:
                j = t.find('\n', j)
                if j < 0: j = n; break
                if t[j - 1] != '\\': break
                j += 1
            m[i:j] = b'\x00' * (j - i); i = j; continue
        if c in '"\'':
            j = i + 1
            while j < n and t[j] != c:
                if t[j] == '\\': j += 1
                j += 1
            m[i:j + 1] = b'\x00' * (j + 1 - i); i = j + 1; line_start = False; continue
        if t.startswith('/*', i):
            j = t.find('*/', i + 2); j = n if j < 0 else j + 2
            m[i:j] = b'\x00' * (j - i); i = j; continue
        if t.startswith('//', i):
            j = t.find('\n', i); j = n if j < 0 else j
            m[i:j] = b'\x00' * (j - i); i = j; continue
        if c == '\n': line_start = True
        elif not c.isspace(): line_start = False
        i += 1
    return m


def _close(t, i, mask):
    """t[i] == '(' -> index of the matching ')'"""
    depth = 0
    while i < len(t):
        if mask[i]:
            if t[i] == '(': depth += 1
            elif t[i] == ')':
                depth -= 1
                if depth == 0: return i
        i += 1
    return -1


def _split_semis(s):
    out, depth, cur = [], 0, []
    for c in s:
        if c in '([{': depth += 1
        elif c in ')]}': depth -= 1
        if c == ';' and depth == 0: out.append(''.join(cur)); cur = []
        else: cur.append(c)
    out.append(''.join(cur))
    return out


def poll(t, functions):
    """functions: [(body_start, body_end)] of the file's top-level functions. Returns (text, loops rewritten)."""
    mask = _code_mask(t)
    edits = []                       # (start, end, replacement)
    for m in KW.finditer(t):
        if not mask[m.start()]: continue
        o = m.end() - 1
        c = _close(t, o, mask)
        if c < 0: continue
        inner = t[o + 1:c]
        if m.group(1) == 'while':
            if inner.strip() == '0': continue           # do { } while (0)
            edits.append((o + 1, c, 'RT_POLL(), (%s)' % inner))
        else:
            parts = _split_semis(inner)
            if len(parts) != 3: continue
            cond = parts[1].strip()
            parts[1] = ' RT_POLL(), ' + ('(%s)' % cond if cond else '1')
            edits.append((o + 1, c, ';'.join(parts)))
    for b0, b1 in functions:
        body = t[b0:b1]
        for g in GOTO.finditer(body):
            if not mask[b0 + g.start()]: continue
            lab = re.search(r'(?m)^[ \t]*%s[ \t]*:(?!:)' % re.escape(g.group(1)), body[:g.start()])
            if lab:
                edits.append((b0 + g.start(), b0 + g.end(), '{ RT_POLL(); %s }' % g.group(0)))
    edits.sort()
    out, last, n = [], 0, 0
    for s, e, r in edits:
        if s < last: continue                     # nested in an edit already taken (cannot happen for loops)
        out.append(t[last:s]); out.append(r); last = e; n += 1
    out.append(t[last:])
    return ''.join(out), n


def fall_through_returns(t, functions, returns_value, v0_from_last_call=None):
    """functions: [(name, return type or None for void, body_start, body_end)]; returns_value(callee) -> bool;
    v0_from_last_call(name) -> bool (the asm keeps the last call's $v0 to the end). Returns (text, functions fixed).
    Three cases: the body ends with `callee(...);` (return it); a bare `return;` right after `callee(...);`
    (a tail call whose value the C drops: `return callee(...);`, as $v0 still holds it on the PS2); or the asm
    says $v0 still holds the last call's result and that call's result was stored, `var = callee(...)` (return
    var at the end)."""
    mask = _code_mask(t)
    edits = []

    def tail_call(text, k):
        """the statement ending at text[k] == ';' when it is exactly `callee(...)` of a value-returning callee:
        the callee's offset in text, else None"""
        start = max(text.rfind(';', 0, k), text.rfind('{', 0, k), text.rfind('}', 0, k)) + 1
        stmt = text[start:k]
        cm = re.match(r'^\s*(\w+)\s*\(', stmt)
        if not cm or cm.group(1) in ('if', 'while', 'for', 'switch', 'sizeof', 'return') \
                or not returns_value(cm.group(1)) or not stmt.rstrip().endswith(')'):
            return None
        depth = 0
        tail = stmt.rstrip()
        for j in range(cm.end() - 1, len(tail)):
            depth += {'(': 1, ')': -1}.get(tail[j], 0)
            if depth == 0: return start + cm.start(1) if j == len(tail) - 1 else None
        return None

    for name, ret, b0, b1 in functions:
        if not ret: continue
        body = t[b0 + 1:b1 - 1]
        bm = ''.join(ch if mask[b0 + 1 + k] else ' ' for k, ch in enumerate(body))
        has_return = re.search(r'\breturn\b', bm)
        for rm in re.finditer(r'\breturn\s*;', bm):
            k = len(bm[:rm.start()].rstrip()) - 1
            if k < 0 or bm[k] != ';': continue
            c = tail_call(bm, k)
            if c is None: continue
            edits.append((b0 + 1 + rm.start(), rm.end() - rm.start(), ''))
            edits.append((b0 + 1 + c, 0, 'return '))
        end = bm.rstrip()
        if not end.endswith(';'): continue
        c = tail_call(end, len(end) - 1)
        if c is not None:
            edits.append((b0 + 1 + c, 0, 'return '))
            continue
        if has_return: continue
        if not v0_from_last_call or not v0_from_last_call(name): continue
        calls = [m for m in re.finditer(r'(?<![\w.>])(\w+)\s*\(', bm)
                 if m.group(1) not in ('if', 'while', 'for', 'switch', 'sizeof', 'return', 'RT_POLL')]
        if not calls: continue
        last = calls[-1]
        am = re.search(r'(\w+)\s*=\s*(\([^()]*\)\s*)?$', bm[:last.start()])
        if not am or not returns_value(last.group(1)): continue
        edits.append((b1 - 1, 0, '    return (%s)%s;\n' % (ret, am.group(1))))
    for pos, n, s in sorted(edits, reverse=True):
        t = t[:pos] + s + t[pos + n:]
    return t, sum(1 for e in edits if e[2])
