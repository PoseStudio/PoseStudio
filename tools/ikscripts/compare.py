# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Ion Hatzithomas and the PoseStudio contributors.
# Licensed under the GNU AGPL v3 with the additional terms in LICENSE-ADDITIONAL-TERMS.txt.
"""Compares a run's shots with reference shots: which poses LOOK different from last time.

usage: compare.py <outdir> <refdir> [--accept]

Per shot: the mean absolute difference (0-255) and the share of pixels that changed by more than
a threshold. A shot over the limits is listed as CHANGED and goes into <outdir>/changes.png — the
reference beside the new frame — so there is one image to look at, holding only what moved.
--accept copies the run's shots over the references (after you have looked).

The references are renders of a figure preset you own the rights to look at, not to publish:
keep them OUTSIDE the repository.
"""
import glob
import os
import shutil
import sys

from PIL import Image, ImageChops, ImageDraw

# Both numbers are read off the BRIGHTNESS of the difference (its three channels weighed as a
# grey conversion weighs them), which is what the eye goes by and is blind to some things: a
# change in blue alone counts for a ninth of the same change in all three, and a difference of
# a few levels on a few hundred pixels is "same". For an exact comparison, diff the files.
PIXEL_THRESHOLD = 24      # a pixel "changed" when the brightness of its difference is this or more
CHANGED_SHARE = 0.002     # a shot changed when more than this share of its pixels did ...
CHANGED_MEAN = 0.35       # ... or its mean absolute difference is over this


def shots(folder):
    return {os.path.basename(f): f for f in glob.glob(os.path.join(folder, '*.png'))
            if os.path.basename(f) not in ('sheet.png', 'changes.png')}


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    accept = '--accept' in sys.argv
    if len(args) < 2:
        sys.exit(__doc__)
    out, ref = args[0], args[1]
    new, old = shots(out), shots(ref)
    if accept:
        os.makedirs(ref, exist_ok=True)
        for name, path in new.items():
            shutil.copyfile(path, os.path.join(ref, name))
        print('accepted %d shot(s) into %s' % (len(new), ref))
        return
    changed = []
    for name in sorted(new):
        if name not in old:
            print('  NEW       %s' % name)
            continue
        a = Image.open(old[name]).convert('RGB')
        b = Image.open(new[name]).convert('RGB')
        if a.size != b.size:
            a = a.resize(b.size)
        diff = ImageChops.difference(a, b)
        histogram = diff.convert('L').histogram()
        pixels = float(b.size[0] * b.size[1])
        mean = sum(i * c for i, c in enumerate(histogram)) / pixels
        share = sum(histogram[PIXEL_THRESHOLD:]) / pixels
        moved = share > CHANGED_SHARE or mean > CHANGED_MEAN
        print('  %s %-40s mean %.2f, %.2f%% of pixels' % ('CHANGED  ' if moved else 'same     ', name, mean, share * 100.0))
        if moved:
            changed.append((name, a, b))
    for name in sorted(set(old) - set(new)):
        print('  MISSING   %s' % name)
    target = os.path.join(out, 'changes.png')
    if os.path.exists(target):
        os.remove(target)
    if changed:
        # The reference beside the new frame, the middle of each (where the figure stands).
        rows = []
        for name, a, b in changed:
            w, h = b.size
            cw = int(w * 0.36)
            box = ((w - cw) // 2, 0, (w + cw) // 2, h)
            pair = Image.new('RGB', (cw * 2 + 6, h), (200, 60, 60))
            pair.paste(a.crop(box), (0, 0))
            pair.paste(b.crop(box), (cw + 6, 0))
            th = 420
            pair = pair.resize((int(pair.size[0] * th / pair.size[1]), th))
            d = ImageDraw.Draw(pair)
            d.rectangle((0, 0, pair.size[0], 16), fill=(20, 20, 20))
            d.text((4, 2), os.path.splitext(name)[0] + '   (reference | now)', fill=(255, 255, 120))
            rows.append(pair)
        cols = 3
        tw, th = rows[0].size
        sheet = Image.new('RGB', (tw * min(cols, len(rows)), th * ((len(rows) + cols - 1) // cols)), (30, 30, 30))
        for i, im in enumerate(rows):
            sheet.paste(im, ((i % cols) * tw, (i // cols) * th))
        sheet.save(target)
        print('  %d shot(s) changed: %s' % (len(changed), target))
    else:
        print('  no shot changed')


if __name__ == '__main__':
    main()
