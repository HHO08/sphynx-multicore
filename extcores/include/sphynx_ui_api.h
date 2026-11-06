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

#ifdef __cplusplus
extern "C" {
#endif

// Paths the UI layer needs in order to find bundled cores and store saves.
// Called once from Java at startup (nativeLibraryDir + app data dirs).
void sphynx_ui_set_paths(const char *native_library_dir, const char *system_dir,
                         const char *save_dir);

// 1 = this file belongs to an external system (Sphynx must play it),
// 0 = leave it to PPSSPP's own core (PSP games), -1 = unknown content.
int sphynx_ui_is_external(const char *path);

// Renders a real thumbnail by booting the game's own core for a few frames.
// On success returns 1 and hands back a PNG in *out/*len (free() with sphynx_ui_free).
// Returns 0 when no thumbnail could be produced (the UI then keeps the default).
int sphynx_ui_make_thumbnail(const char *path, unsigned char **out, int *len);
void sphynx_ui_free(void *p);

// "key|Display name;key|Display name;..." — systems that could claim this file.
// Used when an extension is shared by several systems (.cue/.chd/.iso).
const char *sphynx_ui_candidates(const char *path);

// Human readable reason for the last failure (never null).
const char *sphynx_ui_last_error(void);

// Comma-separated list of every extension Sphynx understands, in the form
// PPSSPP's directory listing expects ("nes:gb:gba:...").
const char *sphynx_ui_extension_filter(void);

#ifdef __cplusplus
}
#endif
