#!/usr/bin/env python3
"""
One-step build for NewDarkVR. Needs only Python 3.8+ and `pip install ziglang`.

    python build.py              build everything into build/ and make the release zip
    python build.py --clean      delete build/
    python build.py --rebuild-loader

What it does
  1. Fetches the OpenXR SDK source at a pinned release tag from Khronos (no binary downloads) and
     builds the 64-bit openxr_loader.dll from it with Zig.
  2. Builds build/ndvr_gltest.exe, the standalone stereo test.
  3. Packs both, plus licenses and a short readme, into build/NewDarkVR-test-win64.zip.

Environment
  NDVR_OPENXR_SRC   path to an existing OpenXR-SDK checkout of OPENXR_TAG (skips the download)
"""
import glob
import os
import shutil
import subprocess
import sys
import urllib.request
import zipfile

ROOT = os.path.dirname(os.path.abspath(__file__))
BUILD = os.path.join(ROOT, "build")
TARGET = "x86_64-windows-gnu"

OPENXR_TAG = "release-1.1.63"
OPENXR_ZIP = "https://github.com/KhronosGroup/OpenXR-SDK/archive/refs/tags/%s.zip" % OPENXR_TAG
OPENXR_GIT = "https://github.com/KhronosGroup/OpenXR-SDK.git"
OPENXR_VER = OPENXR_TAG.replace("release-", "").split(".")  # ['1', '1', '63']

OUT_LOADER = os.path.join(BUILD, "openxr_loader.dll")
OUT_TEST = os.path.join(BUILD, "ndvr_gltest.exe")
OUT_ZIP = os.path.join(BUILD, "NewDarkVR-test-win64.zip")


def rel(p):
    return os.path.relpath(p, ROOT) if os.path.isabs(p) else p


def zig(*args):
    print("  zig " + " ".join(rel(a) for a in args[:4]) + " ...")
    try:
        subprocess.run([sys.executable, "-m", "ziglang", *args], check=True, cwd=ROOT)
    except subprocess.CalledProcessError:
        sys.exit("\nBuild failed (see the compiler output above).")
    except FileNotFoundError:
        sys.exit("Python not found.")


def check_zig():
    r = subprocess.run([sys.executable, "-m", "ziglang", "version"], capture_output=True, text=True, cwd=ROOT)
    if r.returncode != 0:
        sys.exit("Zig is not installed. Run:  python -m pip install ziglang")
    print("zig " + r.stdout.strip())


# --------------------------------------------------------------------------- OpenXR loader

def fetch_openxr_source():
    override = os.environ.get("NDVR_OPENXR_SRC")
    if override:
        return override
    base = os.path.join(BUILD, "openxr-src")
    src = os.path.join(base, "OpenXR-SDK-" + OPENXR_TAG)
    if os.path.isdir(os.path.join(src, "src", "loader")):
        return src
    os.makedirs(base, exist_ok=True)
    zpath = os.path.join(base, OPENXR_TAG + ".zip")
    try:
        print("downloading OpenXR SDK %s source ..." % OPENXR_TAG)
        urllib.request.urlretrieve(OPENXR_ZIP, zpath)
        with zipfile.ZipFile(zpath) as z:
            z.extractall(base)
        os.remove(zpath)
    except Exception as e:  # network problem, proxy, ...: fall back to git
        print("  zip download failed (%s); trying git" % e)
        shutil.rmtree(src, ignore_errors=True)
        r = subprocess.run(["git", "clone", "--depth", "1", "--branch", OPENXR_TAG, OPENXR_GIT, src])
        if r.returncode != 0:
            sys.exit("Could not fetch the OpenXR SDK source. Download %s manually, unzip it, and set "
                     "NDVR_OPENXR_SRC to the folder." % OPENXR_ZIP)
    if not os.path.isdir(os.path.join(src, "src", "loader")):
        sys.exit("OpenXR source layout not as expected under " + src)
    return src


