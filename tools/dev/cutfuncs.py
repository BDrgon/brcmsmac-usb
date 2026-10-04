#!/usr/bin/env python3
"""Delete C function definitions (and their leading comment) by name.
usage: cutfuncs.py FILE NAME [NAME...]   (edits FILE in place, prints what it removed)"""
import re, sys

def find_def(lines, name):
    pat = re.compile(r'^(?:[A-Za-z_][\w\s\*]*\s\**)?' + re.escape(name) + r'\s*\(')
    for i, l in enumerate(lines):
        if not pat.match(l) or l.rstrip().endswith(';'):
            continue
        # walk forward to the opening brace (signature may span lines); skip prototypes
        j = i
        while j < len(lines) and '{' not in lines[j]:
            if lines[j].rstrip().endswith(';'):
                break
            j += 1
        if j >= len(lines) or '{' not in lines[j]:
            continue
        # closing brace at column 0
        k = j
        depth = 0
        while k < len(lines):
            depth += lines[k].count('{') - lines[k].count('}')
            if depth == 0 and lines[k].startswith('}'):
                break
            k += 1
        # return type on the previous line(s)
        s = i
        while s > 0 and lines[s - 1].strip() and not lines[s - 1].rstrip().endswith((';', '}', '*/')) \
                and not lines[s - 1].lstrip().startswith(('#', '/*', '*', '//')):
            s -= 1
        # leading comment block
        if s > 0 and lines[s - 1].rstrip().endswith('*/'):
            c = s - 1
            while c > 0 and '/*' not in lines[c]:
                c -= 1
            s = c
        e = k + 1
        if e < len(lines) and not lines[e].strip():
            e += 1
        return s, e
    return None

path, names = sys.argv[1], sys.argv[2:]
lines = open(path).read().split('\n')
for n in names:
    r = find_def(lines, n)
    if not r:
        print(f'NOT FOUND: {n}')
        continue
    s, e = r
    print(f'removed {n}: lines {s+1}-{e} ({e-s} lines)')
    del lines[s:e]
open(path, 'w').write('\n'.join(lines))
