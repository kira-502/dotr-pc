"""EE data-model fixes for the port generator (found by the layout test, gen/layout.py):

* `long` is 64 bits for the EE compilers (Metrowerks and ee-gcc); on Windows it is 32. Every plain `long`
  becomes `long long`.
* Bit-fields: GCC/Metrowerks pack consecutive bit-fields across declared types; the Microsoft ABI starts a new
  storage unit whenever the declared type's size changes. A run of adjacent bit-fields with mixed type sizes is
  rewritten to one storage type just big enough for the whole run (signedness kept per field), which the
  Microsoft ABI packs the same way. The layout test confirms every rewritten struct.
"""
import re

LONG = re.compile(r'(?<![\w])(?<!long )long(?![\w])(?! long)(?!\s+long)')


def fix_long(text):
    # never touch "long long"; protect it first
    t = re.sub(r'\blong\s+long\b', '\x00LL\x00', text)
    t = re.sub(r'\blong\s+double\b', '\x00LD\x00', t)          # long double stays (8 bytes on both)
    t = re.sub(r'\blong\b', 'long long', t)
    return t.replace('\x00LL\x00', 'long long').replace('\x00LD\x00', 'long double')


SIZES = {'s8': 8, 'u8': 8, 'char': 8, 'signed char': 8, 'unsigned char': 8, 's16': 16, 'u16': 16, 'short': 16,
         'unsigned short': 16, 'signed short': 16, 's32': 32, 'u32': 32, 'int': 32, 'unsigned int': 32,
         'unsigned': 32, 'signed': 32, 'signed int': 32, 's64': 64, 'u64': 64, 'long long': 64,
         'unsigned long long': 64, 'signed long long': 64}
UNSIGNED = {'u8', 'u16', 'u32', 'u64', 'unsigned char', 'unsigned short', 'unsigned int', 'unsigned',
            'unsigned long long'}
BFLINE = re.compile(r'^(\s*)((?:unsigned |signed )?(?:s8|u8|s16|u16|s32|u32|s64|u64|char|short|int|long long|unsigned|signed))'
                    r'\s+([^;]*?:\s*\d+[^;]*);(.*)$')


BFPART = re.compile(r'^\s*\w*\s*:\s*\d+\s*(__attribute__\s*\(\([^)]*\)\))?\s*$')


def struct_name_at(lines, j):
    """name of the struct whose body contains line j: the typedef name after its '}', or its tag."""
    for k in range(j, min(len(lines), j + 400)):
        m = re.match(r'^\s*\}\s*(\w+)?\s*;', lines[k])
        if m:
            if m.group(1): return m.group(1)
            break
    for k in range(j, max(-1, j - 400), -1):
        m = re.search(r'\b(?:struct|union)\s+(\w+)\s*\{', lines[k])
        if m: return m.group(1)
        if re.search(r'\b(?:struct|union)\s*\{', lines[k]): return None
    return None


def fix_bitfields(text, keep=(), noalign=(), half_start=()):
    lines = text.split('\n')
    out = []
    i = 0
    rewritten = 0
    while i < len(lines):
        run = []
        j = i
        while j < len(lines):
            m = BFLINE.match(lines[j])
            if not m or not all(BFPART.match(p) for p in m.group(3).split(',')): break
            run.append((j, m))
            j += 1
        if len(run) >= 2:
            fields = []   # (indent, type, decls-text, tail)
            for _, m in run:
                fields.append((m.group(1), m.group(2).strip(), m.group(3), m.group(4)))
            sizes = set(SIZES.get(t, 32) for _, t, _, _ in fields)
            total = 0
            for _, t, decls, _ in fields:
                for d in decls.split(','):
                    w = re.search(r':\s*(\d+)', d)
                    if w: total += int(w.group(1))
            unit = next(u for u in (8, 16, 32, 64) if u >= total) if total <= 64 else None
            widest = max(sizes)
            nxt = next((l.strip() for l in lines[j:] if l.strip()), '}')
            followed = not nxt.startswith('}')
            sname = struct_name_at(lines, i)
            if sname in half_start and total > 16:
                # the run starts at a 2-byte offset inside a 4-byte unit (the card records' flags at 0x56): GCC fills
                # the unit's 16 free bits, then starts a new unit. Two containers do the same: 16-bit, then the rest.
                used, k = 0, 0
                for k, (_, t, decls, _) in enumerate(fields):
                    w = sum(int(x) for x in re.findall(r':\s*(\d+)', decls))
                    if used + w > 16: break
                    used += w
                rest_bits = total - used
                rest_unit = next(u for u in (8, 16, 32, 64) if u >= rest_bits)
                for n, (ind, t, decls, tail) in enumerate(fields):
                    sign = 'u' if t in UNSIGNED else 's'
                    out.append('%s%s%d %s; /* port: was %s */%s' % (ind, sign, 16 if n < k else max(rest_unit, 32),
                                                                     decls, t, tail))
                rewritten += 1
                i = j
                continue
            force = False
            if sname in noalign and unit and widest > unit:
                widest = unit      # GCC started this run inside an earlier unit: no alignment of its own
                force = True
            if unit and (force or len(sizes) > 1 or (widest > unit and followed)) and sname not in keep:
                for k, (ind, t, decls, tail) in enumerate(fields):
                    sign = 'u' if t in UNSIGNED else 's'
                    # keep the struct's alignment that the wider declared type gave it on the EE
                    attr = ' __attribute__((aligned(%d)))' % (widest // 8) if k == 0 and widest > unit else ''
                    if attr:
                        first, _, rest = decls.partition(',')
                        decls_k = first + attr + ((',' + rest) if rest else '')
                    else:
                        decls_k = decls
                    out.append('%s%s%d %s; /* port: was %s */%s' % (ind, sign, unit, decls_k, t, tail))
                rewritten += 1
                i = j
                continue
        if run:
            for k, _ in run: out.append(lines[k])
            i = j
            continue
        out.append(lines[i])
        i += 1
    return '\n'.join(out), rewritten
