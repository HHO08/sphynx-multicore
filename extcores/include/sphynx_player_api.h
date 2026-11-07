// ============================================================================
//  Sphynx — extcores/include/sphynx_player_api.h
//
//  The player, as PPSSPP's own UI sees it.
//
//  Stage 9 replaced the separate Android player Activity with a normal PPSSPP
//  screen (UI/SphynxPlayerScreen.cpp). Two things follow from that:
//
//    * the game picture is drawn by PPSSPP's renderer, and the buttons, the
//      pause menu and the settings screens are PPSSPP's own — so they are
//      pixel-identical to a PSP game by construction, not by imitation;
//    * nothing can "kick the user out" any more: there is no second Activity,
//      no second EGL context and no second audio thread. A failing core shows
//      a normal PPSSPP dialog and the game list is still behind it.
//
//  This header is plain C: no C++ types, no PPSSPP headers. Everything here is
//  exported from libsphynx_extcore.so, which PPSSPP's UI links against.
//
//  SPDX-License-Identifier: GPL-2.0-or-later
// ============================================================================
#pragma once

#include <stddef.h>

#if defined(__GNUC__)
#define SPHYNX_PLAYER_EXPORT __attribute__((visibility("default")))
#else
#define SPHYNX_PLAYER_EXPORT
#endif

#ifdef __cplusplus
extern "C" {
#endif

// Opaque handle: one running core plus its video/audio/state plumbing.
typedef struct SphynxPlayerHandle SphynxPlayerHandle;

// Button bits, identical to the values PPSSPP's own pad produces (sceCtrl.h),
// so the UI can hand us __CtrlPeekButtons() unchanged and every existing
// mapping — on-screen pad, physical gamepad, user remaps — keeps working.
#define SPHYNX_PAD_SELECT   0x0001
#define SPHYNX_PAD_START    0x0008
#define SPHYNX_PAD_UP       0x0010
#define SPHYNX_PAD_RIGHT    0x0020
#define SPHYNX_PAD_DOWN     0x0040
#define SPHYNX_PAD_LEFT     0x0080
#define SPHYNX_PAD_LTRIGGER 0x0100
#define SPHYNX_PAD_RTRIGGER 0x0200
#define SPHYNX_PAD_L2       0x0400
#define SPHYNX_PAD_R2       0x0800
#define SPHYNX_PAD_L3       0x0002
#define SPHYNX_PAD_R3       0x0004
#define SPHYNX_PAD_TRIANGLE 0x1000
#define SPHYNX_PAD_CIRCLE   0x2000
#define SPHYNX_PAD_CROSS    0x4000
#define SPHYNX_PAD_SQUARE   0x8000

// Opens the game. `path` may be a plain file or an Android content:// URI.
// `system_key` may be NULL, in which case the system is detected from the file
// (the same tables the game list uses).
//
// Returns NULL on failure and writes the reason into `err` when it is not NULL.
SPHYNX_PLAYER_EXPORT SphynxPlayerHandle *sphynx_player_open(const char *path,
                                                           const char *system_key,
                                                           char *err, size_t err_size);

// Stops the core, flushes the battery save and frees everything. Safe on NULL.
SPHYNX_PLAYER_EXPORT void sphynx_player_close(SphynxPlayerHandle *p);

// Runs exactly one emulated frame. Returns 1 while running, 0 when the core
// stopped on its own terms, -1 when it failed (see sphynx_player_status).
SPHYNX_PLAYER_EXPORT int sphynx_player_run(SphynxPlayerHandle *p);

// Copies the newest finished frame into `dst` (RGBA8888, top-down, tightly
// packed) and reports its size. `dst_size` must be at least w*h*4; the call is
// a no-op returning 0 when the buffer is too small or nothing new arrived, so
// the renderer can simply skip the upload and reuse the previous texture.
// Returns 1 when a frame was copied.
SPHYNX_PLAYER_EXPORT int sphynx_player_take_frame(SphynxPlayerHandle *p,
                                                 unsigned char *dst, int dst_size,
                                                 int *width, int *height);

// Pad state for the next frame (PPSSPP button bits + both sticks in -1..1).
SPHYNX_PLAYER_EXPORT void sphynx_player_set_pad(SphynxPlayerHandle *p, unsigned buttons,
                                                float lx, float ly, float rx, float ry);

// Save states. `slot` starts at 0. `msg` receives a human readable result.
// Returns 1 on success, 0 on failure.
SPHYNX_PLAYER_EXPORT int sphynx_player_save_state(SphynxPlayerHandle *p, int slot,
                                                  char *msg, size_t msg_size);
SPHYNX_PLAYER_EXPORT int sphynx_player_load_state(SphynxPlayerHandle *p, int slot,
                                                  char *msg, size_t msg_size);
// 1 when a state exists for this slot, 0 otherwise.
SPHYNX_PLAYER_EXPORT int sphynx_player_state_exists(SphynxPlayerHandle *p, int slot);

// Writes battery-backed save RAM to disk right away (also happens on close and
// every 20 seconds while playing, so a killed app loses at most a moment).
SPHYNX_PLAYER_EXPORT void sphynx_player_flush_save(SphynxPlayerHandle *p);

// Core frame rate in Hz (50 for PAL, 59.94 for NTSC, ...). 60 until known.
SPHYNX_PLAYER_EXPORT double sphynx_player_fps(SphynxPlayerHandle *p);

// One line for the on-screen status: system, resolution, fps.
SPHYNX_PLAYER_EXPORT const char *sphynx_player_status(SphynxPlayerHandle *p);
// Display name of the system that is running ("Super Nintendo", ...).
SPHYNX_PLAYER_EXPORT const char *sphynx_player_system_name(SphynxPlayerHandle *p);
// Which BIOS folder this game reads and what is missing from it.
SPHYNX_PLAYER_EXPORT const char *sphynx_player_bios_status(SphynxPlayerHandle *p);

// Sound on/off (default on). Applies immediately.
SPHYNX_PLAYER_EXPORT void sphynx_player_set_sound(SphynxPlayerHandle *p, int on);
// Fast forward: run up to `multiplier` frames per tick instead of one.
SPHYNX_PLAYER_EXPORT void sphynx_player_set_fast_forward(SphynxPlayerHandle *p, int multiplier);

#ifdef __cplusplus
}
#endif
