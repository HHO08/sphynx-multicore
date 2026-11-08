// ============================================================================
//  Sphynx — extcores/src/hw_render.cpp
//
//  EGL + GLES2/3 context, off-screen framebuffer and read-back for the cores
//  that need a hardware renderer (stage 19). See hw_render.h for why.
//
//  Everything is resolved at run time with dlopen()/dlsym() so that
//     * the APK needs no new link dependency (libEGL/libGLESv2 are part of every
//       Android system image),
//     * the host test binary links the same object file and can therefore test
//       this path for real,
//     * a device without a usable EGL just takes the old refusal path instead
//       of failing to load the library.
//
//  SPDX-License-Identifier: GPL-2.0-or-later
// ============================================================================
#include "hw_render.h"

#include <dlfcn.h>

#include <cstring>

namespace sphynx {
namespace {

// --- EGL constants we need (declared locally: no EGL headers on the host) ----
constexpr int EGL_CONTEXT_CLIENT_VERSION_ = 0x3098;
constexpr int EGL_SURFACE_TYPE_ = 0x3033;
constexpr int EGL_PBUFFER_BIT_ = 0x0001;
constexpr int EGL_RENDERABLE_TYPE_ = 0x3040;
constexpr int EGL_OPENGL_ES2_BIT_ = 0x0004;
constexpr int EGL_OPENGL_ES3_BIT_ = 0x0040;
constexpr int EGL_NONE_ = 0x3038;
constexpr int EGL_WIDTH_ = 0x3057;
constexpr int EGL_HEIGHT_ = 0x3056;
constexpr int EGL_RED_SIZE_ = 0x3024;
constexpr int EGL_GREEN_SIZE_ = 0x3023;
constexpr int EGL_BLUE_SIZE_ = 0x3022;
constexpr int EGL_ALPHA_SIZE_ = 0x3021;
constexpr int EGL_DEPTH_SIZE_ = 0x3025;
constexpr int EGL_OPENGL_ES_API_ = 0x30A0;

// --- GL constants ----------------------------------------------------------
constexpr unsigned GL_FRAMEBUFFER_ = 0x8D40;
constexpr unsigned GL_RENDERBUFFER_ = 0x8D41;
constexpr unsigned GL_COLOR_ATTACHMENT0_ = 0x8CE0;
constexpr unsigned GL_DEPTH_ATTACHMENT_ = 0x8D00;
constexpr unsigned GL_STENCIL_ATTACHMENT_ = 0x8D20;
constexpr unsigned GL_DEPTH_STENCIL_ATTACHMENT_ = 0x821A;
constexpr unsigned GL_TEXTURE_2D_ = 0x0DE1;
constexpr unsigned GL_RGBA_ = 0x1908;
constexpr unsigned GL_UNSIGNED_BYTE_ = 0x1401;
constexpr unsigned GL_TEXTURE_MIN_FILTER_ = 0x2801;
constexpr unsigned GL_TEXTURE_MAG_FILTER_ = 0x2800;
constexpr unsigned GL_LINEAR_ = 0x2601;
constexpr unsigned GL_PACK_ALIGNMENT_ = 0x0D05;
constexpr unsigned GL_FRAMEBUFFER_COMPLETE_ = 0x8CD5;
constexpr unsigned GL_DEPTH_COMPONENT16_ = 0x81A5;
constexpr unsigned GL_DEPTH24_STENCIL8_ = 0x88F0;
constexpr unsigned GL_COLOR_BUFFER_BIT_ = 0x4000;
constexpr unsigned GL_DEPTH_BUFFER_BIT_ = 0x0100;

// --- the entry points we call ---------------------------------------------
using EGLGetProcAddressFn = void *(*)(const char *);
using EGLGetDisplayFn = void *(*)(void *);
using EGLInitializeFn = int (*)(void *, int *, int *);
using EGLChooseConfigFn = int (*)(void *, const int *, void **, int, int *);
using EGLCreateContextFn = void *(*)(void *, void *, void *, const int *);
using EGLCreatePbufferSurfaceFn = void *(*)(void *, void *, const int *);
using EGLMakeCurrentFn = int (*)(void *, void *, void *, void *);
using EGLBindAPIFn = int (*)(unsigned);
using EGLDestroySurfaceFn = int (*)(void *, void *);
using EGLDestroyContextFn = int (*)(void *, void *);
using EGLTerminateFn = int (*)(void *);

struct Api {
	EGLGetProcAddressFn eglGetProcAddress = nullptr;
	EGLGetDisplayFn eglGetDisplay = nullptr;
	EGLInitializeFn eglInitialize = nullptr;
	EGLChooseConfigFn eglChooseConfig = nullptr;
	EGLCreateContextFn eglCreateContext = nullptr;
	EGLCreatePbufferSurfaceFn eglCreatePbufferSurface = nullptr;
	EGLMakeCurrentFn eglMakeCurrent = nullptr;
	EGLBindAPIFn eglBindAPI = nullptr;
	EGLDestroySurfaceFn eglDestroySurface = nullptr;
	EGLDestroyContextFn eglDestroyContext = nullptr;
	EGLTerminateFn eglTerminate = nullptr;

