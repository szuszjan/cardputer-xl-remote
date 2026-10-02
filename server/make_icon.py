"""Draws icon.ico (used by the exe and the server window). Run once: python make_icon.py"""
from PIL import Image, ImageDraw

S = 256
img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
d = ImageDraw.Draw(img)
d.rounded_rectangle((8, 8, S - 8, S - 8), 44, fill=(20, 25, 28))
# PC monitor
d.rounded_rectangle((40, 44, 216, 156), 12, fill=(45, 187, 199))
d.rectangle((54, 58, 202, 142), fill=(16, 32, 36))
d.rectangle((116, 156, 140, 178), fill=(45, 187, 199))
d.rounded_rectangle((88, 176, 168, 188), 6, fill=(45, 187, 199))
# Cardputer: small device with a keyboard grid, overlapping the corner
d.rounded_rectangle((132, 118, 236, 232), 14, fill=(240, 160, 80), outline=(20, 25, 28), width=6)
d.rectangle((146, 132, 222, 168), fill=(16, 32, 36))
for r in range(3):
    for c in range(5):
        x, y = 146 + c * 16, 178 + r * 16
        d.rectangle((x, y, x + 11, y + 10), fill=(20, 25, 28))
# "signal" lines between them
for i, w in enumerate((10, 7, 4)):
    d.arc((70 - i * 14, 80 - i * 14, 120 + i * 14, 130 + i * 14), 200, 250, fill=(83, 211, 179), width=w)
img.save("icon.ico", sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])
img.save("icon.png")
