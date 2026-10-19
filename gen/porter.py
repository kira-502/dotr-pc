#!/usr/bin/env python3
"""DotR-PC port generator.

Turns a pinned decomp snapshot (gen/decomp, made by fetch_decomp.sh) into a PC-compilable copy in gen/out:
  - game C (Metrowerks files) and the C library (newlib/libgcc, ee-gcc 2.96 files); Sony's libraries are
    replaced by the runtime (HLE) and get generated stubs until then
  - Metrowerks-only assembly removed (asm functions -> stubs; functions holding asm blocks -> stubs)
  - every function declaration made to agree with the function's own definition (functions.h rebuilt from
    the definitions; local re-declarations dropped); functions called with a different argument count are
    declared without a prototype when that is ABI-safe (rules.txt lists the rest)
  - C-library names that clash with the PC C runtime renamed (ps2_ prefix)
  - absolute data symbols at their PS2 addresses (syms.s) and the PS2-address -> native function table
The decomp itself is never modified. usage: py -3 gen/porter.py [--decomp gen/decomp] [--out gen/out]
"""
import argparse, os, re, shutil, sys, json
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import remap
import abi
import mmio
import passthrough
import loops
import widths
import oracle
import stackslack
mmio_sites = [0]

ap = argparse.ArgumentParser()
here = os.path.dirname(os.path.abspath(__file__))
ap.add_argument('--decomp', default=os.path.join(here, 'decomp'))
ap.add_argument('--out', default=os.path.join(here, 'out'))
args = ap.parse_args()
D, O = args.decomp, args.out

# ---------------------------------------------------------------------------------------------------------
# which files
gcc = {}
for ln in open(os.path.join(D, 'config/gcc_files.txt')):
    ln = ln.strip()
    if ln and not ln.startswith('#'):
        p = ln.split(); gcc[p[0].split('src/', 1)[-1].replace('/', '_')] = p[1]     # the snapshot's flat names (fetch_decomp.sh)
allc = sorted(f for f in os.listdir(os.path.join(D, 'src')) if f.endswith('.c'))
libc_files = [f for f in allc if gcc.get(f, '').startswith('2.96')]
sony_files = [f for f in allc if f in gcc and not gcc[f].startswith('2.96')]
game_files = [f for f in allc if f not in gcc]
port_files = game_files + libc_files

# ---------------------------------------------------------------------------------------------------------
# text helpers
def balanced_end(t, i, o, c):
    depth = 0
    while i < len(t):
        ch = t[i]
        if ch == '"' or ch == "'":
            q = ch; i += 1
            while i < len(t) and t[i] != q:
                if t[i] == '\\': i += 1
                i += 1
        elif t[i:i + 2] == '/*':
            j = t.find('*/', i + 2); i = len(t) if j < 0 else j + 1
        elif t[i:i + 2] == '//':
            j = t.find('\n', i); i = len(t) if j < 0 else j - 1
        elif ch == o: depth += 1
        elif ch == c:
            depth -= 1
            if depth == 0: return i + 1
        i += 1
    return len(t)

def blank(seg):
    return '\n' * seg.count('\n')

def strip_comments(t):
    out, i, n = [], 0, len(t)
    while i < n:
        if t[i] == '"' or t[i] == "'":
            q = t[i]; j = i + 1
            while j < n and t[j] != q:
                if t[j] == '\\': j += 1
                j += 1
            out.append(t[i:j + 1]); i = j + 1
        elif t.startswith('/*', i):
            j = t.find('*/', i + 2); j = n if j < 0 else j + 2
            out.append(blank(t[i:j])); i = j
        elif t.startswith('//', i):
            j = t.find('\n', i); j = n if j < 0 else j
            i = j
        else:
            out.append(t[i]); i += 1
    return ''.join(out)

ASMFUNC = re.compile(r'^asm\s+[^\n;{]*?\b(\w+)\s*\((?:[^()]|\([^()]*\))*\)\s*\{', re.M)
ASMBLOCK = re.compile(r'\basm\s*\{')
ASMSTMT = re.compile(r'\b(__asm__|asm)\s*(volatile\s*|__volatile__\s*)?\(')

def top_level_functions(t):
    """yield (name, header_text, body_start, body_end, static) for each top-level function definition."""
    i, n, depth = 0, len(t), 0
    last = 0
    while i < n:
        ch = t[i]
        if ch == '"' or ch == "'":
            i = balanced_end(t, i, '\x00', '\x00') if False else i
            q = ch; i += 1
            while i < n and t[i] != q:
                if t[i] == '\\': i += 1
                i += 1
            i += 1; continue
        if ch == '#' and (i == 0 or t[i - 1] == '\n'):
            j = t.find('\n', i)
            while j > 0 and t[j - 1] == '\\': j = t.find('\n', j + 1)
            i = n if j < 0 else j
            last = i; continue
        if ch == ';' or ch == '}':
            last = i + 1; i += 1; continue
        if ch == '{':
            head = t[last:i]
            end = balanced_end(t, i, '{', '}')
            h = ' '.join(head.split())
            m = re.search(r'(\w+)\s*\(([^()]*(?:\([^()]*\)[^()]*)*)\)\s*$', h)
            if m and not re.search(r'\b(struct|union|enum)\s*\w*\s*$', h) and '=' not in h and not h.startswith('typedef'):
                name = m.group(1)
                if name not in ('if', 'while', 'for', 'switch', 'return', 'sizeof'):
                    yield name, h, i, end, bool(re.match(r'(static|inline|__inline__)\b', h))
            i = end; last = i
            # a struct body followed by a declarator: skip to its ';'
            continue
        i += 1

# ---------------------------------------------------------------------------------------------------------
# 1. read and strip every file (Sony files too, for their definitions' signatures)
rules_path = os.path.join(here, 'rules.json')
rules = json.load(open(rules_path)) if os.path.exists(rules_path) else {}
stackslack.COPY_SINKS = set(rules.get('copy_whole_sinks', []))   # locals handed to these start at zero
stackslack.MARK = bool(os.environ.get('PORTER_SLACK_MARK'))         # the marker build (gen/stackslack.py)
SLACK_MARK_HOOKS = {'render_exprim.c': [
    ('s32 ExSortPrim_Pipe(s32 cursorIn, s32 prim, s32 ot, s32 z) {',
     ' rt_mark_prim((const void *)prim, __builtin_return_address(0));'),
    ('int ExSortNode_Pipe(int cursor, void *header_v, int count, s32 vertices_v, void *chain_v, int z)\n{',
     ' rt_mark_node(header_v, __builtin_return_address(0));')]}
# EE/GS register addresses by name (the named decomp, include/ee_regs.h): gen/mmio.py turns them back into addresses
ee_reg_names = {}
if os.path.exists(os.path.join(D, 'include', 'ee_regs.h')):
    for m_ in re.finditer(r'(?m)^#define\s+(\w+_ADDR)\s+(0x[0-9A-Fa-f]+)\s*$', open(os.path.join(D, 'include', 'ee_regs.h')).read()):
        ee_reg_names[m_.group(1)] = m_.group(2)
# renamed functions (config/old_names.txt, "old = new;"): where the decomp's C still calls a function by its old
# name (the decomp links old names as aliases), the port calls the new one. func_<address> names stay as they are.
renamed = {}
if os.path.exists(os.path.join(D, 'config', 'old_names.txt')):
    for m_ in re.finditer(r'(?m)^\s*(\w+)\s*=\s*(\w+)\s*;', open(os.path.join(D, 'config', 'old_names.txt')).read()):
        if not m_.group(1).startswith('func_'): renamed[m_.group(1)] = m_.group(2)
