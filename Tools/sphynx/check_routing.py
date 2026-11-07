#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
check_routing.py — guards the fix for "Failed initializing CPU or memory".

The bug
-------
LaunchFile() was patched to send non-PSP games to the player screen, but the
path a tap in the game grid actually takes is

    MainScreen::OnGameSelected -> GameScreen -> GameScreen::OnPlay -> EmuScreen

which never called LaunchFile(). Every SNES/GB/Mega Drive/Master System file
was therefore handed to the PSP loader, which failed with
"Failed initializing CPU or memory".

This script fails the build if any `new EmuScreen(` can be reached without
going through SphynxRouteExternal() first, so the same mistake cannot come
back when PPSSPP is updated.

Exit code 0 = every launch path is guarded.
SPDX-License-Identifier: GPL-2.0-or-later
"""

import re
import sys
from pathlib import Path

# Files where an unguarded `new EmuScreen(` is correct and must stay:
#   MainScreen.cpp — LaunchFile() has already returned for external files
#   Store.cpp      — the PPSSPP store only ever serves PSP content
ALLOWED_UNGUARDED = {
    "UI/MainScreen.cpp": "LaunchFile() returned early for external files",
    "UI/Store.cpp": "the store only serves PSP games",
}

# Every file that must contain the guard call.
REQUIRED_CALLERS = [
    "UI/MainScreen.cpp",
    "UI/GameScreen.cpp",
    "UI/MiscScreens.cpp",
    "UI/NativeApp.cpp",
    "UI/EmuScreen.cpp",
]

GUARD = "SphynxRouteExternal"
LOOKBACK = 8   # lines of context searched above a `new EmuScreen(`


def main(root: str) -> int:
    repo = Path(root).resolve()
    problems = []

    # 1. the guard exists, and is declared for every caller
    main_screen = (repo / "UI" / "MainScreen.cpp").read_text(encoding="utf-8", errors="surrogateescape")
    if "bool SphynxRouteExternal(ScreenManager *screenManager, const Path &path) {" not in main_screen:
        problems.append("UI/MainScreen.cpp does not define SphynxRouteExternal()")
    header = (repo / "UI" / "MainScreen.h").read_text(encoding="utf-8", errors="surrogateescape")
    if "bool SphynxRouteExternal(ScreenManager *screenManager, const Path &path);" not in header:
        problems.append("UI/MainScreen.h does not declare SphynxRouteExternal()")

    # The old bug in one line: a routing check that only accepts NATIVE paths.
    # Match the code (not the comments explaining it).
    guard_body = main_screen.split("bool SphynxRouteExternal")[1].split("\n}\n")[0]
    if re.search(r"Type\(\)\s*==\s*PathType::NATIVE", guard_body):
        problems.append("SphynxRouteExternal() checks PathType::NATIVE "
                        "(content:// games would be skipped again)")
    if "IsLocalType()" not in guard_body:
        problems.append("SphynxRouteExternal() does not use Path::IsLocalType()")

    # 2. every caller really calls it
    for rel in REQUIRED_CALLERS:
        f = repo / rel
        if not f.exists():
            problems.append(f"{rel} is missing")
            continue
        text = f.read_text(encoding="utf-8", errors="surrogateescape")
        if GUARD not in text:
            problems.append(f"{rel} never calls {GUARD}()")

    # 3. no unguarded EmuScreen construction outside the allowlist
    guarded_sites = 0
    unguarded_sites = 0
    for f in sorted((repo / "UI").glob("*.cpp")):
        rel = "UI/" + f.name
        lines = f.read_text(encoding="utf-8", errors="surrogateescape").splitlines()
        for i, line in enumerate(lines):
            if "new EmuScreen(" not in line:
                continue
            context = "\n".join(lines[max(0, i - LOOKBACK):i + 1])
            if GUARD in context:
                guarded_sites += 1
                continue
            if rel in ALLOWED_UNGUARDED:
                print(f"  ok    {rel}:{i + 1} unguarded on purpose — {ALLOWED_UNGUARDED[rel]}")
                continue
            unguarded_sites += 1
            problems.append(f"{rel}:{i + 1} creates an EmuScreen without {GUARD}() first")

    print(f"  guarded EmuScreen sites: {guarded_sites}, unguarded: {unguarded_sites}")

    # 3b. the two safety nets this stage added
    emu = (repo / "UI" / "EmuScreen.cpp").read_text(encoding="utf-8", errors="surrogateescape")
    if "sphynxBootFailed_" not in emu:
        problems.append("UI/EmuScreen.cpp lost the boot-failure flag")
    if "SphynxRouteBootFailure(screenManager(), gamePath_)" not in emu:
        problems.append("UI/EmuScreen.cpp lost the second-chance routing")
    if "SphynxRouteBootFailure(screenManager(), filename)" not in emu:
        problems.append("UI/EmuScreen.cpp no longer guards bootGame() before the PSP core")
    if "sphynx_ui_is_known_extension" not in main_screen:
        problems.append("UI/MainScreen.cpp does not fall back to the extension table")
    if "sphynx_ui_diag_report" not in emu:
        problems.append("UI/EmuScreen.cpp lost the diagnostic failure message")
    gamescreen = (repo / "UI" / "GameScreen.cpp").read_text(encoding="utf-8", errors="surrogateescape")
    if "sphynxChecked_" not in gamescreen:
        problems.append("UI/GameScreen.cpp does not route when the game sheet opens")
    if "bool SphynxRouteBootFailure(ScreenManager *screenManager, const Path &path) {" not in main_screen:
        problems.append("UI/MainScreen.cpp does not define SphynxRouteBootFailure()")
    if "sphynx_ui_log_line" not in main_screen:
        problems.append("UI/MainScreen.cpp does not record routing decisions in the log")
    if not (repo / "extcores" / "src" / "diag_log.cpp").exists():
        problems.append("extcores/src/diag_log.cpp is missing")

    # 4. the BIOS tree is built at start-up on both sides of the bridge
    native_app = (repo / "UI" / "NativeApp.cpp").read_text(encoding="utf-8", errors="surrogateescape")
    if "sphynx_ui_ensure_bios_tree" not in native_app:
        problems.append("UI/NativeApp.cpp does not build PSP/SYSTEMS_BIOS at start-up")
    activity = repo / "android" / "src" / "org" / "ppsspp" / "ppsspp" / "NativeActivity.java"
    if not activity.exists() or "ensureBiosTree" not in activity.read_text(encoding="utf-8", errors="surrogateescape"):
        problems.append("NativeActivity.java does not create the BIOS tree at start-up")

    # 5. Stage 9: the player is a PPSSPP screen, and it must keep using
    # PPSSPP's own pad and menu instead of drawing its own.
    screen = repo / "UI" / "SphynxPlayerScreen.cpp"
    if not screen.exists():
        problems.append("UI/SphynxPlayerScreen.cpp is missing")
    else:
        text = screen.read_text(encoding="utf-8", errors="surrogateescape")
        if "CreatePadLayout(" not in text:
            problems.append("UI/SphynxPlayerScreen.cpp does not use PPSSPP's own touch pad")
        for needed in ("\"Save state\"", "\"Load state\"", "pa->T(\"Settings\")"):
            if needed not in text:
                problems.append(f"UI/SphynxPlayerScreen.cpp lost the {needed} entry")
        if "GameSettingsScreen" not in text:
            problems.append("UI/SphynxPlayerScreen.cpp does not open PPSSPP's settings screen")
        if "sphynx_player_" not in text:
            problems.append("UI/SphynxPlayerScreen.cpp does not talk to the player core")
    if "new SphynxPlayerScreen(" not in main_screen:
        problems.append("UI/MainScreen.cpp no longer opens the player screen")
    if "new MainScreen()" not in screen.read_text(encoding="utf-8", errors="surrogateescape") if screen.exists() else False:
        problems.append("UI/SphynxPlayerScreen.cpp does not return to the game list on exit")
    # Stage 10: the separate player Activity must be GONE. Keeping it in the
    # build is what let an old, hand-drawn button layout reappear on a device
    # after the new player had already been written.
    for gone in ("android/src/org/sphynx/emulator/SphynxPlayerActivity.java",
                 "extcores/src/player_android.cpp",
                 "extcores/jni/sphynx_launch_jni.cpp"):
        if (repo / gone).exists():
            problems.append(f"{gone} is back: the old Activity-based player must stay deleted")
    manifest = (repo / "android" / "AndroidManifest.xml").read_text(encoding="utf-8", errors="surrogateescape")
    if "SphynxPlayerActivity" in manifest:
        problems.append("AndroidManifest.xml still declares the old player Activity")
    activity = (repo / "android" / "src" / "org" / "ppsspp" / "ppsspp" / "NativeActivity.java").read_text(encoding="utf-8", errors="surrogateescape")
    if "new Intent(this, org.sphynx.emulator.SphynxPlayerActivity.class)" in activity:
        problems.append("NativeActivity.java still starts the old player Activity")

    # ... and the new screen must never leave on its own: every failure path
    # goes through ShowProblem(), which stays on screen until the user taps.
    if "ShowProblem" not in screen.read_text(encoding="utf-8", errors="surrogateescape") if screen.exists() else True:
        problems.append("UI/SphynxPlayerScreen.cpp has no on-screen problem panel (failures would kick the user out)")

    # 4. the BIOS tree is built at start-up on both sides of the bridge
    native_app = (repo / "UI" / "NativeApp.cpp").read_text(encoding="utf-8", errors="surrogateescape")
    if "sphynx_ui_ensure_bios_tree" not in native_app:
        problems.append("UI/NativeApp.cpp does not build PSP/SYSTEMS_BIOS at start-up")
    activity = repo / "android" / "src" / "org" / "ppsspp" / "ppsspp" / "NativeActivity.java"
    if not activity.exists() or "ensureBiosTree" not in activity.read_text(encoding="utf-8", errors="surrogateescape"):
        problems.append("NativeActivity.java does not create the BIOS tree at start-up")

    # 5. Stage 9: the player is a PPSSPP screen, and it must keep using
    # PPSSPP's own pad and menu instead of drawing its own.
    screen = repo / "UI" / "SphynxPlayerScreen.cpp"
    if not screen.exists():
        problems.append("UI/SphynxPlayerScreen.cpp is missing")
    else:
        text = screen.read_text(encoding="utf-8", errors="surrogateescape")
        if "CreatePadLayout(" not in text:
            problems.append("UI/SphynxPlayerScreen.cpp does not use PPSSPP's own touch pad")
        for needed in ("\"Save state\"", "\"Load state\"", "pa->T(\"Settings\")"):
            if needed not in text:
                problems.append(f"UI/SphynxPlayerScreen.cpp lost the {needed} entry")
        if "GameSettingsScreen" not in text:
            problems.append("UI/SphynxPlayerScreen.cpp does not open PPSSPP's settings screen")
        if "sphynx_player_" not in text:
            problems.append("UI/SphynxPlayerScreen.cpp does not talk to the player core")
    if "new SphynxPlayerScreen(" not in main_screen:
        problems.append("UI/MainScreen.cpp no longer opens the player screen")
    if "new MainScreen()" not in screen.read_text(encoding="utf-8", errors="surrogateescape") if screen.exists() else False:
        problems.append("UI/SphynxPlayerScreen.cpp does not return to the game list on exit")
    # The three entries the owner required must exist in the in-process player's
    # pause menu and must keep working through the core's own save states.
    if screen.exists():
        text = screen.read_text(encoding="utf-8", errors="surrogateescape")
        for needed in ("SaveState(", "LoadState(", "GameSettingsScreen("):
            if needed not in text:
                problems.append(f"UI/SphynxPlayerScreen.cpp lost {needed}")

    if problems:
        print("\nROUTING CHECK FAILED:")
        for p in problems:
            print("  - " + p)
        return 1
    print("routing check: every game launch path is guarded")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else "."))
