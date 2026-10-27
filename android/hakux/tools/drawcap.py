#!/usr/bin/env python3
"""Render a hakuX draw capture (debug.hakux.drawcap, pb/pbdraw.c) on the host.

The capture holds each draw as the guest left it: vertices decoded to floats,
indices, the fixed-function composite matrix and viewport. Drawing them again
here, without the HLE's shaders or the GPU, shows whether a broken frame is
broken in the guest's data or in the HLE's drawing of it.

  drawcap.py list drawcap.bin [--frame N]
  drawcap.py render drawcap.bin out.png [--frame N] [--mode diffuse|tex|depth|textured]
                    [--size 640x480] [--draws A-B]
  drawcap.py diff a.bin b.bin [--frame N]   draws matched by texture and
                    vertex count; reports texcoord / diffuse differences

Only fixed-function draws (execution mode 0) are transformed; program draws
are listed but not drawn (their transform is in the program's constants).
"""
import argparse
import struct
import sys

import numpy as np
from PIL import Image

HDR = struct.Struct('<8I')
ATTRS = 16
PRIMS = {1: 'points', 2: 'lines', 3: 'line_loop', 4: 'line_strip', 5: 'triangles', 6: 'tri_strip',
         7: 'tri_fan', 8: 'quads', 9: 'quad_strip', 10: 'polygon'}


class Draw:
    pass


def read(path):
    data = open(path, 'rb').read()
    o, out = 0, []
    while o + HDR.size <= len(data):
        h = HDR.unpack_from(data, o)
        if h[0] not in (0x31575244, 0x32575244, 0x33575244):
            raise SystemExit(f'{path}: bad record at {o:#x}')
        o += HDR.size
        d = Draw()
        d.frame, d.index, d.path, d.prim, d.n, d.nidx, d.mode = h[1:]
        d.fmts = struct.unpack_from(f'<{ATTRS}I', data, o)
        o += 4 * ATTRS
        m = np.frombuffer(data, '<f4', 24, o).astype(np.float64)
        o += 4 * 24
        d.composite = m[:16].reshape(4, 4)
        d.vpoff, d.vpscale = m[16:20], m[20:24]
        d.tex = struct.unpack_from('<3I', data, o)
        o += 12
        d.state = (0, 0, 0, 0, 1, 0)
        if h[0] == 0x33575244:          # DRW3: alpha test and blend state
            d.state = struct.unpack_from('<6I', data, o)
            o += 24
        d.pa = (0, 0, 0)
        if h[0] >= 0x32575244:          # DRW2: the arrays' guest addresses
            d.pa = struct.unpack_from('<3I', data, o)
            o += 12
        attrs = np.frombuffer(data, '<f4', 3 * 4 * d.n, o).reshape(3, d.n, 4).astype(np.float64)
        o += 4 * 3 * 4 * d.n
        d.pos, d.diffuse, d.tc = attrs
        d.idx = np.frombuffer(data, '<u4', d.nidx, o).astype(np.int64)
        o += 4 * d.nidx
        d.texbytes = b''
        if h[0] == 0x33575244:          # DRW3: texture 0's first level
            n, = struct.unpack_from('<I', data, o)
            d.texbytes = data[o + 4:o + 4 + n]
            o += 4 + n
        out.append(d)
    return out


