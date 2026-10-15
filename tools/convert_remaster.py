#!/usr/bin/env python3
"""convert_remaster.py "<System Shock 2 Remastered folder>" "<System Shock 2 folder>" [--name NAME] [--only-weapons]

Turns the 25th Anniversary Remaster's content mod (mods/sshock2ee.kpf, from your own copy of the Remaster) into a
mod folder the NewDark engine can load: <System Shock 2>/DMM/<NAME> (default ss2ee_newdark). Nothing from the
Remaster goes into this repository.

Why: the archive is laid out as a NewDark mod already (LGMD models, NewDark .mtl materials, motions, scripts), but
824 of its textures are DDS files in BC7 compression (a DX10 header), which NewDark's Direct3D 9 renderer cannot
read; every model using one draws white. BC7 textures are decoded and written as PNG with the same name, which
NewDark finds in their place. Everything else is copied as it is.

Weapon and arm materials (materials/ND-viewmodel.inc) get a gentler rim light: Nightdive's NewDark version adds the
full texture again, lit, at grazing angles, which blows skin out to white under bright lights (stock NewDark draws it
the same way). --rim X sets its strength (default 0.35; Nightdive's is 1.0); --keep-materials leaves it alone.

--only-weapons limits it to the first-person and world weapon models, the meshes they need and their textures and
materials. Needs Pillow (pip install pillow)."""
import os, sys, struct, zipfile, io
from PIL import Image

WEAPON = ('obj/', 'mesh/', 'materials/')
def wanted_weapon(name):
    l = name.lower()
    if l.startswith('materials/'): return True
    if l.startswith(('obj/txt16/', 'mesh/txt16/')): return os.path.basename(l).startswith('nd-')
    if l.startswith(('obj/', 'mesh/')): return l.endswith(('_h.bin', '_w.bin', 'lasehand.bin', 'empgun.bin', '.cal'))
    return False

def main():
    argv = sys.argv[1:]; opt = {}
    for k in ('--name', '--rim'):
        if k in argv: i = argv.index(k); opt[k] = argv[i + 1] if i + 1 < len(argv) else ''; del argv[i:i + 2]
    only = '--only-weapons' in argv; keep = '--keep-materials' in argv
    args = [a for a in argv if not a.startswith('--')]
    if len(args) != 2: sys.exit(__doc__)
    name = opt.get('--name') or 'ss2ee_newdark'; rim = float(opt.get('--rim') or 0.35)
    src = os.path.join(args[0], 'mods', 'sshock2ee.kpf')
    if not os.path.isfile(src): sys.exit('no mods/sshock2ee.kpf under ' + args[0])
    dst = os.path.join(args[1], 'DMM', name)
    z = zipfile.ZipFile(src); n_conv = n_copy = 0
    for info in z.infolist():
        if info.is_dir(): continue
        nm = info.filename.replace('\\', '/')
        if only and not wanted_weapon(nm): continue
        data = z.read(info)
        out = os.path.join(dst, *nm.split('/'))
        os.makedirs(os.path.dirname(out), exist_ok=True)
        if nm.lower().endswith('.dds') and data[84:88] == b'DX10':
            img = Image.open(io.BytesIO(data)).convert('RGBA')
            img.save(os.path.splitext(out)[0] + '.png', optimize=False)
            n_conv += 1
            if n_conv % 50 == 0: print(f'  {n_conv} textures converted...', flush=True)
            continue
        if not keep and nm.lower() == 'materials/nd-viewmodel.inc':
            data = data.replace(b'include ../../materials/pass/ND-pass_rim.inc', (
                b'render_pass\r\n{\r\n\ttexture $TEXTURE\r\n\tblend SRC_ALPHA ONE\r\n\tRGB %.2f %.2f %.2f\r\n'
                b'\talpha func INCIDENCE 1.0 1.0 MATERIALS/ND-IR_RIM\r\n\tshaded 1\r\n}' % (rim, rim, rim)))
        with open(out, 'wb') as f: f.write(data)
        n_copy += 1
    with open(os.path.join(dst, 'mod.ini'), 'w') as f:
        f.write('[Config]\nname=SS2 25th Anniversary content for NewDark\ndescription=Converted from your own copy of the Remaster by NewDarkVR tools/convert_remaster.py\n')
    print(f'wrote {dst}: {n_conv} textures converted from BC7 to PNG, {n_copy} files copied')

if __name__ == '__main__': main()