RENAMED = re.compile(r'\b(' + '|'.join(map(re.escape, sorted(renamed, key=len, reverse=True))) + r')\b') if renamed else None
texts, removed_asm, asm_block_funcs = {}, [], set()
asm_stmt_funcs = {}     # file -> functions whose behaviour is in GCC asm statements
ps2_local_funcs = {ln.split()[-1] for ln in open(os.path.join(D, 'main.funcs')) if len(ln.split()) >= 3}
for f in allc:
    t = strip_comments(open(os.path.join(D, 'src', f), encoding='utf-8', errors='replace').read())
    if RENAMED: t = RENAMED.sub(lambda m_: renamed[m_.group(1)], t)
    # fixes to non-matching decomp C that must hold before signatures are read (rules.json source_patches)
    for pt in rules.get('source_patches', []):
        if pt['file'] == f:
            if pt['old'] not in t: sys.exit('rules.json source patch no longer applies: %s (%s)' % (f, pt['why']))
            t = t.replace(pt['old'], pt['new'])
    if stackslack.MARK:                 # the marker build: the packet readers check what they read (runtime/slackmark.c)
        for old_, new_ in SLACK_MARK_HOOKS.get(f, []):
            if old_ not in t: sys.exit('marker build: reader hook no longer applies in %s: %r' % (f, old_[:60]))
            t = t.replace(old_, old_ + new_)
    while True:
        m = ASMFUNC.search(t)
        if not m: break
        e = balanced_end(t, m.end() - 1, '{', '}')
        removed_asm.append((f, m.group(1)))
        t = t[:m.start()] + blank(t[m.start():e]) + t[e:]
    # functions holding asm blocks (VU0 maths etc.) become stubs, except main() (its block only sets sp)
    for name, h, b0, b1, st in list(top_level_functions(t)):
        if ASMBLOCK.search(t[b0:b1]) and name != 'main':
            asm_block_funcs.add(name)
    def kill_blocks(t):
        while True:
            m = ASMBLOCK.search(t)
            if not m: return t
            e = balanced_end(t, m.end() - 1, '{', '}')
            t = t[:m.start()] + ';' + blank(t[m.start():e]) + t[e:]
    t = kill_blocks(t)
    # functions whose behaviour lives in GCC asm statements (newlib's hand-written memmove/strlen, register-bound
    # implicit arguments, COP0/COP2 work): their C would be wrong without it, so they are cut and must be
    # provided (hle/) - a stub logs them loudly until then. A lone sync/ei/di/nop is harmless and stays.
    for name, h, b0, b1, st in list(top_level_functions(t)):
        body = t[b0:b1]
        for m in ASMSTMT.finditer(body):
            e = balanced_end(body, m.end() - 1, '(', ')')
            code = ' '.join(re.findall(r'"((?:[^"\\]|\\.)*)"', body[m.end():e].split(':')[0], re.S))   # re.S: "\<newline>" continuations
            before = body[:m.start()].rstrip()
            binds = before and (before[-1].isalnum() or before[-1] == '_')
            if binds or re.sub(r'(\\[nt]|\s|\bsync(\.[lp])?\b|\bei\b|\bdi\b|\bnop\b|;)', '', code):
                asm_stmt_funcs.setdefault(f, set()).add(name)
                break
    while True:
        m = ASMSTMT.search(t)
        if not m: break
        e = balanced_end(t, m.end() - 1, '(', ')')
        # `register T r asm("$4")` loses its asm; an asm statement becomes the expression 0
        before = t[:m.start()].rstrip()
        rep = '' if before and (before[-1].isalnum() or before[-1] == '_') else '0'
        t = t[:m.start()] + rep + blank(t[m.start():e]) + t[e:]
    # a static function with its own PS2 address (a local symbol in main.funcs) becomes public: the function
    # table and lane R's oracle reach it by that address (lSzDuel_GetFontIndex, lSzFile_NextRequest)
    for name in [n for n, h, b0, b1, st in top_level_functions(t) if st and n in ps2_local_funcs]:
        t = re.sub(r'(?m)^static\s+(?=[^;{(]*\b%s\s*\()' % name, '', t)
    texts[f] = t

# 2. definitions -> canonical prototypes
defs = {}       # name -> (file, header, static)
for f in allc:
    for name, h, b0, b1, st in top_level_functions(texts[f]):
        if name not in defs or (defs[name][2] and not st):   # a public definition wins over a static one
            defs[name] = (f, re.sub(r'^asm\s+', '', h), st)

DECL_START = re.compile(r'(?m)^[ \t]*(?:extern[ \t]+)?[A-Za-z_][\w \t\*]*?\b\w+\s*\(')
def iter_decls(t):
    """function declarations, possibly over several lines: yield (start, end, raw_name, return_text, params)."""
    for m in DECL_START.finditer(t):
        s0 = m.start()
        e = t.find(';', m.end())
        brace = t.find('{', m.end())
        if e < 0 or (0 <= brace < e): continue
        stmt = t[s0:e + 1]
        pre = stmt[:stmt.index('(')]
        if '=' in pre or not stmt.rstrip().endswith(');'): continue
        toks = pre.replace('*', ' * ').split()
        words = [x for x in toks if x != '*']
        if len(words) < 2 or words[0] in ('return', 'else', 'case', 'goto', 'if', 'while', 'do'): continue
        if not all(re.match(r'^\w+$', x) for x in words): continue
        open_i = s0 + stmt.index('(')
        close_i = remap.find_call_end(t, open_i)
        if close_i < 0 or close_i > e + 1: continue
        rt = ' '.join(x for x in toks[:-1] if x not in ('extern', 'static')).replace(' *', '*').strip()
        yield s0, e + 1, words[-1], rt, t[open_i + 1:close_i - 1]
if os.environ.get('PORTER_DEBUG'):
    for n_ in os.environ['PORTER_DEBUG'].split(','): print('DEBUG def', n_, defs.get(n_))
loose = set()   # superseded by the EE-ABI call remap (remap.py); rules 'no_prototype' kept for reference only
# Sony library files compiled natively (their register accesses go to the virtual hardware, gen/mmio.py)
port_files = port_files + [f_ for f_ in rules.get('sony_native_files', []) if f_ in sony_files]
external = set(rules.get('external', []))   # defined by another lane's library linked into dotr.dll: no stub
cut = set(rules.get('cut', [])) | asm_block_funcs  # definitions removed (runtime/HLE or stub provides them)
asm_cut = sorted(n_ for f_, ns_ in asm_stmt_funcs.items() if f_ in port_files for n_ in ns_)
cut |= set(asm_cut)

# functions other parts of the port provide, with their exact prototypes:
#   lane G's VU0 maths (gfx/vu0/vu0_protos.h, including the arguments the decomp's C does not show)
#   the HLE layer (hle/*.c: each definition marked HLE_FUNC)
repo = os.path.dirname(here)
provided = {}          # name -> prototype text (game types)
vu0_h = os.path.join(repo, 'gfx', 'vu0', 'vu0_protos.h')
if os.path.exists(vu0_h):
    for ln in open(vu0_h, encoding='utf-8'):
        m_ = re.match(r'^\s*([\w\s\*]+?\b(\w+)\s*\(.*\))\s*;', ln)
        if m_ and not ln.startswith('#'):
            provided[m_.group(2)] = re.sub(r'\bint32_t\b', 's32', re.sub(r'\buint32_t\b', 'u32', m_.group(1)))
hle_dir = os.path.join(repo, 'hle')
hle_files = sorted(f_ for f_ in os.listdir(hle_dir) if f_.endswith('.c')) if os.path.isdir(hle_dir) else []
for f_ in hle_files:
    src_ = strip_comments(open(os.path.join(hle_dir, f_), encoding='utf-8').read())
    for m_ in re.finditer(r'(?m)^HLE_FUNC\s+([^(;{]*?\b(\w+)\s*\([^{;]*\))\s*\{', src_):
        provided[m_.group(2)] = ' '.join(m_.group(1).split())
cut |= set(provided)
for n_, p_ in provided.items():
    defs[n_] = ('<provided>', p_, False)

def params_of(h):
    m = re.search(r'\((.*)\)\s*$', h)
    return m.group(1).strip() if m else ''

