"""Convert profiler framebuffer dumps (fb_top.raw / fb_bottom.raw, BGR8, 3DS-rotated) to PNGs."""
import struct, sys, os
from PIL import Image

def convert(src, dst):
    data = open(src, 'rb').read()
    w, h = struct.unpack('<HH', data[:4])          # w = 240 (short side), h = 400/800/320
    px = data[4:]
    img = Image.frombytes('RGB', (w, h), px, 'raw', 'BGR')   # stored column-major from the bottom-left
    img = img.transpose(Image.Transpose.ROTATE_90)            # -> landscape
    img.save(dst)
    print(dst, img.size)

if __name__ == '__main__':
    d = sys.argv[1]
    for n in ('fb_top', 'fb_bottom'):
        if os.path.exists(os.path.join(d, n + '.raw')):
            convert(os.path.join(d, n + '.raw'), os.path.join(sys.argv[2], n + '.png'))

def convert_view(src, dst):
    """fb_view.raw: w, h, pitch (u16), 256 RGB565 palette entries, 8-bit pixels."""
    import struct
    data = open(src, 'rb').read()
    w, h, pitch = struct.unpack('<HHH', data[:6])
    pal = struct.unpack('<256H', data[6:518])
    rgb = [((p >> 11) << 3, ((p >> 5) & 63) << 2, (p & 31) << 3) for p in pal]
    px = data[518:]
    img = Image.new('RGB', (w, h))
    img.putdata([rgb[px[y * pitch + x]] for y in range(h) for x in range(w)])
    img.save(dst)
    print(dst, img.size)

if __name__ == '__main__' and os.path.exists(os.path.join(sys.argv[1], 'fb_view.raw')):
    convert_view(os.path.join(sys.argv[1], 'fb_view.raw'), os.path.join(sys.argv[2], 'fb_view.png'))
