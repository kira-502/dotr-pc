"""Record layout check, bit-fields included (port generator).

clang lays out every struct of the generated sources twice: for the EE's rules (mipsel, SysV/GCC bit-field
placement, like Metrowerks) and for the PC build's target (i686-pc-windows-gnu, -mno-ms-bitfields). Every
member offset, bit-field position and record size must agree; a difference is printed and the exit code is 1.
The value layout test (gen/layout.py) can't see bit-fields; this one can.

  py gen/bflayout.py [gen/out]
"""
import os, re, subprocess, sys
from concurrent.futures import ThreadPoolExecutor

LLVM = r'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\Llvm\bin\clang.exe'
TARGETS = {
    'ee': ['--target=mipsel-unknown-linux-gnu'],
    'pc': ['--target=i686-pc-windows-msvc'],
}


def layouts(src, inc, target):
    cmd = [LLVM, *TARGETS[target], '-fsyntax-only', '-w', '-ferror-limit=0', '-fwrapv', '-fno-builtin',
           '-include', 'port.h', '-I', inc, '-Xclang', '-fdump-record-layouts-complete', src]
    r = subprocess.run(cmd, capture_output=True, text=True, errors='replace')
    recs, cur, name = {}, None, None
    for ln in r.stdout.splitlines():
        if ln.startswith('*** Dumping AST Record Layout'):
            cur, name = [], None
            continue
        if cur is None: continue
        m = re.match(r'^\s*(\d+(?::\d+-\d+)?)\s*\|\s*(.*)$', ln)
        if m:
            if name is None: name = m.group(2).strip()
            cur.append('%s | %s' % (m.group(1), m.group(2).strip()))
            continue
        m = re.search(r'\[sizeof=(\d+),.*align=(\d+)', ln)
        if m and name:
            cur.append('size %s align %s' % m.groups())
            recs[name] = cur
            cur = None
    return recs


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), 'out')
    inc = os.path.join(out, 'include')
    srcs = sorted(os.path.join(out, 'src', f) for f in os.listdir(os.path.join(out, 'src')) if f.endswith('.c'))
    jobs = [(s, t) for s in srcs for t in TARGETS]
    with ThreadPoolExecutor(max_workers=max(2, (os.cpu_count() or 4) // 2)) as ex:
        res = dict(zip(jobs, ex.map(lambda j: layouts(j[0], inc, j[1]), jobs)))
    bad, records = 0, 0
    for s in srcs:
        ee, pc = res[(s, 'ee')], res[(s, 'pc')]
        for name in sorted(set(ee) & set(pc)):
            records += 1
            if ee[name] != pc[name]:
                bad += 1
                print('DIFF %s: %s' % (os.path.basename(s), name))
                for a, b in zip(ee[name], pc[name]):
                    if a != b: print('   ee %s\n   pc %s' % (a, b)); break
    print('%d records compared, %d differ' % (records, bad))
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
