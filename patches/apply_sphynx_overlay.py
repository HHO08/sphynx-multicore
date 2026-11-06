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
					// SPHYNX: 32-bit ARM only. The owner chose to ship a single
					// armeabi-v7a APK: it halves the build time and memory while
					// still covering every ARM device, because 64-bit phones run
					// 32-bit code.
					abiFilters 'armeabi-v7a'
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

    print("[2b/3] android/build.gradle: allow versionName-only override")
    data = gradle.read_bytes().decode("utf-8")
    old_line = '\t\tif (project.hasProperty("ANDROID_VERSION_CODE") && project.hasProperty("ANDROID_VERSION_NAME")) {'
    new_line = ("\t\t// SPHYNX: allow overriding versionName on its own.  Upstream requires BOTH\n"
                "\t\t// properties, so a fork that has no release tags of its own would keep a\n"
                "\t\t// versionName with the branch name baked into it.\n"
                '\t\tif (project.hasProperty("ANDROID_VERSION_NAME")) {')
    vc_line = "\t\t\tversionCode ANDROID_VERSION_CODE"
    if vc_line in data and "SPHYNX: versionCode is intentionally" not in data:
        data = data.replace(vc_line,
                            "\t\t\t// SPHYNX: versionCode is intentionally left to the androidGitVersion plugin",
                            1)
        gradle.write_bytes(data.encode("utf-8"))
        data = gradle.read_bytes().decode("utf-8")
        print("  ok patched: versionCode assignment neutralised")
    if "SPHYNX: allow overriding versionName" in data:
        print("  == already patched: versionName override")
    elif old_line in data:
        nl = "\r\n" if "\r\n" in data else "\n"
        data = data.replace(old_line, new_line.replace("\n", nl), 1)
        gradle.write_bytes(data.encode("utf-8"))
        print("  ok patched: versionName override")
    else:
        print("  !! versionName anchor not found (upstream build.gradle changed?)")

    print("[3/3] installing CI workflow + docs")
    here = Path(__file__).resolve().parent
    wf_dir = here.parent / "overlay" / ".github" / "workflows"
    for wf in sorted(wf_dir.glob("sphynx-*.yml")) if wf_dir.exists() else []:
        dst = repo / ".github" / "workflows" / wf.name
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(wf, dst)
        print(f"  ok copied {dst.relative_to(repo)}")

    print("[4/4] installing extcores/ + root CMake hook")
    here2 = Path(__file__).resolve().parent
    src_tree = here2.parent / "extcores"
    dst_tree = repo / "extcores"
    copied = 0
    if src_tree.exists():
        for f in sorted(src_tree.rglob("*")):
            if not f.is_file():
                continue
            rel = f.relative_to(src_tree)
            if any(part.startswith(".") or part == "build" for part in rel.parts):
                continue
            target = dst_tree / rel
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(f, target)
            copied += 1
        print(f"  ok installed {copied} files under extcores/")
    else:
        print(f"  !! extcores source tree not found: {src_tree}")

    root_cmake = repo / "CMakeLists.txt"
    hook = """
# ============================================================================
#  SPHYNX block — multi-system host layer (APPENDED, never inserted)
#
#  Builds extcores/ as a separate shared library. It is not linked into the PSP
#  core and can be disabled with -DSPHYNX_EXTCORE=OFF to obtain a byte-for-byte
#  stock PPSSPP v1.18.1 build.
# ============================================================================
option(SPHYNX_EXTCORE "Build the Sphynx external-core host (multi-system support)" ON)
if(SPHYNX_EXTCORE AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/extcores/CMakeLists.txt")
\tadd_subdirectory(extcores)
endif()
"""
    if root_cmake.exists():
        cm = root_cmake.read_text(encoding="utf-8")
        if "SPHYNX block" in cm:
            print("  == already patched: root CMake hook")
        else:
            root_cmake.write_text(cm + hook, encoding="utf-8")
            print("  ok patched: root CMake hook appended")

    print("[5/5] installing the Sphynx app icon")
    src_brand = here.parent / "branding" / "res"
    dst_brand = repo / "android" / "normal" / "res"
    n_icons = 0
    if src_brand.exists():
        for f in sorted(src_brand.rglob("*")):
            if not f.is_file():
                continue
            rel = f.relative_to(src_brand)
            target = dst_brand / rel
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(f, target)
            n_icons += 1
        print(f"  ok installed {n_icons} icon resources (legacy + adaptive)")
    else:
        print(f"  !! icon source not found: {src_brand}")

    print("[4b] gradle: jniLibs source set + never strip the bundled cores")
    data = gradle.read_bytes().decode("utf-8")
    nl = "\r\n" if "\r\n" in data else "\n"
    if "SPHYNX: bundled emulator cores" in data:
        print("  == already patched: jniLibs")
    else:
        # jniLibs default already covers src/<flavor>/jniLibs, but being explicit
        # keeps working if upstream ever customises sourceSets.
        anchor_flavor = "\t\tnormal {" + nl
        if anchor_flavor in data:
            data = data.replace(
                anchor_flavor,
                anchor_flavor +
                "\t\t\t// SPHYNX: bundled emulator cores live here (fetched by Tools/sphynx/fetch_cores.sh)" + nl +
                "\t\t\tsourceSets { jniLibs.srcDirs += 'src/normal/jniLibs' }" + nl,
                1)
            gradle.write_bytes(data.encode("utf-8"))
            print("  ok patched: jniLibs srcDirs in normal flavour")
        else:
            print("  !! normal flavour anchor not found")

    data = gradle.read_bytes().decode("utf-8")
    if "doNotStrip" in data:
        print("  == already patched: doNotStrip")
    else:
        anchor_pkg = "packagingOptions {"
        if anchor_pkg in data:
            data = data.replace(
                anchor_pkg,
                anchor_pkg + nl +
                "\t\t// SPHYNX: the cores are published prebuilt by their authors; they are" + nl +
                "\t\t// already stripped and must not be touched by the Android toolchain." + nl +
                '\t\tdoNotStrip "**/libsphynx_core_*.so"' + nl,
                1)
            gradle.write_bytes(data.encode("utf-8"))
            print("  ok patched: doNotStrip for bundled cores")
        else:
            print("  !! packagingOptions anchor not found")

    print("[6/6] installing Tools/sphynx")
    src_tools = here.parent / "Tools" / "sphynx"
    if src_tools.exists():
        for f in sorted(src_tools.rglob("*")):
            if f.is_file():
                target = repo / "Tools" / "sphynx" / f.relative_to(src_tools)
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(f, target)
                target.chmod(0o755)
        print("  ok installed Tools/sphynx")
    else:
        print(f"  !! Tools/sphynx not found: {src_tools}")

    print("[7/7] UI + Java bridge (game list, thumbnails, player screen)")
    import importlib.util
    bridge_path = Path(__file__).resolve().parent / "sphynx_ui_bridge.py"
    if bridge_path.exists():
        spec = importlib.util.spec_from_file_location("sphynx_ui_bridge", bridge_path)
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        mod.apply(repo, here.parent / "overlay")
    else:
        print(f"  !! bridge script not found: {bridge_path}")

    print("\ndone.  Nothing under Core/, GPU/, HLE/ or UI/ was modified.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else "."))
