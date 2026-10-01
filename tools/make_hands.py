"""Converts the WebXR generic hand models (third_party/generic-hand/{left,right}.glb,
MIT, Copyright 2019 Amazon) into core/render/hand_model_data.h: a skinned hand
per side for the mod's hands, as fingerless leather gloves.

- Reads the GLB (JSON + binary chunk) with the standard library only.
- Skeleton: the 25 WebXR hand joints in a fixed order (wrist, thumb x4, then
  index, middle, ring, pinky x5), parents from the joint names (the left model's
  nodes are flat), rest (bind) matrices, and each joint's bend axis for curling
  the finger towards the palm.
- Per vertex: position, normal, UV, 4 joints + weights, and how much it is bare
  skin (the fingers beyond the glove, from the finger joints it follows) vs
  leather glove.
- GRID > 0 would cluster vertices into a low-poly mesh (off: it fused fingers).
Everything stays in the model's space (metres, glTF axes); the mod places it.

Also bakes each hand's texture, core/render/hand_{left,right}.png (embedded in
the DLL as resources): the Hafnia Hands skin albedo (third_party/hafnia-hands,
MIT, Copyright (c) 2021 Aske Mottelson; made for this hand mesh's UV layout),
with the glove area turned into black leather that keeps the skin's creases.
Needs Pillow and numpy for the bake.

Usage: python tools/make_hands.py
"""
import json
import math
import os
import struct

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, 'third_party', 'generic-hand')
OUT = os.path.join(ROOT, 'core', 'render', 'hand_model_data.h')
SKIN = os.path.join(ROOT, 'third_party', 'hafnia-hands', 'Skin2-Albedo.png')
TEXTURE_OUT = os.path.join(ROOT, 'core', 'render', 'hand_%s.png')
TEXTURE_SIZE = 1024
GRID = 0.0  # metres (0 = full detail)

FINGERS = ['index-finger', 'middle-finger', 'ring-finger', 'pinky-finger']
JOINTS = ['wrist', 'thumb-metacarpal', 'thumb-phalanx-proximal', 'thumb-phalanx-distal', 'thumb-tip']
for f in FINGERS:
    JOINTS += [f + '-metacarpal', f + '-phalanx-proximal', f + '-phalanx-intermediate',
               f + '-phalanx-distal', f + '-tip']


def parent_of(name):
    i = JOINTS.index(name)
    if i == 0:
        return -1
    if name.endswith('metacarpal'):
        return 0
    return i - 1


# --- small matrix helpers (4x4 row-major lists) ------------------------------------
def mat_mul(a, b):
    return [[sum(a[i][k] * b[k][j] for k in range(4)) for j in range(4)] for i in range(4)]


