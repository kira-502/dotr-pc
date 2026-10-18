"""List unique errors from build/last.txt as file:line message (plus the note naming a function, if any)."""
import re, sys
t = open(sys.argv[1] if len(sys.argv) > 1 else 'build/last.txt', encoding='utf-8', errors='replace').read().split('\n')
seen = set()
for i, ln in enumerate(t):
    m = re.search(r'gen/out/(?:src|include)[/\\](\S+?)\((\d+),\d+\): error: (.*)$', ln)
    if not m: continue
    note = ''
    for j in range(i + 1, min(len(t), i + 8)):
        n = re.search(r": note: '(\w+)' declared here|: note: previous (?:declaration|definition) is here", t[j])
        if n:
            note = ' [%s]' % (n.group(1) or 'prev@' + t[j].split('(')[0].split('/')[-1] + ':' + t[j].split('(')[1].split(',')[0])
            break
    k = (m.group(1), m.group(2), m.group(3))
    if k in seen: continue
    seen.add(k)
    print('%s:%s %s%s' % (m.group(1), m.group(2), m.group(3)[:140], note))
