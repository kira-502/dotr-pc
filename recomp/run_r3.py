#!/usr/bin/env python3
"""run_r3.py - build and run lane R's checks: rc_unit (R1), then rc_diff (R3) split by module range over several
low-priority processes. Results: build/rc_unit.txt, build/rc_diff_<n>.txt, a summary on stdout.

usage: run_r3.py [processes=6] [states=4] [--no-build]

A part that prints nothing for 10 minutes is reported as hung (its last line names the module it was in) and
stopped; the other parts go on.
"""
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
BIN = os.path.join(ROOT, 'build', 'Release', 'bin')
IMG = os.path.join(ROOT, 'build', 'Release', 'recomp', 'img')
NMOD = 762
LOW = 0x00004000          # BELOW_NORMAL_PRIORITY_CLASS
HANG = 600


def main(argv):
    args = [a for a in argv[1:] if not a.startswith('--')]
    procs = int(args[0]) if args else 6
    states = int(args[1]) if len(args) > 1 else 4
    if '--no-build' not in argv:
        r = subprocess.run(['cmd', '/c', os.path.join(HERE, 'build_rc.cmd'), 'rc_unit', 'rc_diff'],
                           creationflags=LOW)
        if r.returncode:
            print('build failed')
            return 1
    for exe in ('rc_unit.exe', 'rc_diff.exe'):
        print(f'{exe} built {time.ctime(os.path.getmtime(os.path.join(BIN, exe)))}')
    with open(os.path.join(ROOT, 'build', 'rc_unit.txt'), 'w') as fh:
        subprocess.run([os.path.join(BIN, 'rc_unit.exe')], stdout=fh, stderr=subprocess.STDOUT, creationflags=LOW)
    print(open(os.path.join(ROOT, 'build', 'rc_unit.txt')).read().strip().splitlines()[-1])

    step = (NMOD + procs - 1) // procs
    parts = []
    for n in range(procs):
        lo, hi = n * step, min(NMOD - 1, (n + 1) * step - 1)
        out = os.path.join(ROOT, 'build', f'rc_diff_{n}.txt')
        fh = open(out, 'w')
        p = subprocess.Popen([os.path.join(BIN, 'rc_diff.exe'), IMG, str(states), str(lo), str(hi)],
                             stdout=fh, stderr=subprocess.STDOUT, creationflags=LOW)
        parts.append([n, p, fh, out, time.time(), 0])
    while any(pt[1].poll() is None for pt in parts):
        time.sleep(10)
        for pt in parts:
            n, p, fh, out, last, size = pt
            if p.poll() is not None:
                continue
            sz = os.path.getsize(out)
            if sz != size:
                pt[4], pt[5] = time.time(), sz
            elif time.time() - last > HANG:
                lines = open(out).read().strip().splitlines()
                print(f'part {n} HUNG in: {lines[-1] if lines else "?"}')
                p.kill()
    bad = 0
    for n, p, fh, out, _, _ in parts:
        fh.close()
        lines = open(out).read().strip().splitlines()
        for ln in lines:
            if ln.startswith(('FAIL', 'no image')):
                print(f'part {n}: {ln}')
        print(f'part {n}: {lines[-1] if lines else "(no output)"}')
        bad |= p.returncode != 0
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
