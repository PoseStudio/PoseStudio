"""Makes PoseStudio's PREPACKAGED MATERIALS: each a `.psm` file with its rendered ball icon.

    python tools/materials/make_stock_materials.py <Materials folder> [--exe <PoseStudio.exe>]
                                                   [--only <name>] [--force]

A prepackaged material is DEFINED here, in the STOCK table below — a name, the category folder
it is filed under, and the values that make it what it is (everything else is the material's
default) — and MADE by the application itself, so that the file is exactly what the Materials
tab's Save writes and the icon is exactly the ball it renders:

  1. each definition is written as a small material file (only the values it names);
  2. PoseStudio is run on a scripted pass (tools/ikscripts) that puts each one on a stand-in
     object (`applymaterial`) and saves it back (`savematerial`): the full, canonical file, and
     beside it the icon — the material on a ball, path traced by the render engine under the
     icon light. The run FAILS unless the render engine made every icon: a prepackaged material
     never ships with the stand-in picture of a copy without one;
  3. the two files are copied into <Materials folder>/<category>/ under the material's name.

An existing file is left alone unless --force is given. Needs a build with the render engine
(-DPOSESTUDIO_WITH_CYCLES=ON) and nothing else: the icons are lit by the application's own icon
studio (scene/materialball.h), so they come out the same on any machine.

The values are the material file's own (scene/materialjson.cpp), in the file's units: colors
LINEAR, weights 0..1. A material's two roughness lobes follow its `roughness` unless it sets
them itself. A color given as a reference swatch ('#RRGGBB', a DISPLAY color, as color charts
and the Materials tab's swatch show one) goes through srgb(), which makes it the linear color
the file holds: the tab then shows that swatch.
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile

# What a surface that is no skin, cloth or glass has none of. (The material's default
# translucency is a soft skin-like wrap; a metal or a plastic does not scatter light inside.)
OPAQUE = {'translucency': 0.0}



def srgb(swatch):
    """A display color '#RRGGBB' as the LINEAR color a material file holds (the sRGB curve
    undone), to four places."""
    def linear(byte):
        c = byte / 255.0
        return round(c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4, 4)
    swatch = swatch.lstrip('#')
    return [linear(int(swatch[i:i + 2], 16)) for i in (0, 2, 4)]


# --- Base / Metal -------------------------------------------------------------------------
# A metal has no diffuse color: what it shows is its surroundings, tinted by its reflectance —
# which is its base color. So a base metal IS a color and a finish, and nothing else.
#
# THE FINISHES: polished is all but mirror-smooth, the finish Chrome was made with; matte is
# where a ball still reads as METAL — a soft dark band under a broad highlight — and no longer
# shows the room (0.3 and 0.4 were a satin that still did; at 0.6 the band has all but gone and the ball
# reads as paint).
POLISHED = 0.05
MATTE = 0.5

# THE COLORS are each metal's reference swatch (its reflectance seen head on, as a display color).
METALS = [
    ('Aluminum', '#F5F6F6', POLISHED),
    ('Beryllium', '#C2C1C1', POLISHED),
    ('Brass', '#F5E4AE', POLISHED),
    ('Cesium', '#DCC486', POLISHED),
    ('Chromium', '#D3D8DA', POLISHED),
    ('Chromium (Matte)', '#D3D8DA', MATTE),
    ('Cobalt', '#DADAD6', POLISHED),
    ('Copper', '#F7CFBF', POLISHED),
    ('Copper (Matte)', '#F7CFBF', MATTE),
    ('Germanium', '#BCBEB6', POLISHED),
    ('Gold', '#FFE496', POLISHED),
    ('Gold (Matte)', '#FFE496', MATTE),
    ('Iridium', '#E0DEDA', POLISHED),
    ('Iron', '#C0BEBB', POLISHED),
    ('Lead', '#CFD1D9', POLISHED),
    ('Lithium', '#F5F2E8', POLISHED),
    ('Magnesium', '#FAFAF9', POLISHED),
    ('Manganese', '#CCCAC7', POLISHED),
    ('Mercury', '#E5E5E4', POLISHED),
    ('Molybdenum', '#CACDCB', POLISHED),
    ('Nickel', '#D9D1C6', POLISHED),
    ('Nickel (Matte)', '#D9D1C6', MATTE),
    ('Palladium', '#DEDAD5', POLISHED),
    ('Platinum', '#E3DED7', POLISHED),
    ('Potassium', '#FDFAF4', POLISHED),
    ('Rubidium', '#F6EEE0', POLISHED),
    ('Silicon', '#9FA4AF', POLISHED),
    ('Silver', '#FEFDFC', POLISHED),
    ('Sodium', '#FCFBF8', POLISHED),
    ('Stainless Steel', '#D6D1CB', POLISHED),
    ('Titanium', '#B1AAA2', POLISHED),
    ('Tungsten', '#C2C1BF', POLISHED),
    ('Vanadium', '#C1C0C3', POLISHED),
    ('Zinc', '#E8EDEF', POLISHED),
]

STOCK = [
    # Polished chromium plate, the first prepackaged material: a neutral, very slightly cool
    # grey (0.549, 0.556, 0.554: chromium's measured normal-incidence reflectance), darker than
    # the Chromium swatch below.
    {
        'name': 'Chrome',
        'folder': 'Base/Metal',
        'values': {**OPAQUE, 'baseColor': [0.549, 0.556, 0.554], 'metalness': 1.0, 'roughness': POLISHED},
    },
] + [
    {
        'name': name,
        'folder': 'Base/Metal',
        'values': {**OPAQUE, 'baseColor': srgb(swatch), 'metalness': 1.0, 'roughness': finish},
    }
    for name, swatch, finish in METALS
]

FORMAT = 'posestudio.material'
VERSION = 1


def number(value):
    """A JSON number or list of numbers, with the digits the definition was written with."""
    if isinstance(value, bool):
        return 'true' if value else 'false'
    if isinstance(value, (list, tuple)):
        return '[' + ', '.join(number(v) for v in value) + ']'
    return repr(float(value))


def source_text(name, values):
    values = dict(values)
    if 'roughness' in values:
        values.setdefault('lobe1Roughness', values['roughness'])
        values.setdefault('lobe2Roughness', values['roughness'])
    body = ',\n'.join('    "%s": %s' % (key, number(values[key])) for key in sorted(values))
    return ('{\n  "format": "%s",\n  "version": %d,\n  "name": "%s",\n  "material": {\n%s\n  }\n}\n'
            % (FORMAT, VERSION, name, body))


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    parser = argparse.ArgumentParser(description="Makes PoseStudio's prepackaged materials.")
    parser.add_argument('materials', help='the Materials folder to file them under')
    parser.add_argument('--exe', default=os.path.join(here, '..', '..', 'build', 'Release', 'PoseStudio.exe'))
    parser.add_argument('--only', action='append', help='make just this material (may be repeated)')
    parser.add_argument('--force', action='store_true', help='replace a material that is already there')
    args = parser.parse_args()

    chosen = [m for m in STOCK if not args.only or m['name'] in args.only]
    if not chosen:
        print('no such material; the table has:', ', '.join(m['name'] for m in STOCK))
        return 2
    todo = []
    for material in chosen:
        folder = os.path.join(args.materials, *material['folder'].split('/'))
        target = os.path.join(folder, material['name'] + '.psm')
        if os.path.exists(target) and not args.force:
            print('kept (already there; --force replaces it):', target)
            continue
        todo.append((material, folder))
    if not todo:
        return 0
    if not os.path.exists(args.exe):
        print('PoseStudio was not found at', args.exe, '(--exe names it)')
        return 2

    work = tempfile.mkdtemp(prefix='posestudio_stock_materials_')
    try:
        os.makedirs(os.path.join(work, 'src'))
        # The stand-in object the materials are put on: one triangle, one zone. (What is rendered
        # is the icon's own ball; the object only has to have a zone.)
        host = os.path.join(work, 'host.obj')
        with open(host, 'w') as f:
            f.write('v -0.2 0 0\nv 0.2 0 0\nv 0 0.4 0\nvn 0 0 1\nf 1//1 2//1 3//1\n')
        lines = ['wait 10', 'select 0', 'expect zones == 1']
        for i, (material, _) in enumerate(todo):
            # (The script splits its lines on spaces, so the files go by number and take their
            # names on the way out.)
            with open(os.path.join(work, 'src', 'm%d.psm' % i), 'w', encoding='utf-8', newline='\n') as f:
                f.write(source_text(material['name'], material['values']))
            lines += ['applymaterial src/m%d.psm' % i, 'expect matfile.ok == 1',
                      'savematerial m%d.psm' % i, 'expect matfile.ok == 1', 'expect matfile.embedded == 0',
                      'expect maticon.ok == 1', 'expect maticon.engine == 1']
        script = os.path.join(work, 'make.txt')
        with open(script, 'w') as f:
            f.write('\n'.join(lines) + '\n')

        env = dict(os.environ)
        env.update({'POSESTUDIO_IK_SCRIPT': script, 'POSESTUDIO_IK_SCRIPT_OUT': work, 'POSESTUDIO_NO_PING': '1',
                    'POSESTUDIO_NO_UPDATE_CHECK': '1', 'QT_FORCE_STDERR_LOGGING': '1'})
        # (No lever of a developer's shell reaches a prepackaged icon.)
        for lever in ('BUILTIN', 'HDRI', 'SAMPLES', 'DENOISE', 'SUPERSAMPLE'):
            env.pop('POSESTUDIO_MATERIAL_ICON_' + lever, None)
        run = subprocess.run([args.exe, host], env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                             errors='replace')
        log = [line for line in run.stdout.splitlines() if '[ikscript]' in line]
        if run.returncode != 0 or not any('] done' in line for line in log):
            print('\n'.join(log[-40:]))
            print('PoseStudio did not make the materials (exit code %d): nothing was filed.' % run.returncode)
            return 1

        for i, (material, folder) in enumerate(todo):
            made = os.path.join(work, 'm%d.psm' % i)
            icon = os.path.join(work, 'm%d.png' % i)
            with open(made, encoding='utf-8') as f:
                text = f.read()
            placeholder = '"name": "m%d"' % i
            if text.count(placeholder) != 1 or not os.path.exists(icon):
                print('the saved file of', material['name'], 'is not as expected: nothing more was filed.')
                return 1
            text = text.replace(placeholder, '"name": "%s"' % material['name'])
            os.makedirs(folder, exist_ok=True)
            with open(os.path.join(folder, material['name'] + '.psm'), 'w', encoding='utf-8', newline='\n') as f:
                f.write(text)
            shutil.copyfile(icon, os.path.join(folder, material['name'] + '.png'))
            took = next((line.split(' in ')[1].split(' s')[0] for line in log if 'm%d.png' % i in line and ' in ' in line), '?')
            print('made', os.path.join(folder, material['name'] + '.psm'), '(icon rendered in %s s)' % took)
        return 0
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == '__main__':
    sys.exit(main())