PROMO_SAFE = re.compile(r'^\s*(const\s+)?(volatile\s+)?(s32|u32|int|unsigned|unsigned int|signed|long|s64|u64|long long|unsigned long long|void)\b|\*')
def knr_ok(h):
    ps = params_of(h)
    if ps in ('', 'void'): return True
    for p in split_params(ps):
        if p.strip() == '...': return False
        if not PROMO_SAFE.search(p) or re.match(r'^\s*(const\s+)?(u8|s8|u16|s16|f32|f64|float|double|char|short|unsigned char|signed char|unsigned short)\s+\w*\s*$', p):
            if '*' not in p: return False
    return True

def split_params(ps):
    out, depth, cur = [], 0, ''
    for ch in ps:
        if ch == '(': depth += 1
        if ch == ')': depth -= 1
        if ch == ',' and depth == 0: out.append(cur); cur = ''
        else: cur += ch
    out.append(cur)
    return out

def ret_and_name(h):
    m = re.search(r'^(.*?)\b(\w+)\s*\(', h)
    return m.group(1).strip(), m.group(2)

# address aliases: the decomp's linker script names some functions a second time by address
# (func_001F9170_b = 0x001F9170, an absolute symbol) for calls with another prototype. They are that function.
_old_txt = {}
for ln in open(os.path.join(D, 'config/old_names.txt')):
    m = re.match(r'^(func_[0-9A-F]{8})\s*=\s*(\w+);', ln)
    if m: _old_txt[m.group(1)] = m.group(2)
_text_at, _abs = {}, []
for ln in open(os.path.join(D, 'main.nm')):
    p_ = ln.split()
    if len(p_) != 3: continue
    if p_[1] in 'Tt' and re.match(r'^[A-Za-z]\w*$', p_[2]): _text_at.setdefault(int(p_[0], 16), p_[2])   # not __gnu_compiled_c labels
    elif p_[1] == 'A' and re.match(r'^(func_[0-9A-F]{8}_\w+|[A-Za-z]\w*_b)$', p_[2]) and not p_[2].startswith('D_'):
        _abs.append((int(p_[0], 16), p_[2]))     # also SzMsg_DrawRect_Pipe_b-style names
# an address carrying a renamed function's old and new names (the decomp's aliases): the new name stands for it
_text_at = {a_: renamed.get(n_, n_) for a_, n_ in _text_at.items()}
addr_alias = {}
# libgcc's helper names (__moddi3, __floatdidf, ...): the PC compiler calls its own (compiler-rt) by the same names
LIBCALL = re.compile(r'^__(ashl|ashr|lshr|mul|div|mod|udiv|umod|udivmod|neg|cmp|ucmp|float|fix|extend|trunc|add|sub|'
                     r'pow|bswap|clz|ctz|ffs|popcount|parity)\w*(di|df|sf|si|ti)\d?$')
_hdr = ' '.join(open(os.path.join(D, 'include', h_), encoding='utf-8', errors='replace').read()
                for h_ in os.listdir(os.path.join(D, 'include')) if h_.endswith('.h'))
for a_, n_ in _abs:
    # a name a header declares with its own prototype (game.h: Taku_Camp_BOTAN_MATI_b) stays an address symbol
    if a_ in _text_at and n_ not in defs and not (n_.endswith('_b') and not n_.startswith('func_') and
                                                  re.search(r'\b%s\s*\(' % re.escape(n_), _hdr)):
        tgt = _old_txt.get(_text_at[a_], _text_at[a_])
        if tgt != n_: addr_alias[n_] = tgt

# definitions returning void whose callers declare (and use) a value: return that type instead
promote = {}
caller_ret = {}
old_alias0 = {}
for ln in open(os.path.join(D, 'config/old_names.txt')):
    m = re.match(r'^(func_[0-9A-F]{8})\s*=\s*(\w+);', ln)
    if m and m.group(1) != m.group(2): old_alias0[m.group(1)] = m.group(2)
old_alias0.update(addr_alias)
for f in allc:
    for s0_, e1_, raw_, rt, ps_ in iter_decls(texts[f]):
        name = old_alias0.get(raw_, raw_)
        if rt and rt != 'void':
            caller_ret.setdefault(name, rt if rt in ('s32', 'u32', 'int') or rt.endswith('*') else 's32')
        if name in defs and name not in provided and rt and rt != 'void' and ret_and_name(defs[name][1])[0].replace('static', '').strip() == 'void':
            promote.setdefault(name, rt if rt in ('s32', 'u32', 'int') or rt.endswith('*') else 's32')
for name, rt in promote.items():
    f0, h, st = defs[name]
    defs[name] = (f0, re.sub(r'\bvoid\s+' + name + r'\s*\(', rt + ' ' + name + '(', h, count=1), st)

# CRT clashes: every named function defined in the C library files, plus main
crt_rename = {'main'}
for f in libc_files:
    for name, h, b0, b1, st in top_level_functions(texts[f]):
        if not name.startswith('func_'): crt_rename.add(name)
for extra in rules.get('rename', []): crt_rename.add(extra)

# ---------------------------------------------------------------------------------------------------------
# 3. write the out tree
if os.path.exists(O): shutil.rmtree(O)
os.makedirs(os.path.join(O, 'src'))
shutil.copytree(os.path.join(D, 'include'), os.path.join(O, 'include'))
inc = os.path.join(O, 'include')

# 3a. header patches (port copy only)
def patch(path, old, new, count=1):
    t = open(path, encoding='utf-8').read()
    if old not in t: sys.exit('patch failed for %s: %r' % (path, old[:60]))
    t = t.replace(old, new) if count == 0 else t.replace(old, new, count)
    open(path, 'w', encoding='utf-8', newline='\n').write(t)
GN = '#if defined(__GNUC__) && !defined(__MWERKS__) && !defined(M2CTX) && !defined(PERMUTER)'
patch(os.path.join(inc, 'common.h'), GN + '\ntypedef int dk_int128 __attribute__((mode(TI)));\n#define __int128 dk_int128\n#endif',
      '/* port: clang has no __int128 on x86-32; _BitInt(128), 16-byte aligned like the EE quadword */\n'
      'typedef signed _BitInt(128) dk_int128 __attribute__((aligned(16)));\n#define __int128 dk_int128\n'
      '/* port: what a quadword pointer points at may sit on a 4-byte boundary on the PC (a stack struct of floats\n'
      '   the EE kept 16-aligned): pointers use this type, so copies through them never assume alignment */\n'
      'typedef signed _BitInt(128) dk_int128_ua __attribute__((aligned(4)));')
patch(os.path.join(inc, 'game.h'), GN, '#if 1 /* port */', 0)
open(os.path.join(inc, 'dk_asm.h'), 'w', newline='\n').write(
    '#ifndef DK_ASM_H\n#define DK_ASM_H\n/* port: no assembly */\n#define INCLUDE_ASM(FOLDER, NAME)\n#define INCLUDE_RODATA(FOLDER, NAME)\n#endif\n')

# 3b. functions.h rebuilt: canonical prototypes from the definitions
old_alias = {}
for ln in open(os.path.join(D, 'config/old_names.txt')):
    m = re.match(r'^(func_[0-9A-F]{8})\s*=\s*(\w+);', ln)
    if m and m.group(1) != m.group(2): old_alias[m.group(1)] = m.group(2)
