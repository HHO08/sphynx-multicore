// ============================================================================
//  Sphynx — extcores/src/player_android.cpp
//
//  The playback surface for external systems.
//
//  It runs in SphynxPlayerActivity, with its OWN GLSurfaceView and its OWN EGL
//  context — deliberately separate from PPSSPP's renderer. That is what makes
//  the "PSP must never be affected" rule structural instead of a promise: a bug
//  here can only break the external-core screen, never the PSP emulator.
//
//  Responsibilities:
//    * own the Session (core lifetime, watchdog, save states)
//    * upload the core's framebuffer as a texture and draw it with aspect fit
//    * feed a lock-protected audio ring that Java drains into an AudioTrack
//    * translate the on-screen/gamepad buttons into libretro joypad state
//
//  Compiled for Android only.
//
//  SPDX-License-Identifier: GPL-2.0-or-later
// ============================================================================
#include "sphynx_extcore.h"
// RetroPad button ids + device constants for the on-screen pad. The core ABI
// header is vendored in extcores/include so we never depend on the core source.
#include "libretro.h"
#include "android_paths.h"

#ifdef SPHYNX_ANDROID

#include <android/log.h>
#include <jni.h>

#include <GLES2/gl2.h>

#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#define PLOG(...) __android_log_print(ANDROID_LOG_INFO, "SphynxPlayer", __VA_ARGS__)
#define PERR(...) __android_log_print(ANDROID_LOG_ERROR, "SphynxPlayer", __VA_ARGS__)

namespace {

// --- state -----------------------------------------------------------------
// The session is owned through a shared_ptr so the render thread can keep
// running a frame while the UI thread asks for a stop: the object is destroyed
// only when the last frame finishes. A raw pointer here was a use-after-free
// waiting to happen (back button pressed mid-frame).
std::mutex g_session_mutex;
std::shared_ptr<sphynx::Session> g_session;
std::string g_error;

struct CaptureSink : sphynx::FrameSink {
	std::vector<uint8_t> pixels;   // converted to RGBA8888, ready for GL
	unsigned w = 0, h = 0;
	bool fresh = false;

	void OnGeometry(unsigned, unsigned, float) override {}

	void OnFrame(const void *data, unsigned width, unsigned height, size_t pitch,
	             sphynx::PixelFormat format) override {
		if (!data || !width || !height) return;
		w = width;
		h = height;
		pixels.resize(size_t(width) * height * 4);
		for (unsigned y = 0; y < height; ++y) {
			const uint8_t *row = static_cast<const uint8_t *>(data) + y * pitch;
			uint8_t *out = pixels.data() + size_t(y) * width * 4;
			if (format == sphynx::PixelFormat::RGB565) {
				const uint16_t *p = reinterpret_cast<const uint16_t *>(row);
				for (unsigned x = 0; x < width; ++x) {
					const uint16_t c = p[x];
					out[x * 4 + 0] = uint8_t(((c >> 11) & 0x1F) * 255 / 31);
					out[x * 4 + 1] = uint8_t(((c >> 5) & 0x3F) * 255 / 63);
					out[x * 4 + 2] = uint8_t((c & 0x1F) * 255 / 31);
					out[x * 4 + 3] = 255;
				}
			} else if (format == sphynx::PixelFormat::XRGB8888) {
				const uint32_t *p = reinterpret_cast<const uint32_t *>(row);
				for (unsigned x = 0; x < width; ++x) {
					const uint32_t c = p[x];
					out[x * 4 + 0] = uint8_t(c & 0xFF);
					out[x * 4 + 1] = uint8_t((c >> 8) & 0xFF);
					out[x * 4 + 2] = uint8_t((c >> 16) & 0xFF);
					out[x * 4 + 3] = 255;
				}
			} else {
				const uint16_t *p = reinterpret_cast<const uint16_t *>(row);
				for (unsigned x = 0; x < width; ++x) {
					const uint16_t c = p[x];
					out[x * 4 + 0] = uint8_t(((c >> 10) & 0x1F) * 255 / 31);
					out[x * 4 + 1] = uint8_t(((c >> 5) & 0x1F) * 255 / 31);
					out[x * 4 + 2] = uint8_t((c & 0x1F) * 255 / 31);
					out[x * 4 + 3] = 255;
				}
			}
		}
		fresh = true;
	}
};

CaptureSink g_capture;

// --- audio ring ------------------------------------------------------------
std::mutex g_audio_mutex;
std::vector<int16_t> g_audio_ring;   // interleaved stereo
size_t g_audio_read = 0, g_audio_write = 0, g_audio_count = 0;

struct AudioSink : sphynx::AudioSink {
	void OnAudio(const int16_t *frames, size_t frame_count) override {
		if (!frames || !frame_count) return;
		std::lock_guard<std::mutex> lock(g_audio_mutex);
		if (g_audio_ring.empty()) return;
		const size_t cap = g_audio_ring.size() / 2;
		for (size_t i = 0; i < frame_count; ++i) {
			if (g_audio_count >= cap) {
				// Drop the oldest frame rather than block the core thread.
				g_audio_read = (g_audio_read + 1) % cap;
				--g_audio_count;
			}
			g_audio_ring[g_audio_write * 2 + 0] = frames[i * 2 + 0];
			g_audio_ring[g_audio_write * 2 + 1] = frames[i * 2 + 1];
			g_audio_write = (g_audio_write + 1) % cap;
			++g_audio_count;
		}
	}
	int TargetSampleRate() const override { return 44100; }
};

AudioSink g_audio;

// --- input -----------------------------------------------------------------
std::mutex g_input_mutex;
int16_t g_buttons[16] = {0};

// --- GL --------------------------------------------------------------------
GLuint g_tex = 0, g_prog = 0, g_vbo = 0;
GLint g_a_pos = -1, g_a_uv = -1, g_u_tex = -1, g_u_fit = -1;
unsigned g_tex_w = 0, g_tex_h = 0;

const char *kVertex = R"(attribute vec2 a_pos;
attribute vec2 a_uv;
varying vec2 v_uv;
uniform vec4 u_fit;   // xy = scale, zw = offset (NDC)
void main() {
	v_uv = a_uv;
	gl_Position = vec4(a_pos * u_fit.xy + u_fit.zw, 0.0, 1.0);
})";

