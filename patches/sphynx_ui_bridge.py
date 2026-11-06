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

    # (b) the routing helper -------------------------------------------------
    #
    # This is THE fix for "Failed initializing CPU or memory". Tapping a game in
    # the grid goes MainScreen -> GameScreen -> EmuScreen and never called
    # LaunchFile(), so a SNES/GB/Mega Drive file was handed to the PSP loader.
    # Every launch path now calls this one function instead of constructing an
    # EmuScreen directly.
    helper = (
        "// SPHYNX: the single choke point for \"this file is not a PSP game\".\n"
        "//\n"
        "// Returns true when the file was handed to the Sphynx player screen, false\n"
        "// when the caller must keep its normal PSP flow. It is called from every\n"
        "// place that can start a game: LaunchFile(), GameScreen::OnPlay(), the\n"
        "// boot-from-intent paths in MiscScreens.cpp / NativeApp.cpp, and as a last\n"
        "// line of defence from EmuScreen::update().\n"
        "bool SphynxRouteExternal(ScreenManager *screenManager, const Path &path) {\n"
        "\t// Local covers plain files and Android content:// URIs alike: a game\n"
        "\t// picked through the Storage Access Framework is a CONTENT_URI, and a\n"
        "\t// check for PathType::NATIVE silently skipped every one of those.\n"
        "\tif (!path.IsLocalType())\n"
        "\t\treturn false;\n"
        "\tconst std::string sphynxPath = path.ToString();\n"
        "\tif (sphynx_ui_is_external(sphynxPath.c_str()) != 1)\n"
        "\t\treturn false;\n"
        "#if PPSSPP_PLATFORM(ANDROID)\n"
        "\t// The player screen is Java-side: ask the app shell to open it.\n"
        "\tSphynx_SendCommandToJava(\"sphynx_launch\", sphynxPath.c_str());\n"
        "\t(void)screenManager;\n"
        "\treturn true;\n"
        "#else\n"
        "\t// Desktop builds ship no player screen: let the caller do what it always\n"
        "\t// did instead of swallowing the request.\n"
        "\t(void)screenManager;\n"
        "\treturn false;\n"
        "#endif\n"
        "}\n\n")
    head = "bool LaunchFile(ScreenManager *screenManager, const Path &path) {"
    if "bool SphynxRouteExternal(ScreenManager *screenManager" in s:
        print("  == MainScreen: routing helper already present")
    elif head in s:
        s = s.replace(head, helper + head, 1)
        print("  ok MainScreen: SphynxRouteExternal() added")
    else:
        print("  !! MainScreen: LaunchFile anchor not found")

    # (c) LaunchFile body: always the same three lines ------------------------
    body_marker = "\t// Depending on the file type, we don't want to launch EmuScreen at all."
    route_call = ("\n\t// SPHYNX: hand non-PSP content to the Sphynx player screen. PSP files fall\n"
                  "\t// straight through to the original code below, unchanged.\n"
                  "\tif (SphynxRouteExternal(screenManager, path))\n"
                  "\t\treturn true;\n\n")
    if head in s and body_marker in s:
        i = s.index(head) + len(head)
        j = s.index(body_marker, i)
        s = s[:i] + route_call + s[j:]
        print("  ok MainScreen: LaunchFile routes external systems to Sphynx")
    else:
        print("  !! MainScreen: LaunchFile body anchor not found")

    # include for the C bridge
    if "sphynx_ui_api.h" not in s:
        anchor = '#include "Core/Config.h"'
        if anchor in s:
            s = s.replace(anchor, anchor + '\n// SPHYNX: C bridge to the external-core host.\n#include "sphynx_ui_api.h"', 1)
            print("  ok MainScreen: bridge header included")
        else:
            print("  !! MainScreen: include anchor not found")

    _write(f, s)