old_alias.update(addr_alias)
fh_lines = open(os.path.join(D, 'include/functions.h'), encoding='utf-8').read().split('\n')
# a prototype may carry a trailing comment (/* JP: L016 */): drop it so every prototype line parses
fh_lines = [re.sub(r';\s*/\*.*?\*/\s*$', ';', l) for l in fh_lines]
out_fh = []
declared = set()
for ln in fh_lines:
    m = re.match(r'^([^#/].*?)\b(\w+)\s*\((.*)\)\s*;\s*$', ln)
    if m and not ln.startswith('typedef'):
        name = m.group(2)
        if name in old_alias and name not in defs:
            out_fh.append('/* old name: %s */' % name)
            continue
        declared.add(name)
        if name in defs and not defs[name][2]:
            h = defs[name][1]
            if name in loose and knr_ok(h):
                r, _ = ret_and_name(h)
                out_fh.append('%s %s();' % (r, name))
            else:
                out_fh.append(h + ';')
            continue
        if name in defs and defs[name][2]:
            out_fh.append('/* static: ' + ln + ' */')
            continue
        if name not in defs and name in caller_ret and re.match(r'^void\s', ln):
            ln = caller_ret[name] + ln[4:]   # no definition (stubbed): callers use a value
    out_fh.append(ln)
out_fh = ['#if 1 /* port: every prototype visible */' if l.startswith('#ifndef __GNUC_MINOR__') else l for l in out_fh]
# public definitions functions.h never declared (C library internals): add their prototypes before the end
extra = ['/* port: public definitions functions.h did not declare */']
for name in sorted(defs):
    f0, h, st = defs[name]
    if not st and name not in declared and f0 in port_files:
        extra.append(h + ';'); declared.add(name)
last_endif = max(i for i, l in enumerate(out_fh) if l.startswith('#endif'))
out_fh[last_endif:last_endif] = extra
open(os.path.join(inc, 'functions.h'), 'w', encoding='utf-8', newline='\n').write('\n'.join(out_fh))

# EE data model in every header (long is 64-bit; bit-field runs)
for root_, dirs_, files_ in os.walk(inc):
    for hf in files_:
        if hf.endswith('.h'):
            hp = os.path.join(root_, hf)
            ht, nbf = abi.fix_bitfields(abi.fix_long(open(hp, encoding="utf-8", errors="replace").read()), set(rules.get("bitfield_keep", [])), set(rules.get("bitfield_noalign", [])), set(rules.get("bitfield_half_start", [])))
            open(hp, 'w', encoding='utf-8', newline='\n').write(ht)

# 3c. port.h
old_names = []
for ln in open(os.path.join(D, 'config/old_names.txt')):
    m = re.match(r'^(func_[0-9A-F]{8})\s*=\s*(\w+);', ln)
    if m: old_names.append((m.group(1), m.group(2)))
old_names += sorted(addr_alias.items())
with open(os.path.join(inc, 'port.h'), 'w', newline='\n') as w:
    w.write('/* generated by gen/porter.py: force-included into every decomp file of the PC build */\n')
    w.write('#ifndef DOTR_PORT_H\n#define DOTR_PORT_H\n#define DOTR_PC 1\n')
    w.write('/* virtual hardware (runtime/mmio.c): register accesses rewritten by gen/mmio.py */\n'
            'unsigned long long hw_mmio_r(unsigned int addr, int size);\n'
            'void hw_mmio_w(unsigned int addr, unsigned long long value, int size);\n'
            'void hw_mmio_w128(unsigned int addr, const void *value);\n'
            'void *hw_mmio_p(unsigned int addr);\n')
    w.write('/* interrupt point in every game loop (gen/loops.py, runtime/threads.c) */\n'
            'extern unsigned int rt_poll_n;\nvoid rt_poll_slow(void);\n'
            '#define RT_POLL() ((++rt_poll_n & 0xFFF) ? (void)0 : rt_poll_slow())\n')
    w.write('/* modules: mwOverlayInit switches in lane R\'s translated code first (runtime/overlay.c, rules.json) */\n'
            'int rt_overlay_switch(void);\n')
    w.write('/* lane R\'s differential (gen/oracle.py, recomp/rc_port.h): only in DOTR_ORACLE builds */\n'
            '#ifdef DOTR_ORACLE\nvoid rc_oracle_hook(unsigned int addr, int ni, const unsigned long long *iargs, int nf, '
            'const float *fargs);\n#endif\n')
    w.write('/* headless bot duels (runtime/botduel.c, rules.json) */\n'
            'void rt_bot_duel_start(void);\nint rt_bot_kind(int side, int kind, int other);\n'
            'void rt_bot_duel_end(int winner, int reason);\nvoid rt_bot_tick(void);\n')
    w.write('/* --debug-menu (dev builds): the first mode after the memory-card check (runtime/dotr_main.c, rules.json) */\n'
            'int rt_first_mode(int mode);\n')
    w.write('/* pad script v2 (runtime/padscript.c): a story event window is drawn (rules.json) */\n'
            'void rt_dialog_drawn(void);\n')
    w.write('/* the EE FPU\'s add (rounds toward zero) where the PC\'s rounding changes the game (runtime/eefloat.c, '
            'rules.json) */\nfloat ee_add_rz(float a, float b);\n')
    w.write('/* the marker build (PORTER_SLACK_MARK=1, gen/stackslack.py): packet readers check what they read '
            '(runtime/slackmark.c) */\nvoid rt_mark_prim(const void *prim, void *ret);\n'
            'void rt_mark_node(const void *header, void *ret);\n')
    w.write('/* C-library names are renamed in the objects (renames.txt), not here */\n')
    w.write('/* old address names still used in some files */\n')
    for a, b in old_names:
        if a not in defs: w.write('#define %s %s\n' % (a, b))
    w.write('#endif\n')

# data symbols from the decomp's linked executable
data_names, data_alias = {}, {}
# the named decomp gives some data tables a symbol of kind T (sideColors, windowFontGlyphs, cardViewerLight): one the
# game's C declares as an extern object (no parameter list) is data all the same
extern_objects = set()
for t_ in [texts[f_] for f_ in port_files] + [_hdr]:
    extern_objects |= set(re.findall(r'(?m)^[ \t]*extern\b[^;(]*?\b([A-Za-z_]\w*)\s*(?:\[[^\]]*\])*\s*;', t_))
    # tables of function pointers: extern s32 (*sortCompareFuncs[])(s32, s32);
    extern_objects |= set(re.findall(r'(?m)^[ \t]*extern\b[^;(]*\(\s*\*\s*([A-Za-z_]\w*)\s*(?:\[[^\]]*\])*\s*\)\s*\(', t_))
for ln in open(os.path.join(D, 'main.nm')):
    p_ = ln.split()
    if len(p_) != 3: continue
    a_, k_, n_ = p_
    if (k_ in 'ADBRdbr' or (k_ in 'Tt' and n_ in extern_objects)) and re.match(r'^[A-Za-z_]\w*$', n_) \
            and n_ not in defs and n_ not in addr_alias and not LIBCALL.match(n_):
        av = int(a_, 16)
        if 0x00100000 <= av < 0x02000000 or 0x70000000 <= av < 0x70004000: data_names.setdefault(n_, av)
# addresses inside data objects that the ELF has no symbol for: the name is the address
for f_ in port_files:
    for n_ in set(re.findall(r'\bD_([0-9A-F]{8})\b', texts[f_])):
        av = int(n_, 16)
        if (0x00100000 <= av < 0x02000000 or 0x70000000 <= av < 0x70004000) and 'D_' + n_ not in defs:
            data_names.setdefault('D_' + n_, av)

# 3d. source files: drop local re-declarations of functions, cut functions
DECL = re.compile(r'^[ \t]*(extern\s+)?(?:(?:const|volatile|static|unsigned|signed|struct\s+\w+|union\s+\w+|\w+)\s*\**\s*)+?\b(\w+)\s*\(([^;{}()]*(?:\([^;{}()]*\)[^;{}()]*)*)\)\s*;[ \t]*$', re.M)
dropped = 0
remapped = []
DECL_START = re.compile(r'(?m)^[ \t]*(?:extern[ \t]+)?[A-Za-z_][\w \t\*]*?\b\w+\s*\(')
CALL = re.compile(r'(?<![\w.>])(\w+)\s*\(')
header_structs = set()
for hf in os.listdir(inc):
    if hf.endswith('.h'):
        header_structs |= set(re.findall(r'\bstruct\s+(\w+)\s*\{', open(os.path.join(inc, hf), encoding='utf-8', errors='replace').read()))