const char *kFragment = R"(precision mediump float;
varying vec2 v_uv;
uniform sampler2D u_tex;
void main() { gl_FragColor = texture2D(u_tex, v_uv); })";

GLuint CompileShader(GLenum type, const char *src) {
	GLuint s = glCreateShader(type);
	glShaderSource(s, 1, &src, nullptr);
	glCompileShader(s);
	GLint ok = 0;
	glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log[512];
		glGetShaderInfoLog(s, sizeof(log), nullptr, log);
		PERR("shader compile failed: %s", log);
		glDeleteShader(s);
		return 0;
	}
	return s;
}

bool InitGL() {
	if (g_prog) return true;
	GLuint vs = CompileShader(GL_VERTEX_SHADER, kVertex);
	GLuint fs = CompileShader(GL_FRAGMENT_SHADER, kFragment);
	if (!vs || !fs) return false;
	g_prog = glCreateProgram();
	glAttachShader(g_prog, vs);
	glAttachShader(g_prog, fs);
	glLinkProgram(g_prog);
	GLint ok = 0;
	glGetProgramiv(g_prog, GL_LINK_STATUS, &ok);
	if (!ok) {
		char log[512];
		glGetProgramInfoLog(g_prog, sizeof(log), nullptr, log);
		PERR("program link failed: %s", log);
		return false;
	}
	g_a_pos = glGetAttribLocation(g_prog, "a_pos");
	g_a_uv = glGetAttribLocation(g_prog, "a_uv");
	g_u_tex = glGetUniformLocation(g_prog, "u_tex");
	g_u_fit = glGetUniformLocation(g_prog, "u_fit");

	// Unit quad, centred, uv 0..1 with v flipped for GL texture orientation.
	static const GLfloat verts[] = {
		-0.5f, -0.5f, 0.0f, 1.0f,
		 0.5f, -0.5f, 1.0f, 1.0f,
		-0.5f,  0.5f, 0.0f, 0.0f,
		 0.5f,  0.5f, 1.0f, 0.0f,
	};
	glGenBuffers(1, &g_vbo);
	glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glGenTextures(1, &g_tex);
	glBindTexture(GL_TEXTURE_2D, g_tex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	PLOG("GL ready");
	return true;
}

void RenderFrame(int view_w, int view_h) {
	if (!InitGL()) return;
	glViewport(0, 0, view_w, view_h);
	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);

	if (g_capture.fresh && g_capture.w && g_capture.h) {
		glBindTexture(GL_TEXTURE_2D, g_tex);
		if (g_tex_w != g_capture.w || g_tex_h != g_capture.h) {
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, GLsizei(g_capture.w), GLsizei(g_capture.h),
			             0, GL_RGBA, GL_UNSIGNED_BYTE, g_capture.pixels.data());
			g_tex_w = g_capture.w;
			g_tex_h = g_capture.h;
		} else {
			glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, GLsizei(g_capture.w), GLsizei(g_capture.h),
			                GL_RGBA, GL_UNSIGNED_BYTE, g_capture.pixels.data());
		}
		g_capture.fresh = false;
	}

	if (!g_tex_w || !g_tex_h || !g_prog) return;

	// Aspect-fit inside the view (integer-free, keeps the picture undistorted).
	const float srcAspect = float(g_tex_w) / float(g_tex_h);
	const float dstAspect = float(view_w) / float(view_h);
	float sx = 1.0f, sy = 1.0f;
	if (srcAspect > dstAspect) {
		sy = dstAspect / srcAspect;
	} else {
		sx = srcAspect / dstAspect;
	}

	glUseProgram(g_prog);
	glUniform4f(g_u_fit, sx, sy, 0.0f, 0.0f);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, g_tex);
	glUniform1i(g_u_tex, 0);

	glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
	glEnableVertexAttribArray(GLuint(g_a_pos));
	glVertexAttribPointer(GLuint(g_a_pos), 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), (void *)0);
	glEnableVertexAttribArray(GLuint(g_a_uv));
	glVertexAttribPointer(GLuint(g_a_uv), 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat),
	                      (void *)(2 * sizeof(GLfloat)));
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	glDisableVertexAttribArray(GLuint(g_a_pos));
	glDisableVertexAttribArray(GLuint(g_a_uv));
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glUseProgram(0);
}

