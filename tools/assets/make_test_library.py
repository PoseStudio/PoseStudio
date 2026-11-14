"""Writes a small stand-in Materials library for looking at the Materials Library tab.

    python tools/assets/make_test_library.py <folder>

makes <folder>/Materials — a few category folders (one nested, one empty) of stand-in material
files, each with a same-name thumbnail beside it, which is the Asset Manager's rule for what an
asset is — and <folder>/triangle.obj, a model for a scripted run to start on. Point the app at
the library with POSESTUDIO_MATERIALS_DIR=<folder>/Materials: the tab then browses it instead
of the user's own "My PoseStudio Library/Materials", which stays untouched
(tools/ikscripts/library/materials_library.txt is the script that looks at it). Needs Pillow.
"""
import os
import sys

from PIL import Image, ImageDraw

LIBRARY = {
    'Skin': [('Pale', (236, 205, 188)), ('Tan', (196, 150, 118)), ('Dark', (110, 74, 58)),
             ('Freckled', (224, 180, 150))],
    'Metals': [('Gold', (212, 175, 55)), ('Copper', (184, 115, 51)), ('Brushed Steel', (150, 155, 160))],
    'Metals/Painted': [('Car Red', (170, 20, 24)), ('Racing Green', (10, 80, 50))],
    'Glass': [('Clear', (200, 225, 235)), ('Green Bottle', (60, 120, 80))],
    'Fabric': [],
    '': [('Chalk', (235, 235, 230))],
}


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    root = sys.argv[1]
    for folder, items in LIBRARY.items():
        path = os.path.join(root, 'Materials', folder)
        os.makedirs(path, exist_ok=True)
        for name, color in items:
            with open(os.path.join(path, name + '.material'), 'w') as f:
                f.write('{ "stand-in material": "%s" }\n' % name)
            image = Image.new('RGB', (256, 256), (38, 39, 41))
            draw = ImageDraw.Draw(image)
            draw.ellipse((28, 28, 228, 228), fill=color)
            draw.ellipse((70, 60, 130, 110), fill=tuple(min(255, c + 50) for c in color))
            image.save(os.path.join(path, name + '.png'))
    with open(os.path.join(root, 'triangle.obj'), 'w') as f:
        f.write('v -0.2 0 0\nv 0.2 0 0\nv 0 0.4 0\nvn 0 0 1\nf 1//1 2//1 3//1\n')
    print('wrote', os.path.join(root, 'Materials'))
    return 0


if __name__ == '__main__':
    sys.exit(main())
