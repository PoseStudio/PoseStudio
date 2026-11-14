"""Synthetic test assets for THE SMOOTHING BY ANGLE (src/viewport/import/smoothnormals.h; the
Materials tab's Smooth by Angle), what tools/ikscripts/materials/smoothing.txt runs on.

    python tools/materials/make_smoothing_assets.py <folder>

Writes into <folder>:

  smooth_faceted.obj    a 32 x 16 sphere with ONE NORMAL PER FACE — what a modelling package
                        exports unless its surface was set to smooth. Faceted in every viewer that
                        draws a file's normals; the import smooths it by angle.
  smooth_authored.obj   the same sphere with smooth normals of its own: left exactly as it is.
  smooth_nonormals.obj  the same sphere with no normals at all: the reader makes some, and the
                        import smooths it by angle instead.
  smooth_shapes.obj     three zones of one model, each with a normal per face: a sphere (Ball),
  smooth_shapes.mtl     a cube (Box) and a capped 32-sided cylinder (Can) — smoothing rounds the
                        ball and the can's side and keeps the box's edges and the can's caps.
                        (Box and Can share ONE material's values: the Materials tab's linked zones.)

The spheres stand side by side (x = -1.15, 0, 1.15) on the floor, half a metre across; the shapes
file's three stand in a row in front of them (z = 1.5).
"""
import math
import os
import sys

out = sys.argv[1]
os.makedirs(out, exist_ok=True)


def face_normal(points):
    a, b, c = points[0], points[1], points[2]
    e1 = [b[k] - a[k] for k in range(3)]
    e2 = [c[k] - a[k] for k in range(3)]
    n = (e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0])
    length = math.sqrt(sum(x * x for x in n)) or 1.0
    return tuple(x / length for x in n)


def sphere(cx, cy, cz, r, around=32, down=16):
    """Unit directions, UVs and faces (counter-clockwise seen from outside) of a UV sphere."""
    v, vt, faces = [], [], []
    for j in range(down + 1):
        th = math.pi * j / down
        for i in range(around + 1):
            ph = 2 * math.pi * i / around
            v.append((math.sin(th) * math.cos(ph), math.cos(th), math.sin(th) * math.sin(ph)))
            vt.append((i / around, 1 - j / down))
    for j in range(down):
        for i in range(around):
            a, b = j * (around + 1) + i, j * (around + 1) + i + 1
            c, d = a + around + 1, b + around + 1
            if j == 0:
                faces.append((a, d, c))
            elif j == down - 1:
                faces.append((a, b, c))
            else:
                faces.append((a, b, d, c))
    points = [(cx + r * p[0], cy + r * p[1], cz + r * p[2]) for p in v]
    return points, v, vt, faces


class Obj:
    """An OBJ being written: positions, UVs and normals pooled, faces by material."""

    def __init__(self):
        self.lines = []
        self.v = self.vt = self.vn = 0

    def flat(self, points, vt, faces, material=None):
        """The faces with a normal each."""
        v0, t0 = self.v, self.vt
        for p in points:
            self.lines.append('v %.6f %.6f %.6f' % p)
        for t in vt:
            self.lines.append('vt %.6f %.6f' % t)
        self.v += len(points)
        self.vt += len(vt)
        if material:
            self.lines.append('usemtl ' + material)
        for face in faces:
            self.lines.append('vn %.6f %.6f %.6f' % face_normal([points[i] for i in face]))
            self.vn += 1
            self.lines.append('f ' + ' '.join('%d/%d/%d' % (v0 + i + 1, t0 + i + 1, self.vn) for i in face))

    def smooth(self, points, normals, vt, faces):
        v0, t0, n0 = self.v, self.vt, self.vn
        for p in points:
            self.lines.append('v %.6f %.6f %.6f' % p)
        for t in vt:
            self.lines.append('vt %.6f %.6f' % t)
        for n in normals:
            self.lines.append('vn %.6f %.6f %.6f' % n)
        self.v += len(points)
        self.vt += len(vt)
        self.vn += len(normals)
        for face in faces:
            self.lines.append('f ' + ' '.join('%d/%d/%d' % (v0 + i + 1, t0 + i + 1, n0 + i + 1) for i in face))

    def bare(self, points, vt, faces):
        v0, t0 = self.v, self.vt
        for p in points:
            self.lines.append('v %.6f %.6f %.6f' % p)
        for t in vt:
            self.lines.append('vt %.6f %.6f' % t)
        self.v += len(points)
        self.vt += len(vt)
        for face in faces:
            self.lines.append('f ' + ' '.join('%d/%d' % (v0 + i + 1, t0 + i + 1) for i in face))

    def write(self, name, header=None):
        with open(os.path.join(out, name), 'w') as f:
            if header:
                f.write(header + '\n')
            f.write('\n'.join(self.lines) + '\n')


# --- the three spheres ---------------------------------------------------------------------
points, normals, vt, faces = sphere(-1.15, 0.5, 0.0, 0.5)
obj = Obj()
obj.flat(points, vt, faces)
obj.write('smooth_faceted.obj')

points, normals, vt, faces = sphere(0.0, 0.5, 0.0, 0.5)
obj = Obj()
obj.smooth(points, normals, vt, faces)
obj.write('smooth_authored.obj')

points, normals, vt, faces = sphere(1.15, 0.5, 0.0, 0.5)
obj = Obj()
obj.bare(points, vt, faces)
obj.write('smooth_nonormals.obj')

# --- the shapes: three zones, each with a normal per face -------------------------------------
with open(os.path.join(out, 'smooth_shapes.mtl'), 'w') as f:
    f.write('newmtl Ball\nKd 0.80 0.25 0.20\nPr 0.45\n\nnewmtl Box\nKd 0.20 0.40 0.80\nPr 0.45\n\nnewmtl Can\nKd 0.20 0.40 0.80\nPr 0.45\n')
obj = Obj()
points, normals, vt, faces = sphere(-1.15, 0.45, 1.5, 0.45)
obj.flat(points, vt, faces, 'Ball')

h = 0.4
cx, cy, cz = 0.0, h, 1.5
corners = [(cx + sx * h, cy + sy * h, cz + sz * h) for sx, sy, sz in
           ((-1, -1, -1), (1, -1, -1), (1, 1, -1), (-1, 1, -1), (-1, -1, 1), (1, -1, 1), (1, 1, 1), (-1, 1, 1))]
obj.flat(corners, [(0.0, 0.0)] * 8, [(4, 5, 6, 7), (1, 0, 3, 2), (5, 1, 2, 6), (0, 4, 7, 3), (7, 6, 2, 3), (0, 1, 5, 4)], 'Box')

sides, radius, height = 32, 0.4, 0.8
cx, cz = 1.15, 1.5
top = [(cx + radius * math.cos(2 * math.pi * i / sides), height, cz - radius * math.sin(2 * math.pi * i / sides)) for i in range(sides)]
bottom = [(p[0], 0.0, p[2]) for p in top]
can = bottom + top
can_faces = [(i, (i + 1) % sides, sides + (i + 1) % sides, sides + i) for i in range(sides)]
can_faces.append(tuple(sides + i for i in range(sides)))            # the top cap
can_faces.append(tuple(reversed(range(sides))))                     # the bottom cap
obj.flat(can, [(0.0, 0.0)] * len(can), can_faces, 'Can')
obj.write('smooth_shapes.obj', 'mtllib smooth_shapes.mtl')

print('wrote the smoothing test assets into', out)
