"""Synthetic test assets for the material import paths (the Materials tab round, 2026-11-09).

Writes into the folder given as argv[1]:
  sg_scalar.gltf     a cube with KHR_materials_pbrSpecularGlossiness, factors only: a gold metal
                     (black diffuse, gold specular, gloss 0.8) and a red plastic (red diffuse, 4%
                     grey specular, gloss 0.5) on two primitives
  sg_textured.gltf   the same cube with a diffuse map (red/blue checker) and a spec/gloss map
                     (left half: gold specular, alpha 0.9; right half: 4% grey, alpha 0.3) — the
                     per-texel bake path
  mr_cube.gltf       the metallic-roughness equivalent of sg_scalar (the reference the conversion
                     must match: a gold metal + a red plastic)
  pbr_cube.obj/.mtl  an OBJ whose MTL uses the PBR extension (Pr / Pm) on one material and the
                     classic Ks / Ns on another, plus a map_Pm on a third
  tex_red.png        a flat red colour map, tex_mask.png a black / white checker cutout, and
  tex_rough.png      a left-to-right roughness ramp — the Materials tab's texture slots
                     (tools/ikscripts/materials/textures.txt)
  ext_cube.gltf      the KHR material extensions (emissive strength, ior, specular, clearcoat,
                     sheen, iridescence, anisotropy, transmission + volume, texture transform)
                     on two materials — the wider material's import (materials/ext_cube.txt)
  showcase.gltf      THE VIEWPORT'S MATERIALS (2026-11-10): a row of six spheres on the floor —
                     Glass (transmission, a green volume tint), Velvet (sheen), Brushed (an
                     anisotropic metal), Iridescent (a thin film over a metal), CarPaint (a clear coat over
                     a red metallic) and Chalk (a rough diffuse, its Oren-Nayar roughness set
                     by the script) — what materials/showcase.txt looks at
Every buffer is a base64 data URI, every image a PNG data URI — one file each, no sidecars.
"""
import base64, json, math, struct, sys, zlib, os

out = sys.argv[1]
os.makedirs(out, exist_ok=True)

def png(width, height, rgba_rows):
    def chunk(tag, data):
        c = struct.pack('>I', len(data)) + tag + data
        return c + struct.pack('>I', zlib.crc32(tag + data) & 0xffffffff)
    raw = b''.join(b'\x00' + bytes(row) for row in rgba_rows)
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 6, 0, 0, 0)) +
            chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b''))

def data_uri(mime, data):
    return f"data:{mime};base64," + base64.b64encode(data).decode('ascii')

# A unit cube: 24 vertices (4 per face, flat normals, one UV square per face), 12 triangles; split
# into two primitives (faces 0-2 and 3-5) so two materials show.
faces = [((0,0,1),(1,0,0),(0,1,0)), ((0,0,-1),(-1,0,0),(0,1,0)), ((1,0,0),(0,0,-1),(0,1,0)),
         ((-1,0,0),(0,0,1),(0,1,0)), ((0,1,0),(1,0,0),(0,0,-1)), ((0,-1,0),(1,0,0),(0,0,1))]
positions, normals, uvs, indices = [], [], [], []
for f, (n, u, v) in enumerate(faces):
    base = len(positions)
    for (su, sv), uv in zip([(-1,-1),(1,-1),(1,1),(-1,1)], [(0,1),(1,1),(1,0),(0,0)]):
        p = [0.5*n[i] + 0.5*su*u[i] + 0.5*sv*v[i] for i in range(3)]
        positions.append(p); normals.append(n); uvs.append(uv)
    indices += [base, base+1, base+2, base, base+2, base+3]

def accessor_blob():
    pos = b''.join(struct.pack('<fff', *p) for p in positions)
    nrm = b''.join(struct.pack('<fff', *n) for n in normals)
    uv = b''.join(struct.pack('<ff', *t) for t in uvs)
    idx = b''.join(struct.pack('<H', i) for i in indices)
    return pos, nrm, uv, idx