def patch_mainscreen_header(repo: Path) -> None:
    """Declare the routing helper next to LaunchFile()."""
    f = repo / "UI" / "MainScreen.h"
    s = _read(f)
    if "SphynxRouteExternal" in s:
        print("  == MainScreen.h: routing helper already declared")
        return
    anchor = "bool LaunchFile(ScreenManager *screenManager, const Path &path);"
    if anchor not in s:
        print("  !! MainScreen.h: LaunchFile declaration not found")
        return
    decl = (anchor + "\n\n"
            "// SPHYNX: true when the file belongs to an external system and the player\n"
            "// screen took over; false when the caller must run its normal PSP flow.\n"
            "bool SphynxRouteExternal(ScreenManager *screenManager, const Path &path);")
    s = s.replace(anchor, decl, 1)
    _write(f, s)
    print("  ok MainScreen.h: SphynxRouteExternal() declared")


def patch_gamescreen(repo: Path) -> None:
    """GameScreen::OnPlay is the path a tap in the game grid actually takes."""
    f = repo / "UI" / "GameScreen.cpp"
    if not f.exists():
        print("  !! UI/GameScreen.cpp missing")
        return
    s = _read(f)
    old = ("UI::EventReturn GameScreen::OnPlay(UI::EventParams &e) {\n"
           "\tscreenManager()->switchScreen(new EmuScreen(gamePath_));")
    new = ("UI::EventReturn GameScreen::OnPlay(UI::EventParams &e) {\n"
           "\t// SPHYNX: a non-PSP game must never be booted by the PSP core. This is the\n"
           "\t// exact path taken when a game is tapped in the grid, and it never went\n"
           "\t// through LaunchFile() — the reason every external title used to fail with\n"
           "\t// \"Failed initializing CPU or memory\".\n"
           "\tif (SphynxRouteExternal(screenManager(), gamePath_))\n"
           "\t\treturn UI::EVENT_DONE;\n"
           "\tscreenManager()->switchScreen(new EmuScreen(gamePath_));")
    if "SphynxRouteExternal" in s:
        print("  == GameScreen: already routed")
    elif old in s:
        s = s.replace(old, new, 1)
        print("  ok GameScreen: OnPlay routes external systems to Sphynx")
    else:
        print("  !! GameScreen: OnPlay anchor not found")
    if '#include "UI/MainScreen.h"' not in s:
        anchor = '#include "UI/EmuScreen.h"'
        if anchor in s:
            s = s.replace(anchor, anchor + '\n#include "UI/MainScreen.h"   // SPHYNX: SphynxRouteExternal()', 1)
    _write(f, s)


def patch_miscscreens(repo: Path) -> None:
    """Boot-from-intent and the post-logo screen also start a game directly."""
    f = repo / "UI" / "MiscScreens.cpp"
    if not f.exists():
        print("  !! UI/MiscScreens.cpp missing")
        return
    s = _read(f)
    pairs = [
        ("\t\t\tif (!gamePath.empty()) {\n"
         "\t\t\t\tscreenManager()->switchScreen(new EmuScreen(gamePath));",
         "\t\t\tif (!gamePath.empty()) {\n"
         "\t\t\t\t// SPHYNX: never hand a non-PSP file to the PSP core.\n"
         "\t\t\t\tif (!SphynxRouteExternal(screenManager(), gamePath))\n"
         "\t\t\t\t\tscreenManager()->switchScreen(new EmuScreen(gamePath));"),
        ("\t\t\tif (boot_filename.size()) {\n"
         "\t\t\t\tscreenManager()->switchScreen(new EmuScreen(gamePath));",
         "\t\t\tif (boot_filename.size()) {\n"
         "\t\t\t\t// SPHYNX: a game opened from another app can be an external one.\n"
         "\t\t\t\tif (!SphynxRouteExternal(screenManager(), gamePath))\n"
         "\t\t\t\t\tscreenManager()->switchScreen(new EmuScreen(gamePath));"),
        ("\tif (message == UIMessage::REQUEST_GAME_BOOT && screenManager()->topScreen() == this) {\n"
         "\t\tscreenManager()->switchScreen(new EmuScreen(Path(value)));",
         "\tif (message == UIMessage::REQUEST_GAME_BOOT && screenManager()->topScreen() == this) {\n"
         "\t\t// SPHYNX: same guard for the \"boot this file now\" message.\n"
         "\t\tconst Path sphynxBootPath(value);\n"
         "\t\tif (!SphynxRouteExternal(screenManager(), sphynxBootPath))\n"
         "\t\t\tscreenManager()->switchScreen(new EmuScreen(sphynxBootPath));"),
    ]
    changed = False
    for old, new in pairs:
        if new in s:
            continue
        if old in s:
            s = s.replace(old, new, 1)
            changed = True
        else:
            print("  !! MiscScreens: anchor not found\n" + old.replace("\n", " | "))
    if changed:
        _write(f, s)
        print("  ok MiscScreens: every boot path routed through SphynxRouteExternal")
    else:
        print("  == MiscScreens: already routed")
    if "SphynxRouteExternal" in s and '#include "UI/MainScreen.h"' not in s:
        anchor = '#include "Core/Config.h"'
        if anchor in s:
            s = s.replace(anchor, anchor + '\n#include "UI/MainScreen.h"', 1)
            _write(f, s)


