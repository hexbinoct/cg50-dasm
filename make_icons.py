"""Generate the 92x64 unselected/selected menu icons for DASM.g3a (fxsdk generate_g3a)."""
from PIL import Image, ImageDraw
import os

d = os.path.join(os.path.dirname(os.path.abspath(__file__)), "assets-cg")
os.makedirs(d, exist_ok=True)
for name, bg, fg, acc in (("icon-uns.png", (24, 26, 32), (220, 220, 220), (90, 200, 120)),
                          ("icon-sel.png", (0, 70, 160), (255, 255, 255), (255, 220, 80))):
    im = Image.new("RGB", (92, 64), bg)
    dr = ImageDraw.Draw(im)
    dr.rectangle((1, 1, 90, 62), outline=fg)
    # a few fake listing rows
    for i, (w, c) in enumerate(((22, acc), (34, fg), (18, fg), (40, acc), (26, fg))):
        y = 8 + i * 6
        dr.rectangle((8, y, 8 + 10, y + 2), fill=(120, 120, 130))
        dr.rectangle((22, y, 22 + w, y + 2), fill=c)
    dr.text((14, 44), "DASM", fill=fg)
    dr.text((52, 44), "SH4", fill=acc)
    im.save(os.path.join(d, name))
    print("wrote", name)
