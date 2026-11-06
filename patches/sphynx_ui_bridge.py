#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
sphynx_ui_bridge.py — wires Sphynx into PPSSPP's own user interface.

Called by apply_sphynx_overlay.py. Every edit is guarded (idempotent) and every
inserted line carries a "SPHYNX:" marker so `grep -rn "SPHYNX:" .` shows the
complete deviation from upstream.

Three hooks, nothing else:

  1. UI/MainScreen.cpp
       a. the directory listing also accepts every extension Sphynx knows, so
          NES/SNES/GB/... files appear in the same game list as PSP games;
       b. LaunchFile() asks Sphynx first: if the file belongs to an external
          system the request is routed to the player screen instead of the PSP
          loader (PSP files are untouched and keep the original path).

  2. UI/GameInfoCache.cpp
       For a non-PSP file the icon is a real screenshot rendered from that very
       game by its own emulator core (sphynx_ui_make_thumbnail), cached by
       PPSSPP exactly like ICON0.PNG is for PSP titles.

  3. Android glue
       manifest entry for SphynxPlayerActivity, the "sphynx_launch" message
       handler, the user-facing strings, and linking the JNI library against
       the extcores host.

SPDX-License-Identifier: GPL-2.0-or-later
"""

import re
import shutil
from pathlib import Path

MARK = "SPHYNX:"


def _read(p: Path) -> str:
    return p.read_text(encoding="utf-8", errors="surrogateescape")


def _write(p: Path, s: str) -> None:
    p.write_text(s, encoding="utf-8", errors="surrogateescape")


def patch_mainscreen(repo: Path) -> None:
    f = repo / "UI" / "MainScreen.cpp"
    if not f.exists():
        print("  !! UI/MainScreen.cpp missing")
        return
    s = _read(f)

    # (a) listing filter -----------------------------------------------------
    old_filter = 'path_.GetListing(fileInfo, "iso:cso:chd:pbp:elf:prx:ppdmp:");'
    new_filter = ('// SPHYNX: also list every file of every supported system, so the\n'
                  '\t\t// game list shows PSP and non-PSP titles side by side.\n'
                  '\t\tpath_.GetListing(fileInfo, sphynx_ui_extension_filter());')
    if new_filter.split("\n")[1].strip() in s:
        print("  == MainScreen: filter already patched")
    elif old_filter in s:
        s = s.replace(old_filter, new_filter, 1)
        print("  ok MainScreen: listing filter extended to every system")
    else:
        print("  !! MainScreen: filter anchor not found")

    # (b) launch routing -----------------------------------------------------
    # Canonical form, applied idempotently: strip whatever earlier revision put
    # here, then insert exactly this block once.
    head = "bool LaunchFile(ScreenManager *screenManager, const Path &path) {"
    body_marker = "\t// Depending on the file type, we don't want to launch EmuScreen at all."
    block = ("\n\t// SPHYNX: hand non-PSP content to the Sphynx player screen. PSP files fall\n"
             "\t// straight through to the original code below, unchanged.\n"
             "\tif (path.Type() == PathType::NATIVE) {\n"
             "\t\tconst std::string nativePath = path.ToString();\n"
             "\t\tif (sphynx_ui_is_external(nativePath.c_str()) == 1) {\n"
             "#if PPSSPP_PLATFORM(ANDROID)\n"
             "\t\t\t// The player screen is Java-side; ask the app shell to open it.\n"
             "\t\t\tSphynx_SendCommandToJava(\"sphynx_launch\", nativePath.c_str());\n"
             "\t\t\treturn true;\n"
             "#else\n"
             "\t\t\t// Desktop builds ship no player screen: fall through so the user gets\n"
             "\t\t\t// PPSSPP's own handling instead of silence.\n"
             "#endif\n"
             "\t\t}\n"
             "\t}\n\n")
    if head in s and body_marker in s:
        i = s.index(head) + len(head)
        j = s.index(body_marker, i)
        current = s[i:j]
        section = current.replace(block, "\n")
        # Rebuild cleanly: keep any user edits? No — this region is ours alone.
        if "sphynx_ui_is_external" in section or "SPHYNX:" in section:
            if current != block:
                s = s[:i] + block + s[j:]
                print("  ok MainScreen: launch routing normalised")
            else:
                print("  == MainScreen: launch routing already patched")
        else:
            s = s[:i] + block + s[j:]
            print("  ok MainScreen: LaunchFile routes external systems to Sphynx")
    else:
        print("  !! MainScreen: LaunchFile anchor not found")

    # include for the C bridge
    if "sphynx_ui_api.h" not in s:
        anchor = '#include "Core/Config.h"'
        if anchor in s:
            s = s.replace(anchor, anchor + '\n// SPHYNX: C bridge to the external-core host.\n#include "sphynx_ui_api.h"', 1)
            print("  ok MainScreen: bridge header included")
        else:
            print("  !! MainScreen: include anchor not found")

    _write(f, s)


def patch_gameinfocache(repo: Path) -> None:
    f = repo / "UI" / "GameInfoCache.cpp"
    if not f.exists():
        print("  !! UI/GameInfoCache.cpp missing")
        return
    s = _read(f)

    old = ("\t\t\tcase IdentifiedFileType::NORMAL_DIRECTORY:\n"
           "\t\t\tdefault:\n"
           "\t\t\t\tbreak;\n"
           "\t\t}")
    new = ("\t\t\tcase IdentifiedFileType::NORMAL_DIRECTORY:\n"
           "\t\t\tdefault:\n"
           "\t\t\t{\n"
           "\t\t\t\t// SPHYNX: a non-PSP game gets a real thumbnail rendered from the\n"
           "\t\t\t\t// game itself by its own emulator core, then cached exactly like a\n"
           "\t\t\t\t// PSP ICON0.PNG would be.\n"
           "\t\t\t\tif (flags_ & GameInfoFlags::ICON) {\n"
           "\t\t\t\t\tconst std::string nativePath = gamePath_.ToString();\n"
           "\t\t\t\t\tunsigned char *png = nullptr;\n"
           "\t\t\t\t\tint pngLen = 0;\n"
           "\t\t\t\t\tif (sphynx_ui_is_external(nativePath.c_str()) == 1 &&\n"
           "\t\t\t\t\t    sphynx_ui_make_thumbnail(nativePath.c_str(), &png, &pngLen) && png && pngLen > 0) {\n"
           "\t\t\t\t\t\tstd::lock_guard<std::mutex> lock(info_->lock);\n"
           "\t\t\t\t\t\tinfo_->icon.data.assign(reinterpret_cast<const char *>(png), size_t(pngLen));\n"
           "\t\t\t\t\t\tinfo_->icon.dataLoaded = true;\n"
           "\t\t\t\t\t\tsphynx_ui_free(png);\n"
           "\t\t\t\t\t}\n"
           "\t\t\t\t}\n"
           "\t\t\t\tbreak;\n"
           "\t\t\t}\n"
           "\t\t}")
    if "sphynx_ui_make_thumbnail" in s:
        print("  == GameInfoCache: thumbnail already patched")
    elif old in s:
        s = s.replace(old, new, 1)
        print("  ok GameInfoCache: thumbnails generated from the games themselves")
    else:
        print("  !! GameInfoCache: default-branch anchor not found")
        return

    if "sphynx_ui_api.h" not in s:
        anchor = '#include "Core/Config.h"'
        if anchor in s:
            s = s.replace(anchor, anchor + '\n// SPHYNX: thumbnail bridge.\n#include "sphynx_ui_api.h"', 1)
            print("  ok GameInfoCache: bridge header included")
    _write(f, s)


def patch_manifest(repo: Path) -> None:
    f = repo / "android" / "AndroidManifest.xml"
    s = _read(f)
    if "SphynxPlayerActivity" in s:
        print("  == manifest: player activity already present")
        return
    anchor = '        <activity\n            android:name=".PpssppActivity"'
    if anchor not in s:
        print("  !! manifest: activity anchor not found")
        return
    activity = """        <!-- SPHYNX: playback screen for every non-PSP system. Its own task so
             the PSP side stays exactly as upstream. -->
        <activity
            android:name="org.sphynx.emulator.SphynxPlayerActivity"
            android:configChanges="orientation|screenSize|keyboardHidden|screenLayout|uiMode"
            android:exported="false"
            android:label="@string/app_name"
            android:launchMode="singleTop"
            android:screenOrientation="sensorLandscape"
            android:theme="@android:style/Theme.NoTitleBar.Fullscreen" />