def patch_nativeapp(repo: Path) -> None:
    """Startup: create the BIOS tree, and route a game opened from outside."""
    f = repo / "UI" / "NativeApp.cpp"
    if not f.exists():
        print("  !! UI/NativeApp.cpp missing")
        return
    s = _read(f)
    changed = False

    # (a) the BIOS tree, created the moment the app starts
    bios_block = (
        "\t// SPHYNX: build PSP/SYSTEMS_BIOS — one folder per system that uses a BIOS\n"
        "\t// (12 mandatory + 4 recommended) with a README naming the exact files, and\n"
        "\t// copy anything already sitting in PSP/SYSTEM into the matching folder.\n"
        "\t// Only a real filesystem memstick folder can hold it; a Storage-Access\n"
        "\t// Framework folder is handled by the Java side instead.\n"
        "\tif (g_Config.memStickDirectory.Type() == PathType::NATIVE &&\n"
        "\t    !g_Config.memStickDirectory.empty()) {\n"
        "\t\tsphynx_ui_ensure_bios_tree(g_Config.memStickDirectory.ToString().c_str());\n"
        "\t}\n\n")
    anchor = ('\tDEBUG_LOG(Log::System, "ScreenManager!");\n'
              '\tg_screenManager = new ScreenManager();')
    if "sphynx_ui_ensure_bios_tree" in s:
        print("  == NativeApp: BIOS tree already created at startup")
    elif anchor in s:
        s = s.replace(anchor, bios_block + anchor, 1)
        changed = True
        print("  ok NativeApp: BIOS tree created at startup")
    else:
        print("  !! NativeApp: ScreenManager anchor not found")

    # (b) a game opened from another app
    old = ("\t} else if (skipLogo) {\n"
           "\t\tg_screenManager->switchScreen(new EmuScreen(boot_filename));")
    new = ("\t} else if (skipLogo) {\n"
           "\t\t// SPHYNX: \"open with\" can hand us a file of any system.\n"
           "\t\tif (!SphynxRouteExternal(g_screenManager, boot_filename))\n"
           "\t\t\tg_screenManager->switchScreen(new EmuScreen(boot_filename));")
    if "SphynxRouteExternal(g_screenManager, boot_filename)" in s:
        print("  == NativeApp: boot path already routed")
    elif old in s:
        s = s.replace(old, new, 1)
        changed = True
        print("  ok NativeApp: boot-from-intent routed through Sphynx")
    else:
        print("  !! NativeApp: skipLogo anchor not found")

    if "sphynx_ui_api.h" not in s:
        anchor2 = '#include "Core/Config.h"'
        if anchor2 in s:
            s = s.replace(anchor2, anchor2 + '\n// SPHYNX: BIOS tree + routing bridge.\n#include "sphynx_ui_api.h"', 1)
            changed = True
    if '#include "UI/MainScreen.h"' not in s:
        anchor3 = '#include "UI/EmuScreen.h"'
        if anchor3 in s:
            s = s.replace(anchor3, anchor3 + '\n#include "UI/MainScreen.h"   // SPHYNX: SphynxRouteExternal()', 1)
            changed = True

    if changed:
        _write(f, s)


