"""Contact sheet of a GBARECOMP_FRAMEDUMP_DIR dump: frames that differ from the previous kept one.

usage: python tools/frame_sheet.py DIR OUT.png [--start N] [--end N] [--every K] [--cols C] [--max M]
                                   [--crop x,y,w,h] [--scale S]
"""
import argparse
import glob
import os

from PIL import Image, ImageChops

p = argparse.ArgumentParser()
p.add_argument("dir")
p.add_argument("out")
p.add_argument("--start", type=int, default=0)
p.add_argument("--end", type=int, default=10**9)
p.add_argument("--every", type=int, default=4)
p.add_argument("--cols", type=int, default=8)
p.add_argument("--max", type=int, default=64)
p.add_argument("--scale", type=int, default=1)
p.add_argument("--crop", help="x,y,w,h")
p.add_argument("--label", action="store_true")
a = p.parse_args()
files = sorted(glob.glob(os.path.join(a.dir, "f_*.png")))
files = [f for f in files if a.start <= int(os.path.basename(f)[2:8]) < a.end][:: a.every]
kept, last = [], None
for f in files:
    img = Image.open(f).convert("RGB")
    if a.crop:
        x, y, w, h = map(int, a.crop.split(","))
        img = img.crop((x, y, x + w, y + h))
    if last is None or ImageChops.difference(img, last).getbbox():
        kept.append((os.path.basename(f)[2:8], img))
        last = img
kept = kept[: a.max]
if not kept:
    raise SystemExit("no frames")
w, h = kept[0][1].size
rows = (len(kept) + a.cols - 1) // a.cols
sheet = Image.new("RGB", (a.cols * (w + 2), rows * (h + 2)), (255, 0, 255))
for i, (_, img) in enumerate(kept):
    sheet.paste(img, ((i % a.cols) * (w + 2), (i // a.cols) * (h + 2)))
if a.scale > 1:
    sheet = sheet.resize((sheet.width * a.scale, sheet.height * a.scale), Image.NEAREST)
sheet.save(a.out)
print(a.out, len(kept), "frames:", kept[0][0], "..", kept[-1][0])
