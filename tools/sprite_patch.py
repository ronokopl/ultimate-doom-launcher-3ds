"""PNG sprite -> Doom patch, bit-exact with what ZDoom 2.8.1's software renderer makes of the PNG.

Rules mirrored from engine/zdoom/src (textures/pngtexture.cpp, v_palette.cpp, colormatcher.cpp):
  paletted (type 3): tRNS == 0 -> transparent; other entries -> exact PLAYPAL match among 1..255 (FPalette::MakeRemap),
                     else BestColor over 1..254 (BestColor(first=1, num=255) stops before 255)
  RGBA (type 6):     alpha < 128 -> transparent; else RGB32k[r>>3][g>>3][b>>3], RGB32k = BestColor of the
                     5-bit colour expanded as (c<<3)|(c>>2)
  grAb chunk:        left/top offsets (out of int16 range -> 0, as the engine does)
Patch pixels go through GPalette.Remap, which is the identity for 1..255, and no rule above yields index 0.
Anything else (other colour types, 16-bit, alPh, taller than 254 rows) is left as PNG.
"""
import io, struct, zlib
import numpy as np
from PIL import Image


def read_playpal(wad_path):
    data = open(wad_path, 'rb').read()
    n, off = struct.unpack('<4xII', data[:12])
    for k in range(n):
        pos, size, name = struct.unpack('<II8s', data[off + 16 * k: off + 16 * k + 16])
        if name.rstrip(b'\0') == b'PLAYPAL':
            return np.frombuffer(data[pos:pos + 768], np.uint8).reshape(256, 3).astype(np.int32)
    raise ValueError('no PLAYPAL in ' + wad_path)


class Palette:
    def __init__(self, playpal):
        self.pal = playpal
        cand = playpal[1:255]  # BestColor candidates 1..254
        self._cand = cand
        self.exact = {}
        for i in range(255, 0, -1):  # exact matches over 1..255, lowest index wins for duplicate colours
            self.exact[tuple(playpal[i])] = i
        c = np.arange(32)
        c = (c << 3) | (c >> 2)
        rgb = np.stack(np.meshgrid(c, c, c, indexing='ij'), -1).reshape(-1, 3)
        self.rgb32k = self.best(rgb).reshape(32, 32, 32)

    def best(self, rgb):
        """BestColor(first=1, num=255) for an (n, 3) array: first minimum distance (lowest index) wins."""
        rgb = np.asarray(rgb, np.int32)
        out = np.empty(len(rgb), np.uint8)
        for s in range(0, len(rgb), 4096):
            d = ((rgb[s:s + 4096, None, :] - self._cand[None, :, :]) ** 2).sum(-1)
            out[s:s + 4096] = d.argmin(1) + 1
        return out

    def remap(self, colors):
        """FPalette::MakeRemap for a foreign palette: exact match else BestColor."""
        return np.array([self.exact.get(tuple(c)) or int(self.best([c])[0]) for c in colors], np.uint8)


def chunks(data):
    pos = 8
    while pos + 8 <= len(data):
        ln, cid = struct.unpack('>I4s', data[pos:pos + 8])
        yield cid, data[pos + 8:pos + 8 + ln]
        pos += 12 + ln
        if cid == b'IEND':
            break


def to_patch(data, palette):
    """PNG bytes -> Doom patch bytes, or None when the PNG must stay as it is."""
    if data[:8] != b'\x89PNG\r\n\x1a\n':
        return None
    w, h, depth, ctype = struct.unpack('>IIBB', data[16:26])
    if ctype not in (3, 6) or (ctype == 6 and depth != 8) or h > 254 or w > 2048:
        return None
    left = top = 0
    plte, trns = None, None
    for cid, body in chunks(data):
        if cid == b'alPh':
            return None
        if cid == b'grAb':
            x, y = struct.unpack('>ii', body[:8])
            left = x if -32768 <= x <= 32767 else 0
            top = y if -32768 <= y <= 32767 else 0
        elif cid == b'PLTE':
            plte = np.frombuffer(body[:len(body) // 3 * 3], np.uint8).reshape(-1, 3)[:256].astype(np.int32)
        elif cid == b'tRNS':
            trns = body
    try:  # the engine ignores chunk CRCs (offset editors often leave grAb's stale); Pillow checks them
        fixed = data[:8] + b''.join(struct.pack('>I', len(b)) + c + b + struct.pack('>I', zlib.crc32(c + b))
                                    for c, b in chunks(data))
        im = Image.open(io.BytesIO(fixed))
        im.load()
    except Exception:  # Pillow is stricter than the engine (CRCs, truncation): leave such files as PNG
        return None
    if ctype == 3:
        if plte is None:
            return None
        idx = np.array(im, np.uint8)  # palette indices (Pillow expands 1/2/4-bit)
        if im.mode != 'P' or idx.shape != (h, w):
            return None
        trans = np.full(256, 255, np.uint8)
        if trns is not None:
            trans[:len(trns)] = np.frombuffer(trns[:256], np.uint8)
        pmap = np.zeros(256, np.uint8)
        pmap[:len(plte)] = palette.remap(plte)
        opaque_entry = trans != 0
        opaque_entry[len(plte):] = False  # out-of-palette indices: engine maps through a short table; keep PNG
        if (idx >= len(plte)).any():
            return None
        pix = pmap[idx]
        mask = opaque_entry[idx]
    else:
        a = np.array(im.convert('RGBA') if im.mode != 'RGBA' else im, np.uint8)
        mask = a[:, :, 3] >= 128
        pix = palette.rgb32k[a[:, :, 0] >> 3, a[:, :, 1] >> 3, a[:, :, 2] >> 3]
    cols = []
    for x in range(w):
        col, m, y = bytearray(), mask[:, x], 0
        while y < h:
            if not m[y]:
                y += 1
                continue
            start = y
            while y < h and m[y] and y - start < 254:
                y += 1
            col += bytes((start, y - start, 0)) + pix[start:y, x].tobytes() + b'\0'
        cols.append(bytes(col) + b'\xff')
    head = struct.pack('<hhhh', w, h, left, top)
    offs, pos = [], 8 + 4 * w
    for c in cols:
        offs.append(pos)
        pos += len(c)
    return head + struct.pack('<%dI' % w, *offs) + b''.join(cols)
