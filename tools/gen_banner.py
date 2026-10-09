"""Build sheet banner_logo / banner_text / banner_voxels -> gen/banner.png (256x128 HOME Menu banner).

The HOME Menu banner is a flat 256x128 picture, so the 3D look is pre-rendered here: the logo is voxelized
(one cube per banner_voxels-wide grid cell, colour = average of the logo pixels it covers), extruded, turned a
little towards the viewer and lit (top faces bright, side faces dark, cube edges outlined). The subtitle is
set in Doom's own HUD font (STCFN glyphs from the IWAD) and extruded the same way.
"""
import os, struct
import numpy as np
from PIL import Image, ImageDraw, ImageFilter
from export import rows
from pk3 import ROOT, GEN, IWADS

PARENT = os.path.dirname(ROOT)
OUT = os.path.join(GEN, 'banner.png')
SS = 4  # supersampling


def wad_lumps(path):
    data = open(path, 'rb').read()
    n, off = struct.unpack('<4xii', data[:12])
    out = {}
    for k in range(n):
        pos, size, nm = struct.unpack('<ii8s', data[off + 16 * k:off + 16 * k + 16])
        out[nm.split(b'\0')[0].decode('latin1').upper()] = data[pos:pos + size]
    return out


def patch_rgba(data, pal):
    w, h = struct.unpack('<hh', data[:4])
    img = np.zeros((h, w, 4), np.uint8)
    for x, p in enumerate(struct.unpack('<%dI' % w, data[8:8 + 4 * w])):
        while data[p] != 0xFF:
            top, n = data[p], data[p + 1]
            img[top:top + n, x, :3] = pal[np.frombuffer(data[p + 3:p + 3 + n], np.uint8)]
            img[top:top + n, x, 3] = 255
            p += n + 4
    return img


def text_rgba(text, lumps, pal):
    """Doom HUD font (STCFNnnn) string -> RGBA array, 1 px gap between glyphs, 4 px spaces."""
    glyphs = []
    for ch in text.upper():
        name = 'STCFN%03d' % ord(ch)
        glyphs.append(patch_rgba(lumps[name], pal) if name in lumps else np.zeros((7, 4, 4), np.uint8))
    h = max(g.shape[0] for g in glyphs)
    out = np.zeros((h, sum(g.shape[1] + 1 for g in glyphs), 4), np.uint8)
    x = 0
    for g in glyphs:
        out[h - g.shape[0]:, x:x + g.shape[1]] = g
        x += g.shape[1] + 1
    return out


def voxelize(rgba, cols):
    """RGBA image -> (mask, colours) on a grid `cols` cells wide (square cells)."""
    h, w = rgba.shape[:2]
    cell = w / cols
    rows_ = max(1, round(h / cell))
    mask = np.zeros((rows_, cols), bool)
    col = np.zeros((rows_, cols, 3), np.float32)
    for j in range(rows_):
        for i in range(cols):
            blk = rgba[int(j * cell):max(int(j * cell) + 1, int((j + 1) * cell)),
                       int(i * cell):max(int(i * cell) + 1, int((i + 1) * cell))].reshape(-1, 4).astype(np.float32)
            a = blk[:, 3] / 255
            if a.mean() > 0.45:
                mask[j, i] = True
                col[j, i] = (blk[:, :3] * a[:, None]).sum(0) / a.sum()
    return mask, col