def mat_from_trs(t, r, s):
    x, y, z, w = r
    rot = [[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
           [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
           [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]]
    m = [[rot[i][j] * s[j] for j in range(3)] + [t[i]] for i in range(3)]
    return m + [[0, 0, 0, 1]]


def mat_inv_rigid(m):  # rotation (+ near-unit scale) and translation
    r = [[m[j][i] for j in range(3)] for i in range(3)]  # transpose
    t = [-sum(r[i][k] * m[k][3] for k in range(3)) for i in range(3)]
    return [r[i] + [t[i]] for i in range(3)] + [[0, 0, 0, 1]]


def transform(m, p):
    return [sum(m[i][k] * p[k] for k in range(3)) + m[i][3] for i in range(3)]


def rot_axis(axis, angle):
    x, y, z = axis
    c, s, t = math.cos(angle), math.sin(angle), 1 - math.cos(angle)
    return [[c + x * x * t, x * y * t - z * s, x * z * t + y * s, 0],
            [y * x * t + z * s, c + y * y * t, y * z * t - x * s, 0],
            [z * x * t - y * s, z * y * t + x * s, c + z * z * t, 0], [0, 0, 0, 1]]


# --- GLB --------------------------------------------------------------------------
def load_glb(path):
    d = open(path, 'rb').read()
    jlen = struct.unpack_from('<I', d, 12)[0]
    gltf = json.loads(d[20:20 + jlen])
    bin_off = 20 + jlen + 8
    return gltf, d[bin_off:]


def accessor(gltf, binary, index):
    acc = gltf['accessors'][index]
    view = gltf['bufferViews'][acc['bufferView']]
    comps = {'SCALAR': 1, 'VEC2': 2, 'VEC3': 3, 'VEC4': 4, 'MAT4': 16}[acc['type']]
    fmt = {5126: 'f', 5125: 'I', 5123: 'H', 5121: 'B'}[acc['componentType']]
    size = struct.calcsize(fmt)
    stride = view.get('byteStride', comps * size)
    base = view.get('byteOffset', 0) + acc.get('byteOffset', 0)
    out = []
    for i in range(acc['count']):
        vals = struct.unpack_from('<' + fmt * comps, binary, base + i * stride)
        if acc.get('normalized') and fmt in 'BH':
            vals = tuple(v / (255.0 if fmt == 'B' else 65535.0) for v in vals)
        out.append(vals)
    return out


def global_matrices(gltf):
    nodes = gltf['nodes']
    parent = {}
    for i, n in enumerate(nodes):
        for c in n.get('children', []):
            parent[c] = i
    cache = {}

    def world(i):
        if i in cache:
            return cache[i]
        n = nodes[i]
        if 'matrix' in n:
            m = n['matrix']
            local = [[m[c * 4 + r] for c in range(4)] for r in range(4)]
        else:
            local = mat_from_trs(n.get('translation', [0, 0, 0]), n.get('rotation', [0, 0, 0, 1]),
                                 n.get('scale', [1, 1, 1]))
        m = mat_mul(world(parent[i]), local) if i in parent else local
        cache[i] = m
        return m
    return world


def convert(side):
    gltf, binary = load_glb(os.path.join(SRC, side + '.glb'))
    skin = gltf['skins'][0]
    prim = gltf['meshes'][0]['primitives'][0]
    names = [gltf['nodes'][j]['name'] for j in skin['joints']]
    world = global_matrices(gltf)
    order = [names.index(n) for n in JOINTS]  # canonical -> skin joint index
    remap = {skin_i: canon for canon, skin_i in enumerate(order)}

    ibm = accessor(gltf, binary, skin['inverseBindMatrices'])
    inv_bind, rest = [], []
    for skin_i in order:
        m = ibm[skin_i]
        ib = [[m[c * 4 + r] for c in range(4)] for r in range(4)]
        inv_bind.append(ib)
        rest.append(mat_inv_rigid(ib))  # the joint's bind pose in model space

    # Bend axis per joint (joint-local): the local axis about which a small
    # rotation moves the next joint towards the palm (-Y of the wrist).
    # Fingers bend towards the palm side (-Y of the wrist); the thumb bends
    # towards the centre of the palm (its joints are angled, so "towards -Y"
    # picks the wrong way for the tip).
    palm = [-rest[0][i][1] for i in range(3)]
    def pos(name):
        return [rest[JOINTS.index(name)][i][3] for i in range(3)]
    mids = [pos(f + '-metacarpal') for f in ('index-finger', 'middle-finger', 'ring-finger', 'pinky-finger')]
    palm_centre = [sum(m[i] for m in mids) / 4 + palm[i] * 0.03 for i in range(3)]
    axes = []
    for j, name in enumerate(JOINTS):
        child = j + 1 if j + 1 < len(JOINTS) and parent_of(JOINTS[j + 1]) == j else -1
        best = [1.0, 0.0, 0.0]
        if child >= 0 and not name.endswith('tip'):
            tip = [rest[child][i][3] for i in range(3)]
            local_tip = transform(mat_inv_rigid(rest[j]), tip)
            best_gain = -1e9
            for axis in ([1, 0, 0], [-1, 0, 0], [0, 1, 0], [0, -1, 0]):
                moved = transform(mat_mul(rest[j], rot_axis(axis, 0.3)), local_tip)
                if name.startswith('thumb'):
                    before = sum((tip[i] - palm_centre[i]) ** 2 for i in range(3))
                    after = sum((moved[i] - palm_centre[i]) ** 2 for i in range(3))
                    gain = before - after
                else:
                    gain = sum((moved[i] - tip[i]) * palm[i] for i in range(3))
                if gain > best_gain:
                    best_gain, best = gain, axis
        # The thumb's last joint bends in the same plane as the one before it
        # (its own test is ambiguous: the bone is short).
        if name == 'thumb-phalanx-distal':
            best = axes[JOINTS.index('thumb-phalanx-proximal')]
        axes.append(best)

    pos = accessor(gltf, binary, prim['attributes']['POSITION'])
    nrm = accessor(gltf, binary, prim['attributes']['NORMAL'])
    uvs = accessor(gltf, binary, prim['attributes']['TEXCOORD_0'])
    jnt = accessor(gltf, binary, prim['attributes']['JOINTS_0'])
    wgt = accessor(gltf, binary, prim['attributes']['WEIGHTS_0'])
    idx = [i[0] for i in accessor(gltf, binary, prim['indices'])]

    # Bare skin: the parts of the fingers past the glove (fingerless gloves end
    # partway along the first finger bone; the thumb's glove ends at its middle).
    skin_amount = {}
    for j, n in enumerate(JOINTS):
        if n.endswith(('phalanx-intermediate', 'phalanx-distal', 'tip')):
            skin_amount[j] = 1.0
        elif n.endswith('phalanx-proximal') and not n.startswith('thumb'):
            skin_amount[j] = 0.45

    # Optional low poly: cluster vertices on a grid.
    clusters = {}
    for i, p in enumerate(pos):
        key = tuple(int(math.floor(c / GRID)) for c in p) if GRID > 0 else (i,)
        clusters.setdefault(key, []).append(i)
    vmap, verts = {}, []
    for key, members in clusters.items():
        centre = [sum(pos[m][k] for m in members) / len(members) for k in range(3)]
        rep = min(members, key=lambda m: sum((pos[m][k] - centre[k]) ** 2 for k in range(3)))
        joints = [remap[j] for j in jnt[rep]]
        weights = list(wgt[rep])
        total = sum(weights) or 1.0
        weights = [w / total for w in weights]
        skin = sum(w * skin_amount.get(j, 0.0) for j, w in zip(joints, weights))
        verts.append((centre, list(nrm[rep]), list(uvs[rep]), joints, weights, 1.0 if skin > 0.5 else 0.0, skin))
        for m in members:
            vmap[m] = len(verts) - 1
    tris, seen = [], set()
    for t in range(0, len(idx), 3):
        a, b, c = vmap[idx[t]], vmap[idx[t + 1]], vmap[idx[t + 2]]
        if a == b or b == c or a == c:
            continue
        k = tuple(sorted((a, b, c)))
        if k in seen:
            continue
        seen.add(k)
        tris.append((a, b, c))
    print('%s: %d joints, %d -> %d vertices, %d -> %d triangles' %
          (side, len(JOINTS), len(pos), len(verts), len(idx) // 3, len(tris)))
    return rest, inv_bind, axes, verts, tris


# Where the gloves end: across each finger's first bone (proximal phalanx), this
# far from the knuckle towards the middle joint; the thumb's, this far along its
# first bone towards its last joint.
GLOVE_END_FINGER = 0.7
GLOVE_END_THUMB = 0.85
HEM_WIDTH = 0.0035  # metres: the darker rolled hem just inside the glove's edge


def finger_of(joint):
    """0 = thumb, 1..4 = index..pinky, None = the wrist."""
    return None if joint == 0 else (0 if joint <= 4 else 1 + (joint - 5) // 5)


def bake_texture(side, verts, tris, rest):
    """The hand's texture: skin where the fingers are bare, black leather over
    the glove (the skin's shading kept as the leather's creases). The glove
    ends in a clean ring around each finger: each texel's point on the hand
    (rest pose) is tested against a cut plane across the finger it belongs to
    (its triangle's most weighted joint), anti-aliased over about a texel."""
    import numpy as np
    from PIL import Image, ImageFilter

    n = TEXTURE_SIZE
    albedo = np.asarray(Image.open(SKIN).convert('RGB').resize((n, n), Image.LANCZOS), dtype=np.float32) / 255.0

    def joint_pos(name):
        m = rest[JOINTS.index(name)]
        return np.array([m[0][3], m[1][3], m[2][3]], dtype=np.float64)

    # The cut plane per finger: a point and the bone's direction (towards the tip).
    cuts = []
    for f, name in enumerate(['thumb'] + [x[:-len('-finger')] for x in FINGERS]):
        if f == 0:
            a_, b_, frac = joint_pos('thumb-phalanx-proximal'), joint_pos('thumb-phalanx-distal'), GLOVE_END_THUMB
        else:
            finger = FINGERS[f - 1]
            a_, b_ = joint_pos(finger + '-phalanx-proximal'), joint_pos(finger + '-phalanx-intermediate')
            frac = GLOVE_END_FINGER
        d = (b_ - a_) / np.linalg.norm(b_ - a_)
        cuts.append((a_ + (b_ - a_) * frac, d))

    # Per texel: signed distance past the cut (metres; + = bare), and the
    # texel's size on the hand for anti-aliasing (NaN = not covered).
    dist = np.full((n, n), np.nan, dtype=np.float32)
    texel = np.full((n, n), np.nan, dtype=np.float32)
    ys, xs = np.mgrid[0:n, 0:n]
    px, py = xs + 0.5, ys + 0.5
    for a, b, c in tris:
        p = [(verts[i][2][0] * n, verts[i][2][1] * n) for i in (a, b, c)]
        x0, x1 = int(max(0, min(q[0] for q in p) - 1)), int(min(n, max(q[0] for q in p) + 2))
        y0, y1 = int(max(0, min(q[1] for q in p) - 1)), int(min(n, max(q[1] for q in p) + 2))
        if x0 >= x1 or y0 >= y1:
            continue
        (ax, ay), (bx, by), (cx, cy) = p
        det = (by - cy) * (ax - cx) + (cx - bx) * (ay - cy)
        if abs(det) < 1e-9:
            continue
        # Its finger: the most weighted joint of its vertices.
        best, joint = -1.0, 0
        for i in (a, b, c):
            for j, w in zip(verts[i][3], verts[i][4]):
                if w > best:
                    best, joint = w, j
        f = finger_of(joint)
        P = [np.array(verts[i][0], dtype=np.float64) for i in (a, b, c)]
        if f is None:
            d3 = [-1.0, -1.0, -1.0]  # the wrist: glove
        else:
            point, axis = cuts[f]
            d3 = [float(np.dot(q - point, axis)) for q in P]
        area3 = np.linalg.norm(np.cross(P[1] - P[0], P[2] - P[0])) * 0.5
        size = float(np.sqrt(area3 / max(abs(det) * 0.5, 1e-9)))  # metres per texel
        sx, sy = px[y0:y1, x0:x1], py[y0:y1, x0:x1]
        w0 = ((by - cy) * (sx - cx) + (cx - bx) * (sy - cy)) / det
        w1 = ((cy - ay) * (sx - cx) + (ax - cx) * (sy - cy)) / det
        w2 = 1 - w0 - w1
        inside = (w0 >= -0.02) & (w1 >= -0.02) & (w2 >= -0.02)
        dist[y0:y1, x0:x1][inside] = (w0 * d3[0] + w1 * d3[1] + w2 * d3[2])[inside]
        texel[y0:y1, x0:x1][inside] = size

    covered = ~np.isnan(dist)
    e = np.where(covered, texel, 1.0) * 0.75
    t = np.clip((np.where(covered, dist, -1.0) + e) / (2 * e), 0, 1)
    bare = t * t * (3 - 2 * t)
    inner = np.where(covered, -dist, 1.0)  # distance inside the glove
    hem = np.where((inner > 0) & (inner < HEM_WIDTH), np.sin(np.clip(inner / HEM_WIDTH, 0, 1) * np.pi), 0.0)

    # Past the layout's edges: grow the covered values outwards (so filtering
    # and mip levels don't pull in the wrong material).
    def grow(values):
        values = np.where(covered, values, 0).astype(np.float32)
        weight = covered.astype(np.float32)
        for _ in range(24):
            g = np.asarray(Image.fromarray(values * 255).filter(ImageFilter.MaxFilter(3)), dtype=np.float32) / 255
            w = np.asarray(Image.fromarray(weight * 255).filter(ImageFilter.MaxFilter(3)), dtype=np.float32) / 255
            fill = (weight == 0) & (w > 0)
            values[fill] = g[fill]
            weight = np.maximum(weight, w)
        return values
    bare, hem = grow(bare), grow(hem)

    # Leather: the skin's shading relative to its local average (creases,
    # knuckles), on a near-black brown, with a faint fine grain.
    lum = albedo @ np.array([0.299, 0.587, 0.114], dtype=np.float32)
    local = np.asarray(Image.fromarray((lum * 255).astype(np.uint8)).filter(ImageFilter.GaussianBlur(24)),
                       dtype=np.float32) / 255 + 1e-3
    detail = np.clip(lum / local, 0.55, 1.4) ** 1.6
    rnd = np.random.default_rng(7)
    grain = np.asarray(Image.fromarray((rnd.random((n, n)) * 255).astype(np.uint8)).filter(ImageFilter.GaussianBlur(0.8)),
                       dtype=np.float32) / 255
    base = np.array([0.13, 0.117, 0.108], dtype=np.float32)
    leather = base[None, None, :] * (detail * (0.85 + 0.3 * grain) * (1 - 0.5 * hem))[:, :, None]

    out = albedo * bare[:, :, None] + leather * (1 - bare[:, :, None])
    img = Image.fromarray(np.clip(out * 255 + 0.5, 0, 255).astype(np.uint8), 'RGB')
    img.save(TEXTURE_OUT % side, optimize=True)
    print('wrote', TEXTURE_OUT % side)


def c_floats(values):
    return ', '.join('%.6ff' % v for v in values)


def main():
    lines = ['// Generated by tools/make_hands.py from the WebXR generic hand models',
             '// (third_party/generic-hand, MIT License, Copyright (c) 2019 Amazon). Do not edit.',
             '#pragma once', '', 'namespace hand_model {', '',
             'constexpr int kJoints = %d;' % len(JOINTS),
             '// Canonical joint order; parents: -1 for the wrist, else the index given here.',
             'constexpr int kParent[kJoints] = {%s};' % ', '.join(str(parent_of(n)) for n in JOINTS),
             '']
    for side in ('left', 'right'):
        rest, inv_bind, axes, verts, tris = convert(side)
        s = side.capitalize()
        lines.append('// %s hand: joint rest matrices (row-major 3x4), inverse bind matrices, bend axes.' % s)
        lines.append('constexpr float k%sRest[kJoints][12] = {' % s)
        lines += ['    {%s},' % c_floats([m[r][c] for r in range(3) for c in range(4)]) for m in rest]
        lines.append('};')
        lines.append('constexpr float k%sInvBind[kJoints][12] = {' % s)
        lines += ['    {%s},' % c_floats([m[r][c] for r in range(3) for c in range(4)]) for m in inv_bind]
        lines.append('};')
        lines.append('constexpr float k%sAxis[kJoints][3] = {%s};' %
                     (s, ', '.join('{%s}' % c_floats(a) for a in axes)))
        lines.append('constexpr int k%sVertexCount = %d;' % (s, len(verts)))
        lines.append('// x y z, normal x y z, u v, 4 joints, 4 weights, bare skin (0/1)')
        lines.append('constexpr float k%sVertices[][17] = {' % s)
        lines += ['    {%s, %s, %s, %s, %s, %.1ff},' % (c_floats(p), c_floats(n), c_floats(uv),
                                                  ', '.join('%d.0f' % j for j in js), c_floats(ws), sk)
                  for p, n, uv, js, ws, sk, _ in verts]
        lines.append('};')
        lines.append('constexpr int k%sTriangleCount = %d;' % (s, len(tris)))
        lines.append('constexpr unsigned short k%sTriangles[][3] = {' % s)
        lines += ['    {%d, %d, %d},' % t for t in tris]
        lines.append('};')
        lines.append('')
        bake_texture(side, verts, tris, rest)
    lines.append('}  // namespace hand_model')
    open(OUT, 'w', newline='\n').write('\n'.join(lines) + '\n')
    print('wrote', OUT)


if __name__ == '__main__':
    main()
