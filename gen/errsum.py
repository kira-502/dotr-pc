"""Summarise build/last.txt (clang errors) by message kind and function. usage: py errsum.py [n]"""
import re, sys, collections
t = open(sys.argv[1] if len(sys.argv) > 1 else 'build/last.txt', encoding='utf-8', errors='replace').read()
errs = re.findall(r'^(.*?)\((\d+),\d+\): error: (.*)$', t, re.M)
seen = set(); uniq = []
for e in errs:
    if e in seen: continue
    seen.add(e); uniq.append(e)
kinds = collections.Counter(re.sub(r"'[^']*'", "X", m)[:90] for f, l, m in uniq)
print('errors (unique):', len(uniq))
for k, c in kinds.most_common(40): print('%5d  %s' % (c, k))
files = collections.Counter(f.split('\\')[-1] for f, l, m in uniq)
print(files.most_common(15))
