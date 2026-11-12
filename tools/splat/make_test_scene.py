#!/usr/bin/env python3
# A small synthetic Gaussian splat SCENE, for looking at the splat pass and for the render check
# (tools/ikscripts/render/splats.txt): a checkered floor at y = 0, six metres square; a shaded
# sphere of half a metre resting on it at the origin; a wall four metres wide and two tall, three
# metres off; and a row of five soft balls at head height whose color tints with the view (a
# degree-1 harmonic). 18,000 splats, 4.5 MB, in the reference 3D Gaussian Splatting .ply layout
# (binary little-endian) and the capture's Y-DOWN frame, which the importer turns over — so in
# the scene the wall stands at z = +3 and the balls at z = -1. Needs nothing but Python.
#
#     python tools/splat/make_test_scene.py [out.ply]        (default: test_scene.ply here)
#
# (make_test_capture.py beside it writes a capture of a standing PERSON at a real capture's size,
# for measuring the pass.) The test assets stay out of the repository.
import math, os, random, struct, sys

out = sys.argv[1] if len(sys.argv) > 1 else 'test_scene.ply'
if os.path.dirname(out):
    os.makedirs(os.path.dirname(out), exist_ok=True)
random.seed(7)
C0 = 0.28209479177387814

splats = []  # (x, y, z, r, g, b, opacity, sx, sy, sz, qw, qx, qy, qz, rest[45])

def add(p, rgb, opacity, scale, quat=(1, 0, 0, 0), rest=None):
    splats.append((p, rgb, opacity, scale, quat, rest or [0.0] * 45))

def rand_quat():
    u1, u2, u3 = random.random(), random.random(), random.random()
    a, b = math.sqrt(1 - u1), math.sqrt(u1)
    return (b * math.cos(2 * math.pi * u3), a * math.sin(2 * math.pi * u2), a * math.cos(2 * math.pi * u2), b * math.sin(2 * math.pi * u3))

# The floor: 6 m square, 3 cm cells of flat discs (thin in y), a checker of two greys.
step = 0.06
n = int(6.0 / step)
for i in range(n):
    for j in range(n):
        x = -3.0 + (i + 0.5) * step
        z = -3.0 + (j + 0.5) * step
        light = ((i // 5) + (j // 5)) % 2 == 0
        c = (0.75, 0.72, 0.68) if light else (0.35, 0.36, 0.40)
        add((x, 0.0, z), c, 0.95, (0.05, 0.004, 0.05), (1, 0, 0, 0))

# The sphere: radius 0.5, centre 0.5 above the floor (y-down: y = -0.5), shaded as if lit from
# above-left; splats on the surface, flattened along the normal.
for k in range(6000):
    th = math.acos(1 - 2 * random.random())
    ph = 2 * math.pi * random.random()
    nx, ny, nz = math.sin(th) * math.cos(ph), math.cos(th), math.sin(th) * math.sin(ph)
    p = (nx * 0.5, -0.5 + ny * 0.5, nz * 0.5 + 0.0)
    # lit from (−1, −1 (up in y-down), −0.5)
    l = (-0.58, -0.58, -0.58)
    lam = max(0.0, nx * l[0] + ny * l[1] + nz * l[2])
    base = (0.85, 0.30, 0.25)
    c = tuple(min(1.0, b * (0.25 + 0.75 * lam)) for b in base)
    # a tangent frame: quaternion rotating +z onto the normal, scales (0.03, 0.03, 0.004)
    # (rotation from z axis to n)
    ax = (-ny, nx, 0.0)  # z × n = (-n.y, n.x, 0)
    al = math.sqrt(ax[0] ** 2 + ax[1] ** 2 + ax[2] ** 2)
    if al < 1e-6:
        q = (1, 0, 0, 0) if nz > 0 else (0, 1, 0, 0)
    else:
        ang = math.acos(max(-1.0, min(1.0, nz)))
        s = math.sin(ang / 2) / al
        q = (math.cos(ang / 2), ax[0] * s, ax[1] * s, ax[2] * s)
    add(p, c, 0.9, (0.03, 0.03, 0.004), q)

# The back wall at z = -3 (behind the sphere from the default view), a blue-grey, 4 m wide, 2 m tall (y from 0 to -2).
for i in range(int(4.0 / step)):
    for j in range(int(2.0 / step)):
        x = -2.0 + (i + 0.5) * step
        y = -(j + 0.5) * step
        add((x, y, -3.0), (0.45, 0.52, 0.62), 0.95, (0.05, 0.05, 0.004), (1, 0, 0, 0))

# View-dependent splats: a row of balls at head height whose colour tints with the view (a
# degree-1 term along x: f_rest for the x-basis coefficient, index 2 of each channel's 15).
for k in range(5):
    x = -1.6 + k * 0.8
    rest = [0.0] * 45
    # channel-major: R's 15 first. Degree-1 basis order in the reference: (y, z, x) -> coefficient indices 0, 1, 2 of the 15.
    rest[2] = 0.8      # R brightens looking from +x
    rest[15 + 2] = -0.8  # G darkens
    add((x, -1.6, 1.0), (0.6, 0.6, 0.6), 1.0, (0.12, 0.12, 0.12), (1, 0, 0, 0), rest)

props = ['x', 'y', 'z', 'nx', 'ny', 'nz', 'f_dc_0', 'f_dc_1', 'f_dc_2'] + ['f_rest_%d' % i for i in range(45)] + \
        ['opacity', 'scale_0', 'scale_1', 'scale_2', 'rot_0', 'rot_1', 'rot_2', 'rot_3']
header = 'ply\nformat binary_little_endian 1.0\nelement vertex %d\n' % len(splats) + ''.join('property float %s\n' % p for p in props) + 'end_header\n'

def logit(o):
    o = min(max(o, 1e-4), 1 - 1e-4)
    return math.log(o / (1 - o))

with open(out, 'wb') as f:
    f.write(header.encode('ascii'))
    for (p, rgb, opacity, scale, quat, rest) in splats:
        dc = [(c - 0.5) / C0 for c in rgb]
        rec = list(p) + [0.0, 0.0, 0.0] + dc + rest + [logit(opacity)] + [math.log(s) for s in scale] + list(quat)
        f.write(struct.pack('<%df' % len(rec), *rec))
print('wrote', out, len(splats), 'splats')
