#!/usr/bin/env python3
"""Read a bisect run's verdict from the c3.x sign histogram.

POSITIVE dominant -> vista CORRECT (group is not the cause)
NEGATIVE dominant -> vista BROKE   (cause is in this group)
"""
import re, sys

pat = re.compile(r'c1=\(([^)]*)\).*?c3=\(([^)]*)\)')

def verdict(path):
    pos = neg = 0
    for line in open(path, errors='ignore'):
        m = pat.search(line)
        if not m:
            continue
        c3x = float(m.group(2).split(',')[0])
        c1z = float(m.group(1).split(',')[2])
        # real camera states only: |c3.x| ~0.99 but not the 1.0 identity
        if 0.97 < abs(c3x) < 0.999:
            pos += c3x > 0
            neg += c3x < 0
    total = pos + neg
    if total == 0:
        return 'NO CAMERA STATES (run failed / never reached menu)', pos, neg
    return ('CORRECT (positive)' if pos > neg else 'BROKE (negative)'), pos, neg

for p in sys.argv[1:]:
    v, pos, neg = verdict(p)
    print(f'{p.split("/")[-1]:28} {v:26} pos={pos:4d} neg={neg:4d}')
