"""Build sheet 'icon_source' ('<iwad>:<LUMP>', a Doom patch in an IWAD) -> platform/3ds/icon.png (48x48 SMDH icon)."""
import os, struct
from PIL import Image
from export import rows
from pk3 import IWADS, ROOT

OUT = os.path.join(ROOT, 'platform', '3ds', 'icon.png')

def wad_lump(path, name):
    with open(path, 'rb') as f:
        magic, n, off = struct.unpack('<4sii', f.read(12))
        f.seek(off)
        d = f.read(16 * n)
        for i in range(n):
            pos, size, lname = struct.unpack('<ii8s', d[i * 16:i * 16 + 16])
            if lname.split(b'\0')[0] == name.encode():
                f.seek(pos)
                return f.read(size)
    raise KeyError(name)

def decode_patch(data, pal):
    w, h, lo, to = struct.unpack('<hhhh', data[:8])
    img = Image.new('RGBA', (w, h), (0, 0, 0, 0))
    px = img.load()
    for x in range(w):
        p = struct.unpack('<I', data[8 + 4 * x:12 + 4 * x])[0]
        while data[p] != 0xFF:
            top, length = data[p], data[p + 1]
            for i in range(length):
                c = data[p + 3 + i]
                px[x, top + i] = (pal[3 * c], pal[3 * c + 1], pal[3 * c + 2], 255)
            p += length + 4
    return img

def main():
    B = {r['key']: r.get('value') for r in rows('build', 'Build')}
    iwad, lump = B['icon_source'].split(':')
    logo = decode_patch(wad_lump(IWADS[iwad], lump), wad_lump(IWADS[iwad], 'PLAYPAL')[:768])
    scale = 46 / max(logo.size)	# pixel art: nearest-neighbour upscale keeps it crisp
    logo = logo.resize((round(logo.width * scale), round(logo.height * scale)), Image.NEAREST)
    icon = Image.new('RGBA', (48, 48), (10, 0, 0, 255))
    icon.alpha_composite(logo, ((48 - logo.width) // 2, (48 - logo.height) // 2))
    icon.convert('RGB').save(OUT)
    print(OUT, logo.size)

if __name__ == '__main__':
    main()
