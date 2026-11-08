// ============================================================================
//  Sphynx — extcores/src/hw_render.h
//
//  A small OpenGL ES context of our own, for the cores that cannot run without
//  one (stage 19).
//
//  Why this file exists
//  --------------------
//  N64 and Dreamcast are the two systems whose libretro cores are *hardware*
//  renderers: mupen64plus_next and flycast both ask the frontend for a GL
//  context with RETRO_ENVIRONMENT_SET_HW_RENDER inside retro_load_game(), and
//  when the frontend answers "no" they return false - which the player, quite
//  correctly, reported as "core refused this content (unsupported format or
//  missing BIOS)". The file, the BIOS and the core were all fine; the renderer
//  was missing.
//
//  What this is not
//  ----------------
//  It does not touch PPSSPP's renderer, its context or its thread. The core
//  draws into an off-screen framebuffer created here, that framebuffer is read
//  back once per frame into the same CPU frame the software cores already
//  produce, and from there nothing in the pipeline can tell the difference.
//  Every GL entry point is resolved with dlopen()/dlsym(), so the host tests
//  run the exact same code as the APK and no build configuration changes.
//
//  If anything here fails (no EGL, no GLES2, no framebuffer), Setup() returns
//  false and the caller answers the core exactly as before: a refusal with a
//  readable reason. Nothing else in the player changes.
//
//  SPDX-License-Identifier: GPL-2.0-or-later
// ============================================================================
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace sphynx {

class HwRender {
public:
	HwRender() = default;
	~HwRender();
	HwRender(const HwRender &) = delete;
	HwRender &operator=(const HwRender &) = delete;

	// Creates the EGL context and the off-screen framebuffer. `context_type` is
	// a RETRO_HW_CONTEXT_* value. Must be called on the thread that will run the
	// core, because the context is made current there.
	bool Setup(int context_type, unsigned version_major, unsigned version_minor,
	           bool want_depth, bool want_stencil, std::string *error);

	// True once Setup() succeeded and nothing has torn it down.
	bool Active() const { return context_ != nullptr; }

	// The framebuffer id handed to the core through get_current_framebuffer().
	unsigned Framebuffer() const { return fbo_; }

	// Resolved GL symbol, handed to the core through get_proc_address().
	void *ProcAddress(const char *name);

	// Grows the off-screen framebuffer when the core presents a bigger picture.
	bool EnsureSize(unsigned width, unsigned height);

	// Reads the framebuffer back into `rgba` as top-down RGBA8888.
	// `flip` is the core's bottom_left_origin flag: GL reads rows bottom-up, so
	// a bottom-left-origin core needs the rows reversed to become top-down.
	bool ReadPixels(unsigned width, unsigned height, bool flip, std::vector<uint8_t> *rgba,
	                std::string *error);

	// Makes the context current again (after a core reset or a context switch).
	bool MakeCurrent();

	// Tears everything down. Safe to call twice.
	void Destroy();

	const std::string &LastError() const { return error_; }

private:
	bool EnsureLibraryLoaded(std::string *error);
	bool CreateContext(int context_type, unsigned major, unsigned minor, std::string *error);
	bool CreateFramebuffer(unsigned width, unsigned height, bool depth, bool stencil,
	                       std::string *error);

	void *egl_ = nullptr;            // libEGL
	void *gles_ = nullptr;           // libGLESv2
	// EGL
	void *display_ = nullptr;
	void *context_ = nullptr;
	void *surface_ = nullptr;        // pbuffer fallback; null when surfaceless
	// GL objects
	unsigned fbo_ = 0;
	unsigned color_tex_ = 0;
	unsigned depth_rb_ = 0;
	unsigned width_ = 0;
	unsigned height_ = 0;
	bool has_depth_ = false;
	bool has_stencil_ = false;
	std::string error_;
};

}  // namespace sphynx