def patch_emuscreen(repo: Path) -> None:
    """Last line of defence: an EmuScreen created by a path we did not patch."""
    h = repo / "UI" / "EmuScreen.h"
    s = _read(h)
    if "sphynxRouted_" in s:
        print("  == EmuScreen.h: guard flag already present")
    else:
        anchor = "\tbool bootPending_ = true;\n\tPath gamePath_;"
        if anchor in s:
            new = ("\tbool bootPending_ = true;\n"
                   "\t// SPHYNX: set once this screen has checked whether the file is really\n"
                   "\t// a PSP game, so the check runs exactly once per screen.\n"
                   "\tbool sphynxRouted_ = false;\n"
                   "\tPath gamePath_;")
            s = s.replace(anchor, new, 1)
            _write(h, s)
            print("  ok EmuScreen.h: guard flag added")
        else:
            print("  !! EmuScreen.h: bootPending_ anchor not found")

    f = repo / "UI" / "EmuScreen.cpp"
    s = _read(f)
    if "SphynxRouteExternal(screenManager(), gamePath_)" in s:
        print("  == EmuScreen.cpp: catch-all already present")
        return
    anchor = ("void EmuScreen::update() {\n"
              "\tusing namespace UI;\n")
    guard = (anchor + "\n"
             "\t// SPHYNX: last line of defence. Every launch path routes through\n"
             "\t// SphynxRouteExternal() before this screen is even created; this block\n"
             "\t// makes sure that a path nobody patched still ends up in the player\n"
             "\t// instead of failing with \"Failed initializing CPU or memory\".\n"
             "\tif (!sphynxRouted_) {\n"
             "\t\tsphynxRouted_ = true;\n"
             "\t\tif (SphynxRouteExternal(screenManager(), gamePath_)) {\n"
             "\t\t\tscreenManager()->switchScreen(new MainScreen());\n"
             "\t\t\treturn;\n"
             "\t\t}\n"
             "\t}\n")
    if anchor in s:
        s = s.replace(anchor, guard, 1)
        _write(f, s)
        print("  ok EmuScreen.cpp: catch-all routing added")
    else:
        print("  !! EmuScreen.cpp: update() anchor not found")


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
                "\t\t\t\tgetApplicationInfo().nativeLibraryDir,\n"
                "\t\t\t\torg.sphynx.emulator.SphynxExtCore.systemDir(this),\n"
                "\t\t\t\torg.sphynx.emulator.SphynxExtCore.saveDir(this));\n"
                "\t\t// SPHYNX: create PSP/SYSTEMS_BIOS now, so the folders are there\n"
                "\t\t// the first time the user opens the app.\n"
                "\t\torg.sphynx.emulator.SphynxExtCore.ensureBiosTree(this);")
        if anchor2 in s:
            s = s.replace(anchor2, init, 1)
            changed = True
            print("  ok NativeActivity: Sphynx paths + context initialised at startup")
        else:
            print("  !! NativeActivity: onCreate anchor not found")

    if changed:
        _write(f, s)

    # (c) the BIOS tree must exist from the very first launch. Added as its own
    # idempotent step so a tree patched by an earlier revision still gets it.
    s = _read(f)
    if "SphynxExtCore.ensureBiosTree" not in s:
        anchor = ("\t\torg.sphynx.emulator.SphynxExtCore.setPaths(getApplicationInfo().nativeLibraryDir,\n"
                  "\t\t\t\torg.sphynx.emulator.SphynxExtCore.saveDir(this));")
        if anchor in s:
            s = s.replace(anchor, anchor + (
                "\n\t\t// SPHYNX: create PSP/SYSTEMS_BIOS at start-up, so the BIOS folders are\n"
                "\t\t// there the first time the user opens the app — not after a game fails.\n"
                "\t\torg.sphynx.emulator.SphynxExtCore.ensureBiosTree(this);"), 1)
            _write(f, s)
            print("  ok NativeActivity: BIOS tree created at start-up")
        else:
            print("  !! NativeActivity: setPaths anchor not found for the BIOS tree")
    else:
        print("  == NativeActivity: BIOS tree already created at start-up")


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
    patch_mainscreen_header(repo)
    patch_gamescreen(repo)
    patch_miscscreens(repo)
    patch_nativeapp(repo)
    patch_emuscreen(repo)
    patch_app_android(repo)
    patch_gameinfocache(repo)
    patch_manifest(repo)
    patch_nativeactivity(repo)
    patch_strings(repo)
    install_java(repo, overlay_dir)
    patch_cmake(repo)
