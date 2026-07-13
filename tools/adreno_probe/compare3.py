import csv
import sys
import numpy as np

path = sys.argv[1] if len(sys.argv) > 1 else 'explog_results.csv'
rows = list(csv.DictReader(open(path)))
print(f"{len(rows)} rows from {path}\n")

worst_exp = (0.0, None)
worst_log = (0.0, None)
mova_mismatches = []

for row in rows:
    x = np.float32(row['input'])
    ref_exp2 = np.float32(np.exp2(np.float64(x)))
    ref_log2 = np.float32(np.log2(np.float64(abs(x)) + 1e-30))
    gpu_exp2 = np.float32(row['exp2'])
    gpu_log2 = np.float32(row['log2'])

    exp_err = abs(float(gpu_exp2) - float(ref_exp2))
    exp_rel = exp_err / max(abs(float(ref_exp2)), 1e-30)
    log_err = abs(float(gpu_log2) - float(ref_log2))

    if exp_rel > worst_exp[0]:
        worst_exp = (exp_rel, (float(x), float(gpu_exp2), float(ref_exp2)))
    if log_err > worst_log[0]:
        worst_log = (log_err, (float(x), float(gpu_log2), float(ref_log2)))

    # mova check: a0 = (int)clamp(floor(x+0.5), -256, 255) - reference computed
    # the same way in numpy for comparison.
    ref_a0f = np.clip(np.floor(np.float64(x) + 0.5), -256.0, 255.0)
    ref_a0i = int(ref_a0f)
    gpu_a0f = float(row['nclamp_floor'])
    gpu_a0i = int(float(row['convertftos']))
    if gpu_a0i != ref_a0i:
        mova_mismatches.append((float(x), gpu_a0f, ref_a0f, gpu_a0i, ref_a0i))

print(f"EXP2 worst relative error: {worst_exp[0]:.6e} at (x,gpu,ref)={worst_exp[1]}")
print(f"LOG2 worst absolute error: {worst_log[0]:.6e} at (x,gpu,ref)={worst_log[1]}")
print(f"\nmova (address register) mismatches: {len(mova_mismatches)}")
for x, gf, rf, gi, ri in mova_mismatches[:20]:
    print(f"  x={x!r:>14}  GPU: floor+clamp={gf!r} -> int={gi}   ref: floor+clamp={rf!r} -> int={ri}")
