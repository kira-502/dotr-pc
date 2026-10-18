"""Read build/last.txt and add to gen/rules.json every function whose call sites disagree with its definition
on the argument count (they get a prototype-less declaration, or a 'review' entry when that is not ABI-safe).
usage: py gen/learn.py"""
import json, os, re
here = os.path.dirname(os.path.abspath(__file__))
t = open(os.path.join(here, '..', 'build', 'last.txt'), encoding='utf-8', errors='replace').read().split('\n')
rules_p = os.path.join(here, 'rules.json')
rules = json.load(open(rules_p)) if os.path.exists(rules_p) else {}
loose = set(rules.get('no_prototype', []))
pending = False
found = set()
for ln in t:
    if ': error: too many arguments to function call' in ln or ': error: too few arguments to function call' in ln:
        pending = True
        continue
    m = re.search(r": note: '(\w+)' declared here", ln)
    if m and pending:
        found.add(m.group(1)); pending = False
new = found - loose
rules['no_prototype'] = sorted(loose | found)
json.dump(rules, open(rules_p, 'w'), indent=1)
print('argument-count functions: %d found, %d new' % (len(found), len(new)))