callee_params = {}
callee_rets = {}
for n_, (f_, h_, st_) in defs.items():
    callee_params[n_] = remap.param_list(params_of(h_))
    callee_rets[n_] = ' '.join(x for x in ret_and_name(h_)[0].replace('*', ' * ').split()
                               if x not in ('static', 'inline', '__inline__', 'extern')).replace(' *', '*')
orig_params = {}
for ln in fh_lines:
    m = re.match(r'^([^#/].*?)\b(\w+)\s*\((.*)\)\s*;\s*$', ln)
    if m and not ln.startswith('typedef'):
        orig_params[old_alias.get(m.group(2), m.group(2))] = remap.param_list(m.group(3))
for n_, ln in [(n_, ln) for n_ in declared if n_ not in defs for ln in [None]]:
    pass
for ln in out_fh:
    m = re.match(r'^([^#/].*?)\b(\w+)\s*\((.*)\)\s*;\s*$', ln)
    if m and not ln.startswith('typedef') and m.group(2) not in callee_params:
        callee_params[m.group(2)] = remap.param_list(m.group(3))

# argument registers a definition passes through to its first callee (passthrough.py)
passthru = {}          # function -> (callee, [int register indices])
own_pass = {}          # function -> {int register index: own parameter name} (passthrough.own_untouched)
asm_root = os.path.join(D, 'asm', 'matchings')
def n_int(name):
    pl = callee_params.get(old_alias.get(name, name))
    return None if pl is None else sum(1 for k, _ in pl if k == 'I')
if os.path.isdir(asm_root):
    for name_, (f_, h_, st_) in defs.items():
        if f_ not in port_files or name_ in provided: continue
        pl = callee_params.get(name_)
        if pl is None or any(k == 'V' for k, _ in pl): continue
        # its own integer parameters still in their registers at the first call: {register index: parameter name}
        ints_ = [d_ for d_, (k, _) in zip(remap.split_top(params_of(h_)), pl) if k == 'I']
        own_ = {}
        for k in passthrough.own_untouched(asm_root, name_, len(ints_)):
            nm_ = re.search(r'(\w+)\s*$', ints_[k].strip())
            if nm_: own_[k] = nm_.group(1)
        if own_: own_pass[name_] = own_
        r_ = passthrough.find(asm_root, name_, sum(1 for k, _ in pl if k == 'I'), n_int)
        if r_ and r_[1] == list(range(r_[1][0], r_[1][0] + len(r_[1]))):
            passthru[name_] = (old_alias.get(r_[0], r_[0]), r_[1])
            extra_ = ', '.join('s32 pt_r%d' % k for k in r_[1])
            ps_ = params_of(h_)
            newh = h_[:h_.rindex('(') + 1] + (extra_ if ps_ in ('', 'void') else ps_ + ', ' + extra_) + ')'
            defs[name_] = (f_, newh, st_)
            callee_params[name_] = remap.param_list(params_of(newh))
# static helpers without parameters that the matching C split out of such a function (get_card_001E17D0 inside
# SzDuel_IsUnitMovableOnLabyrinth): on the PS2 they are inlined, so their calls see the caller's argument
# registers too. They get the same pass-through parameters, and the callers' calls to them fill those in.
if os.path.isdir(asm_root):
    for f_ in port_files:
        fns_ = list(top_level_functions(texts[f_]))
        for hn_, hh_, hb0_, hb1_, hst_ in fns_:
            if not hst_ or hn_ in passthru or params_of(hh_) not in ('', 'void') or hn_ not in defs: continue
            regs_ = set()
            for cn_, ch_, cb0_, cb1_, cst_ in fns_:
                if cn_ == hn_ or not re.search(r'\b%s\s*\(' % re.escape(hn_), texts[f_][cb0_:cb1_]): continue
                if cn_ in passthru: regs_ |= set(passthru[cn_][1])
                if cn_ in own_pass: regs_ |= set(own_pass[cn_])
            if not regs_: continue
            regs_ = list(range(max(regs_) + 1))      # no gaps: parameter k is register k (the callers fill by index)
            passthru[hn_] = (None, regs_)
            hf_, hh2_, hst2_ = defs[hn_]
            newh = hh2_[:hh2_.rindex('(') + 1] + ', '.join('s32 pt_r%d' % k for k in regs_) + ')'
            defs[hn_] = (hf_, newh, hst2_)
            callee_params[hn_] = remap.param_list(params_of(newh))
for ln_i, ln in enumerate(out_fh):     # functions.h follows the extended definitions
    m = re.match(r'^([^#/].*?)\b(\w+)\s*\((.*)\)\s*;\s*$', ln)
    if m and m.group(2) in passthru: out_fh[ln_i] = defs[m.group(2)][1] + ';'
open(os.path.join(inc, 'functions.h'), 'w', encoding='utf-8', newline='\n').write(abi.fix_long('\n'.join(out_fh)))

# return types for the fall-through-return fix (loops.py)
def is_void(h):
    return re.sub(r'\b(static|inline|__inline__|const|extern)\b', '', ret_and_name(h)[0]).strip() == 'void'
ret_of = {}
for ln in out_fh:
    m = re.match(r'^([^#/].*?)\b(\w+)\s*\((.*)\)\s*;\s*$', ln)
    if m and not ln.startswith('typedef'): ret_of[m.group(2)] = ln
for name_, (f_, h_, st_) in defs.items(): ret_of[name_] = h_
fall_through, poll_sites = [0], [0]
# 64-bit parameters through pointers (gen/widths.py): EE-convention twins for the definitions that have them
wide = widths.wide_defs({n_: d_ for n_, d_ in defs.items() if d_[0] in port_files}, cut | set(provided),
                        abi.fix_long, remap.split_top)
ee_values = [0]
slack_locals = [0]
# struct/union (and array) typedef names from the headers, for the stack-slack rule (gen/stackslack.py)
AGG_TYPEDEF = re.compile(r'typedef\s+(?:struct|union)\b[^;{]*(?:\{[^{}]*(?:\{[^{}]*\}[^{}]*)*\})?\s*(\w+)\s*;'
                         r'|typedef\s+[\w\s]+?\b(\w+)\s*\[[^\]]*\]\s*;')
hdr_aggs = set()
for h_ in os.listdir(inc):
    if h_.endswith('.h'):
        for m_ in AGG_TYPEDEF.finditer(open(os.path.join(inc, h_), encoding='utf-8', errors='replace').read()):
            hdr_aggs.add(m_.group(1) or m_.group(2))
oracle_hooks = [0]
mmio_funcs = set()
addr_early = {}
for ln in open(os.path.join(D, 'main.funcs')):
    p_ = ln.split()
    if len(p_) >= 3: addr_early.setdefault(p_[-1], int(p_[0], 16))
uncast = [0]
def _plain_callee(n_):
    # a cast call to a known definition becomes a direct call; not when floats are involved (EE soft-float doubles
    # travel as bit patterns in integer registers, a numeric conversion would change them)
    if n_ not in defs or is_void(defs[n_][1]): return False      # a void definition read as a value: keep the cast
    return not re.search(r'\b(float|double|f32|f64)\b', defs[n_][1])

