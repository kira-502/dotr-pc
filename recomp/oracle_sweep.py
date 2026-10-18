#!/usr/bin/env python3
"""oracle_sweep.py - the main-program oracle over many bot-duel seeds, several at a time, low priority.

Each worker runs from its own copy of the oracle build (build/Oracle/w<n>), so logs never collide. Afterwards every
DIFF line of every seed is gathered; functions not in the KNOWN list (already reported) are printed first.

usage: oracle_sweep.py <first seed> <last seed> [workers=6] [deep]   -> build/Oracle/sweep/
       DOTR_ORACLE_FROM / DOTR_ORACLE_CALLS in the environment pass through (check late, check more calls)
"""
import os
import re
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SRC = os.path.join(ROOT, 'build', 'Oracle', 'bin')
OUT = os.path.join(ROOT, 'build', 'Oracle', 'sweep')
ISO = os.path.join(os.path.dirname(ROOT), 'ISOs', 'Yu-Gi-Oh! The Duelists of the Roses (USA).iso')
LOW = 0x00000040          # IDLE_PRIORITY_CLASS: never compete with a game being played
NOWIN = 0x08000000        # CREATE_NO_WINDOW

KNOWN = {   # functions already reported to lane P (17 Oct)
    'func_00198A88', 'SzMem_Init', 'func_00197BC0', 'malloc_extend_top', 'func_00197C18', 'func_00184810',
    'SzDuel_InitCardInfo', 'SzPad_Act', 'SzDuel_CloseMsgPopup', 'SzEffect_SetIntensity', 'lRhModel_SetupOptionObject',
    'Ta_IsExzodia', 'TaLogic_IsExzodia_0016CFF0', 'SzDuel_DrawDeck', 'SzDuel_PackDeck', 'SzDuel_AddHand',
    'SzDuel_AddGrave', 'SzDuel_PackGrave', 'SzDuel_PackHand', 'lSzDuel_LockCamera', 'lSzDuel_LockCamera2',
    'SzView_SetViewCamera', 'lSzView_CalcLook', 'Ex3d_ViewClip', 'lSzDuel_InitHandInfo', 'TaLogic022_GetSpellPlay',
    'std', '__sinit', 'sceDmaReset', 'TaLogic_TreeAloc', 'TaLogic_RootTreeAloc'}


def main(argv):
    if ',' in argv[1]:                          # a seed list: oracle_sweep.py 8,20,24 - [workers] [deep]
        seeds_given = [int(x) for x in argv[1].split(',')]
        first, last = min(seeds_given), max(seeds_given)
    else:
        first, last = int(argv[1]), int(argv[2])
        seeds_given = list(range(first, last + 1))
    workers = int(argv[3]) if len(argv) > 3 else 6
    deep = len(argv) > 4 and argv[4] == 'deep'
    os.makedirs(OUT, exist_ok=True)
    dirs = []
    for w in range(workers):
        d = os.path.join(ROOT, 'build', 'Oracle', f'w{w}')
        os.makedirs(d, exist_ok=True)
        for f in ('DotR.exe', 'dotr.dll'):
            shutil.copy2(os.path.join(SRC, f), os.path.join(d, f))
        dirs.append(d)
    seeds = list(seeds_given)
    running = {}
    t0 = time.time()
    while seeds or running:
        for w in range(workers):
            if w in running and running[w][0].poll() is not None:
                p, seed, fh = running.pop(w)
                fh.close()
                print(f'seed {seed} done (exit {p.returncode}, {time.time() - t0:.0f} s)', flush=True)
            if w not in running and seeds:
                seed = seeds.pop(0)
                env = dict(os.environ, DOTR_ORACLE_LOG=os.path.join(OUT, f'oracle-{seed}.log'))
                if deep:
                    env['DOTR_ORACLE_DEEP'] = '1'
                fh = open(os.path.join(OUT, f'run-{seed}.out'), 'w')
                p = subprocess.Popen([os.path.join(dirs[w], 'DotR.exe'), '--boot', '--headless', '--fast',
                                      '--bot-duel', str(seed), '--bot-log', os.path.join(OUT, f'bot-{seed}.log'),
                                      '--iso', ISO], cwd=dirs[w], env=env, stdout=fh, stderr=subprocess.STDOUT,
                                     creationflags=LOW | NOWIN)
                running[w] = (p, seed, fh)
        time.sleep(5)

    funcs = {}
    for f in sorted(os.listdir(OUT)):
        m = re.match(r'oracle-(\d+)\.log$', f)
        if not m:
            continue
        for line in open(os.path.join(OUT, f), errors='replace'):
            if line.startswith(('DIFF', 'diff-hle')):
                parts = line.split()
                name = parts[3]
                funcs.setdefault(name, []).append((int(m.group(1)), line.strip()))
    new = {n: v for n, v in funcs.items() if n not in KNOWN}
    print(f'{len(funcs)} functions differ over seeds {first}-{last}; {len(new)} NEW:')
    for n, v in sorted(new.items(), key=lambda kv: -len(kv[1])):
        print(f'  {n}: {len(v)} times, seeds {sorted({s for s, _ in v})[:8]}')
        print(f'    {v[0][1][:300]}')
    for f in sorted(os.listdir(OUT)):
        if f.startswith('run-'):
            txt = open(os.path.join(OUT, f), errors='replace').read()
            if 'crashed' in txt or 'stuck' in txt:
                print(f'{f}: {txt.strip().splitlines()[0][:200]}')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
