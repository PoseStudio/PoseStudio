# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Ion Hatzithomas and the PoseStudio contributors.
# Licensed under the GNU AGPL v3 with the additional terms in LICENSE-ADDITIONAL-TERMS.txt.
"""Tabulates a sweep's failed gates (see run.sh): per rig, how many are NEAR-MISSES — a gate set
on the reference rigs, exceeded by no more than a quarter — and how many are REAL, and lists the
real ones. Rows of the left/right asymmetry gate are listed apart and not counted. The exit code
is the number of real failures.
    python summarize.py <results folder> [near-miss share, default 0.25]
"""
import glob
import os
import re
import sys

results = sys.argv[1]
near_share = float(sys.argv[2]) if len(sys.argv) > 2 else 0.25
near_total = real_total = 0
real_rows = []
lopsided_rows = []
for path in sorted(glob.glob(os.path.join(results, "*.txt"))):
    rig = os.path.basename(path)[:-4]
    phase = None
    verdict = "did not finish"
    near = real = 0
    for line in open(path, encoding="utf-8", errors="replace"):
        line = line.rstrip()
        m = re.match(r"^\[(PASS|FAIL|SKIP)\] (.*)$", line)
        if m:
            phase = m.group(2)
            continue
        if re.match(r"^\d+ phase\(s\):", line):
            verdict = line
        if "<-- FAIL" not in line:
            continue
        g = re.match(r"^\s+(.*?)\s{2,}(-?[0-9.]+)\s+(<=|>=)\s+(-?[0-9.]+)", line)
        if not g:
            continue
        value, op, limit = float(g.group(2)), g.group(3), float(g.group(4))
        if "asymmetry" in g.group(1):
            # (The left/right gate is applied only on a rig that is exactly symmetric at rest; on
            # such a CHARACTER a lopsided result is the body's own as often as a symmetry break -
            # skin that differs left to right kneels askew on a symmetric skeleton: listed apart.)
            lopsided_rows.append(f"  {rig}: {phase[:64]} | gained {value:g} mm")
            continue
        over =(value - limit if op == "<=" else limit - value) / max(abs(limit), 1e-6)
        if over <= near_share:
            near += 1
        else:
            real += 1
            real_rows.append(f"  {rig}: {phase[:64]} | {g.group(1)[:48]} = {value:g} (gate {op} {limit:g})")
    print(f"{rig:24s} {verdict:40s} near-misses {near:2d}   real {real:2d}")
    near_total += near
    real_total += real
print(f"TOTAL: near-misses {near_total}, real {real_total}")
if real_rows:
    print("\n".join(real_rows))
if lopsided_rows:
    print("LOPSIDED after a symmetric gesture (the body's own asymmetry, or a symmetry break - look):")
    print("\n".join(lopsided_rows))
sys.exit(min(real_total, 255))