def build_loader(force):
    if os.path.exists(OUT_LOADER) and not force:
        print("openxr_loader.dll (cached)")
        return
    src = fetch_openxr_source()
    print("openxr_loader.dll (x86_64, from OpenXR SDK %s)" % OPENXR_TAG)
    obj_dir = os.path.join(BUILD, "obj")
    os.makedirs(obj_dir, exist_ok=True)

    defs = [
        "-DXR_OS_WINDOWS", "-DXR_USE_PLATFORM_WIN32",
        "-DXR_USE_GRAPHICS_API_OPENGL", "-DXR_USE_GRAPHICS_API_D3D11", "-DXR_USE_GRAPHICS_API_D3D12",
        "-DJSON_USE_EXCEPTION=0", "-DNOMINMAX", "-DWIN32_LEAN_AND_MEAN",
        "-DXR_CURRENT_API_MAJOR_VERSION=" + OPENXR_VER[0],
        "-DXR_CURRENT_API_MINOR_VERSION=" + OPENXR_VER[1],
        "-DXR_CURRENT_API_PATCH_VERSION=" + OPENXR_VER[2],
    ]
    incs = ["-I" + os.path.join(src, p) for p in
            ("include", "src", os.path.join("src", "common"), os.path.join("src", "loader"),
             os.path.join("src", "external", "jsoncpp", "include"))]

    # the generated dispatch table is plain C, so it is compiled on its own
    c_obj = os.path.join(obj_dir, "xr_generated_dispatch_table_core.o")
    zig("cc", "-target", TARGET, "-c", "-O2", *defs, *incs,
        os.path.join(src, "src", "xr_generated_dispatch_table_core.c"), "-o", c_obj)

    cpp = [f for f in sorted(glob.glob(os.path.join(src, "src", "loader", "*.cpp")))
           if "android" not in os.path.basename(f)]
    cpp += [os.path.join(src, "src", "common", n) for n in ("filesystem_utils.cpp", "object_info.cpp")]
    cpp += [os.path.join(src, "src", "external", "jsoncpp", "src", "lib_json", n)
            for n in ("json_reader.cpp", "json_value.cpp", "json_writer.cpp")]
    zig("c++", "-target", TARGET, "-std=c++17", "-O2", "-shared", *defs, *incs, *cpp, c_obj,
        os.path.join(src, "src", "loader", "openxr-loader.def"),
        "-o", OUT_LOADER, "-ladvapi32", "-lshell32", "-lole32")

    # keep the Apache license text that has to ship with the DLL
    lic = os.path.join(src, "LICENSE")
    if os.path.exists(lic):
        shutil.copy(lic, os.path.join(BUILD, "OPENXR_LICENSE.txt"))


# --------------------------------------------------------------------------- test app

def build_test():
    print("ndvr_gltest.exe (x86_64)")
    zig("cc", "-target", TARGET, "-std=c99", "-O2", "-Wall", "-Wno-unused-function",
        "-I" + os.path.join(ROOT, "include"),
        "-I" + os.path.join(ROOT, "third_party", "openxr", "include"),
        "tools/ndvr_gltest.c", "src/ndvr_xr.c",
        "-o", OUT_TEST, "-lopengl32", "-lgdi32", "-luser32")


# --------------------------------------------------------------------------- release zip

README_FIRST = """NewDarkVR stereo test (Windows 64-bit)

1. Connect your Quest with Quest Link or Air Link, and make sure Quest Link is the active
   OpenXR runtime (Meta Quest Link app > Settings > General).
2. Double-click ndvr_gltest.exe (keep openxr_loader.dll next to it) and put the headset on.
3. You should see a spinning cube 1.5 m ahead with real depth, a floor grid, and a cube on each
   controller (blue left, orange right). The world must stay put when you turn your head.
   Esc or closing the window quits.

If it fails, the console window says why. Project: https://github.com/Owloeb/NewDarkVR
"""


def make_zip():
    print("release zip")
    with zipfile.ZipFile(OUT_ZIP, "w", zipfile.ZIP_DEFLATED) as z:
        z.write(OUT_TEST, "ndvr_gltest.exe")
        z.write(OUT_LOADER, "openxr_loader.dll")
        z.write(os.path.join(ROOT, "LICENSE"), "LICENSE.txt")
        lic = os.path.join(BUILD, "OPENXR_LICENSE.txt")
        if os.path.exists(lic):
            z.write(lic, "OPENXR_LICENSE.txt")
        z.writestr("README-FIRST.txt", README_FIRST)
    print("  -> " + rel(OUT_ZIP))


def main():
    if "--clean" in sys.argv:
        shutil.rmtree(BUILD, ignore_errors=True)
        print("cleaned")
        return
    os.makedirs(BUILD, exist_ok=True)
    check_zig()
    build_loader("--rebuild-loader" in sys.argv)
    build_test()
    make_zip()
    print("\ndone. Run build\\ndvr_gltest.exe (or unzip the release zip anywhere).")


if __name__ == "__main__":
    main()
