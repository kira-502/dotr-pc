#!/usr/bin/env python3
"""Struct layout test (PORT-PLAN.md 4.2): every file-scope struct's size and member offsets, compiled by
Metrowerks (the PS2 build) and by clang-cl (the PC build), must agree.

  py gen/layout.py gen   <decomp snapshot> <port out> <work dir>   -> probe sources + manifest.json
  (compile: work/mw/*.c with tools/cc.sh in the decomp copy (WSL); work/pc/*.c with clang-cl)
  py gen/layout.py check <work dir>                                 -> report, exit 1 on any difference

Each probe is the file itself followed by one array of sizeof/offset values in .data (the files define no
data of their own), so the two objects' .data sections can be compared word by word. Bit-fields and members
of nested anonymous aggregates are not probed (offsetof cannot name them); the size of their struct is.
"""
import json, os, re, sys, struct

def strip_comments(t):
    t = re.sub(r'/\*.*?\*/', lambda m: '\n' * m.group(0).count('\n'), t, flags=re.S)
    return re.sub(r'//[^\n]*', '', t)

def balanced(t, i):
    d = 0
    for j in range(i, len(t)):
        if t[j] == '{': d += 1
        elif t[j] == '}':
            d -= 1
            if d == 0: return j
    return -1

def top_structs(t):
    """file-scope struct/union definitions: yield (type_expr, members)"""
    depth = 0; i = 0
    out = []
    pat = re.compile(r'\b(typedef\s+)?(struct|union)\s*(\w*)\s*\{')
    while True:
        m = pat.search(t, i)
        if not m: break
        # file scope only: brace depth before the match must be 0
        depth = t.count('{', 0, m.start()) - t.count('}', 0, m.start())
        ob = m.end() - 1
        cb = balanced(t, ob)
        if cb < 0: break
        if depth == 0:
            body = t[ob + 1:cb]
            tail = t[cb + 1:t.find(';', cb) if t.find(';', cb) >= 0 else cb + 1]
            tail = re.sub(r'__attribute__\s*\(\(.*?\)\)', ' ', tail)
            names = re.findall(r'\b([A-Za-z_]\w*)\b', tail)
            kind, tag = m.group(2), m.group(3)
            if m.group(1) and names: texpr = names[0]
            elif tag and not m.group(1): texpr = '%s %s' % (kind, tag)
            else: texpr = None
            if texpr: out.append((texpr, members(body)))
        i = cb + 1
    return out

def members(body):
    names = []
    for decl in split_decls(body):
        d = decl.strip()
        if not d or ':' in d.split('[')[0] or d.startswith('#'): continue
        if '{' in d:
            after = d[d.rfind('}') + 1:]
            for n in re.findall(r'\b([A-Za-z_]\w*)\s*(?:\[|$|,)', after.strip() + '$'): names.append(n)
            continue
        fp = re.search(r'\(\s*\*\s*(\w+)\s*\)', d)
        if fp: names.append(fp.group(1)); continue
        parts = d.split(',')
        first = parts[0]
        m = re.search(r'([A-Za-z_]\w*)\s*(\[[^\]]*\])*\s*$', first)
        if m and len(first.split()) >= 2: names.append(m.group(1))
        for p in parts[1:]:
            m = re.search(r'([A-Za-z_]\w*)\s*(\[[^\]]*\])*\s*$', p)
            if m: names.append(m.group(1))
    return [n for n in names if n not in ('struct', 'union', 'const', 'volatile', 'unsigned', 'signed')
            and not n.startswith('__')]

def split_decls(body):
    out, d, cur = [], 0, ''
    for c in body:
        if c in '{(': d += 1
        if c in '})': d -= 1
        if c == ';' and d == 0: out.append(cur); cur = ''
        else: cur += c
    return out

PROBE_HEAD = '\n/* ---- layout probe ---- */\n#define DOTR_PROBE_OFF(T, m) ((unsigned int)&((T *)0)->m)\n'

