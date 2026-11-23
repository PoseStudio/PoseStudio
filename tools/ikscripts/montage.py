# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Ion Hatzithomas and the PoseStudio contributors.
# Licensed under the GNU AGPL v3 with the additional terms in LICENSE-ADDITIONAL-TERMS.txt.
"""Builds a run's CONTACT SHEET: every shot of a folder in one image, to look at a whole script.

usage: montage.py <outdir> <sheet.png> [cols] [cropfrac]

Each <outdir>/*.png (an earlier sheet*.png aside) is cropped to the middle <cropfrac> of its
width — where the figure stands; 0.42 unless given, 1 keeps the whole frame — scaled to one
height and labelled with its file name; <cols> shots to a row (4). run.sh calls it after a
script (SHEET_COLS / SHEET_CROP set the two). Needs Pillow.
"""
import sys, os, glob
from PIL import Image, ImageDraw
out, sheet = sys.argv[1], sys.argv[2]
cols = int(sys.argv[3]) if len(sys.argv) > 3 else 4
crop = float(sys.argv[4]) if len(sys.argv) > 4 else 0.42
files = sorted(f for f in glob.glob(os.path.join(out, '*.png')) if not os.path.basename(f).startswith('sheet'))
thumbs = []
for f in files:
    im = Image.open(f).convert('RGB')
    w, h = im.size
    cw = int(w * crop)
    im = im.crop(((w - cw) // 2, 0, (w + cw) // 2, h))
    th = 520
    im = im.resize((int(im.size[0] * th / im.size[1]), th))
    d = ImageDraw.Draw(im)
    d.rectangle((0, 0, im.size[0], 16), fill=(20, 20, 20))
    d.text((4, 2), os.path.splitext(os.path.basename(f))[0], fill=(255, 255, 120))
    thumbs.append(im)
if not thumbs:
    sys.exit('no shots')
tw, th = thumbs[0].size
rows = (len(thumbs) + cols - 1) // cols
sheetim = Image.new('RGB', (tw * cols, th * rows), (30, 30, 30))
for i, im in enumerate(thumbs):
    sheetim.paste(im, ((i % cols) * tw, (i // cols) * th))
sheetim.save(sheet)
print('sheet', sheet, sheetim.size, len(thumbs), 'shots')