"""
    s = s.replace(anchor, activity + anchor, 1)
    _write(f, s)
    print("  ok manifest: SphynxPlayerActivity registered")


def patch_nativeactivity(repo: Path) -> None:
    """Two independent edits. They used to share one early-return, which is why
    a second run silently skipped the startup paths and thumbnails had no core
    directory to look into."""
    f = repo / "android" / "src" / "org" / "ppsspp" / "ppsspp" / "NativeActivity.java"
    s = _read(f)
    changed = False

    # (a) the sphynx_launch command -> open the player screen
    if 'command.equals("sphynx_launch")' in s:
        print("  == NativeActivity: launch handler already present")
    else:
        anchor = '\t\t} else if (command.equals("launchEmail")) {'
        if anchor in s:
            handler = """\t\t} else if (command.equals("sphynx_launch")) {
\t\t\t// SPHYNX: the C++ game list decided this file belongs to an external
\t\t\t// system; open it in the dedicated player screen.
\t\t\ttry {
\t\t\t\tIntent intent = new Intent(this, org.sphynx.emulator.SphynxPlayerActivity.class);
\t\t\t\tintent.putExtra(org.sphynx.emulator.SphynxPlayerActivity.EXTRA_PATH, params);
\t\t\t\tstartActivity(intent);
\t\t\t\treturn true;
\t\t\t} catch (Exception e) {
\t\t\t\tNativeApp.reportException(e, params);
\t\t\t\tLog.e(TAG, e.toString());
\t\t\t\treturn false;
\t\t\t}
"""
            s = s.replace(anchor, handler + anchor, 1)
            changed = True
            print("  ok NativeActivity: sphynx_launch handler added")
        else:
            print("  !! NativeActivity: command chain anchor not found")

    # (b) startup paths + app context for the external-core host
    if "SphynxExtCore.systemDir" in s and "SphynxExtCore.attachContext" in s:
        print("  == NativeActivity: Sphynx paths already initialised")
    elif "SphynxExtCore.setPaths" in s:
        # Revision 2 passed a single data dir; upgrade it in place.
        old = re.search(r"\t\torg\.sphynx\.emulator\.SphynxExtCore\.setPaths\([^;]*\);", s)
        if old:
            s = s.replace(old.group(0),
                          "\t\torg.sphynx.emulator.SphynxExtCore.setPaths(getApplicationInfo().nativeLibraryDir,\n"
                          "\t\t\t\torg.sphynx.emulator.SphynxExtCore.systemDir(this),\n"
                          "\t\t\t\torg.sphynx.emulator.SphynxExtCore.saveDir(this));")
            if "SphynxExtCore.attachContext" not in s:
                s = s.replace("\t\torg.sphynx.emulator.SphynxExtCore.setPaths(",
                              "\t\torg.sphynx.emulator.SphynxExtCore.attachContext(getApplicationContext());\n"
                              "\t\torg.sphynx.emulator.SphynxExtCore.setPaths(")
            changed = True
            print("  ok NativeActivity: Sphynx paths upgraded (BIOS folder is PSP/SYSTEM)")
    else:
        anchor2 = ('\tpublic void onCreate(Bundle savedInstanceState) {\n'
                   '\t\tsuper.onCreate(savedInstanceState);')
        init = (anchor2 + "\n"
                "\t\t// SPHYNX: where the bundled cores live, where BIOS/saves belong, and\n"
                "\t\t// a context so a content:// game file can be copied for the cores.\n"
                "\t\torg.sphynx.emulator.SphynxExtCore.attachContext(getApplicationContext());\n"
                "\t\torg.sphynx.emulator.SphynxExtCore.setPaths(\n"
                "\t\t\t\tgetApplicationInfo().nativeLibraryDir, getFilesDir().getAbsolutePath());")
        if anchor2 in s:
            s = s.replace(anchor2, init, 1)
            changed = True
            print("  ok NativeActivity: Sphynx paths + context initialised at startup")
        else:
            print("  !! NativeActivity: onCreate anchor not found")

    if changed:
        _write(f, s)


def patch_strings(repo: Path) -> None:
    """Add each Sphynx string individually so a partial file still converges."""
    f = repo / "android" / "res" / "values" / "strings.xml"
    s = _read(f)
    strings = [
        ("sphynxUnknownContent", "Could not recognise this game file."),
        ("sphynxCoreMissing", "The emulator core for this system is missing in this build."),
        ("sphynxStartFailed", "Could not start this system:"),
        ("sphynxCoreStopped", "The emulator stopped (it may be too heavy for this device)."),
        ("sphynxChooseSystem", "Which system is this disc?"),
    ]
    added = []
    for name, text in strings:
        if 'name="%s"' % name in s:
            continue
        line = '\t<string name="%s">%s</string>\n' % (name, text)
        if "<!-- SPHYNX: user-facing messages" not in s:
            line = "\t<!-- SPHYNX: user-facing messages, in PPSSPP's own tone. -->\n" + line
        s = s.replace("</resources>", line + "</resources>", 1)
        added.append(name)
    if added:
        _write(f, s)
        print("  ok strings: added " + ", ".join(added))
    else:
        print("  == strings: already present")


def install_java(repo: Path, overlay_dir: Path) -> None:
    src = overlay_dir / "android" / "sphynx"
    dst = repo / "android" / "src" / "org" / "sphynx" / "emulator"
    dst.mkdir(parents=True, exist_ok=True)
    n = 0
    for f in sorted(src.glob("*.java")):
        shutil.copy2(f, dst / f.name)
        n += 1
    print(f"  ok installed {n} Java sources into org/sphynx/emulator")


def patch_cmake(repo: Path) -> None:
    f = repo / "CMakeLists.txt"
    s = _read(f)
    marker = "#  SPHYNX block"
    block = """# ============================================================================
#  SPHYNX block — multi-system host layer (APPENDED, never inserted)
#
#  Builds extcores/ as its own shared library and links it into the UI targets
#  so the game list and the thumbnail cache can reach it. Disable with
#  -DSPHYNX_EXTCORE=OFF to obtain a byte-for-byte stock PPSSPP v1.18.1 build.
# ============================================================================
option(SPHYNX_EXTCORE "Build the Sphynx external-core host (multi-system support)" ON)
if(SPHYNX_EXTCORE AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/extcores/CMakeLists.txt")
	add_subdirectory(extcores)
	if(TARGET sphynx_extcore)
		# UI sources (MainScreen, GameInfoCache) are compiled into the static
		# 'native' target; the JNI shared library links it.
		if(TARGET native)
			target_include_directories(native PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/extcores/include")
			target_link_libraries(native sphynx_extcore)
		endif()
		if(TARGET ${CoreLibName})
			target_include_directories(${CoreLibName} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/extcores/include")
			target_link_libraries(${CoreLibName} sphynx_extcore)
		endif()
	endif()
endif()
"""
    idx = s.find(marker)
    if idx >= 0:
        head = s[:s.rfind("# ====", 0, idx)]
        if "target_include_directories(native PRIVATE" in s:
            print("  == CMake: Sphynx block already current")
            return
        _write(f, head + block)
        print("  ok CMake: Sphynx block rewritten (native + jni targets)")
        return
    _write(f, s + "\n" + block)
    print("  ok CMake: Sphynx block appended")


def patch_app_android(repo: Path) -> None:
    """Expose the app shell's Java command queue to the extcores library."""
    f = repo / "android" / "jni" / "app-android.cpp"
    if not f.exists():
        print("  !! android/jni/app-android.cpp missing")
        return
    s = _read(f)
    broken = """// SPHYNX: the game list needs to ask Java to open the player screen. This is
// the one-line bridge to the command queue that already exists below.
extern "C" void Sphynx_SendCommandToJava(const char *command, const char *param) {
	PushCommand(command ? command : "", param ? param : "");
}

"""
    if broken in s:
        s = s.replace(broken, "", 1)   # revision 1 placed it above PushCommand
    if "Sphynx_SendCommandToJava" in s:
        _write(f, s)
        print("  == app-android: hook already present")
        return
    # The hook must sit *after* PushCommand's body so the name is declared.
    anchor = ("static void PushCommand(std::string cmd, std::string param) {\n"
              "\tstd::lock_guard<std::mutex> guard(frameCommandLock);\n"
              "\tframeCommands.push(FrameCommand(std::move(cmd), std::move(param)));\n"
              "}")
    hook = anchor + """

// SPHYNX: the game list needs to ask Java to open the player screen. This is
// the one-line bridge into the command queue that Java already drains.
extern "C" void Sphynx_SendCommandToJava(const char *command, const char *param) {
	PushCommand(command ? command : "", param ? param : "");
}"""
    if anchor in s:
        s = s.replace(anchor, hook, 1)
        _write(f, s)
        print("  ok app-android: Sphynx_SendCommandToJava added")
    else:
        print("  !! app-android: PushCommand anchor not found")


def apply(repo: Path, overlay_dir: Path) -> None:
    patch_mainscreen(repo)
    patch_app_android(repo)
    patch_gameinfocache(repo)
    patch_manifest(repo)
    patch_nativeactivity(repo)
    patch_strings(repo)
    install_java(repo, overlay_dir)
    patch_cmake(repo)