def gltf(materials, extension_used=None, images=None, textures=None, name='cube'):
    pos, nrm, uv, idx = accessor_blob()
    blob = pos + nrm + uv + idx
    views = [
        {"buffer": 0, "byteOffset": 0, "byteLength": len(pos)},
        {"buffer": 0, "byteOffset": len(pos), "byteLength": len(nrm)},
        {"buffer": 0, "byteOffset": len(pos)+len(nrm), "byteLength": len(uv)},
        {"buffer": 0, "byteOffset": len(pos)+len(nrm)+len(uv), "byteLength": len(idx)},
    ]
    mins = [min(p[i] for p in positions) for i in range(3)]
    maxs = [max(p[i] for p in positions) for i in range(3)]
    accessors = [
        {"bufferView": 0, "componentType": 5126, "count": len(positions), "type": "VEC3", "min": mins, "max": maxs},
        {"bufferView": 1, "componentType": 5126, "count": len(normals), "type": "VEC3"},
        {"bufferView": 2, "componentType": 5126, "count": len(uvs), "type": "VEC2"},
        {"bufferView": 3, "componentType": 5123, "count": 18, "type": "SCALAR", "byteOffset": 0},
        {"bufferView": 3, "componentType": 5123, "count": 18, "type": "SCALAR", "byteOffset": 36},
    ]
    doc = {
        "asset": {"version": "2.0", "generator": "PoseStudio material test"},
        "scene": 0, "scenes": [{"nodes": [0]}],
        "nodes": [{"mesh": 0, "name": name}],
        "meshes": [{"primitives": [
            {"attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2}, "indices": 3, "material": 0},
            {"attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2}, "indices": 4, "material": 1},
        ]}],
        "materials": materials,
        "buffers": [{"byteLength": len(blob), "uri": data_uri("application/octet-stream", blob)}],
        "bufferViews": views, "accessors": accessors,
    }
    if extension_used:
        doc["extensionsUsed"] = [extension_used]
    if images:
        doc["images"] = images
        doc["samplers"] = [{}]
        doc["textures"] = textures
    return doc

gold_spec = [1.0, 0.766, 0.336]
# sg_scalar: a gold metal (black diffuse, gold specular, glossy) + a red plastic (red diffuse, 4% spec, gloss 0.5)
sg_scalar = gltf([
    {"name": "Gold", "extensions": {"KHR_materials_pbrSpecularGlossiness": {
        "diffuseFactor": [0, 0, 0, 1], "specularFactor": gold_spec, "glossinessFactor": 0.8}}},
    {"name": "RedPlastic", "extensions": {"KHR_materials_pbrSpecularGlossiness": {
        "diffuseFactor": [0.8, 0.05, 0.05, 1], "specularFactor": [0.04, 0.04, 0.04], "glossinessFactor": 0.5}}},
], "KHR_materials_pbrSpecularGlossiness")
json.dump(sg_scalar, open(os.path.join(out, "sg_scalar.gltf"), "w"), indent=1)

# mr_cube: the metallic-roughness reference of the same two materials
mr = gltf([
    {"name": "Gold", "pbrMetallicRoughness": {"baseColorFactor": gold_spec + [1], "metallicFactor": 1.0, "roughnessFactor": 0.2}},
    {"name": "RedPlastic", "pbrMetallicRoughness": {"baseColorFactor": [0.8, 0.05, 0.05, 1], "metallicFactor": 0.0, "roughnessFactor": 0.5}},
])
json.dump(mr, open(os.path.join(out, "mr_cube.gltf"), "w"), indent=1)

# sg_textured: a 64x64 diffuse checker (red / blue) and a spec/gloss map: left half gold spec with
# gloss 0.9 (a metal), right half 4% grey with gloss 0.3 (a rough dielectric).
W = 64
def srgb8(c):  # linear -> sRGB byte
    c = max(0.0, min(1.0, c))
    e = c * 12.92 if c <= 0.0031308 else 1.055 * c ** (1/2.4) - 0.055
    return int(round(e * 255))
diffuse_rows, sg_rows = [], []
for y in range(W):
    drow, srow = [], []
    for x in range(W):
        checker = ((x // 16) + (y // 16)) % 2 == 0
        drow += [srgb8(0.8), srgb8(0.05), srgb8(0.05), 255] if checker else [srgb8(0.05), srgb8(0.05), srgb8(0.8), 255]
        if x < W // 2:
            srow += [srgb8(gold_spec[0]), srgb8(gold_spec[1]), srgb8(gold_spec[2]), int(0.9 * 255)]
        else:
            srow += [srgb8(0.04), srgb8(0.04), srgb8(0.04), int(0.3 * 255)]
    diffuse_rows.append(drow); sg_rows.append(srow)
images = [{"uri": data_uri("image/png", png(W, W, diffuse_rows)), "name": "diffuse"},
          {"uri": data_uri("image/png", png(W, W, sg_rows)), "name": "specgloss"}]
textures = [{"source": 0, "sampler": 0}, {"source": 1, "sampler": 0}]
sg_textured = gltf([
    {"name": "Mapped", "extensions": {"KHR_materials_pbrSpecularGlossiness": {
        "diffuseFactor": [1, 1, 1, 1], "diffuseTexture": {"index": 0},
        "specularFactor": [1, 1, 1], "glossinessFactor": 1.0, "specularGlossinessTexture": {"index": 1}}}},
    {"name": "Plain", "extensions": {"KHR_materials_pbrSpecularGlossiness": {
        "diffuseFactor": [0.2, 0.6, 0.2, 1], "specularFactor": [0.04, 0.04, 0.04], "glossinessFactor": 0.6}}},
], "KHR_materials_pbrSpecularGlossiness", images, textures)
json.dump(sg_textured, open(os.path.join(out, "sg_textured.gltf"), "w"), indent=1)

# pbr_cube.obj + .mtl: three materials over the six faces.
metal_png = png(8, 8, [[255 if x < 4 else 0, 255 if x < 4 else 0, 255 if x < 4 else 0, 255][k] for k in range(0)] or
                [[v for x in range(8) for v in ([255, 255, 255, 255] if x < 4 else [0, 0, 0, 255])] for _ in range(8)])
open(os.path.join(out, "metal_mask.png"), "wb").write(metal_png)
mtl = """# PBR extension + classic channels
newmtl PbrGold
Kd 1.000 0.766 0.336
Pr 0.25
Pm 1.0

newmtl ClassicPlastic
Kd 0.100 0.300 0.900
Ks 0.500 0.500 0.500
Ns 250

newmtl MappedMetal
Kd 0.8 0.8 0.8
Pm 1.0
map_Pm metal_mask.png
Pr 0.4
"""
open(os.path.join(out, "pbr_cube.mtl"), "w").write(mtl)
obj = ["mtllib pbr_cube.mtl"]
for p in positions:
    obj.append("v %.4f %.4f %.4f" % tuple(p))
for t in uvs:
    obj.append("vt %.4f %.4f" % (t[0], 1.0 - t[1]))
for n in normals:
    obj.append("vn %.1f %.1f %.1f" % tuple(n))
mats = ["PbrGold", "PbrGold", "ClassicPlastic", "ClassicPlastic", "MappedMetal", "MappedMetal"]
for f in range(6):
    obj.append("usemtl " + mats[f])
    i = f * 6
    for tri in (indices[i:i+3], indices[i+3:i+6]):
        obj.append("f " + " ".join(f"{k+1}/{k+1}/{k+1}" for k in tri))
open(os.path.join(out, "pbr_cube.obj"), "w").write("\n".join(obj) + "\n")

# --- Loose textures for the Materials tab's texture slots (tools/ikscripts/materials/textures.txt):
# a flat red colour map, a black / white checker cutout, and a left-to-right roughness ramp.
size = 32
red_rows = [[200, 40, 40, 255] * size for _ in range(size)]
checker_rows = [sum(([255, 255, 255, 255] if ((x // 8) + (y // 8)) % 2 == 0 else [0, 0, 0, 255] for x in range(size)), [])
                for y in range(size)]
ramp_rows = [sum(([int(255 * x / (size - 1))] * 3 + [255] for x in range(size)), []) for _ in range(size)]
open(os.path.join(out, "tex_red.png"), "wb").write(png(size, size, red_rows))
open(os.path.join(out, "tex_mask.png"), "wb").write(png(size, size, checker_rows))
open(os.path.join(out, "tex_rough.png"), "wb").write(png(size, size, ramp_rows))

# ext_cube: the KHR material extensions the glTF reader fills the wider material from — a glowing,
# coated, sheened, iridescent, anisotropic dielectric with a specular tint (Glow), and a tinted
# glass whose base map carries a KHR_texture_transform (Glass).
ext = gltf([
    {"name": "Glow", "pbrMetallicRoughness": {"baseColorFactor": [0.9, 0.9, 0.9, 1], "metallicFactor": 0.0, "roughnessFactor": 0.4},
     "emissiveFactor": [1.0, 0.5, 0.25],
     "extensions": {
         "KHR_materials_emissive_strength": {"emissiveStrength": 4.0},
         "KHR_materials_ior": {"ior": 1.33},
         "KHR_materials_specular": {"specularFactor": 0.5, "specularColorFactor": [1.0, 0.8, 0.6]},
         "KHR_materials_clearcoat": {"clearcoatFactor": 0.8, "clearcoatRoughnessFactor": 0.1},
         "KHR_materials_sheen": {"sheenColorFactor": [0.5, 0.5, 0.5], "sheenRoughnessFactor": 0.3},
         "KHR_materials_iridescence": {"iridescenceFactor": 0.7, "iridescenceIor": 1.3, "iridescenceThicknessMaximum": 400.0},
         "KHR_materials_anisotropy": {"anisotropyStrength": 0.5, "anisotropyRotation": 1.5707963}}},
    {"name": "Glass", "pbrMetallicRoughness": {"baseColorFactor": [1, 1, 1, 1], "metallicFactor": 0.0, "roughnessFactor": 0.05,
                                              "baseColorTexture": {"index": 0, "extensions": {"KHR_texture_transform": {"scale": [2.0, 3.0], "offset": [0.25, 0.5]}}}},
     "extensions": {
         "KHR_materials_transmission": {"transmissionFactor": 0.6},
         "KHR_materials_volume": {"thicknessFactor": 0.1, "attenuationColor": [0.2, 0.9, 0.3], "attenuationDistance": 0.5}}},
], None, images=[{"uri": data_uri("image/png", png(size, size, checker_rows))}], textures=[{"sampler": 0, "source": 0}])
ext["extensionsUsed"] = ["KHR_materials_emissive_strength", "KHR_materials_ior", "KHR_materials_specular", "KHR_materials_clearcoat",
                        "KHR_materials_sheen", "KHR_materials_iridescence", "KHR_materials_anisotropy", "KHR_materials_transmission",
                        "KHR_materials_volume", "KHR_texture_transform"]
json.dump(ext, open(os.path.join(out, "ext_cube.gltf"), "w"), indent=1)
# showcase.gltf: six UV spheres in a row on the floor, one material each — the viewport's
# rendering of the PBR layers (materials/showcase.txt shoots them with each layer on and off).
def sphere(cx, cy, cz, r, segments=48, rings=24):
    pos, nrm, uv, idx = [], [], [], []
    for j in range(rings + 1):
        v = j / rings
        theta = v * math.pi
        for i in range(segments + 1):
            u = i / segments
            phi = u * 2.0 * math.pi
            n = (math.sin(theta) * math.cos(phi), math.cos(theta), math.sin(theta) * math.sin(phi))
            pos.append((cx + r * n[0], cy + r * n[1], cz + r * n[2])); nrm.append(n); uv.append((u, v))
    for j in range(rings):
        for i in range(segments):
            a = j * (segments + 1) + i
            b = a + segments + 1
            idx += [a, a + 1, b, a + 1, b + 1, b]  # counter-clockwise seen from outside
    return pos, nrm, uv, idx

showcase_materials = [
    {"name": "Glass", "pbrMetallicRoughness": {"baseColorFactor": [1, 1, 1, 1], "metallicFactor": 0.0, "roughnessFactor": 0.02},
     "extensions": {"KHR_materials_transmission": {"transmissionFactor": 1.0}, "KHR_materials_ior": {"ior": 1.5},
                    "KHR_materials_volume": {"thicknessFactor": 0.5, "attenuationColor": [0.6, 0.9, 0.7], "attenuationDistance": 0.5}}},
    {"name": "Velvet", "pbrMetallicRoughness": {"baseColorFactor": [0.45, 0.03, 0.08, 1], "metallicFactor": 0.0, "roughnessFactor": 0.9},
     "extensions": {"KHR_materials_sheen": {"sheenColorFactor": [0.9, 0.5, 0.6], "sheenRoughnessFactor": 0.5}}},
    {"name": "Brushed", "pbrMetallicRoughness": {"baseColorFactor": [0.95, 0.93, 0.88, 1], "metallicFactor": 1.0, "roughnessFactor": 0.35},
     "extensions": {"KHR_materials_anisotropy": {"anisotropyStrength": 0.9, "anisotropyRotation": 0.0}}},
    {"name": "Iridescent", "pbrMetallicRoughness": {"baseColorFactor": [0.85, 0.85, 0.85, 1], "metallicFactor": 1.0, "roughnessFactor": 0.2},
     "extensions": {"KHR_materials_iridescence": {"iridescenceFactor": 1.0, "iridescenceIor": 1.3, "iridescenceThicknessMaximum": 400.0}}},
    {"name": "CarPaint", "pbrMetallicRoughness": {"baseColorFactor": [0.55, 0.02, 0.03, 1], "metallicFactor": 0.7, "roughnessFactor": 0.45},
     "extensions": {"KHR_materials_clearcoat": {"clearcoatFactor": 1.0, "clearcoatRoughnessFactor": 0.03}}},
    {"name": "Chalk", "pbrMetallicRoughness": {"baseColorFactor": [0.85, 0.82, 0.75, 1], "metallicFactor": 0.0, "roughnessFactor": 1.0}},
]
sc_pos, sc_nrm, sc_uv, sc_idx, sc_prims = [], [], [], [], []
for i, mat in enumerate(showcase_materials):
    p_, n_, u_, i_ = sphere((i - 2.5) * 1.1, 0.5, 0.0, 0.5)
    base = len(sc_pos)
    sc_prims.append((base, len(p_), len(sc_idx), len(i_)))
    sc_pos += p_; sc_nrm += n_; sc_uv += u_; sc_idx += [base + k for k in i_]
pos_b = b"".join(struct.pack("<fff", *q) for q in sc_pos)
nrm_b = b"".join(struct.pack("<fff", *q) for q in sc_nrm)
uv_b = b"".join(struct.pack("<ff", *q) for q in sc_uv)
idx_b = b"".join(struct.pack("<H", k) for k in sc_idx)
blob = pos_b + nrm_b + uv_b + idx_b
views = [{"buffer": 0, "byteOffset": 0, "byteLength": len(pos_b)},
         {"buffer": 0, "byteOffset": len(pos_b), "byteLength": len(nrm_b)},
         {"buffer": 0, "byteOffset": len(pos_b) + len(nrm_b), "byteLength": len(uv_b)},
         {"buffer": 0, "byteOffset": len(pos_b) + len(nrm_b) + len(uv_b), "byteLength": len(idx_b)}]
mins = [min(q[k] for q in sc_pos) for k in range(3)]
maxs = [max(q[k] for q in sc_pos) for k in range(3)]
accessors = [{"bufferView": 0, "componentType": 5126, "count": len(sc_pos), "type": "VEC3", "min": mins, "max": maxs},
             {"bufferView": 1, "componentType": 5126, "count": len(sc_nrm), "type": "VEC3"},
             {"bufferView": 2, "componentType": 5126, "count": len(sc_uv), "type": "VEC2"}]
prims = []
for m, (vbase, vcount, ibase, icount) in enumerate(sc_prims):
    accessors.append({"bufferView": 3, "componentType": 5123, "count": icount, "type": "SCALAR", "byteOffset": ibase * 2})
    prims.append({"attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2}, "indices": len(accessors) - 1, "material": m})
showcase = {
    "asset": {"version": "2.0", "generator": "PoseStudio material test"},
    "scene": 0, "scenes": [{"nodes": [0]}], "nodes": [{"mesh": 0, "name": "showcase"}],
    "meshes": [{"primitives": prims}], "materials": showcase_materials,
    "buffers": [{"byteLength": len(blob), "uri": data_uri("application/octet-stream", blob)}],
    "bufferViews": views, "accessors": accessors,
    "extensionsUsed": ["KHR_materials_transmission", "KHR_materials_ior", "KHR_materials_volume", "KHR_materials_sheen",
                       "KHR_materials_anisotropy", "KHR_materials_iridescence", "KHR_materials_clearcoat"],
}
json.dump(showcase, open(os.path.join(out, "showcase.gltf"), "w"), indent=1)
print("wrote", out)