def gen(decomp, out, work):
    os.makedirs(os.path.join(work, 'mw'), exist_ok=True); os.makedirs(os.path.join(work, 'pc'), exist_ok=True)
    gccf = set()
    for ln in open(os.path.join(decomp, 'config/gcc_files.txt')):
        ln = ln.strip()
        if ln and not ln.startswith('#'): gccf.add(os.path.basename(ln.split()[0]))
    manifest = {}
    # headers: one probe with only common.h
    jobs = [('headers', '#include "common.h"\n', '#include "common.h"\n',
             ''.join(open(os.path.join(decomp, 'include', h), encoding='utf-8', errors='replace').read()
                     for h in ('structs.h', 'game.h')))]
    for f in sorted(os.listdir(os.path.join(decomp, 'src'))):
        if not f.endswith('.c') or f in gccf: continue
        mw_src = open(os.path.join(decomp, 'src', f), encoding='utf-8', errors='replace').read()
        pc_path = os.path.join(out, 'src', f)
        if not os.path.exists(pc_path): continue
        pc_src = open(pc_path, encoding='utf-8', errors='replace').read()
        jobs.append((f[:-2], mw_src, pc_src, mw_src))
    for name, mw_src, pc_src, scan in jobs:
        # copies under `#ifndef GAME_H` never compile (game.h is always included)
        scan = re.sub(r'(?ms)^[ \t]*#[ \t]*ifndef[ \t]+GAME_H\b.*?^[ \t]*#[ \t]*endif', '', strip_comments(scan))
        structs = top_structs(scan)
        if not structs: continue
        vals, rows = [], []
        for texpr, mem in structs:
            rows.append([texpr, 'sizeof']); vals.append('sizeof(%s)' % texpr)
            for mname in mem:
                rows.append([texpr, mname]); vals.append('DOTR_PROBE_OFF(%s, %s)' % (texpr, mname))
        arr = PROBE_HEAD + 'unsigned int dotr_layout_probe[] = {\n    0xD07B5EEDu,\n    ' +',\n    '.join(vals) + '\n};\n'
        open(os.path.join(work, 'mw', name + '.c'), 'w', newline='\n').write(mw_src + arr)
        open(os.path.join(work, 'pc', name + '.c'), 'w', newline='\n').write(pc_src + arr)
        manifest[name] = rows
    json.dump(manifest, open(os.path.join(work, 'manifest.json'), 'w'), indent=0)
    print('probes:', len(manifest), 'files,', sum(len(v) for v in manifest.values()), 'values')

def check(work):
    manifest = json.load(open(os.path.join(work, 'manifest.json')))
    bad, ok, missing = [], 0, []
    for name, rows in sorted(manifest.items()):
        pm = os.path.join(work, 'mw', name + '.bin'); pp = os.path.join(work, 'pc', name + '.bin')
        if not (os.path.exists(pm) and os.path.exists(pp)): missing.append(name); continue
        a = open(pm, 'rb').read(); b = open(pp, 'rb').read()
        n = len(rows)
        mk = struct.pack('<I', 0xD07B5EED)
        if mk in a: a = a[a.index(mk) + 4:]
        if mk in b: b = b[b.index(mk) + 4:]
        if len(a) < 4 * n or len(b) < 4 * n: missing.append(name + ' (short .data)'); continue
        va = struct.unpack('<%dI' % n, a[:4 * n]); vb = struct.unpack('<%dI' % n, b[:4 * n])
        for (texpr, mem), x, y in zip(rows, va, vb):
            if x == y: ok += 1
            else: bad.append('%s: %s.%s  PS2 %d  PC %d' % (name, texpr, mem, x, y))
    print('layout values equal: %d, different: %d, files not compared: %d' % (ok, len(bad), len(missing)))
    for x in bad[:200]: print('  ' + x)
    if missing: print('not compared:', ' '.join(missing))
    open(os.path.join(work, 'report.txt'), 'w').write('\n'.join(bad + ['missing: ' + m for m in missing]) + '\n')
    return 1 if bad or missing else 0

if __name__ == '__main__':
    if sys.argv[1] == 'gen': gen(*sys.argv[2:5])
    else: sys.exit(check(sys.argv[2]))
