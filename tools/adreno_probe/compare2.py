import csv
import sys
import numpy as np

path = sys.argv[1] if len(sys.argv) > 1 else 'probe2_results.csv'
rows = list(csv.DictReader(open(path)))
print(f"{len(rows)} rows from {path}\n")

print(f"{'input':>12} | {'native_sin err':>15} | {'portable_sin err':>17} | {'native_cos err':>15} | {'portable_cos err':>17}")
print("-" * 90)

native_sin_worst = 0.0
portable_sin_worst = 0.0
native_cos_worst = 0.0
portable_cos_worst = 0.0
native_sin_total = 0.0
portable_sin_total = 0.0

for row in rows:
    x = np.float64(np.float32(row['input']))
    ref_sin = np.sin(x)
    ref_cos = np.cos(x)
    ns = float(row['native_sin'])
    ps = float(row['portable_sin'])
    nc = float(row['native_cos'])
    pc = float(row['portable_cos'])
    ns_err = abs(ns - ref_sin)
    ps_err = abs(ps - ref_sin)
    nc_err = abs(nc - ref_cos)
    pc_err = abs(pc - ref_cos)
    native_sin_worst = max(native_sin_worst, ns_err)
    portable_sin_worst = max(portable_sin_worst, ps_err)
    native_cos_worst = max(native_cos_worst, nc_err)
    portable_cos_worst = max(portable_cos_worst, pc_err)
    native_sin_total += ns_err
    portable_sin_total += ps_err
    if ns_err > 1e-5 or ps_err > 1e-5:
        print(f"{float(row['input']):12.2f} | {ns_err:15.3e} | {ps_err:17.3e} | {nc_err:15.3e} | {pc_err:17.3e}")

print()
print(f"SIN  worst abs error: native={native_sin_worst:.6e}  portable={portable_sin_worst:.6e}")
print(f"COS  worst abs error: native={native_cos_worst:.6e}  portable={portable_cos_worst:.6e}")
print(f"SIN  mean abs error:  native={native_sin_total/len(rows):.6e}  portable={portable_sin_total/len(rows):.6e}")