def triangles(d):
    """Vertex index triples of a draw (indices or the vertices in order)."""
    v = d.idx if d.nidx else np.arange(d.n)
    p = d.prim
    if p == 5:
        return v[:len(v) // 3 * 3].reshape(-1, 3)
    if p == 6:
        t = [(v[i], v[i + 1], v[i + 2]) if i % 2 == 0 else (v[i + 1], v[i], v[i + 2]) for i in range(len(v) - 2)]
    elif p in (7, 10):
        t = [(v[0], v[i], v[i + 1]) for i in range(1, len(v) - 1)]
    elif p == 8:
        t = []
        for i in range(0, len(v) - 3, 4):
            t += [(v[i], v[i + 1], v[i + 2]), (v[i], v[i + 2], v[i + 3])]
    elif p == 9:
        t = []
        for i in range(0, len(v) - 3, 2):
            t += [(v[i], v[i + 1], v[i + 3]), (v[i], v[i + 3], v[i + 2])]
    else:
        return np.zeros((0, 3), np.int64)
    return np.array(t, np.int64).reshape(-1, 3)


def screen(d):
    """Screen x, y, z and w of the draw's vertices: the composite matrix
    (rows are its registers) includes the viewport scale; the offset is
    added after the divide, as the NV2A's fixed function does."""
    clip = d.pos @ d.composite.T
    w = clip[:, 3].copy()
    w[w == 0] = 1e-9
    xyz = clip[:, :3] / w[:, None] + d.vpoff[:3]
    return xyz, w


def tex_desc(t):
    off, fmt, rect = t
    return f'tex {off:08x} fmt {(fmt >> 8) & 0xFF:02x} {rect >> 16}x{rect & 0xFFFF}'


def cmd_list(a):
    for d in read(a.file):
        if a.frame is not None and d.frame != a.frame:
            continue
        xyz, w = screen(d)
        ok = w > 0
        box = (f'x {xyz[ok, 0].min():8.1f}..{xyz[ok, 0].max():8.1f} y {xyz[ok, 1].min():8.1f}..'
               f'{xyz[ok, 1].max():8.1f}') if ok.any() else 'behind'
        print(f'f{d.frame} #{d.index:5} {"inl" if d.path == 1 else "arr"} {PRIMS.get(d.prim, d.prim):10} '
              f'n {d.n:5} idx {d.nidx:5} mode {d.mode} pos {d.fmts[0] & 0xFF:02x}/{d.fmts[0] >> 8:<3} '
              f'{tex_desc(d.tex)} pa {d.pa[0]:08x} {box}')


def raster_tex(img, zbuf, xyz, d, tex, tris):
    """Texture x diffuse, the alpha test and the blend of the draw (depth
    tested, written by opaque draws only)."""
    hgt, wid = zbuf.shape
    ate, afn, aref, ben, sf, df = d.state
    th, tw = tex.shape[:2]
    for t in tris:
        p = xyz[t]
        x0, x1 = int(max(np.floor(p[:, 0].min()), 0)), int(min(np.ceil(p[:, 0].max()), wid - 1))
        y0, y1 = int(max(np.floor(p[:, 1].min()), 0)), int(min(np.ceil(p[:, 1].max()), hgt - 1))
        if x0 > x1 or y0 > y1 or (x1 - x0) * (y1 - y0) > wid * hgt:
            continue
        ys, xs = np.mgrid[y0:y1 + 1, x0:x1 + 1] + 0.5
        (ax, ay), (bx, by), (cx, cy) = p[:, :2]
        den = (by - cy) * (ax - cx) + (cx - bx) * (ay - cy)
        if abs(den) < 1e-12:
            continue
        l0 = ((by - cy) * (xs - cx) + (cx - bx) * (ys - cy)) / den
        l1 = ((cy - ay) * (xs - cx) + (ax - cx) * (ys - cy)) / den
        l2 = 1 - l0 - l1
        inside = (l0 >= 0) & (l1 >= 0) & (l2 >= 0)
        if not inside.any():
            continue
        z = l0 * p[0, 2] + l1 * p[1, 2] + l2 * p[2, 2]
        zb = zbuf[y0:y1 + 1, x0:x1 + 1]
        hit = inside & (z <= zb + 1e-3 * max(1.0, abs(z).max()))
        uv = l0[..., None] * d.tc[t[0], :2] + l1[..., None] * d.tc[t[1], :2] + l2[..., None] * d.tc[t[2], :2]
        tx = (np.floor(np.mod(uv[..., 0], 1) * tw).astype(int)) % tw
        ty = (np.floor(np.mod(uv[..., 1], 1) * th).astype(int)) % th
        texel = tex[ty, tx]
        dif = l0[..., None] * d.diffuse[t[0]] + l1[..., None] * d.diffuse[t[1]] + l2[..., None] * d.diffuse[t[2]]
        src = texel * dif
        if ate:
            ref = (aref & 0xFF) / 255.0
            cmp = {0x200: np.zeros_like, 0x201: lambda v: v < ref, 0x202: lambda v: v == ref,
                   0x203: lambda v: v <= ref, 0x204: lambda v: v > ref, 0x205: lambda v: v != ref,
                   0x206: lambda v: v >= ref}.get(afn, lambda v: np.ones_like(v, bool))
            hit &= cmp(src[..., 3]).astype(bool)
        dst = img[y0:y1 + 1, x0:x1 + 1]
        if ben:
            sa = src[..., 3:4]
            out = src[..., :3] * factor(GL_FACTORS.get(sf, 'one'), src[..., :3], sa, dst) + \
                  dst * factor(GL_FACTORS.get(df, 'zero'), src[..., :3], sa, dst)
        else:
            out = src[..., :3]
            zb[hit] = z[hit]
        dst[hit] = out[hit]


def raster(img, zbuf, xyz, col, tris):
    hgt, wid = zbuf.shape
    for t in tris:
        p = xyz[t]
        x0, x1 = int(max(np.floor(p[:, 0].min()), 0)), int(min(np.ceil(p[:, 0].max()), wid - 1))
        y0, y1 = int(max(np.floor(p[:, 1].min()), 0)), int(min(np.ceil(p[:, 1].max()), hgt - 1))
        if x0 > x1 or y0 > y1 or (x1 - x0) * (y1 - y0) > wid * hgt:
            continue
        ys, xs = np.mgrid[y0:y1 + 1, x0:x1 + 1] + 0.5
        (ax, ay), (bx, by), (cx, cy) = p[:, :2]
        den = (by - cy) * (ax - cx) + (cx - bx) * (ay - cy)
        if abs(den) < 1e-12:
            continue
        l0 = ((by - cy) * (xs - cx) + (cx - bx) * (ys - cy)) / den
        l1 = ((cy - ay) * (xs - cx) + (ax - cx) * (ys - cy)) / den
        l2 = 1 - l0 - l1
        inside = (l0 >= 0) & (l1 >= 0) & (l2 >= 0)
        if not inside.any():
            continue
        z = l0 * p[0, 2] + l1 * p[1, 2] + l2 * p[2, 2]
        zb = zbuf[y0:y1 + 1, x0:x1 + 1]
        hit = inside & (z <= zb)
        zb[hit] = z[hit]
        c = l0[..., None] * col[t[0]] + l1[..., None] * col[t[1]] + l2[..., None] * col[t[2]]
        img[y0:y1 + 1, x0:x1 + 1][hit] = c[hit]


def rgb565(c):
    r, g, b = (c >> 11) & 31, (c >> 5) & 63, c & 31
    return np.stack([(r << 3 | r >> 2), (g << 2 | g >> 4), (b << 3 | b >> 2)], -1).astype(np.float64)


def decode_dxt(fmt, raw, w, h):
    """BC1/2/3 to an (h, w, 4) float RGBA image, as the specification decodes."""
    bb = 8 if fmt == 0x0C else 16
    bw, bh = (w + 3) // 4, (h + 3) // 4
    blk = np.frombuffer(raw, np.uint8)[:bw * bh * bb].reshape(bh * bw, bb)
    cb = blk[:, bb - 8:]
    c0 = cb[:, 0].astype(np.uint32) | cb[:, 1].astype(np.uint32) << 8
    c1 = cb[:, 2].astype(np.uint32) | cb[:, 3].astype(np.uint32) << 8
    idx = cb[:, 4:8].copy().view('<u4')[:, 0]
    p0, p1 = rgb565(c0), rgb565(c1)
    four = (c0 > c1) | (fmt != 0x0C)
    pal = np.stack([p0, p1, np.where(four[:, None], (2 * p0 + p1) / 3, (p0 + p1) / 2),
                    np.where(four[:, None], (p0 + 2 * p1) / 3, 0)], 1)
    sel = (idx[:, None] >> (2 * np.arange(16))) & 3
    rgb = np.take_along_axis(pal, sel[:, :, None].astype(np.int64), 1)
    if fmt == 0x0C:
        alpha = np.where(~four[:, None] & (sel == 3), 0.0, 255.0)
    elif fmt == 0x0E:
        nib = np.stack([blk[:, :8] & 15, blk[:, :8] >> 4], -1).reshape(-1, 16)
        alpha = nib * 17.0
    else:
        a0, a1 = blk[:, 0].astype(np.float64), blk[:, 1].astype(np.float64)
        i = np.arange(2, 8)
        hi = ((8 - i) * a0[:, None] + (i - 1) * a1[:, None]) / 7
        lo = np.concatenate([((6 - i[:4]) * a0[:, None] + (i[:4] - 1) * a1[:, None]) / 5,
                             np.zeros((len(a0), 1)), np.full((len(a0), 1), 255.0)], 1)
        ap = np.concatenate([a0[:, None], a1[:, None], np.where((a0 > a1)[:, None], hi, lo)], 1)
        bits = np.zeros(len(blk), np.uint64)
        for k in range(6):
            bits |= blk[:, 2 + k].astype(np.uint64) << np.uint64(8 * k)
        ai = (bits[:, None] >> (np.uint64(3) * np.arange(16, dtype=np.uint64))) & np.uint64(7)
        alpha = np.take_along_axis(ap, ai.astype(np.int64), 1)
    img = np.concatenate([rgb, alpha[:, :, None]], -1) / 255.0      # (blocks, 16, 4)
    img = img.reshape(bh, bw, 4, 4, 4).transpose(0, 2, 1, 3, 4).reshape(bh * 4, bw * 4, 4)
    return img[:h, :w]


def texture(d):
    fmt, w, h = (d.tex[1] >> 8) & 0xFF, 1 << ((d.tex[1] >> 20) & 15), 1 << ((d.tex[1] >> 24) & 15)
    if d.texbytes and fmt in (0x0C, 0x0E, 0x0F):
        return decode_dxt(fmt, d.texbytes, w, h)
    return None


GL_FACTORS = {0: 'zero', 1: 'one', 0x302: 'sa', 0x303: '1-sa', 0x306: 'dc', 0x300: 'sc', 0x304: 'da', 0x305: '1-da'}


def factor(name, src, srca, dst):
    return {'zero': 0.0, 'one': 1.0, 'sa': srca, '1-sa': 1 - srca, 'dc': dst, 'sc': src}.get(name, 1.0)


def cmd_render(a):
    draws = [d for d in read(a.file) if d.frame == (a.frame or 0)]
    lo, hi = (int(x) for x in a.draws.split('-')) if a.draws else (0, 1 << 30)
    wid, hgt = (int(x) for x in a.size.split('x'))
    img = np.zeros((hgt, wid, 3))
    zbuf = np.full((hgt, wid), np.inf)
    drawn = 0
    for d in draws:
        if d.mode & 3 == 2 or not lo <= d.index <= hi or d.prim not in (5, 6, 7, 8, 9, 10):
            continue
        xyz, w = screen(d)
        tris = triangles(d)
        tris = tris[(w[tris] > 0).all(axis=1)] if len(tris) else tris
        if a.mode == 'textured':
            tex = texture(d)
            if tex is not None:
                raster_tex(img, zbuf, xyz, d, tex, tris)
                drawn += 1
            continue
        if a.mode == 'tex':
            col = np.stack([np.mod(d.tc[:, 0], 1), np.mod(d.tc[:, 1], 1), np.zeros(d.n)], axis=1)
        elif a.mode == 'depth':
            col = np.repeat(np.clip(xyz[:, 2:3] / (np.abs(xyz[:, 2]).max() or 1), 0, 1), 3, axis=1)
        else:
            col = d.diffuse[:, :3]
        raster(img, zbuf, xyz, col, tris)
        drawn += 1
    Image.fromarray((np.clip(img, 0, 1) * 255).astype(np.uint8)).save(a.out)
    print(f'{a.out}: {drawn} of {len(draws)} draws of frame {a.frame or 0}')


def key(d):
    return (d.tex[0], d.tex[1] >> 8 & 0xFF, d.tex[2], d.n, d.nidx, d.prim)


def cmd_diff(a):
    fa = [d for d in read(a.a) if a.frame is None or d.frame == a.frame]
    fb = [d for d in read(a.b) if a.frame is None or d.frame == a.frame]
    byk = {}
    for d in fb:
        byk.setdefault((d.n, d.nidx, d.prim), []).append(d)
    for d in fa:
        cands = byk.get((d.n, d.nidx, d.prim), [])
        if not cands:
            continue
        best = min(cands, key=lambda e: np.abs(e.tc[:, :2] - d.tc[:, :2]).max())
        dtc = np.abs(best.tc[:, :2] - d.tc[:, :2]).max()
        ddf = np.abs(best.diffuse - d.diffuse).max()
        same_idx = d.nidx == 0 or np.array_equal(d.idx, best.idx)
        if dtc > 1e-4 or ddf > 0.02 or not same_idx:
            print(f'a f{d.frame} #{d.index} ~ b f{best.frame} #{best.index}: n {d.n} '
                  f'texcoord max diff {dtc:.4g} diffuse {ddf:.3f} indices {"same" if same_idx else "DIFFER"}')


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest='cmd', required=True)
    s = sub.add_parser('list')
    s.add_argument('file')
    s.add_argument('--frame', type=int)
    s.set_defaults(fn=cmd_list)
    s = sub.add_parser('render')
    s.add_argument('file')
    s.add_argument('out')
    s.add_argument('--frame', type=int)
    s.add_argument('--mode', choices=('diffuse', 'tex', 'depth', 'textured'), default='diffuse')
    s.add_argument('--size', default='640x480')
    s.add_argument('--draws')
    s.set_defaults(fn=cmd_render)
    s = sub.add_parser('diff')
    s.add_argument('a')
    s.add_argument('b')
    s.add_argument('--frame', type=int)
    s.set_defaults(fn=cmd_diff)
    a = p.parse_args()
    a.fn(a)


if __name__ == '__main__':
    sys.exit(main())
