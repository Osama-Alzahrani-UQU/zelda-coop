"""Draws the setup's icon (two linked rings in gold on a deep green disc: two players together) as a multi-size .ico.
python -I make_icon.py out.ico"""
import sys

from PIL import Image, ImageDraw


def draw(size):
    s = size * 4   # (drawn large, then scaled down smooth)
    im = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    d.ellipse((s * 0.03, s * 0.03, s * 0.97, s * 0.97), fill=(22, 74, 52, 255), outline=(214, 178, 92, 255), width=max(2, s // 24))
    r = s * 0.2
    w = max(3, s // 14)
    for cx, col in ((s * 0.39, (240, 206, 120, 255)), (s * 0.61, (250, 228, 160, 255))):
        d.ellipse((cx - r, s * 0.5 - r, cx + r, s * 0.5 + r), outline=col, width=w)
    return im.resize((size, size), Image.LANCZOS)


out = sys.argv[1]
sizes = [16, 24, 32, 48, 64, 128, 256]
images = [draw(n) for n in sizes]
images[-1].save(out, format="ICO", sizes=[(n, n) for n in sizes], append_images=images[:-1])
print("icon:", out)
