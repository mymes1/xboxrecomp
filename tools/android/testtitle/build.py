"""Build the self-made test title and its disc image.

    python3 -m tools.android.testtitle.build [--out build/testtitle]

Steps (all reproducible: pinned zig from requirements-dev.txt, /Brepro, fixed
timestamps):

  1. import library for xboxkrnl.exe from xboxkrnl.def    (zig dlltool)
  2. compile xbrtest.c for a Pentium III, x87 only        (zig cc, i386-windows-msvc)
  3. link a fixed-base PE at 0x00010000                    (zig lld-link)
  4. convert the PE to default.xbe                         (pe2xbe.py)
  5. lay out the disc: default.xbe, data/xbrtest.bin, media/sub/hello.txt
  6. pack it as an XDVDFS image                            (tools/android/xiso_build.py)

Prints the image's SHA-256, which is what the pipeline verifies it against.
"""

import argparse
import hashlib
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
sys.path.insert(0, ROOT)

from tools.android import xiso_build            # noqa: E402
from tools.android.testtitle import pe2xbe      # noqa: E402

TITLE_NAME = "xboxrecomp test title"
TITLE_ID = 0x58425254          # "XBRT"


def zig(*args, cwd=None):
    cmd = [sys.executable, "-m", "ziglang"] + list(args)
    r = subprocess.run(cmd, cwd=cwd, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if r.returncode != 0:
        raise SystemExit("zig %s failed:\n%s" % (args[0], r.stdout))
    return r.stdout


def file_pattern(n):
    return bytes(((i * 7 + 3) & 0xFF) for i in range(n))


def build(out_dir):
    out_dir = os.path.abspath(out_dir)
    obj_dir = os.path.join(out_dir, "obj")
    disc = os.path.join(out_dir, "disc")
    for d in (obj_dir,):
        os.makedirs(d, exist_ok=True)
    if os.path.exists(disc):
        shutil.rmtree(disc)
    os.makedirs(os.path.join(disc, "data"))
    os.makedirs(os.path.join(disc, "media", "sub"))

    lib = os.path.join(obj_dir, "xboxkrnl.lib")
    zig("dlltool", "-d", os.path.join(HERE, "xboxkrnl.def"), "-l", lib, "-m", "i386")
    obj = os.path.join(obj_dir, "xbrtest.obj")
    zig("cc", "-target", "x86-windows-msvc", "-march=pentium3", "-mno-sse", "-O1",
        "-fno-omit-frame-pointer", "-fno-stack-protector", "-ffreestanding", "-fno-builtin",
        "-mno-stack-arg-probe", "-Wall", "-Werror", "-c", os.path.join(HERE, "xbrtest.c"),
        "-o", obj)
    exe = os.path.join(obj_dir, "xbrtest.exe")
    zig("lld-link", "-nodefaultlib", "-entry:XboxEntry", "-subsystem:windows",
        "-base:0x10000", "-fixed", "-brepro", "-safeseh:no", "-out:" + exe, obj, lib)

    with open(exe, "rb") as f:
        xbe, info = pe2xbe.build_xbe(f.read(), TITLE_NAME, TITLE_ID)
    xbe_path = os.path.join(disc, "default.xbe")
    with open(xbe_path, "wb") as f:
        f.write(xbe)
    with open(os.path.join(disc, "data", "xbrtest.bin"), "wb") as f:
        f.write(file_pattern(4096))
    with open(os.path.join(disc, "media", "sub", "hello.txt"), "wb") as f:
        f.write(b"hello from the test disc\n")

    iso = os.path.join(out_dir, "xbrtest.iso")
    xiso_build.build(disc, iso)
    h = hashlib.sha256()
    with open(iso, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return {"iso": iso, "sha256": h.hexdigest(), "xbe": xbe_path, **info}


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", default=os.path.join(ROOT, "build", "testtitle"))
    args = ap.parse_args(argv)
    info = build(args.out)
    print("test title: %s" % info["xbe"])
    print("  entry 0x%08X, kernel thunk 0x%08X, ordinals %s"
          % (info["entry"], info["thunk"], info["kernel_ordinals"]))
    print("disc image: %s" % info["iso"])
    print("sha256: %s" % info["sha256"])
    return 0


if __name__ == "__main__":
    sys.exit(main())