def render(draw, mask, col, depth, box, yaw, pitch, outline=0.55):
    """Painter's-algorithm cube renderer (orthographic), scaled and centred to fit box = (x0, y0, x1, y1) px.
    Only faces that can be visible from the front/top/turned side are emitted."""
    cy, sy, cp, sp = np.cos(yaw), np.sin(yaw), np.cos(pitch), np.sin(pitch)

    def proj(x, y, z):  # x right, y down, z away from viewer; unit cubes
        x1, z1 = x * cy + z * sy, -x * sy + z * cy
        y1, z2 = y * cp - z1 * sp, y * sp + z1 * cp
        return x1, y1, z2

    faces = []
    R, C = mask.shape
    side = 1 if yaw < 0 else -1  # which vertical side faces the viewer
    for j in range(R):
        for i in range(C):
            if not mask[j, i]:
                continue
            c = col[j, i]
            for k in range(depth):
                if k == 0:  # front
                    faces.append(((i, j, 0), (i + 1, j, 0), (i + 1, j + 1, 0), (i, j + 1, 0), c * 1.0))
                if j == 0 or not mask[j - 1, i]:  # top
                    faces.append(((i, j, k), (i + 1, j, k), (i + 1, j, k + 1), (i, j, k + 1), c * 1.3))
                ni = i + side
                if ni < 0 or ni >= C or not mask[j, ni]:  # visible side
                    x = i + (1 if side > 0 else 0)
                    faces.append(((x, j, k), (x, j, k + 1), (x, j + 1, k + 1), (x, j + 1, k), c * 0.6))
    polys = []
    for f in faces:
        pts = [proj(*p) for p in f[:4]]
        polys.append((sum(p[2] for p in pts) / 4, [(p[0], p[1]) for p in pts], f[4]))
    polys.sort(key=lambda t: -t[0])  # far first
    xs = [p[0] for _, pts, _ in polys for p in pts]
    ys = [p[1] for _, pts, _ in polys for p in pts]
    scale = min((box[2] - box[0]) / (max(xs) - min(xs)), (box[3] - box[1]) / (max(ys) - min(ys)))
    ox = box[0] + ((box[2] - box[0]) - (max(xs) - min(xs)) * scale) / 2 - min(xs) * scale
    oy = box[1] + ((box[3] - box[1]) - (max(ys) - min(ys)) * scale) / 2 - min(ys) * scale
    for _, pts, c in polys:
        fill = tuple(int(v) for v in np.clip(c, 0, 255))
        draw.polygon([(ox + x * scale, oy + y * scale) for x, y in pts], fill=fill,
                     outline=tuple(int(v * outline) for v in fill))


def main():
    B = {r['key']: r.get('value', '~') for r in rows('build', 'Build')}
    W, H = 256 * SS, 128 * SS
    # background: black to deep red, warm glow behind the logo
    y = np.linspace(0, 1, H)[:, None]
    bg = np.zeros((H, W, 3), np.float32)
    bg[..., 0] = 20 + 70 * y ** 1.5
    bg[..., 1] = 4 + 8 * y
    bg[..., 2] = 4
    xx, yy = np.meshgrid(np.linspace(-1, 1, W), np.linspace(-1, 1, H))
    glow = np.exp(-((xx / 0.8) ** 2 + ((yy + 0.25) / 0.55) ** 2) * 2.2)
    bg += glow[..., None] * np.array([110, 40, 6])
    img = Image.fromarray(np.clip(bg, 0, 255).astype(np.uint8)).convert('RGBA')

    # logo voxels
    logo = Image.open(os.path.join(PARENT, B['banner_logo'])).convert('RGBA')
    logo = logo.crop(logo.getbbox())
    mask, col = voxelize(np.array(logo), int(B['banner_voxels']))
    layer = Image.new('RGBA', (W, H), (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    R, C = mask.shape
    render(d, mask, col, 4, (10 * SS, 4 * SS, 246 * SS, 92 * SS), yaw=-0.30, pitch=0.22)

    # subtitle in the Doom HUD font, extruded
    lumps = wad_lumps(IWADS['doom2'])
    pal = np.frombuffer(lumps['PLAYPAL'][:768], np.uint8).reshape(256, 3)
    txt = text_rgba(B['banner_text'], lumps, pal)
    tmask = txt[..., 3] > 0
    tcol = txt[..., :3].astype(np.float32)
    # recolour the red font to the logo's gold-to-orange
    g = np.linspace(0, 1, tmask.shape[0])[:, None, None]
    tcol = (np.array([255, 214, 90]) * (1 - g) + np.array([230, 100, 20]) * g) * (0.55 + 0.45 * tcol.max(-1, keepdims=True) / 255)
    render(d, tmask, tcol, 2, (12 * SS, 99 * SS, 244 * SS, 123 * SS), yaw=-0.10, pitch=0.35, outline=0.75)

    # drop shadow, then compose and downsample
    shadow = Image.new('RGBA', (W, H), (0, 0, 0, 0))
    shadow.paste((0, 0, 0, 170), (3 * SS, 4 * SS), layer)
    shadow = shadow.filter(ImageFilter.GaussianBlur(3 * SS))
    img = Image.alpha_composite(Image.alpha_composite(img, shadow), layer)
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    img.convert('RGB').resize((256, 128), Image.LANCZOS).save(OUT)
    print(OUT, 'logo %dx%d voxels, text %dx%d' % (C, R, tmask.shape[1], tmask.shape[0]))


if __name__ == '__main__':
    main()
