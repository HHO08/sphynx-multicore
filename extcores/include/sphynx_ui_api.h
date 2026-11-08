// ============================================================================
//  Sphynx — extcores/include/sphynx_ui_api.h
//
//  The ONLY surface PPSSPP's own C++ is allowed to call. Plain C, no C++
//  types, no PPSSPP headers — so a change callback stays a two-line diff and
//  nothing in the PSP core can be pulled into this layer.
//
//  SPDX-License-Identifier: GPL-2.0-or-later
// ============================================================================
#pragma once

// The host library is compiled with -fvisibility=hidden (project size rule),
// so the few entry points that PPSSPP's UI calls must be exported explicitly.
#if defined(__GNUC__)
#define SPHYNX_UI_EXPORT __attribute__((visibility("default")))
#else
#define SPHYNX_UI_EXPORT
#endif


#ifdef __cplusplus
extern "C" {
#endif

// Paths the UI layer needs in order to find bundled cores and store saves.
// Called once from Java at startup (nativeLibraryDir + app data dirs).
// Same as sphynx_ui_set_paths, but also records the process JavaVM so native
// worker threads (thumbnail generation) can call back into Java.
SPHYNX_UI_EXPORT void sphynx_ui_set_paths_and_vm(const char *native_library_dir,
                                                 const char *system_dir,
                                                 const char *save_dir, void *jni_env);

SPHYNX_UI_EXPORT void sphynx_ui_set_paths(const char *native_library_dir, const char *system_dir,
                         const char *save_dir);

// The same three directories, read back. Used by the player core so it never
// keeps a second copy that could disagree with the game list.
SPHYNX_UI_EXPORT const char *sphynx_ui_native_lib_dir(void);
SPHYNX_UI_EXPORT const char *sphynx_ui_system_dir(void);
SPHYNX_UI_EXPORT const char *sphynx_ui_save_dir(void);

// 1 = this file belongs to an external system (Sphynx must play it),
// 0 = leave it to PPSSPP's own core (PSP games), -1 = unknown content.
SPHYNX_UI_EXPORT int sphynx_ui_is_external(const char *path);

// Renders a real thumbnail by booting the game's own core for a few frames.
// On success returns 1 and hands back a PNG in *out/*len (free() with sphynx_ui_free).
// Returns 0 when no thumbnail could be produced (the UI then keeps the default).
SPHYNX_UI_EXPORT int sphynx_ui_make_thumbnail(const char *path, unsigned char **out, int *len);
SPHYNX_UI_EXPORT void sphynx_ui_free(void *p);

// Opens the Sphynx player screen for this file.
//
// Implemented by the Android app shell (android/jni/app-android.cpp), which owns
// the command queue Java drains every frame. It is declared here — not defined
// in the host library — because the shell links *against* the host library, so
// the host library must never reference the shell.
#ifdef __cplusplus
extern "C"
#endif
void Sphynx_SendCommandToJava(const char *command, const char *param);

// "key|Display name;key|Display name;..." — systems that could claim this file.
// Used when an extension is shared by several systems (.cue/.chd/.iso).
SPHYNX_UI_EXPORT const char *sphynx_ui_candidates(const char *path);

// Resolves an Android content:// URI (or a plain path) to a file the emulator
// cores can fopen(). Returns a malloc'd string; free with sphynx_ui_free().
// NULL when the file cannot be reached at all.
SPHYNX_UI_EXPORT char *sphynx_ui_local_path(const char *path);

// Human readable reason for the last failure (never null).
SPHYNX_UI_EXPORT const char *sphynx_ui_last_error(void);

// Comma-separated list of every extension Sphynx understands, in the form
// PPSSPP's directory listing expects ("nes:gb:gba:...").
SPHYNX_UI_EXPORT const char *sphynx_ui_extension_filter(void);

// ---------------------------------------------------------------------------
// BIOS tree: <memstick>/SYSTEMS_BIOS
// ---------------------------------------------------------------------------
// Called once at startup with PPSSPP's own memstick folder (the "PSP" folder
// the user already knows). Creates one sub-folder per system that needs a BIOS
// — the 12 mandatory ones and the 4 recommended ones — with a README naming
// the exact files, and copies anything already sitting in PSP/SYSTEM into the
// matching folder. Never overwrites or deletes a user file.
//
// Returns the number of folders created, or -1 when the folder is unusable.
SPHYNX_UI_EXPORT int sphynx_ui_ensure_bios_tree(const char *memstick_dir);

// Same value, without creating anything (used by Java when the C++ start-up
// path has not run yet, e.g. the player opened directly).
SPHYNX_UI_EXPORT void sphynx_ui_set_memstick(const char *memstick_dir);

// SPHYNX (stage 17): tell the host about one more PSP folder the device has.
// Java reports every folder it knows (internal storage, Android/media, SD
// card); the host then points each system at the folder that really contains
// that system's BIOS files, instead of whichever folder was reported last.
// Calling sphynx_ui_set_memstick() also registers its folder as a candidate.
SPHYNX_UI_EXPORT void sphynx_ui_add_memstick_candidate(const char *memstick_dir);

// "…/PSP/SYSTEMS_BIOS" or "" when unknown.
SPHYNX_UI_EXPORT const char *sphynx_ui_bios_root(void);

// Folder for one system ("psx", "saturn", …), created on demand.
// malloc'd string; free with sphynx_ui_free(). NULL when the system needs no
// BIOS, which is the signal to fall back to PPSSPP's normal SYSTEM folder.
SPHYNX_UI_EXPORT char *sphynx_ui_bios_dir_for_system(const char *system_key);

// Comma-separated expected files that are missing ("" = complete or no BIOS).
SPHYNX_UI_EXPORT char *sphynx_ui_bios_missing(const char *system_key);

// "complete" / "missing x, y" / "no BIOS needed".
SPHYNX_UI_EXPORT const char *sphynx_ui_bios_status(const char *system_key);

// SPHYNX (stage 15): called right before a core is loaded. Recreates the tree,
// adopts anything new from PSP/SYSTEM, and — the part that was missing — gives
// every expected BIOS file the exact, lower-case name the core's fopen() uses,
// so "BIOS7.BIN" or "nds_bios7.bin" sitting in the folder finally counts as a
// BIOS instead of being invisible. Deletes nothing.
SPHYNX_UI_EXPORT void sphynx_ui_prepare_bios(const char *system_key);

// Detailed per-file state of one system's folder, e.g.
//   "nds · /…/SYSTEMS_BIOS/Nintendo DS · found: firmware.bin (256 KiB) ·
//    missing: bios7.bin, bios9.bin"
// malloc'd string; free with sphynx_ui_free(). Never NULL.
SPHYNX_UI_EXPORT char *sphynx_ui_bios_report(const char *system_key);

// ---------------------------------------------------------------------------
// Opening the player screen
// ---------------------------------------------------------------------------
// The Android app shell hands us its Context once at start-up. With it the
// player can be opened straight from native code, without going through
// PPSSPP's per-frame command queue — one less thing that can silently fail.
SPHYNX_UI_EXPORT void sphynx_ui_set_context(void *jni_env, void *context);

// 1 = the player screen was started, 0 = no Context yet (the caller then falls
// back to the command queue), -1 = Java refused (see the log).
SPHYNX_UI_EXPORT int sphynx_ui_launch_player(const char *path);

// 1 when the file extension belongs to a system Sphynx handles (it reads the
// same table the game-list filter uses, so the two can never disagree).
SPHYNX_UI_EXPORT int sphynx_ui_is_known_extension(const char *path);

// Short build tag of this binary, printed on screen when a game starts so a
// screenshot says unambiguously which build is running.
SPHYNX_UI_EXPORT const char *sphynx_ui_build_tag(void);

// Flight recorder: PSP/sphynx_log.txt (see diag_log.h).
SPHYNX_UI_EXPORT void sphynx_ui_set_log_path(const char *path);
SPHYNX_UI_EXPORT const char *sphynx_ui_log_path(void);
// One line into the same log, callable from PPSSPP's UI code.
SPHYNX_UI_EXPORT void sphynx_ui_log_line(const char *text);

// A short multi-line report for on-screen display when something goes wrong:
// build id, the last failure, and the tail of the log. It is meant to be
// screenshotted, so a problem on a device I cannot touch becomes readable.
SPHYNX_UI_EXPORT const char *sphynx_ui_diag_report(void);

#ifdef __cplusplus
}
#endif