for f in port_files:
    t = texts[f]
    t, nu_ = widths.uncast_calls(t, _plain_callee)
    uncast[0] += nu_
    # definitions that pass argument registers through get those parameters
    for name_, (callee_, regs_) in passthru.items():
        if defs[name_][0] != f: continue
        for nm_, h_, b0_, b1_, st_ in top_level_functions(t):
            if nm_ != name_: continue
            close = t.rfind(')', 0, b0_)
            open_ = t.rfind('(', 0, close)
            inner = t[open_ + 1:close].strip()
            extra_ = ', '.join('s32 pt_r%d' % k for k in regs_)
            # parameter lists with nested parentheses: find the list's own opening parenthesis
            depth_ = 0
            for j_ in range(close, -1, -1):
                if t[j_] == ')': depth_ += 1
                elif t[j_] == '(':
                    depth_ -= 1
                    if depth_ == 0: open_ = j_; break
            inner = t[open_ + 1:close].strip()
            t = t[:open_ + 1] + (extra_ if inner in ('', 'void') else inner + ', ' + extra_) + t[close:]
            break
    for name, rt in promote.items():
        if True:   # every file that defines it (a function may be defined in more than one file)
            t = re.sub(r'(^|\n)((?:static\s+)?)void(\s+)' + name + r'(\s*\()',
                       lambda m: m.group(1) + m.group(2) + rt + m.group(3) + name + m.group(4), t)
    # remove cut definitions (and asm-block functions) bottom-up
    funcs = list(top_level_functions(t))
    for name, h, b0, b1, st in reversed(funcs):
        if name in cut:
            # find start of header: walk back to previous ';' '}' or newline-after-preprocessor
            s = t.rfind(name, 0, b0)
            hs = t.rfind('\n', 0, s) + 1
            while hs > 0:
                prev_start = t.rfind('\n', 0, hs - 1) + 1
                prev = t[prev_start:hs - 1].strip()
                if not prev or prev.endswith((';', '}', '{')) or prev.startswith('#'): break
                hs = prev_start
            t = t[:hs] + blank(t[hs:b1]) + t[b1:]
    # block-scope extern declarations of data: each function's own view gets its own symbol name at the same
    # address (MWCC accepted one object declared with different types in one file; C does not)
    for name, h, b0, b1, st in reversed(list(top_level_functions(t))):
        body = t[b0:b1]
        locals_ = set()
        for m in re.finditer(r'^[ \t]+extern\b(?:\s*struct\s*\w*\s*\{[^}]*\})?[^;(]*?\b([A-Za-z_]\w*)\s*(?:\[[^\]]*\])*\s*;', body, re.M):
            if m.group(1) in data_names: locals_.add(m.group(1))
        for sym in locals_:
            alias = '%s__%s' % (sym, name)
            body = re.sub(r'\b%s\b' % re.escape(sym), alias, body)
            data_alias[alias] = data_names[sym]
        t = t[:b0] + body + t[b1:]
    # file-scope extern declarations of one data symbol with different types: from the second one on, the
    # code below each declaration uses its own alias symbol (same address)
    FSDECL = r'(?m)^extern\b[^;(]*?\b(%s)\s*(?:\[[^\]]*\])*\s*;'
    dead, stack = [], []
    for pm in re.finditer(r'(?m)^[ \t]*#[ \t]*(ifndef[ \t]+GAME_H\b|if\w*|endif)', t):
        d = pm.group(1)
        if d.startswith('if'): stack.append((d.startswith('ifndef') and 'GAME_H' in d, pm.start()))
        elif stack:
            is_dead, s_ = stack.pop()
            if is_dead: dead.append((s_, pm.start()))
    syms_ = sorted(set(m.group(1) for m in re.finditer(FSDECL % r'[A-Za-z_]\w*', t) if m.group(1) in data_names))
    for sym in syms_:
        lst = [(m.start(), ' '.join(m.group(0).split())) for m in re.finditer(FSDECL % re.escape(sym), t)
               if not any(a <= m.start() < b for a, b in dead)]   # copies inside #ifndef GAME_H never compile
        if len(set(x[1] for x in lst)) < 2: continue
        for k in range(len(lst) - 1, 0, -1):
            a0 = lst[k][0]
            a1 = lst[k + 1][0] if k + 1 < len(lst) else len(t)
            alias = '%s__fs%d' % (sym, k)
            t = t[:a0] + re.sub(r'\b%s\b' % re.escape(sym), alias, t[a0:a1]) + t[a1:]
            data_alias[alias] = data_names[sym]
    # function declarations (one or several lines): record each scope's view of the callee, then drop them;
    # functions.h now carries every function's real prototype
    ranges = [(b0, b1, name) for name, h, b0, b1, st in top_level_functions(t)]
    def scope_at(pos):
        for b0, b1, nm in ranges:
            if b0 <= pos < b1: return nm
        return None
    views = {}           # (scope or None, callee) -> param list
    view_rets = {}       # (scope or None, callee) -> return type as that scope declared it
    cuts = []
    for s0, e1, raw, rt_, ps_ in iter_decls(t):
        name = old_alias.get(raw, raw)
        if name not in defs and name not in declared: continue
        views[(scope_at(s0), name)] = remap.param_list(ps_)
        view_rets[(scope_at(s0), name)] = rt_
        if name in defs and defs[name][2]: continue      # keep forward declarations of static functions
        cuts.append((s0, e1))
    for s0, e1 in reversed(cuts):
        t = t[:s0] + blank(t[s0:e1]) + t[e1:]
        dropped += 1
    # rewrite calls whose caller view differs from the definition (EE-ABI argument remap)
    macros = set(re.findall(r'(?m)^[ \t]*#[ \t]*define[ \t]+(\w+)\(', t))
    def remap_text(body, fname):
        out, pos = [], 0
        for m in CALL.finditer(body):
            if m.start() < pos: continue
            raw = m.group(1)
            name = old_alias.get(raw, raw)
            if name not in callee_params or raw in macros: continue
            op_ = m.end() - 1
            ce_ = remap.find_call_end(body, op_)
            if ce_ > 0 and any(re.search(r'\b%s\s*\(' % re.escape(mc), body[op_:ce_]) for mc in macros):
                continue   # an argument is a macro that may expand to several arguments
            op = m.end() - 1
            ce = remap.find_call_end(body, op)
            if ce < 0: continue
            args = [remap_text(a, fname) for a in remap.split_top(body[op + 1:ce - 1])]
            if len(args) == 1 and args[0].strip() == '': args = []
            view = views.get((fname, name), views.get((None, name), orig_params.get(name)))
            callee = callee_params[name]
            new = None
            if not (view == callee and len(args) == len(callee or [])):
                fill = None
                if fname in passthru or fname in own_pass:
                    # every call that leaves those registers missing gets them (a switch of tail calls, e.g.
                    # Ta_ManageHandEffect passes $a0/$a1/$a2 on to three different callees), not only the first one
                    fill = dict(own_pass.get(fname, {}))
                    fill.update({k: 'pt_r%d' % k for k in passthru.get(fname, (None, []))[1]})
                new = remap.remap_args(args, view, callee, fill)
            if new is not None:
                remapped.append('%s: %s -> %s (%d -> %d args)' % (f, fname, name, len(args), len(new)))
            call = body[m.start():op + 1] + ', '.join(new if new is not None else args) + ')'
            # the caller saw a different pointer return type (e.g. s32 * for a void * definition): cast the result
            vr = view_rets.get((fname, name), view_rets.get((None, name)))
            cr = callee_rets.get(name)
            if vr and cr and vr != cr and vr.endswith('*') and cr.endswith('*'):
                call = '((%s)%s)' % (vr, call)
            out.append(body[pos:m.start()]); out.append(call)
            pos = ce
        out.append(body[pos:])
        return ''.join(out)
    ranges = [(b0, b1, name) for name, h, b0, b1, st in top_level_functions(t)]
    for b0, b1, fname in reversed(ranges):
        t = t[:b0] + remap_text(t[b0:b1], fname) + t[b1:]
    # calls written inside #define bodies use the file-level view
    def remap_define(m):
        return m.group(1) + remap_text(m.group(2), None)
    t = re.sub(r'(?m)^([ \t]*#[ \t]*define[ \t]+\w+(?:\([^)]*\))?)((?:[^\n]*\\\n)*[^\n]*)$', remap_define, t)
    # an extern array of a struct type defined later in the file (MWCC allowed it): move that file-scope
    # definition up, before the function that first uses it
    for m in list(re.finditer(r'\bextern\s+struct\s+(\w+)\s+\w+\s*\[[^\]]*\]\s*;', t)):
        tag = m.group(1)
        dm = re.search(r'(?m)^typedef\s+struct\s+%s\s*\{|^struct\s+%s\s*\{' % (tag, tag), t)
        if not dm or dm.start() < m.start(): continue
        ob = t.index('{', dm.start())
        cb = balanced_end(t, ob, '{', '}')
        semi = t.index(';', cb)
        block = t[dm.start():semi + 1]
        users = [b0 for nm, h, b0, b1, st in top_level_functions(t) if b0 <= m.start() < b1]
        anchor = users[0] if users else m.start()
        ins = t.rfind('\n\n', 0, anchor) + 1
        t = t[:dm.start()] + blank(block) + t[semi + 1:]
        t = t[:ins] + block + '\n' + t[ins:]
    # extern arrays of a struct type that is never defined (MWCC allowed it; only their address is used)
    def incomplete(m):
        tag = m.group(1)
        if re.search(r'\bstruct\s+%s\s*\{' % tag, t) or tag in header_structs: return m.group(0)
        return m.group(0).replace('struct ' + tag, 'u8 /* port: struct %s never defined */' % tag, 1)
    t = re.sub(r'\bextern\s+struct\s+(\w+)\s+\w+\s*\[[^\]]*\]\s*;', incomplete, t)
    for pt in rules.get('patches', []):
        if pt['file'] == f:
            if pt['old'] not in t: sys.exit('rules.json patch no longer applies: %s (%s)' % (f, pt['why']))
            t = t.replace(pt['old'], pt['new'])
    t, nbf = abi.fix_bitfields(abi.fix_long(t), set(rules.get("bitfield_keep", [])), set(rules.get("bitfield_noalign", [])), set(rules.get("bitfield_half_start", [])))
    t, nmm = mmio.rewrite(t, ee_reg_names)
    t = re.sub(r'\b__int128(\s*\*)', r'dk_int128_ua\1', t)
    fns_ =list(top_level_functions(t))
    t, nfr = loops.fall_through_returns(
        t, [(n_, None if is_void(h_) else re.sub(r'\b(static|inline|__inline__|extern)\b', '', ret_and_name(h_)[0]).strip(),
             b0_, b1_) for n_, h_, b0_, b1_, s_ in fns_],
        lambda c_: c_ in ret_of and not is_void(ret_of[c_]),
        lambda n_: passthrough.v0_from_last_call(os.path.join(D, 'asm', 'matchings'), n_))
    fall_through[0] += nfr
    # ends without a return where the oracle saw what $v0 holds on the PS2 (rules.json fall_through_value)
    ftv_ = {k_: v_ for k_, v_ in rules.get('fall_through_value', {}).items() if not k_.startswith('_')}
    for n_, h_, b0_, b1_, s_ in reversed(list(top_level_functions(t))):
        if n_ in ftv_ and not re.search(r'\breturn\b[^;]*;\s*$', t[b0_ + 1:b1_ - 1]):
            t = t[:b1_ - 1] + '    return %s;\n' % ftv_[n_] + t[b1_ - 1:]
            fall_through[0] += 1
    if f not in libc_files:
        t, npl = loops.poll(t, [(b0_, b1_) for n_, h_, b0_, b1_, s_ in top_level_functions(t)])
        poll_sites[0] += npl
    mmio_sites[0] += nmm
    for n_, h_, b0_, b1_, s_ in top_level_functions(t):
        if 'hw_mmio_' in t[b0_:b1_]: mmio_funcs.add(n_)             # reads/writes virtual hardware: oracle runs it natively
    file_aggs_ = hdr_aggs | {m_.group(1) or m_.group(2) for m_ in AGG_TYPEDEF.finditer(t)}
    t, nsl_ = (t, 0) if os.environ.get('PORTER_NO_SLACK') else stackslack.wrap(t, list(top_level_functions(t)),
                              lambda ty_: ty_.split()[-1] in file_aggs_ or bool(re.match(r'^(const\s+)?(struct|union)\b', ty_)))
    slack_locals[0] += nsl_
    t, no_ = oracle.insert(t, list(top_level_functions(t)), addr_early, params_of, remap.split_top)
    oracle_hooks[0] += no_
    t, nv_ = widths.rewrite_values(t, wide)
    ee_values[0] += nv_
    for n_, (f_, h_, st_, ty_) in sorted(wide.items()):
        if f_ == f and st_: t += '\n' + widths.thunk(n_, h_, ty_)[1]   # a static one's twin lives in its file
    open(os.path.join(O, 'src', f), 'w', encoding='utf-8', newline='\n').write(t)

