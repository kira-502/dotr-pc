"""Hardware register accesses for the port generator.

The game and Sony's libraries touch EE/GS registers at fixed addresses, e.g.
    *(volatile u64 *)0x12000000 = pmode;            while (*(volatile s32 *)0x1000A000 & 0x100) ...
On the PC those pages are plain memory (the loader maps them), so a read or write by itself does nothing.
This pass rewrites every constant-address access in 0x10000000..0x1200FFFF into a call to the runtime's
virtual hardware (runtime/mmio.c), which keeps the register values and performs the side effects (start a DMA,
set a GS display register, ...):
    write: hw_mmio_w(ADDR, (unsigned long long)(EXPR), SIZE);      128-bit: hw_mmio_w128(ADDR, &tmp)
    read:  ((TYPE)hw_mmio_r(ADDR, SIZE))
Object-like macros that name such an address (#define D1_MADR ((volatile u32 *)0x10009010)) are expanded first.
"""
import re

TYPES = r'(?:unsigned\s+long\s+long|long\s+long|unsigned\s+int|unsigned\s+short|unsigned\s+char|signed\s+char|' \
        r'u8|s8|u16|s16|u32|s32|u64|s64|int|short|char|dk_int128|__int128)'
SIZE = {'u8': 1, 's8': 1, 'char': 1, 'unsigned char': 1, 'signed char': 1, 'u16': 2, 's16': 2, 'short': 2,
        'unsigned short': 2, 'u32': 4, 's32': 4, 'int': 4, 'unsigned int': 4, 'u64': 8, 's64': 8,
        'long long': 8, 'unsigned long long': 8, 'dk_int128': 16, '__int128': 16}
ADDR = r'(0x1[0-2]0[0-9A-Fa-f]{5})'
ACCESS = re.compile(r'\*\s*(\(\s*)?\(\s*(?:volatile\s+)?(' + TYPES + r')\s*\*\s*\)\s*' + ADDR)


def norm(t):
    return ' '.join(t.split())


def rewrite(text, addr_names=None):
    """addr_names: {NAME: '0x1...'} from the decomp's include/ee_regs.h (#define T1_COUNT_ADDR 0x10000800): the named
    decomp writes registers by name, so the names become their addresses first"""
    n = 0
    if addr_names:
        text = re.sub(r'\b(' + '|'.join(map(re.escape, sorted(addr_names, key=len, reverse=True))) + r')\b',
                      lambda m: addr_names[m.group(1)], text)
    # expand object-like macros naming a register pointer
    macros = {}
    for m in re.finditer(r'(?m)^[ \t]*#[ \t]*define[ \t]+(\w+)[ \t]+\(\s*\(\s*(?:volatile\s+)?(' + TYPES +
                         r')\s*\*\s*\)\s*' + ADDR + r'\s*\)[ \t]*$', text):
        macros[m.group(1)] = (norm(m.group(2)), m.group(3))
    for name, (typ, addr) in macros.items():
        text = re.sub(r'\*\s*%s\b' % name, '*(volatile %s *)%s' % (typ, addr), text)
    out, pos = [], 0
    for m in ACCESS.finditer(text):
        if m.start() < pos:
            continue
        typ, addr = norm(m.group(2)), m.group(3).upper().replace('0X', '0x')
        size = SIZE[typ]
        mend = m.end()
        if m.group(1):                      # *((T *)ADDR): its own closing parenthesis
            cm = re.match(r'\s*\)', text[mend:])
            if not cm:
                continue
            mend += cm.end()
        after = text[mend:]
        w = re.match(r'\s*=(?!=)', after)
        # a compound assignment like |= is a read followed by a write
        cw = re.match(r'\s*(\|=|&=|\^=|\+=|-=)', after)
        if w or cw:
            end = text.find(';', mend)
            if end < 0 or '\n' in text[mend:end]:
                continue                     # not a plain statement (macro body, multi-line): left alone
            rhs = text[mend + (w or cw).end():end]
            if cw:
                rhs = '((%s)hw_mmio_r(%s, %d)) %s (%s)' % (typ, addr, size, cw.group(1)[0], rhs)
            if size == 16:
                stmt = '{ dk_int128 mmio_t__ = (%s); hw_mmio_w128(%s, &mmio_t__); }' % (rhs.strip(), addr)
            else:
                stmt = 'hw_mmio_w(%s, (unsigned long long)(%s), %d)' % (addr, rhs.strip(), size)
            out.append(text[pos:m.start()]); out.append(stmt)
            pos = end
        else:
            if size == 16:
                rd = '(*(dk_int128 *)hw_mmio_p(%s))' % addr
            else:
                rd = '((%s)hw_mmio_r(%s, %d))' % (typ, addr, size)
            out.append(text[pos:m.start()]); out.append(rd)
            pos = mend
        n += 1
    out.append(text[pos:])
    return ''.join(out), n
