#!/usr/bin/env python3
"""Adds files to an APK made by aapt2 (which only holds the manifest, resources and assets).
usage: apk_add.py <in.apk> <out.apk> <path in apk>=<file> ...
Code (classes.dex) and native libraries go in compressed; the app sets extractNativeLibs."""
import shutil, sys, zipfile
src, dst = sys.argv[1], sys.argv[2]
shutil.copyfile(src, dst)
with zipfile.ZipFile(dst, "a", zipfile.ZIP_DEFLATED) as z:
    for spec in sys.argv[3:]:
        name, path = spec.split("=", 1)
        z.write(path, name)
        print("added", name)
