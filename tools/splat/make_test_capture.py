#!/usr/bin/env python3
"""A synthetic Gaussian-splat capture of a standing person, for measuring the splat pass.

Writes a .ply in the reference 3D Gaussian Splatting layout (positions, nx/ny/nz, f_dc_0..2,
f_rest_0..44, opacity as a logit, scales as logs, a (w, x, y, z) rotation) of the size of a real
capture of a person — 600,000 splats by default, spherical harmonics of degree 3, ~150 MB — so
the splat pass can be measured on any machine without a real capture (the test assets stay out
of the repository). The body is a stack of ellipsoid shells of thin disc-shaped splats (a trained
scene's splats are thin: two axes of a few millimetres, the third a tenth of that), on a floor
disc of larger discs, with a cloud of faint floaters about it; it stands ~1.8 m tall, facing +z,
and is written in the capture frame (y down, z toward the camera) that the importer turns over.
PESSIMISTIC in its fill: the floor discs are randomly oriented where a real floor's lie flat, and
the body's shell is dense and opaque. Needs numpy.

    python tools/splat/make_test_capture.py [count] [out.ply]

The measuring levers: POSESTUDIO_GPU_PERF=1 (the desktop frames' GPU time and the splats' share,
once a second, rendering continuously), POSESTUDIO_SPLAT_TINY=1 (every quad a pixel: the vertex
stage alone), POSESTUDIO_SPLAT_SCALE=<0.25..1> (the photo layer's size), POSESTUDIO_SPLAT_TRACE=1
(the sort's cost). The same picture in a headset: POSESTUDIO_VR_PERF=1 with VR Mode on.
"""
import sys

import numpy as np

n_total = int(sys.argv[1]) if len(sys.argv) > 1 else 600_000
out = sys.argv[2] if len(sys.argv) > 2 else "person_disc.ply"
rng = np.random.default_rng(11)
n_body = int(n_total * 0.75)
n_floor = int(n_total * 0.20)
n_fuzz = n_total - n_body - n_floor


def shell(count, cx, cy, cz, rx, ry, rz):
    u = rng.random(count) * 2 * np.pi
    v = np.arccos(2 * rng.random(count) - 1)
    r = 1.0 + rng.normal(0, 0.01, count)
    x = cx + rx * r * np.sin(v) * np.cos(u)
    y = cy + ry * r * np.cos(v)
    z = cz + rz * r * np.sin(v) * np.sin(u)
    return np.stack([x, y, z], 1)


parts = [
    shell(int(n_body * 0.18), -0.10, 0.45, 0, 0.09, 0.45, 0.09),  # legs
    shell(int(n_body * 0.18), 0.10, 0.45, 0, 0.09, 0.45, 0.09),
    shell(int(n_body * 0.40), 0, 1.15, 0, 0.20, 0.32, 0.12),  # torso
    shell(int(n_body * 0.12), 0, 1.62, 0, 0.11, 0.13, 0.12),  # head
    shell(int(n_body * 0.06), -0.28, 1.05, 0, 0.05, 0.30, 0.05),  # arms
    shell(n_body - int(n_body * 0.94), 0.28, 1.05, 0, 0.05, 0.30, 0.05),
]
body = np.concatenate(parts)
a = rng.random(n_floor) * 2 * np.pi
rr = 1.5 * np.sqrt(rng.random(n_floor))
floor = np.stack([rr * np.cos(a), rng.normal(0, 0.003, n_floor), rr * np.sin(a)], 1)
fuzz = np.stack([rng.normal(0, 0.4, n_fuzz), rng.random(n_fuzz) * 1.9, rng.normal(0, 0.4, n_fuzz)], 1)
pos = np.concatenate([body, floor, fuzz]).astype(np.float32)
n = len(pos)


def discs(count, lo, hi):
    s = rng.uniform(lo, hi, (count, 2))
    t = s.mean(1, keepdims=True) * rng.uniform(0.05, 0.15, (count, 1))
    return np.concatenate([s, t], 1)


sc = np.concatenate([discs(len(body), 0.002, 0.005), discs(n_floor, 0.008, 0.02), discs(n_fuzz, 0.005, 0.015)])
logscale = np.log(sc).astype(np.float32)
op = np.concatenate([rng.uniform(0.6, 0.99, len(body)), rng.uniform(0.7, 0.99, n_floor), rng.uniform(0.02, 0.3, n_fuzz)])
logit = np.log(op / (1 - op)).astype(np.float32)
q = rng.normal(size=(n, 4))
q /= np.linalg.norm(q, axis=1, keepdims=True)
q = q.astype(np.float32)
C0 = 0.28209479177387814
col = np.concatenate([
    np.tile([0.75, 0.55, 0.45], (len(body), 1)) + rng.normal(0, 0.05, (len(body), 3)),
    np.tile([0.5, 0.5, 0.48], (n_floor, 1)) + rng.normal(0, 0.08, (n_floor, 3)),
    np.tile([0.4, 0.4, 0.4], (n_fuzz, 1)),
])
f_dc = ((np.clip(col, 0, 1) - 0.5) / C0).astype(np.float32)
f_rest = rng.normal(0, 0.05, (n, 45)).astype(np.float32)
pos_file = pos * np.array([1, -1, -1], np.float32)  # the capture frame: y down, z toward the camera
props = (["x", "y", "z", "nx", "ny", "nz", "f_dc_0", "f_dc_1", "f_dc_2"] + [f"f_rest_{i}" for i in range(45)]
         + ["opacity", "scale_0", "scale_1", "scale_2", "rot_0", "rot_1", "rot_2", "rot_3"])
header = ("ply\nformat binary_little_endian 1.0\nelement vertex %d\n" % n
          + "".join("property float %s\n" % p for p in props) + "end_header\n")
rec = np.concatenate([pos_file, np.zeros((n, 3), np.float32), f_dc, f_rest, logit[:, None], logscale, q], 1).astype(np.float32)
with open(out, "wb") as f:
    f.write(header.encode("ascii"))
    f.write(rec.tobytes())
print("wrote", out, n, "splats", rec.nbytes / 1e6, "MB")
