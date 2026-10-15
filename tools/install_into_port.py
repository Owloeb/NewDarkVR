#!/usr/bin/env python3
"""install_into_port.py PORT_CHECKOUT [--probe] [--uninstall]

Puts the NewDarkVR code into a NewDarkRecompiled checkout (the vanilla repo is never touched from here; this changes
your local copy only):

  - copies port-glue/vr_stereo.c, src/ndvr_xr.c (guarded for Windows), include/ndvr.h and the OpenXR headers into port/core/
  - applies patches/vr-port-hooks.patch: a forwarder in port/core/fastpaths.c, two small functions in port/dx/d3d9.c,
    one in port/backends/sdl2/gl_render.c and the Windows link libraries in port/CMakeLists.txt

Then build the port as usual, with --fastpaths:   python3 port/build.py <SS2.exe> --target windows --install --fastpaths
and put openxr_loader.dll (from the NewDarkVR release zip) next to ss2port.exe.

--probe also adds the HUD / weapon / fire-direction probe (port-glue/vr_probe.c, patches/m5-hud-weapon-probe.patch): it logs
how the engine draws the HUD and the held weapon and where a shot's direction is read, and changes nothing in the game
(run it with NDVR_PROBE=1, see the header of port-glue/vr_probe.c). Install it on a clean checkout; it is on top of the VR patch.

--uninstall restores the four patched files with git and removes the copied ones (probe included)."""
import os, shutil, subprocess, sys

here = os.path.dirname(os.path.abspath(__file__)); repo = os.path.dirname(here)
args = [a for a in sys.argv[1:] if not a.startswith('--')]
undo = '--uninstall' in sys.argv; probe = '--probe' in sys.argv
if len(args) != 1: sys.exit(__doc__)
port = os.path.abspath(args[0])
core = os.path.join(port, 'port', 'core')
if not os.path.isfile(os.path.join(core, 'fastpaths.c')): sys.exit(f'{port} does not look like a NewDarkRecompiled checkout (no port/core/fastpaths.c)')

tracked = ['port/core/fastpaths.c', 'port/dx/d3d9.c', 'port/backends/sdl2/gl_render.c', 'port/CMakeLists.txt', 'port/core/heap.c', 'port/win32/user32.c']
copied = ['port/core/vr_stereo.c', 'port/core/ndvr_xr.c', 'port/core/ndvr.h', 'port/core/openxr', 'port/core/vr_probe.c', 'port/core/vr_probe.h']

def git(*a, check=True): return subprocess.run(['git', '-C', port, *a], check=check, capture_output=True, text=True)

def remove_copied():
    for c in copied:
        p = os.path.join(port, c)
        if os.path.isdir(p): shutil.rmtree(p)
        elif os.path.exists(p): os.remove(p)

if undo:
    git('checkout', '--', *tracked); remove_copied(); print('removed the NewDarkVR changes'); sys.exit(0)

patch = os.path.join(repo, 'patches', 'vr-port-hooks.patch')
chk = git('apply', '--check', patch, check=False)
if chk.returncode and os.path.exists(os.path.join(core, 'vr_stereo.c')):   # an earlier NewDarkVR install: replace it
    print('replacing the earlier NewDarkVR install')
    git('checkout', '--', *tracked); remove_copied()
    chk = git('apply', '--check', patch, check=False)
if chk.returncode:
    sys.exit('The patch does not apply to this checkout:\n' + chk.stderr +
             '\nIf an earlier NewDarkVR patch is applied, undo it first:  python3 tools/install_into_port.py ' + port + ' --uninstall\n'
             '(or: git checkout port/core/fastpaths.c port/dx/d3d9.c port/backends/sdl2/gl_render.c port/CMakeLists.txt, and delete port/core/vr_stereo.c)')
def build_id():                       # the NewDarkVR commit, printed at start-up so a log says which build ran
    r = subprocess.run(['git', '-C', repo, 'describe', '--always', '--dirty'], capture_output=True, text=True)
    return r.stdout.strip() if r.returncode == 0 and r.stdout.strip() else 'unknown'
with open(os.path.join(repo, 'port-glue', 'vr_stereo.c'), encoding='utf-8') as f: vs = f.read()
with open(os.path.join(core, 'vr_stereo.c'), 'w', encoding='utf-8', newline='\n') as f: f.write('#define NDVR_BUILD "%s"\n' % build_id() + vs)
shutil.copy(os.path.join(repo, 'include', 'ndvr.h'), os.path.join(core, 'ndvr.h'))
with open(os.path.join(repo, 'src', 'ndvr_xr.c'), encoding='utf-8') as f: src = f.read()
with open(os.path.join(core, 'ndvr_xr.c'), 'w', encoding='utf-8', newline='\n') as f: f.write('#ifdef _WIN32\n' + src + '\n#endif /* _WIN32 */\n')
dst = os.path.join(core, 'openxr')
if os.path.isdir(dst): shutil.rmtree(dst)
shutil.copytree(os.path.join(repo, 'third_party', 'openxr', 'include', 'openxr'), dst)
git('apply', patch)
if probe:
    ppatch = os.path.join(repo, 'patches', 'm5-hud-weapon-probe.patch')
    r = git('apply', ppatch, check=False)
    if r.returncode: sys.exit('The probe patch does not apply on top of the VR patch:\n' + r.stderr)
    for f in ('vr_probe.c', 'vr_probe.h'): shutil.copy(os.path.join(repo, 'port-glue', f), os.path.join(core, f))
print('NewDarkVR', build_id(), 'installed into', port, '(with the probe)' if probe else '')
print('Next: python3 port/build.py "<path to SS2.exe>" --target windows --install --fastpaths')
if probe: print('Probe run (not with NDVR_VR):  $env:NDVR_PROBE=1; $env:SS2PORT_NOVSYNC=1; .\\ss2port.exe --windowed SS2.exe    then walk around, fire a weapon a few times, quit, and send the log')
print('Then copy openxr_loader.dll next to ss2port.exe and run with:  $env:NDVR_VR=1; $env:SS2PORT_NOVSYNC=1; .\\ss2port.exe --windowed --verbose SS2.exe')
