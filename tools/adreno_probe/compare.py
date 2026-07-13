import csv
import struct
import sys
import numpy as np

def ulp_diff(a, b):
    """ULP distance between two float32 values."""
    ai = struct.unpack('<i', struct.pack('<f', np.float32(a)))[0]
    bi = struct.unpack('<i', struct.pack('<f', np.float32(b)))[0]
    if ai < 0:
        ai = 0x80000000 - ai
    if bi < 0:
        bi = 0x80000000 - bi
    return abs(ai - bi)

path = sys.argv[1] if len(sys.argv) > 1 else 'adreno610_results.csv'
label = sys.argv[2] if len(sys.argv) > 2 else 'GPU'

rows = list(csv.DictReader(open(path)))
print(f"{len(rows)} rows from {path} ({label})\n")

fields = ['sin', 'cos', 'sqrt', 'inversesqrt', 'trunc', 'floor', 'fract']
# "ULP at unit magnitude" - absolute error divided by the float32 ULP size at
# magnitude 1.0 (~1.1920929e-7). Meaningful near zero crossings, unlike raw
# ULP-of-the-result (which explodes for any result close to 0).
UNIT_ULP = np.float32(1.0).view(np.uint32)
UNIT_ULP = (np.uint32(UNIT_ULP) + 1).view(np.float32) - np.float32(1.0)

worst = {f: (0.0, None, None, None) for f in fields}
flagged = []

for row in rows:
    x = np.float32(row['input'])
    ref = {
        'sin': np.float32(np.sin(np.float64(x))),
        'cos': np.float32(np.cos(np.float64(x))),
        'sqrt': np.float32(np.sqrt(np.float64(abs(x)))),
        'inversesqrt': np.float32(1.0 / np.sqrt(np.float64(abs(x)) + 1e-20)),
        'trunc': np.float32(np.trunc(np.float64(x))),
        'floor': np.float32(np.floor(np.float64(x))),
        'fract': np.float32(np.float64(x) - np.floor(np.float64(x))),
    }
    for f in fields:
        gpu_val = np.float32(row[f])
        abs_err = abs(float(gpu_val) - float(ref[f]))
        unit_ulps = abs_err / float(UNIT_ULP)
        if unit_ulps > worst[f][0]:
            worst[f] = (unit_ulps, x, float(gpu_val), float(ref[f]))
        # Flag anything where trunc/floor DISAGREES on the integer part -
        # this is the exact failure mode hypothesized for Halo 3's
        # trunc-then-exact-equality branch chain.
        if f in ('trunc', 'floor') and gpu_val != ref[f]:
            flagged.append((x, f, float(gpu_val), float(ref[f]), abs_err))

print("Worst-case error per operation (GPU vs float64-computed float32 reference),")
print("measured in units of the float32 ULP at magnitude 1.0 (~1.19e-7):")
for f in fields:
    unit_ulps, x, gv, rv = worst[f]
    print(f"  {f:14s} max_err={unit_ulps:12.2f} unit-ulps  at input={x}  (GPU={gv!r} ref={rv!r})")

print(f"\nExact trunc/floor MISMATCHES (integer part differs - the Halo3 failure-mode class): {len(flagged)}")
for x, f, gv, rv, d in flagged[:30]:
    print(f"  input={x!r:>14} {f}: GPU={gv!r} ref={rv!r} abs_err={d!r}")