# 3e. stubs: every declared function with no native definition
native = set()
for f in port_files:
    for name, h, b0, b1, st in top_level_functions(texts[f]):
        if name not in cut and not st: native.add(name)
stub_lines = ['/* generated by gen/porter.py: functions the PC build does not compile from the decomp yet */',
              '#include "common.h"', 'void hle_unimplemented(const char *name);', '']
stubbed = []
proto = {}
for ln in out_fh:
    m = re.match(r'^([^#/].*?)\b(\w+)\s*\((.*)\)\s*;\s*$', ln)
    if m and not ln.startswith('typedef'): proto[m.group(2)] = ln
for name in sorted(declared):
    if name in native or name in provided or name not in proto or name in external: continue
    h = proto[name].rstrip(';').strip()
    if name in defs: h = defs[name][1] if not defs[name][2] else h
    r, _ = ret_and_name(h)
    ps = params_of(h)
    body = 'hle_unimplemented("%s");' % name
    if r.replace('const', '').strip() != 'void':
        body += ' { static %s z; return z; }' % r if not r.endswith('*') else ' return 0;'
    if '...' in ps and ps.strip() == '...': h = '%s %s(void)' % (r, name)
    stub_lines.append('%s { %s }' % (re.sub(r'\b%s\s*\(\s*\)' % name, '%s(void)' % name, h), body))
    stubbed.append(name)
open(os.path.join(O, 'src', 'zz_stubs.c'), 'w', newline='\n').write('\n'.join(stub_lines) + '\n')

# functions whose PC version is replaced on purpose (HLE, stubs, cut, other lanes): lane R's oracle runs these
# natively even in deep mode
open(os.path.join(O, 'native_only.txt'), 'w', newline='\n').write(
    '# not compiled from the decomp (HLE, stubs, cut, other lanes) or touching virtual hardware (gen/oracle.py)\n'
    + '\n'.join('%s 0x%08X' % (n_, addr_early[n_]) for n_ in sorted(set(provided) | set(stubbed) | cut | external | mmio_funcs)
                 if n_ in addr_early) + '\n')

# 3f. absolute data symbols (i686 COFF: C names get a leading underscore)
funcs_all = set(defs) | declared
data = []
for ln in open(os.path.join(D, 'main.nm')):
    p = ln.split()
    if len(p) != 3: continue
    addr, kind, name = p
    if kind not in 'ADBRdbr' or not re.match(r'^[A-Za-z_]\w*$', name) or name in funcs_all or name in addr_alias: continue
    if LIBCALL.match(name): continue      # the PC compiler's own helper names (compiler-rt): never a PS2 address
    a = int(addr, 16)
    if 0x00100000 <= a < 0x02000000 or 0x70000000 <= a < 0x70004000:
        data.append((name, a))
seen = set()
with open(os.path.join(O, 'syms.s'), 'w', newline='\n') as w:
    w.write('# generated by gen/porter.py: decomp data symbols at their PS2 addresses\n')
    for name, a in list(data) + sorted(data_names.items()):
        if name in seen or name in funcs_all: continue
        seen.add(name)
        w.write('.globl _%s\n.set _%s, 0x%08X\n' % (name, name, a))
    for name, a in sorted(data_alias.items()):
        w.write('.globl _%s\n.set _%s, 0x%08X\n' % (name, name, a))

