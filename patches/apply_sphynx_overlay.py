#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
apply_sphynx_overlay.py — SPHYNX overlay for PPSSPP v1.18.1
===========================================================

Layered, idempotent rebranding/multi-ABI patch.  Run it against a *clean*
checkout of tag `v1.18.1` of https://github.com/hrydgard/ppsspp :

    python3 apply_sphynx_overlay.py /path/to/ppsspp-checkout

What it touches (and ONLY this, in this stage):
  1. android/res/values/strings.xml   -> visible app name ("Sphynx")
  2. android/build.gradle             -> per-ABI builds, ARM-only, distinct
                                         applicationId so Sphynx can be
                                         installed next to the real PPSSPP.

What it deliberately does NOT touch:
  * Core/, GPU/, HLE/, UI/  (the PSP emulator itself) — hard rule of the project.
  * assets/, native code, or any PPSSPP behaviour.

Everything is marked with "SPHYNX:" comments in the patched files so a future
maintainer can find every deviation from upstream with:

    grep -rn "SPHYNX:" .
"""

import re
import shutil
import sys
from pathlib import Path

APP_LABEL = "Sphynx"
APP_ID = "org.sphynx.emulator"

# --- helpers ---------------------------------------------------------------


def patch_file(path: Path, old: str, new: str, label: str, count: int = 1) -> bool:
    """Replace the first `count` occurrences of `old` in `path`, CRLF-safe."""
    if not path.exists():
        print(f"  !! missing: {path}")
        return False
    data = path.read_bytes().decode("utf-8")
    if new in data:
        print(f"  == already patched: {label}")
        return False
    if old not in data:
        print(f"  !! anchor not found: {label}  (file {path.name} differs from v1.18.1?)")
        return False
    data = data.replace(old, new, count)
    path.write_bytes(data.encode("utf-8"))
    print(f"  ok patched: {label}")
    return True


def main(root: str) -> int:
    repo = Path(root).resolve()
    if not (repo / "android" / "build.gradle").exists():
        print(f"error: {repo} does not look like a PPSSPP checkout")
        return 2

    print(f"[1/3] rebranding strings in {repo}")
    patch_file(
        repo / "android" / "res" / "values" / "strings.xml",
        '<string name="app_name">PPSSPP</string>',
        f'<string name="app_name">{APP_LABEL}</string>',
        "app_name -> " + APP_LABEL,
    )
    patch_file(
        repo / "android" / "res" / "values" / "strings.xml",
        '<string name="shortcut_name">PPSSPP game</string>',
        f'<string name="shortcut_name">{APP_LABEL} game</string>',
        "shortcut_name",
    )

    print("[2/3] android/build.gradle: ARM-only, per-ABI, own applicationId")
    gradle = repo / "android" / "build.gradle"
    data = gradle.read_bytes().decode("utf-8")

    # Only touch the `normal` flavour (the one we ship), anchored after "productFlavors {"
    idx = data.find("productFlavors {")
    if idx < 0:
        print("  !! productFlavors block not found")
        return 3
    head, tail = data[:idx], data[idx:]

    if "SPHYNX: per-ABI build" not in tail:
        anchor = "abiFilters 'armeabi-v7a', 'arm64-v8a', 'x86_64'"
        if anchor not in tail:
            print("  !! normal-flavour abiFilters anchor not found")
            return 3
        replacement = """// SPHYNX: per-ABI build.  We ship one APK per ARM ABI instead of a fat
				// APK (this is the largest single size win: ~45% off the download).
				// x86_64 is dropped on purpose: Sphynx targets ARM devices only.
				// Pass -PsphynxAbi=arm64-v8a (or armeabi-v7a) to build one ABI.
				def sphynxAbi = project.findProperty('sphynxAbi')
				if (sphynxAbi != null && !sphynxAbi.isEmpty()) {
					abiFilters(*sphynxAbi.split(','))
				} else {
					abiFilters 'armeabi-v7a', 'arm64-v8a'
				}"""
        tail = tail.replace(anchor, replacement, 1)
        print("  ok patched: abiFilters -> property driven, ARM only")

    if APP_ID not in tail:
        anchor = "applicationId 'org.ppsspp.ppsspp'"
        if anchor in tail:
            tail = tail.replace(
                anchor,
                "// SPHYNX: own package id so Sphynx installs alongside PPSSPP\n"
                f"\t\t\tapplicationId '{APP_ID}'",
                1,
            )
            print(f"  ok patched: normal-flavour applicationId -> {APP_ID}")
        else:
            print("  !! normal-flavour applicationId anchor not found")
    else:
        print("  == already patched: applicationId")

    gradle.write_bytes((head + tail).encode("utf-8"))

    print("[3/3] installing CI workflow + docs")
    here = Path(__file__).resolve().parent
    src_wf = here.parent / "overlay" / ".github" / "workflows" / "sphynx-apk.yml"
    dst_wf = repo / ".github" / "workflows" / "sphynx-apk.yml"
    if src_wf.exists():
        dst_wf.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src_wf, dst_wf)
        print(f"  ok copied {dst_wf.relative_to(repo)}")
    else:
        print(f"  !! workflow source not found: {src_wf}")

    print("\ndone.  Nothing under Core/, GPU/, HLE/ or UI/ was modified.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else "."))