std::string JStr(JNIEnv *env, jstring s) {
	if (!s) return std::string();
	const char *c = env->GetStringUTFChars(s, nullptr);
	std::string out = c ? c : "";
	env->ReleaseStringUTFChars(s, c);
	return out;
}

}  // namespace

extern "C" {

JNIEXPORT jstring JNICALL
Java_org_sphynx_emulator_SphynxPlayer_nativeStart(JNIEnv *env, jclass, jstring jcore,
                                                  jstring jcontent, jstring jsys, jstring jsave) {
	const std::string core = JStr(env, jcore);
	const std::string requested = JStr(env, jcontent);
	const std::string sysdir = JStr(env, jsys);
	const std::string savedir = JStr(env, jsave);

	// Accept a content:// URI here as well: cores can only fopen() real files.
	// Java resolves before calling us, this is the belt-and-braces second path.
	std::string content = sphynx::LocalPathFor(requested);
	if (content.empty()) {
		g_error = "cannot read the game file (permission or unsupported storage)";
		PERR("%s: %s", g_error.c_str(), requested.c_str());
		return env->NewStringUTF(g_error.c_str());
	}

	{
		std::lock_guard<std::mutex> lock(g_session_mutex);
		g_session.reset();   // destroys the previous session once its frame ends
	}

	auto session = std::make_shared<sphynx::Session>();
	session->SetFrameSink(&g_capture);
	session->SetAudioSink(&g_audio);
	session->SetWatchdogMs(5000);
	session->SetInputSource([](unsigned port, unsigned device, unsigned, unsigned id) -> int16_t {
		if (port != 0 || device != RETRO_DEVICE_JOYPAD) return 0;
		if (id >= 16) return 0;
		std::lock_guard<std::mutex> lock(g_input_mutex);
		return g_buttons[id];
	});

	std::string err;
	if (!session->Start(core, content, sysdir, savedir, &err)) {
		PERR("start failed: %s (core=%s content=%s)", err.c_str(), core.c_str(), content.c_str());
		g_error = err;
		return env->NewStringUTF(err.c_str());
	}

	const int rate = int(session->SampleRate());
	{
		std::lock_guard<std::mutex> lock(g_audio_mutex);
		g_audio_ring.assign(size_t(rate > 8000 ? rate : 44100) * 2, 0);   // ~1 s stereo
		g_audio_read = g_audio_write = g_audio_count = 0;
	}
	g_capture.frames = 0;
	{
		std::lock_guard<std::mutex> lock(g_session_mutex);
		g_session = session;
	}
	PLOG("session started: core=%s content=%s %.0f Hz", core.c_str(), content.c_str(),
	     session->SampleRate());
	return nullptr;
}

JNIEXPORT jint JNICALL
Java_org_sphynx_emulator_SphynxPlayer_nativeFrame(JNIEnv *, jclass, jint w, jint h) {
	std::shared_ptr<sphynx::Session> session;
	{
		std::lock_guard<std::mutex> lock(g_session_mutex);
		session = g_session;   // keeps the session alive for the whole frame
	}
	if (!session) return -1;
	const bool ok = session->RunFrame();
	RenderFrame(int(w), int(h));
	return ok ? 0 : 1;
}

JNIEXPORT void JNICALL
Java_org_sphynx_emulator_SphynxPlayer_nativeStop(JNIEnv *, jclass) {
	std::shared_ptr<sphynx::Session> doomed;
	{
		std::lock_guard<std::mutex> lock(g_session_mutex);
		doomed.swap(g_session);
	}
	if (doomed) doomed->Stop();   // safe: the frame in flight holds a reference
	g_capture.pixels.clear();
	g_capture.fresh = false;
}

JNIEXPORT jint JNICALL
Java_org_sphynx_emulator_SphynxPlayer_nativeSampleRate(JNIEnv *, jclass) {
	std::lock_guard<std::mutex> lock(g_session_mutex);
	return g_session ? int(g_session->SampleRate()) : 44100;
}

JNIEXPORT jint JNICALL
Java_org_sphynx_emulator_SphynxPlayer_nativeReadAudio(JNIEnv *env, jclass, jshortArray out) {
	if (!out) return 0;
	const jsize want = env->GetArrayLength(out);
	if (want <= 0) return 0;
	static std::vector<int16_t> scratch;
	scratch.resize(size_t(want));
	size_t n = 0;
	{
		std::lock_guard<std::mutex> lock(g_audio_mutex);
		const size_t cap = g_audio_ring.empty() ? 0 : g_audio_ring.size() / 2;
		const size_t frames = size_t(want) / 2;
		while (n < frames && g_audio_count > 0 && cap > 0) {
			scratch[n * 2 + 0] = g_audio_ring[g_audio_read * 2 + 0];
			scratch[n * 2 + 1] = g_audio_ring[g_audio_read * 2 + 1];
			g_audio_read = (g_audio_read + 1) % cap;
			--g_audio_count;
			++n;
		}
	}
	if (n == 0) return 0;
	env->SetShortArrayRegion(out, 0, jsize(n * 2), scratch.data());
	return jint(n * 2);
}

JNIEXPORT void JNICALL
Java_org_sphynx_emulator_SphynxPlayer_nativeSetButton(JNIEnv *, jclass, jint id, jboolean down) {
	if (id < 0 || id >= 16) return;
	std::lock_guard<std::mutex> lock(g_input_mutex);
	g_buttons[id] = down ? 1 : 0;
}

JNIEXPORT void JNICALL
Java_org_sphynx_emulator_SphynxPlayer_nativeReleaseAll(JNIEnv *, jclass) {
	std::lock_guard<std::mutex> lock(g_input_mutex);
	for (auto &b : g_buttons) b = 0;
}

JNIEXPORT jstring JNICALL
Java_org_sphynx_emulator_SphynxPlayer_nativeStatus(JNIEnv *env, jclass) {
	std::shared_ptr<sphynx::Session> session;
	{
		std::lock_guard<std::mutex> lock(g_session_mutex);
		session = g_session;
	}
	if (!session) return env->NewStringUTF(g_error.c_str());
	char buf[256];
	snprintf(buf, sizeof(buf), "%.1f ms/frame (worst %.1f) | %llu frames",
	         session->Stats().last_frame_ms, session->Stats().worst_frame_ms,
	         (unsigned long long)session->Stats().frames);
	return env->NewStringUTF(buf);
}

}  // extern "C"

#endif  // SPHYNX_ANDROID