	void (*glGenFramebuffers)(int, unsigned *) = nullptr;
	void (*glBindFramebuffer)(unsigned, unsigned) = nullptr;
	void (*glDeleteFramebuffers)(int, const unsigned *) = nullptr;
	void (*glFramebufferTexture2D)(unsigned, unsigned, unsigned, unsigned, int) = nullptr;
	void (*glGenTextures)(int, unsigned *) = nullptr;
	void (*glBindTexture)(unsigned, unsigned) = nullptr;
	void (*glDeleteTextures)(int, const unsigned *) = nullptr;
	void (*glTexImage2D)(unsigned, int, int, int, int, int, unsigned, unsigned, const void *) = nullptr;
	void (*glTexParameteri)(unsigned, unsigned, int) = nullptr;
	void (*glGenRenderbuffers)(int, unsigned *) = nullptr;
	void (*glBindRenderbuffer)(unsigned, unsigned) = nullptr;
	void (*glDeleteRenderbuffers)(int, const unsigned *) = nullptr;
	void (*glRenderbufferStorage)(unsigned, unsigned, int, int) = nullptr;
	void (*glFramebufferRenderbuffer)(unsigned, unsigned, unsigned, unsigned) = nullptr;
	unsigned (*glCheckFramebufferStatus)(unsigned) = nullptr;
	void (*glReadPixels)(int, int, int, int, unsigned, unsigned, void *) = nullptr;
	void (*glPixelStorei)(unsigned, int) = nullptr;
	void (*glClear)(unsigned) = nullptr;
	void (*glFinish)() = nullptr;
};

Api g_api;
bool g_api_ready = false;
bool g_api_failed = false;
std::string g_api_error;

template <typename T>
void Load(void *lib, const char *name, T *slot) {
	*slot = reinterpret_cast<T>(dlsym(lib, name));
}

// eglGetProcAddress() covers the GL entry points as well on Android and on
// Mesa; dlsym() on the GLES library is the fallback.
template <typename T>
void LoadGl(void *gles, const char *name, T *slot) {
	if (g_api.eglGetProcAddress) {
		*slot = reinterpret_cast<T>(g_api.eglGetProcAddress(name));
		if (*slot) return;
	}
	if (gles) *slot = reinterpret_cast<T>(dlsym(gles, name));
}

bool LoadApi(std::string *error) {
	if (g_api_ready) return true;
	if (g_api_failed) {
		if (error) *error = g_api_error;
		return false;
	}
	// Android exposes the plain names; a Linux host (the tests) ships versioned
	// ones. Both are tried, in that order, so no build flag is involved.
	void *egl = dlopen("libEGL.so", RTLD_NOW);
	if (!egl) egl = dlopen("libEGL.so.1", RTLD_NOW);
	if (!egl) {
		g_api_failed = true;
		g_api_error = "this device has no EGL library";
		if (error) *error = g_api_error;
		return false;
	}
	void *gles = dlopen("libGLESv2.so", RTLD_NOW);
	if (!gles) gles = dlopen("libGLESv2.so.2", RTLD_NOW);

	Load(egl, "eglGetProcAddress", &g_api.eglGetProcAddress);
	Load(egl, "eglGetDisplay", &g_api.eglGetDisplay);
	Load(egl, "eglInitialize", &g_api.eglInitialize);
	Load(egl, "eglChooseConfig", &g_api.eglChooseConfig);
	Load(egl, "eglCreateContext", &g_api.eglCreateContext);
	Load(egl, "eglCreatePbufferSurface", &g_api.eglCreatePbufferSurface);
	Load(egl, "eglMakeCurrent", &g_api.eglMakeCurrent);
	Load(egl, "eglBindAPI", &g_api.eglBindAPI);
	Load(egl, "eglDestroySurface", &g_api.eglDestroySurface);
	Load(egl, "eglDestroyContext", &g_api.eglDestroyContext);
	Load(egl, "eglTerminate", &g_api.eglTerminate);

	if (!g_api.eglGetDisplay || !g_api.eglInitialize || !g_api.eglChooseConfig ||
	    !g_api.eglCreateContext || !g_api.eglMakeCurrent) {
		g_api_failed = true;
		g_api_error = "the EGL library is incomplete";
		if (error) *error = g_api_error;
		return false;
	}

	LoadGl(gles, "glGenFramebuffers", &g_api.glGenFramebuffers);
	LoadGl(gles, "glBindFramebuffer", &g_api.glBindFramebuffer);
	LoadGl(gles, "glDeleteFramebuffers", &g_api.glDeleteFramebuffers);
	LoadGl(gles, "glFramebufferTexture2D", &g_api.glFramebufferTexture2D);
	LoadGl(gles, "glGenTextures", &g_api.glGenTextures);
	LoadGl(gles, "glBindTexture", &g_api.glBindTexture);
	LoadGl(gles, "glDeleteTextures", &g_api.glDeleteTextures);
	LoadGl(gles, "glTexImage2D", &g_api.glTexImage2D);
	LoadGl(gles, "glTexParameteri", &g_api.glTexParameteri);
	LoadGl(gles, "glGenRenderbuffers", &g_api.glGenRenderbuffers);
	LoadGl(gles, "glBindRenderbuffer", &g_api.glBindRenderbuffer);
	LoadGl(gles, "glDeleteRenderbuffers", &g_api.glDeleteRenderbuffers);
	LoadGl(gles, "glRenderbufferStorage", &g_api.glRenderbufferStorage);
	LoadGl(gles, "glFramebufferRenderbuffer", &g_api.glFramebufferRenderbuffer);
	LoadGl(gles, "glCheckFramebufferStatus", &g_api.glCheckFramebufferStatus);
	LoadGl(gles, "glReadPixels", &g_api.glReadPixels);
	LoadGl(gles, "glPixelStorei", &g_api.glPixelStorei);
	LoadGl(gles, "glClear", &g_api.glClear);
	LoadGl(gles, "glFinish", &g_api.glFinish);

	const bool complete = g_api.glGenFramebuffers && g_api.glBindFramebuffer &&
	                      g_api.glFramebufferTexture2D && g_api.glGenTextures &&
	                      g_api.glBindTexture && g_api.glTexImage2D &&
	                      g_api.glFramebufferRenderbuffer && g_api.glGenRenderbuffers &&
	                      g_api.glRenderbufferStorage && g_api.glCheckFramebufferStatus &&
	                      g_api.glReadPixels;
	if (!complete) {
		g_api_failed = true;
		g_api_error = "this device has no OpenGL ES framebuffer support";
		if (error) *error = g_api_error;
		return false;
	}
	g_api_ready = true;
	return true;
}

}  // namespace

HwRender::~HwRender() { Destroy(); }

bool HwRender::Setup(int context_type, unsigned version_major, unsigned version_minor,
                     bool want_depth, bool want_stencil, std::string *error) {
	Destroy();
	if (!EnsureLibraryLoaded(error)) return false;
	if (!CreateContext(context_type, version_major, version_minor, error)) {
		Destroy();
		return false;
	}
	// 1024x768 covers N64 (640x480) and Dreamcast (640x480), with room for the
	// cores that raise their resolution; EnsureSize() grows it if one goes
	// bigger. Memory is only paid when a hardware core is actually started.
	if (!CreateFramebuffer(1024, 768, want_depth, want_stencil, error)) {
		Destroy();
		return false;
	}
	return true;
}

bool HwRender::EnsureLibraryLoaded(std::string *error) {
	if (g_api_ready) return true;
	if (!LoadApi(error)) {
		error_ = error ? *error : "no graphics interface";
		return false;
	}
	return true;
}

bool HwRender::CreateContext(int context_type, unsigned major, unsigned minor,
                             std::string *error) {
	// Only OpenGL ES exists on Android, and OpenGL ES is what these cores ask
	// for on this ABI (mupen64plus_next_gles2, flycast). A Vulkan request cannot
	// be honoured by a GL context at all, so it is refused rather than answered
	// with something the core would crash on; the core then keeps its refusal,
	// which the player reports honestly. Anything else is a GL request and is
	// served with the ES version below.
	if (context_type == 7 /* RETRO_HW_CONTEXT_VULKAN */) {
		error_ = "this build cannot provide a Vulkan context";
		if (error) *error = error_;
		return false;
	}
	unsigned es_version = 2;
	if (context_type == 4 /* RETRO_HW_CONTEXT_OPENGLES3 */ || major >= 3 || minor >= 3)
		es_version = 3;

	display_ = g_api.eglGetDisplay(reinterpret_cast<void *>(0));
	if (!display_) {
		error_ = "EGL could not open a display";
		if (error) *error = error_;
		return false;
	}
	int emaj = 0, emin = 0;
	if (!g_api.eglInitialize(display_, &emaj, &emin)) {
		error_ = "EGL could not be initialised";
		if (error) *error = error_;
		display_ = nullptr;
		return false;
	}
	if (g_api.eglBindAPI) g_api.eglBindAPI(EGL_OPENGL_ES_API_);

	const int renderable = es_version >= 3 ? EGL_OPENGL_ES3_BIT_ : EGL_OPENGL_ES2_BIT_;
	const int config_attribs[] = {
		EGL_RENDERABLE_TYPE_, renderable,
		EGL_SURFACE_TYPE_, EGL_PBUFFER_BIT_,
		EGL_RED_SIZE_, 5,
		EGL_GREEN_SIZE_, 6,
		EGL_BLUE_SIZE_, 5,
		EGL_ALPHA_SIZE_, 0,
		EGL_DEPTH_SIZE_, 16,
		EGL_NONE_,
	};
	void *config = nullptr;
	int config_count = 0;
	if (!g_api.eglChooseConfig(display_, config_attribs, &config, 1, &config_count) ||
	    config_count < 1) {
		// A device may offer no pbuffer config; retry without the surface-type
		// restriction before giving up.
		const int fallback_attribs[] = {
			EGL_RENDERABLE_TYPE_, EGL_OPENGL_ES2_BIT_,
			EGL_RED_SIZE_, 5, EGL_GREEN_SIZE_, 6, EGL_BLUE_SIZE_, 5,
			EGL_NONE_,
		};
		if (!g_api.eglChooseConfig(display_, fallback_attribs, &config, 1, &config_count) ||
		    config_count < 1) {
			error_ = "this device offers no usable EGL configuration";
			if (error) *error = error_;
			g_api.eglTerminate(display_);
			display_ = nullptr;
			return false;
		}
	}

	int ctx_attribs[] = {EGL_CONTEXT_CLIENT_VERSION_, static_cast<int>(es_version), EGL_NONE_};
	context_ = g_api.eglCreateContext(display_, config, nullptr, ctx_attribs);
	if (!context_ && es_version == 3) {
		// A device without OpenGL ES 3.0 still plays these games: the cores can
		// run on ES2, so ask for that instead of giving up on the system.
		ctx_attribs[1] = 2;
		context_ = g_api.eglCreateContext(display_, config, nullptr, ctx_attribs);
	}
	if (!context_) {
		error_ = "an OpenGL ES context could not be created";
		if (error) *error = error_;
		g_api.eglTerminate(display_);
		display_ = nullptr;
		return false;
	}

	// Surfaceless first (no window is involved anywhere in this player); a 1x1
	// pbuffer is the fallback for drivers that insist on a surface.
	if (!g_api.eglMakeCurrent(display_, nullptr, nullptr, context_)) {
		if (g_api.eglCreatePbufferSurface) {
			const int pb_attribs[] = {EGL_WIDTH_, 1, EGL_HEIGHT_, 1, EGL_NONE_};
			surface_ = g_api.eglCreatePbufferSurface(display_, config, pb_attribs);
		}
		if (!surface_ || !g_api.eglMakeCurrent(display_, surface_, surface_, context_)) {
			error_ = "the OpenGL ES context could not be made current";
			if (error) *error = error_;
			if (surface_ && g_api.eglDestroySurface) g_api.eglDestroySurface(display_, surface_);
			surface_ = nullptr;
			g_api.eglDestroyContext(display_, context_);
			context_ = nullptr;
			g_api.eglTerminate(display_);
			display_ = nullptr;
			return false;
		}
	}
	return true;
}

bool HwRender::CreateFramebuffer(unsigned width, unsigned height, bool depth, bool stencil,
                                 std::string *error) {
	if (!g_api_ready || !context_) {
		error_ = "no graphics context";
		if (error) *error = error_;
		return false;
	}
	if (width < 64) width = 64;
	if (height < 64) height = 64;

	if (fbo_) {
		g_api.glDeleteFramebuffers(1, &fbo_);
		fbo_ = 0;
	}
	if (color_tex_) {
		g_api.glDeleteTextures(1, &color_tex_);
		color_tex_ = 0;
	}
	if (depth_rb_) {
		g_api.glDeleteRenderbuffers(1, &depth_rb_);
		depth_rb_ = 0;
	}

	g_api.glGenTextures(1, &color_tex_);
	g_api.glBindTexture(GL_TEXTURE_2D_, color_tex_);
	g_api.glTexImage2D(GL_TEXTURE_2D_, 0, static_cast<int>(GL_RGBA_), static_cast<int>(width),
	                   static_cast<int>(height), 0, GL_RGBA_, GL_UNSIGNED_BYTE_, nullptr);
	g_api.glTexParameteri(GL_TEXTURE_2D_, GL_TEXTURE_MIN_FILTER_, static_cast<int>(GL_LINEAR_));
	g_api.glTexParameteri(GL_TEXTURE_2D_, GL_TEXTURE_MAG_FILTER_, static_cast<int>(GL_LINEAR_));

	g_api.glGenFramebuffers(1, &fbo_);
	g_api.glBindFramebuffer(GL_FRAMEBUFFER_, fbo_);
	g_api.glFramebufferTexture2D(GL_FRAMEBUFFER_, GL_COLOR_ATTACHMENT0_, GL_TEXTURE_2D_, color_tex_, 0);

	if (depth || stencil) {
		g_api.glGenRenderbuffers(1, &depth_rb_);
		g_api.glBindRenderbuffer(GL_RENDERBUFFER_, depth_rb_);
		g_api.glRenderbufferStorage(GL_RENDERBUFFER_,
		                            stencil ? GL_DEPTH24_STENCIL8_ : GL_DEPTH_COMPONENT16_,
		                            static_cast<int>(width), static_cast<int>(height));
		g_api.glFramebufferRenderbuffer(GL_FRAMEBUFFER_, GL_DEPTH_ATTACHMENT_, GL_RENDERBUFFER_, depth_rb_);
		if (stencil)
			g_api.glFramebufferRenderbuffer(GL_FRAMEBUFFER_, GL_STENCIL_ATTACHMENT_, GL_RENDERBUFFER_, depth_rb_);
	}

	if (g_api.glCheckFramebufferStatus(GL_FRAMEBUFFER_) != GL_FRAMEBUFFER_COMPLETE_) {
		error_ = "the off-screen framebuffer could not be created";
		if (error) *error = error_;
		return false;
	}
	// A defined starting picture: never show uninitialised memory.
	g_api.glClear((depth || stencil) ? (GL_COLOR_BUFFER_BIT_ | GL_DEPTH_BUFFER_BIT_) : GL_COLOR_BUFFER_BIT_);
	g_api.glBindFramebuffer(GL_FRAMEBUFFER_, 0);

	width_ = width;
	height_ = height;
	has_depth_ = depth || stencil;
	has_stencil_ = stencil;
	return true;
}

bool HwRender::EnsureSize(unsigned width, unsigned height) {
	if (!fbo_) return false;
	if (width <= width_ && height <= height_) return true;
	// Grow to a comfortable size once rather than reallocating every frame.
	unsigned want_w = width_ * 2, want_h = height_ * 2;
	while (want_w < width) want_w *= 2;
	while (want_h < height) want_h *= 2;
	return CreateFramebuffer(want_w, want_h, has_depth_, has_stencil_, nullptr);
}

void *HwRender::ProcAddress(const char *name) {
	if (!g_api_ready || !name) return nullptr;
	if (g_api.eglGetProcAddress) {
		if (void *p = g_api.eglGetProcAddress(name)) return p;
	}
	return dlsym(RTLD_DEFAULT, name);
}

bool HwRender::MakeCurrent() {
	if (!g_api_ready || !display_ || !context_) return false;
	return g_api.eglMakeCurrent(display_, surface_, surface_, context_) != 0;
}

bool HwRender::ReadPixels(unsigned width, unsigned height, bool flip,
                          std::vector<uint8_t> *rgba, std::string *error) {
	if (!fbo_ || !g_api_ready) {
		error_ = "no framebuffer to read from";
		if (error) *error = error_;
		return false;
	}
	if (!width || !height) return false;
	if (!EnsureSize(width, height)) {
		if (error) *error = error_.empty() ? "the framebuffer could not be resized" : error_;
		return false;
	}
	rgba->resize(static_cast<size_t>(width) * height * 4u);

	g_api.glBindFramebuffer(GL_FRAMEBUFFER_, fbo_);
	if (g_api.glPixelStorei) g_api.glPixelStorei(GL_PACK_ALIGNMENT_, 1);
	// glReadPixels() returns rows bottom-up; `flip` says whether that is
	// upside-down for this core (bottom_left_origin).
	const size_t stride = static_cast<size_t>(width) * 4u;
	if (!flip) {
		g_api.glReadPixels(0, 0, static_cast<int>(width), static_cast<int>(height),
		                   GL_RGBA_, GL_UNSIGNED_BYTE_, rgba->data());
	} else {
		std::vector<uint8_t> tmp(stride * height);
		g_api.glReadPixels(0, 0, static_cast<int>(width), static_cast<int>(height),
		                   GL_RGBA_, GL_UNSIGNED_BYTE_, tmp.data());
		for (unsigned y = 0; y < height; ++y) {
			std::memcpy(rgba->data() + static_cast<size_t>(y) * stride,
			            tmp.data() + static_cast<size_t>(height - 1 - y) * stride, stride);
		}
	}
	if (g_api.glFinish) g_api.glFinish();
	g_api.glBindFramebuffer(GL_FRAMEBUFFER_, 0);
	// The core drew with alpha it rarely fills in; the player's texture wants
	// opaque pixels, exactly like the software path.
	for (size_t i = 3; i < rgba->size(); i += 4) (*rgba)[i] = 0xFF;
	return true;
}

void HwRender::Destroy() {
	if (g_api_ready && context_) {
		if (fbo_) { g_api.glDeleteFramebuffers(1, &fbo_); fbo_ = 0; }
		if (color_tex_) { g_api.glDeleteTextures(1, &color_tex_); color_tex_ = 0; }
		if (depth_rb_) { g_api.glDeleteRenderbuffers(1, &depth_rb_); depth_rb_ = 0; }
		if (display_) g_api.eglMakeCurrent(display_, nullptr, nullptr, nullptr);
	}
	if (g_api_ready && display_ && surface_ && g_api.eglDestroySurface)
		g_api.eglDestroySurface(display_, surface_);
	surface_ = nullptr;
	if (g_api_ready && display_ && context_ && g_api.eglDestroyContext)
		g_api.eglDestroyContext(display_, context_);
	context_ = nullptr;
	if (g_api_ready && display_ && g_api.eglTerminate) g_api.eglTerminate(display_);
	display_ = nullptr;
	width_ = height_ = 0;
	has_depth_ = has_stencil_ = false;
}

}  // namespace sphynx