# EE-convention twins of the non-static wide definitions, and their prototypes for everyone (lane R's exits too)
ee_protos = []
with open(os.path.join(O, 'src', 'zz_eethunks.c'), 'w', newline='\n') as w:
    w.write('/* generated by gen/porter.py (gen/widths.py): one 32-bit slot per argument, for callers through pointers */\n'
            '#include "common.h"\n')
    for n_, (f_, h_, st_, ty_) in sorted(wide.items()):
        proto_, body_ = widths.thunk(n_, h_, ty_)
        if not st_:
            w.write(body_)
            ee_protos.append(proto_ + ';')
with open(os.path.join(inc, 'functions.h'), 'a', encoding='utf-8', newline='\n') as w:
    w.write('\n/* EE-convention twins (gen/widths.py): a function reached by its PS2 address or through a pointer */\n')
    w.write('\n'.join(ee_protos) + '\n')
open(os.path.join(O, 'widths.txt'), 'w', newline='\n').write(
    '# 64-bit parameters left behind function pointers (gen/widths.py)\n' + '\n'.join(
        widths.report({f_: open(os.path.join(O, 'src', f_), encoding='utf-8').read() for f_ in port_files},
                      {n_: remap.param_list(params_of(abi.fix_long(d_[1]))) or [] for n_, d_ in defs.items()
                       if n_ not in wide})) + '\n')

# 3g. function table: PS2 address -> native function (functions compiled natively or stubbed)
addr_of = {}
for ln in open(os.path.join(D, 'main.funcs')):
    p = ln.split()
    if len(p) >= 3: addr_of.setdefault(p[-1], int(p[0], 16))
rows = []
for name in sorted(native | set(stubbed) | (set(provided) & set(addr_of)), key=lambda n: addr_of.get(n, 0)):
    a = addr_of.get(name)
    if a is None or a == 0x00100000: continue
    rows.append((a, name))
with open(os.path.join(O, 'src', 'zz_functab.c'), 'w', newline='\n') as w:
    w.write('/* generated by gen/porter.py */\n#include "common.h"\n')
    w.write('typedef struct ps2_func { unsigned int addr; void *native; const char *name; } ps2_func;\n')
    for a, n in rows:
        if n in native or n in stubbed: pass
    w.write('const ps2_func ps2_functab[] = {\n')
    for a, n in rows: w.write('    { 0x%08X, (void *)%s, "%s" },\n' % (a, ('ee_' + n) if n in wide else n, n))
    w.write('};\nconst unsigned int ps2_functab_count = %d;\n' % len(rows))
    w.write('const unsigned int ps2_functab_native = %d; /* compiled from the decomp; the rest are placeholders */\n'
            % sum(1 for a, n in rows if n in native))

# 3h. build file
with open(os.path.join(O, 'CMakeLists.txt'), 'w', newline='\n') as w:
    w.write('# generated by gen/porter.py\nenable_language(ASM)\n')
    srcs = ' '.join('${CMAKE_CURRENT_LIST_DIR}/src/%s' % f for f in port_files + ['zz_stubs.c', 'zz_functab.c', 'zz_eethunks.c'])
    srcs += ''.join(' ${CMAKE_SOURCE_DIR}/hle/%s' % f for f in hle_files)   # HLE: Sony's libraries, game headers
    w.write('add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/syms.obj\n'
            '  COMMAND clang --target=i686-pc-windows-msvc -c ${CMAKE_CURRENT_LIST_DIR}/syms.s -o ${CMAKE_CURRENT_BINARY_DIR}/syms.obj\n'
            '  DEPENDS ${CMAKE_CURRENT_LIST_DIR}/syms.s)\n')
    w.write('add_library(dotr_game_raw STATIC %s ${CMAKE_CURRENT_BINARY_DIR}/syms.obj)\n' % srcs)
    w.write('target_include_directories(dotr_game_raw PRIVATE ${CMAKE_CURRENT_LIST_DIR}/include ${CMAKE_SOURCE_DIR}/runtime ${CMAKE_SOURCE_DIR}/hw)\n')
    # every local starts at zero: where the game reads a local before writing it the EE sees its own stack leftover,
    # x86 a leftover that moves with dotr.dll's code layout (a bot duel's timing changed with an unrelated edit)
    w.write('target_compile_options(dotr_game_raw PRIVATE /FIport.h /GS- /Z7 /W0 -fwrapv -fno-strict-aliasing -fno-builtin\n'
            '  -ftrivial-auto-var-init=zero -Wno-everything -Werror=implicit-function-declaration -ferror-limit=0)\n')
    w.write('# cmake -DDOTR_ORACLE=ON: every native function first calls lane R\'s differential (gen/oracle.py)\n'
            'if(DOTR_ORACLE)\n  target_compile_definitions(dotr_game_raw PRIVATE DOTR_ORACLE)\nendif()\n')
    # C-library names that clash with the PC C runtime get a ps2_ prefix in the objects (not in the source:
    # newlib redefines some of them as macros)
    w.write('set(DOTR_GAME_LIB ${CMAKE_CURRENT_BINARY_DIR}/dotr_game.lib PARENT_SCOPE)\n')
    w.write('add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/dotr_game.lib\n'
            '  COMMAND llvm-objcopy --redefine-syms=${CMAKE_CURRENT_LIST_DIR}/renames.txt $<TARGET_FILE:dotr_game_raw> ${CMAKE_CURRENT_BINARY_DIR}/dotr_game.lib\n'
            '  DEPENDS dotr_game_raw ${CMAKE_CURRENT_LIST_DIR}/renames.txt)\n')
    w.write('add_custom_target(dotr_game DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/dotr_game.lib)\n')
# data symbols with real names (the C library's environ, _impure_ptr, ...) can clash with the PC runtime too
crt_rename |= set(n for n in seen if not n.startswith('D_') and '__' not in n)
with open(os.path.join(O, 'renames.txt'), 'w', newline='\n') as w:
    for n in sorted(crt_rename): w.write('_%s _ps2_%s\n' % (n, n))

report = {
    'commit': open(os.path.join(D, 'COMMIT')).read().strip(),
    'game_files': len(game_files), 'libc_files': len(libc_files), 'sony_files_replaced': len(sony_files),
    'asm_functions_removed': len(removed_asm), 'asm_block_functions_stubbed': sorted(asm_block_funcs),
    'native_functions': len(native), 'stubbed': len(stubbed), 'local_declarations_dropped': dropped,
    'calls_remapped': len(remapped), 'register_accesses': mmio_sites[0], 'loop_poll_points': poll_sites[0], 'ee_twins': len(wide), 'oracle_hooks': oracle_hooks[0], 'stack_slack_locals': slack_locals[0], 'zeroed_copied_locals': stackslack.zeroed[0], 'ee_value_uses': ee_values[0], 'cast_calls_made_direct': uncast[0], 'fall_through_returns': fall_through[0], 'passthrough_functions': len(passthru),
    'crt_renamed': len(crt_rename), 'data_symbols': len(seen), 'functab': len(rows),
}
json.dump(report, open(os.path.join(O, 'report.json'), 'w'), indent=1)
open(os.path.join(O, 'remapped.txt'), 'w').write('\n'.join(remapped) + '\n')
open(os.path.join(O, 'asm_functions.txt'), 'w').write(
    '# functions whose behaviour is in GCC asm: cut; provided by hle/ or stubbed (logged)\n' +
    ''.join('%s %s\n' % (n_, 'provided' if n_ in provided else 'STUB') for n_ in asm_cut))
open(os.path.join(O, 'passthrough.txt'), 'w').write(
    ''.join('%s -> %s registers %s\n' % (k, v[0], v[1]) for k, v in sorted(passthru.items())))
print(json.dumps({k: (v if not isinstance(v, list) else len(v)) for k, v in report.items()}))
