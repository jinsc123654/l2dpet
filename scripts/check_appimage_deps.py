#!/usr/bin/env python3
"""Reject incomplete AppImage ELF dependency closure before producing a release."""
import argparse
from fnmatch import fnmatchcase
from pathlib import Path
import re
import subprocess
import sys

# These ABI/platform libraries deliberately come from the host. libcrypto is not libcrypt.
HOST_LIBRARIES = (
    "ld-linux*", "libc.so*", "libm.so*", "libpthread.so*", "libdl.so*", "librt.so*",
    "libresolv.so*", "libnss_*", "libutil.so*", "libanl.so*", "libcrypt.so*",
    "libgcc_s.so*", "libstdc++.so*", "libGL.so*", "libEGL*", "libGLX*",
    "libGLdispatch*", "libOpenGL*", "libGLES*", "libvulkan*", "libnvidia*",
)

def check(appdir):
    library_dir = appdir / "usr/lib"
    files = [appdir / "usr/bin/l2dtest", appdir / "usr/bin/l2dpet-ui"]
    files += sorted(p for p in library_dir.iterdir() if p.is_file())
    errors = []
    for elf in files:
        if not elf.is_file():
            errors.append(f"missing executable: {elf.name}")
            continue
        with elf.open("rb") as f:
            if f.read(4) != b"\x7fELF":
                errors.append(f"not an ELF file: {elf.name}")
                continue
        dynamic = subprocess.check_output(["readelf", "-d", str(elf)], text=True)
        for needed in re.findall(r"\(NEEDED\).*?\[([^\]]+)\]", dynamic):
            if any(fnmatchcase(needed, pattern) for pattern in HOST_LIBRARIES):
                continue
            if not (library_dir / needed).is_file():
                errors.append(f"{elf.name} requires bundled {needed}, but it is missing")
    return errors

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("appdir", type=Path)
    args = parser.parse_args()
    failures = check(args.appdir)
    if failures:
        print("AppImage dependency check failed:\n" + "\n".join(failures), file=sys.stderr)
        sys.exit(1)
    print("AppImage ELF dependency closure complete (including OpenSSL's paired libraries)")
