// ============================================================================
//  Sphynx — extcores/src/player_core.cpp
//
//  One running external core, exposed as the plain-C API in
//  sphynx_player_api.h. PPSSPP's own UI (UI/SphynxPlayerScreen.cpp) is the only
//  caller.
//
//  Why a new file instead of the old Android player
//  ------------------------------------------------
//  The stage-6 player lived in a separate Activity with its own GLSurfaceView,
//  its own EGL context and its own Java AudioTrack. Launching any game therefore
//  paused PPSSPP's Activity and started a second one; when the core or the
//  surface had the slightest problem the user was thrown back to a restarted
//  PPSSPP with a black screen - the bug reported on the device.
//
//  This file keeps only what was good (session handling, per-core save states,
//  battery saves, BIOS folders) and leaves pixels, input and audio routing to
//  PPSSPP itself. Nothing here touches a surface, a window or the PSP core.
//
//  Threading: everything is called from PPSSPP's UI thread. The only other
//  thread is the audio device's, and it only ever reads the ring buffer.
//
//  SPDX-License-Identifier: GPL-2.0-or-later
// ============================================================================
#include "sphynx_player_api.h"

// Local mkdir -p for the core-options file.
#include <dirent.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>

#include "sphynx_extcore.h"
#include "sphynx_ui_api.h"
#include "android_paths.h"
#include "bios_layout.h"
#include "core_defaults.h"
#include "diag_log.h"
#include "hw_render.h"
#include "player_audio.h"
#include "state_file.h"

#include "libretro.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <csetjmp>
#include <csignal>
#include <cstdlib>
#include <new>
#include <thread>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace {

// libretro only has one active environment per process (see env_callback.cpp),
// so exactly one external core may run at a time.
//
// SPHYNX (stage 15): this used to be a plain bool, and a player that was never
// closed - a screen replaced while its game was still open, an exit path that
// skipped the destructor - left it set forever. Every later game then refused
// to start with "another game is already running", which is exactly the message
// the owner photographed, and only restarting the app cleared it.
//
// It is now the active handle itself. Opening a game *repairs* the situation:
// whatever was left behind is stopped and marked dead instead of blocking the
// new game. A handle is only freed by its own sphynx_player_close(), so the
// screen that still points at it never sees a dangling pointer.
std::mutex g_one_core_mutex;
std::atomic<SphynxPlayerHandle *> g_active_handle{nullptr};

void StopWorker(SphynxPlayerHandle *p);   // defined below, needs the layout

// ---------------------------------------------------------------------------
//  A core that dies must not take the app with it (stage 18)
//
//  "the app ejects me when I tap this game" on a handful of systems is the
//  process ending: the game list the owner lands on is a restarted app, not a
//  deliberate exit. Two things make that happen with a libretro core:
//
//   1. Stack. A default pthread gets 1 MB here. Several of the bundled cores are
//      desktop builds with deep call chains (DOSBox, ScummVM, PUAE, FBNeo,
//      the mednafen cores) and overflow that 1 MB on the first frames: the
//      guard page turns it into SIGSEGV and the whole process is gone.
//   2. The core itself faulting. A wrong guess at what the file is (a .bin that
//      is not a Mega Drive ROM, a .cue the core cannot parse) can make it write
//      through a null pointer instead of returning an error, and then there is
//      no process left to show a message in.
//
//  So the emulation thread now gets a real stack (CoreStackBytes) and every
//  call into the core is armed with a landing point. If the core faults anyway,
//  the thread lands back here, the crashed core is dropped untouched, and the
//  next system that claims the file is tried - which is how the few systems
//  that were "ejecting" become systems that just work, and how the rest get an
//  honest message on the game screen instead of the app closing.
//
//  Nothing here changes what the player draws, how the buttons work, or which
//  core runs a game.
// ---------------------------------------------------------------------------
constexpr size_t CoreStackBytes = 32u * 1024u * 1024u;   // desktop cores need it

struct CoreFaultGuard {
	jmp_buf env;
	volatile sig_atomic_t armed = 0;
	volatile sig_atomic_t signo = 0;
};

// One core at a time runs in this process (see g_one_core_mutex), so one
// landing point and one alternate signal stack are enough.
thread_local CoreFaultGuard g_fault;
thread_local bool g_fault_stack_ready = false;
char *g_fault_stack_mem = nullptr;

const char *SignalName(int sig) {
	switch (sig) {
	case SIGSEGV: return "a bad memory access";
	case SIGBUS:  return "a bus error";
	case SIGILL:  return "an illegal instruction";
	case SIGFPE:  return "a math fault";
	case SIGABRT: return "an abort inside the core";
	default:      return "a fatal signal";
	}
}

// Runs on the alternate stack, so it also survives a stack overflow.
void CoreFaultHandler(int sig, siginfo_t *, void *) {
	if (g_fault.armed) {
		g_fault.armed = 0;
		g_fault.signo = sig;
		// The signal is blocked while this handler runs; unblock it by hand,
		// because a plain longjmp() does not restore the signal mask. Without
		// this a second fault on the same thread could not be caught.
		sigset_t only;
		sigemptyset(&only);
		sigaddset(&only, sig);
		sigprocmask(SIG_UNBLOCK, &only, nullptr);
		longjmp(g_fault.env, 1);   // never returns
	}
	// A fault we did not arm: behave exactly as the system would have.
	struct sigaction sa;
	std::memset(&sa, 0, sizeof(sa));
	sa.sa_handler = SIG_DFL;
	sigemptyset(&sa.sa_mask);
	sigaction(sig, &sa, nullptr);
	raise(sig);
}

void InstallCoreFaultHandlers() {
	static std::once_flag once;
	std::call_once(once, [] {
		struct sigaction sa;
		std::memset(&sa, 0, sizeof(sa));
		sa.sa_sigaction = &CoreFaultHandler;
		sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
		sigemptyset(&sa.sa_mask);
		const int signals[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT};
		for (int sig : signals) sigaction(sig, &sa, nullptr);
	});
}

// Prepared on the emulation thread, before it touches a core.
void PrepareCoreFaultStack() {
	if (g_fault_stack_ready) return;
	if (!g_fault_stack_mem) g_fault_stack_mem = static_cast<char *>(std::malloc(64u * 1024u));
	if (!g_fault_stack_mem) return;
	stack_t ss;
	std::memset(&ss, 0, sizeof(ss));
	ss.ss_sp = g_fault_stack_mem;
	ss.ss_size = 64u * 1024u;
	ss.ss_flags = 0;
	if (sigaltstack(&ss, nullptr) == 0) g_fault_stack_ready = true;
}

std::string BaseNameOf(const std::string &path) {
	const size_t slash = path.find_last_of("/\\");
	return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string StripExtension(const std::string &name) {
	const size_t dot = name.find_last_of('.');
	return (dot == std::string::npos || dot == 0) ? name : name.substr(0, dot);
}

// "libsphynx_core_gambatte.so" -> "gambatte". Stored inside every state file so
// a state can never be loaded into the wrong core.
std::string CoreTagFromPath(const std::string &core_path) {
	std::string base = StripExtension(BaseNameOf(core_path));
	const std::string prefix = "libsphynx_core_";
	if (base.compare(0, prefix.size(), prefix) == 0) base = base.substr(prefix.size());
	return base;
}

// A file name safe on any filesystem (it ends up in a path).
std::string SafeFileName(const std::string &name) {
	std::string out;
	for (char c : name) {
		switch (c) {
		case '/': case '\\': case ':': case '*': case '?': case '"': case '<': case '>': case '|':
		case '\n': case '\r': case '\t':
			out += '_';
			break;
		default:
			out += c;
		}
	}
	if (out.empty()) out = "game";
	return out.size() > 96 ? out.substr(0, 96) : out;
}

// Turns whatever the core hands us into tightly packed RGBA8888, which is the
// only format PPSSPP's UpdateTextureLevels() accepts without a conversion step
// in the renderer.
// 65536 entries of "RGB565 -> RGBA8888", built once on first use. 256 KB of
// read-only data is a cheap price for removing six operations per pixel.
const uint32_t *RgbaFrom565Table() {
	static uint32_t table[65536];
	static bool built = false;
	if (!built) {
		for (uint32_t c = 0; c < 65536; ++c) {
			const uint32_t r = ((c >> 11) & 0x1F) << 3;
			const uint32_t g = ((c >> 5) & 0x3F) << 2;
			const uint32_t b = (c & 0x1F) << 3;
			table[c] = 0xFF000000u | (b << 16) | (g << 8) | r;
		}
		built = true;
	}
	return table;
}

class FrameCapture : public sphynx::FrameSink {
public:
	void OnGeometry(unsigned, unsigned, float) override {}

	void OnFrame(const void *data, unsigned width, unsigned height, size_t pitch,
	             sphynx::PixelFormat format) override {
		if (!data || !width || !height) return;
		std::lock_guard<std::mutex> lock(mutex_);
		const size_t bytes = static_cast<size_t>(width) * height * 4;
		if (pixels_.size() != bytes) pixels_.resize(bytes);
		width_ = static_cast<int>(width);
		height_ = static_cast<int>(height);
		// SPHYNX (performance, stage 13): the per-pixel work here was the single
		// most expensive thing in the whole player - three shifts and four byte
		// stores per pixel, sixty times a second, on the same thread that runs the
		// emulated CPU. Two changes, both invisible in the result:
		//   * RGB565 goes through a 64K lookup table, so a pixel costs one load
		//     and one 32-bit store;
		//   * XRGB8888 is a byte swap inside a 32-bit word (three bit
		//     operations) instead of four byte stores through an index.
		uint8_t *dst = pixels_.data();
		if (format == sphynx::PixelFormat::RGB565) {
			const uint32_t *lut = RgbaFrom565Table();
			for (unsigned y = 0; y < height; ++y) {
				const uint16_t *src = reinterpret_cast<const uint16_t *>(
					static_cast<const uint8_t *>(data) + y * pitch);
				uint32_t *out = reinterpret_cast<uint32_t *>(dst + static_cast<size_t>(y) * width * 4);
				for (unsigned x = 0; x < width; ++x) out[x] = lut[src[x]];
			}
		} else {
			// XRGB8888 in memory is B,G,R,X; RGBA wants R,G,B,255. Swapping the
			// first and third byte of every word is exactly that, and ORing in
			// 0xFF000000 restores the alpha the core leaves as padding.
			for (unsigned y = 0; y < height; ++y) {
				const uint8_t *row = static_cast<const uint8_t *>(data) + y * pitch;
				uint32_t *out = reinterpret_cast<uint32_t *>(dst + static_cast<size_t>(y) * width * 4);
				for (unsigned x = 0; x < width; ++x) {
					uint32_t w = 0;
					std::memcpy(&w, row + static_cast<size_t>(x) * 4, 4);
					w = 0xFF000000u | ((w & 0x000000FFu) << 16) | (w & 0x0000FF00u) |
					    ((w & 0x00FF0000u) >> 16);
					out[x] = w;
				}
			}
		}
		fresh_ = true;
	}

	// SPHYNX (stage 19): a hardware-rendering core announces a frame without any
	// pixels - it drew into the framebuffer the frontend gave it. The request is
	// remembered here and fulfilled by HardwareFrameReady() as soon as the frame
	// has been read back, so the rest of the player sees an ordinary frame.
	void OnHardwareFrame(unsigned width, unsigned height) override {
		std::lock_guard<std::mutex> lock(mutex_);
		if (!width || !height) return;
		hw_width_ = static_cast<int>(width);
		hw_height_ = static_cast<int>(height);
		hw_pending_ = true;
	}

	// Takes the pending hardware frame request (width/height). False when the
	// core produced no new frame since the last call.
	bool TakeHardwareFrame(int *width, int *height) {
		std::lock_guard<std::mutex> lock(mutex_);
		if (!hw_pending_) return false;
		hw_pending_ = false;
		*width = hw_width_;
		*height = hw_height_;
		return true;
	}

	// Publishes a frame read back from the core's framebuffer. The buffer is
	// already RGBA8888 in the layout the texture upload expects, which is what
	// glReadPixels() produced.
	void HardwareFrameReady(const uint8_t *rgba, int width, int height) {
		if (!rgba || width <= 0 || height <= 0) return;
		std::lock_guard<std::mutex> lock(mutex_);
		const size_t bytes = static_cast<size_t>(width) * static_cast<size_t>(height) * 4u;
		if (pixels_.size() != bytes) pixels_.resize(bytes);
		std::memcpy(pixels_.data(), rgba, bytes);
		width_ = width;
		height_ = height;
		fresh_ = true;
	}

	// SPHYNX (performance, stage 13): the frame used to be copied into a fresh
	// std::vector on every frame - up to 4 MB of memcpy plus an allocation and a
	// free at 60 fps. That is a large part of the stutter, and of the audio
	// dropouts that come with it. It now hands back a pointer to the buffer it
	// already owns; the caller copies straight into the texture upload buffer.
	const uint8_t *Take(int *w, int *h, int *bytes) {
		std::lock_guard<std::mutex> lock(mutex_);
		if (!fresh_ || pixels_.empty()) return nullptr;
		*w = width_;
		*h = height_;
		*bytes = static_cast<int>(pixels_.size());
		fresh_ = false;
		return pixels_.data();
	}

	// Size of the frame that is waiting (0x0 when nothing new). No copy, so the
	// player can size its destination buffer *before* calling Take(): Take()
	// consumes the frame, so a buffer that is too small silently loses it and
	// the screen stays black for every core that renders bigger than the guess.
	void Peek(int *w, int *h) const {
		std::lock_guard<std::mutex> lock(mutex_);
		if (!fresh_) {
			*w = 0;
			*h = 0;
			return;
		}
		*w = width_;
		*h = height_;
	}

	int width() const { return width_; }
	int height() const { return height_; }

private:
	mutable std::mutex mutex_;
	std::vector<uint8_t> pixels_;
	int width_ = 0;
	int height_ = 0;
	bool fresh_ = false;
	// Hardware frames (stage 19): the request the core announced, waiting for
	// the read-back that turns it into an ordinary frame.
	bool hw_pending_ = false;
	int hw_width_ = 0, hw_height_ = 0;
};

class AudioPush : public sphynx::AudioSink {
public:
	explicit AudioPush(sphynx::AudioOutput *out) : out_(out) {}
	void OnAudio(const int16_t *frames, size_t frame_count) override {
		if (out_) out_->Push(frames, frame_count);
	}
	int TargetSampleRate() const override { return 0; }   // follow the core

private:
	sphynx::AudioOutput *out_ = nullptr;
};

}  // namespace

// ---------------------------------------------------------------------------
//  The hardware renderer the player owns (stage 19)
//
//  Created by the core's own SET_HW_RENDER request, on the thread that runs the
//  core, and never touched from anywhere else. It carries the framebuffer id
//  and the symbol lookup the core needs, plus the orientation the core asked
//  for so the read-back is the right way up.
// ---------------------------------------------------------------------------
class PlayerHardwareRender : public sphynx::HardwareRenderHost {
public:
	// The context is created on first use, so a device that never runs a
	// hardware core (every software system) pays nothing for this.
	bool SetupHardwareRender(int context_type, unsigned version_major, unsigned version_minor,
	                         bool depth, bool stencil, bool *bottom_left_origin) override {
		if (!render_) render_.reset(new sphynx::HwRender());
		std::string error;
		if (!render_->Setup(context_type, version_major, version_minor, depth, stencil, &error)) {
			sphynx::LogLine("player: the hardware renderer could not start: " + error);
			active_ = false;
			return false;
		}
		active_ = true;
		// A GL core draws with the bottom-left origin, which is the way up a
		// read-back comes back; every core that says otherwise is honoured.
		flip_ = true;
		if (bottom_left_origin) *bottom_left_origin = flip_;
		sphynx::LogLine(std::string("player: hardware renderer ready (OpenGL ES ") +
		                std::to_string(version_major >= 3 ? 3 : 2) + ")");
		return true;
	}

	void *HardwareRenderProcAddress(const char *name) override {
		return render_ ? render_->ProcAddress(name) : nullptr;
	}

	uintptr_t HardwareRenderFramebuffer() override {
		return render_ ? static_cast<uintptr_t>(render_->Framebuffer()) : 0;
	}

	bool Active() const { return active_ && render_ && render_->Active(); }
	bool Flip() const { return flip_; }
	sphynx::HwRender *Renderer() { return render_.get(); }

	// Reads the frame the core just drew into the picture the player shows.
	bool ReadFrame(unsigned width, unsigned height, std::vector<uint8_t> *rgba) {
		if (!Active()) return false;
		if (!render_->MakeCurrent()) return false;
		std::string error;
		if (!render_->ReadPixels(width, height, flip_, rgba, &error)) {
			sphynx::LogLine("player: the hardware frame could not be read back: " + error);
			return false;
		}
		return true;
	}

	void Destroy() {
		if (render_) render_->Destroy();
		active_ = false;
	}

private:
	std::unique_ptr<sphynx::HwRender> render_;
	bool active_ = false;
	bool flip_ = true;
};

// The handle PPSSPP holds. Declared in the header as an opaque forward
// declaration, defined here so the layout stays private.
struct SphynxPlayerHandle {
	sphynx::Session session;
	FrameCapture frame;
	// The hardware renderer, only for the cores that ask for one (stage 19).
	std::unique_ptr<PlayerHardwareRender> hw;
	std::vector<uint8_t> hw_frame;      // read-back buffer, reused every frame
	std::unique_ptr<sphynx::AudioOutput> audio_out;
	std::unique_ptr<AudioPush> audio_push;

	std::string system_key;
	std::string system_name;
	std::string save_dir;
	std::string sram_path;
	std::string game_tag;
	std::string core_tag;
	std::string status;
	std::string error;
	std::string bios_status;
	// Detailed "which file is really in the folder" report (stage 15). It is
	// what the player shows when a core starts but never draws a picture.
	std::string bios_report;

	std::atomic<unsigned> buttons{0};
	std::atomic<float> lx{0.0f}, ly{0.0f}, rx{0.0f}, ry{0.0f};
	std::atomic<int> fast_forward{1};
	// Written by the UI thread, read by the emulation thread: atomic, not bool.
	std::atomic<bool> sound{true};

	// SPHYNX (stage 15): set once the emulation thread has been asked to leave
	// and has returned. A dead handle answers every call with "stopped" but is
	// still a valid pointer until its owner calls sphynx_player_close().
	std::atomic<bool> dead{false};
	// Serialises shutdown, so a reclaim from one thread and the owner's own
	// close from another cannot both join() the same thread.
	std::mutex close_mutex;

	double fps = 60.0;
	unsigned frame_w = 0, frame_h = 0;
	uint64_t frames_run = 0;
	double last_save_check = 0.0;

	// ------------------------------------------------------------------
	//  The core runs on its own thread (stage 14)
	//
	//  Until now retro_run() was called from PPSSPP's UI update, on the same
	//  thread that draws the buttons and reacts to touches. One slow emulated
	//  frame therefore froze the whole interface: the picture stuttered, the pad
	//  felt unresponsive and the audio queue underran (clicks). Every serious
	//  frontend, including V5, keeps emulation off the UI thread; this is that
	//  split, done with the smallest possible surface.
	// ------------------------------------------------------------------
	// SPHYNX (stage 18): a plain pthread instead of std::thread, because the
	// stack size has to be chosen (CoreStackBytes, not the 1 MB default).
	pthread_t worker{};
	bool worker_valid = false;
	std::atomic<bool> quit{false};
	std::atomic<bool> paused{false};
	std::atomic<bool> started{false};
	std::atomic<bool> failed{false};
	// SPHYNX (stage 18): set once the core has really delivered a few frames.
	// A core that faults on its first frame is a wrong guess at what the file
	// is, and open() uses this to move on to the next system instead of handing
	// the user a game that dies one second after he taps it.
	std::atomic<bool> warm{false};
	// Serialises everything that touches the core: the frame loop and the
	// infrequent save/load/option calls from the UI thread.
	std::mutex session_mutex;
	// Start-up handshake: open() waits here until the core is really running.
	std::mutex start_mutex;
	std::condition_variable start_cv;
	std::string start_error;
	// Measured, not guessed: frames per second the core actually delivers, and
	// how long one emulated frame takes. Both are shown on screen.
	std::atomic<double> measured_fps{0.0};
	std::atomic<double> frame_ms{0.0};
	// One flattened option line, reused so sphynx_player_option() can return a
	// const char* without allocating (see sphynx_player_api.h).
	std::string option_line_cache;
};

namespace {

// SPHYNX (stage 15): stops the emulation thread and marks the handle dead.
// Idempotent, and safe to call from the UI thread at any time - the worker
// saves the battery RAM and stops the core itself before it returns, so this is
// the only correct way to end a session.
void StopWorker(SphynxPlayerHandle *p) {
	if (!p) return;
	std::lock_guard<std::mutex> lock(p->close_mutex);
	p->quit = true;
	if (p->worker_valid) {
		pthread_join(p->worker, nullptr);
		p->worker_valid = false;
	}
	p->dead = true;
}

// ---------------------------------------------------------------------------
//  Why a core refuses a file (stage 17)
//
//  "core refused this content (unsupported format or missing BIOS)" is true for
//  every possible cause, which makes it useless: the owner saw it on many
//  systems and there was no way to tell a missing disc track from a missing
//  BIOS from a format the core does not read. The two helpers below find the
//  most common causes *before* the core is asked, so the message can name them.
// ---------------------------------------------------------------------------

bool FileReadable(const std::string &path) {
	return !path.empty() && ::access(path.c_str(), R_OK) == 0;
}

long long SizeOfFile(const std::string &path) {
	struct stat st;
	if (::stat(path.c_str(), &st) != 0) return -1;
	return static_cast<long long>(st.st_size);
}

std::string HumanSize(long long bytes) {
	if (bytes < 0) return "?";
	char buf[48];
	if (bytes < 1024)
		std::snprintf(buf, sizeof(buf), "%lld B", bytes);
	else if (bytes < 1024 * 1024)
		std::snprintf(buf, sizeof(buf), "%lld KiB", (bytes + 512) / 1024);
	else
		std::snprintf(buf, sizeof(buf), "%.1f MiB", static_cast<double>(bytes) / (1024.0 * 1024.0));
	return buf;
}

std::string ExtensionOf(const std::string &path) {
	const size_t slash = path.find_last_of('/');
	const size_t dot = path.find_last_of('.');
	if (dot == std::string::npos) return std::string();
	if (slash != std::string::npos && dot < slash) return std::string();
	std::string ext = path.substr(dot + 1);
	for (char &c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	return ext;
}

std::string DirectoryOf(const std::string &path) {
	const size_t slash = path.find_last_of('/');
	return slash == std::string::npos ? std::string(".") : path.substr(0, slash);
}

// Does `name` exist inside `dir`, allowing for the capital letters the copy
// tools of the various file managers produce?
bool ExistsInDir(const std::string &dir, const std::string &name) {
	if (FileReadable(dir + "/" + name)) return true;
	DIR *d = ::opendir(dir.c_str());
	if (!d) return false;
	bool found = false;
	while (struct dirent *entry = ::readdir(d)) {
		const std::string candidate = entry->d_name;
		if (candidate.size() != name.size()) continue;
		bool same = true;
		for (size_t i = 0; i < name.size() && same; ++i)
			same = std::tolower(static_cast<unsigned char>(candidate[i])) ==
			       std::tolower(static_cast<unsigned char>(name[i]));
		if (same) {
			found = true;
			break;
		}
	}
	::closedir(d);
	return found;
}

// A .cue sheet names its tracks; a .gdi names its data tracks. When one of those
// files is not next to the sheet - the usual result of copying only the small
// file through the storage picker - every core refuses the disc, and the reason
// "unsupported format or missing BIOS" is simply wrong.
std::string MissingTrackReason(const std::string &local) {
	const std::string ext = ExtensionOf(local);
	const bool cue = ext == "cue";
	const bool gdi = ext == "gdi" || ext == "m3u";
	if (!cue && !gdi) return std::string();
	FILE *f = std::fopen(local.c_str(), "rb");
	if (!f) return std::string();
	const std::string dir = DirectoryOf(local);
	std::string missing;
	char line[512];
	while (std::fgets(line, sizeof(line), f)) {
		std::string text = line;
		std::string name;
		if (cue) {
			// FILE "name.bin" BINARY   (or without quotes)
			const size_t at = text.find("FILE");
			if (at == std::string::npos) continue;
			size_t begin = text.find('"', at);
			size_t end = std::string::npos;
			if (begin != std::string::npos) {
				end = text.find('"', begin + 1);
				if (end == std::string::npos) continue;
				name = text.substr(begin + 1, end - begin - 1);
			} else {
				// unquoted: everything between FILE and the track type
				std::string rest = text.substr(at + 6);
				while (!rest.empty() && rest.front() == ' ') rest.erase(rest.begin());
				const size_t stop = rest.find_last_of(' ');
				name = stop == std::string::npos ? rest : rest.substr(0, stop);
			}
		} else {
			// GDI: line, LBA, type, sector size, "file", offset  -> 4th field
			int field = 0;
			size_t pos = 0;
			while (pos < text.size()) {
				const size_t space = text.find(' ', pos);
				const std::string token = text.substr(pos, space == std::string::npos ? std::string::npos : space - pos);
				if (++field == 5) {
					name = token;
					break;
				}
				if (space == std::string::npos) break;
				pos = space + 1;
			}
			if (!name.empty() && name.front() == '"') name.erase(name.begin());
			if (!name.empty() && name.back() == '"') name.pop_back();
			if (!name.empty() && name.front() == '"') name.erase(name.begin());
		}
		while (!name.empty() && (name.back() == '\r' || name.back() == '\n' || name.back() == ' '))
			name.pop_back();
		// Absolute and Windows paths in old sheets: only the file name is useful.
		const size_t slash = name.find_last_of("/\\");
		if (slash != std::string::npos) name = name.substr(slash + 1);
		if (name.empty()) continue;
		if (ExistsInDir(dir, name)) continue;
		if (!missing.empty()) missing += ", ";
		missing += name;
	}
	std::fclose(f);
	if (missing.empty()) return std::string();
	return "the disc image(s) next to it are missing: " + missing;
}

// PPSSPP pad bits -> libretro RetroPad ids. This is the standard mapping every
// frontend uses: the pad's X (cross) is the primary action (B), circle is (A).
int16_t PadIdForBit(unsigned bit) {
	switch (bit) {
	case SPHYNX_PAD_CROSS:    return RETRO_DEVICE_ID_JOYPAD_B;       // 0
	case SPHYNX_PAD_CIRCLE:   return RETRO_DEVICE_ID_JOYPAD_A;       // 8
	case SPHYNX_PAD_SQUARE:   return RETRO_DEVICE_ID_JOYPAD_Y;       // 1
	case SPHYNX_PAD_TRIANGLE: return RETRO_DEVICE_ID_JOYPAD_X;       // 9
	case SPHYNX_PAD_UP:       return RETRO_DEVICE_ID_JOYPAD_UP;      // 4
	case SPHYNX_PAD_DOWN:     return RETRO_DEVICE_ID_JOYPAD_DOWN;    // 5
	case SPHYNX_PAD_LEFT:     return RETRO_DEVICE_ID_JOYPAD_LEFT;    // 6
	case SPHYNX_PAD_RIGHT:    return RETRO_DEVICE_ID_JOYPAD_RIGHT;   // 7
	case SPHYNX_PAD_START:    return RETRO_DEVICE_ID_JOYPAD_START;   // 3
	case SPHYNX_PAD_SELECT:   return RETRO_DEVICE_ID_JOYPAD_SELECT;  // 2
	case SPHYNX_PAD_LTRIGGER: return RETRO_DEVICE_ID_JOYPAD_L;       // 10
	case SPHYNX_PAD_RTRIGGER: return RETRO_DEVICE_ID_JOYPAD_R;       // 11
	case SPHYNX_PAD_L2:       return RETRO_DEVICE_ID_JOYPAD_L2;      // 12
	case SPHYNX_PAD_R2:       return RETRO_DEVICE_ID_JOYPAD_R2;      // 13
	case SPHYNX_PAD_L3:       return RETRO_DEVICE_ID_JOYPAD_L3;      // 14
	case SPHYNX_PAD_R3:       return RETRO_DEVICE_ID_JOYPAD_R3;      // 15
	default:                  return -1;
	}
}

int16_t ToAxis(float v) {
	if (v > 1.0f) v = 1.0f;
	if (v < -1.0f) v = -1.0f;
	return static_cast<int16_t>(v * 32767.0f);
}

// Copies the core's battery RAM to disk. Called on close, on pause and every
// 20 seconds, so a killed app loses at most a few seconds of play.
void FlushBatterySave(SphynxPlayerHandle *p) {
	if (!p || p->sram_path.empty()) return;
	size_t size = 0;
	void *data = p->session.MemoryData(RETRO_MEMORY_SAVE_RAM, &size);
	if (!data || size == 0) return;
	FILE *f = fopen(p->sram_path.c_str(), "wb");
	if (!f) return;
	fwrite(data, 1, size, f);
	fclose(f);
}

// Battery saves are raw images, not snapshots: they are simply copied back.
void LoadBatterySave(SphynxPlayerHandle *p) {
	if (!p || p->sram_path.empty()) return;
	size_t size = 0;
	void *data = p->session.MemoryData(RETRO_MEMORY_SAVE_RAM, &size);
	if (!data || size == 0) return;
	FILE *f = fopen(p->sram_path.c_str(), "rb");
	if (!f) return;
	const size_t got = fread(data, 1, size, f);
	fclose(f);
	(void)got;
}

// ---------------------------------------------------------------------------
//  Per-system core options on disk (stage 13)
//
//  <save_dir>/sphynx_core_options/<system>.ini, one "key=value" per line. Plain
//  text, so an update never loses the choices and deleting a line returns that
//  single option to the core's own default.
// ---------------------------------------------------------------------------
void EnsureDirectoryExists(const std::string &path) {
	if (path.empty()) return;
	struct stat st;
	if (::stat(path.c_str(), &st) == 0) return;
	const size_t slash = path.find_last_of('/');
	if (slash != std::string::npos && slash > 0) EnsureDirectoryExists(path.substr(0, slash));
	::mkdir(path.c_str(), 0777);
}

std::string OptionsFilePath(const SphynxPlayerHandle *p) {
	if (!p || p->save_dir.empty()) return std::string();
	return p->save_dir + "/sphynx_core_options/" +
	       (p->system_key.empty() ? "unknown" : p->system_key) + ".ini";
}

std::vector<std::pair<std::string, std::string>> LoadOptions(const std::string &path) {
	std::vector<std::pair<std::string, std::string>> out;
	if (path.empty()) return out;
	FILE *f = fopen(path.c_str(), "rb");
	if (!f) return out;
	char line[512];
	while (fgets(line, sizeof(line), f)) {
		std::string text(line);
		while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
		const size_t eq = text.find('=');
		if (eq == std::string::npos || eq == 0) continue;
		out.emplace_back(text.substr(0, eq), text.substr(eq + 1));
	}
	fclose(f);
	return out;
}

void SaveOptions(const std::string &path, const std::vector<sphynx::CoreOption> &options) {
	if (path.empty()) return;
	const size_t slash = path.find_last_of('/');
	if (slash != std::string::npos) EnsureDirectoryExists(path.substr(0, slash));
	FILE *f = fopen(path.c_str(), "wb");
	if (!f) return;
	for (const auto &option : options)
		fprintf(f, "%s=%s\n", option.key.c_str(), option.current.c_str());
	fclose(f);
}

// "key|label|current|v1;v2;v3" — see sphynx_player_api.h for the shape.
std::string FlattenOption(const sphynx::CoreOption &option) {
	std::string out = option.key;
	out += '|';
	out += option.label;
	out += '|';
	out += option.current;
	out += '|';
	for (size_t i = 0; i < option.values.size(); ++i) {
		if (i) out += ';';
		out += option.values[i];
	}
	return out;
}

void SetStatus(SphynxPlayerHandle *p) {
	char buf[192];
	std::snprintf(buf, sizeof(buf), "%s · %ux%u · %.2f fps%s", p->system_name.c_str(),
	              p->frame_w, p->frame_h, p->fps,
	              p->session.Stats().watchdog_trips ? " · slow" : "");
	p->status = buf;
}

// ---------------------------------------------------------------------------
//  The emulation thread
//
//  Everything the libretro ABI requires to happen on "the core thread" happens
//  here: retro_init/retro_load_game (inside Session::Start), retro_run, the
//  battery-save flush and retro_deinit. The frame pacing mirrors what V5 does -
//  one emulated frame per display frame - but on a thread of its own, so a slow
//  frame can never stall PPSSPP's interface or the audio queue.
// ---------------------------------------------------------------------------
// One guarded frame. The setjmp()/longjmp() pair lives entirely inside this
// function, so no variable of the caller is live across the landing point (that
// is also what keeps the compiler's -Wclobbered quiet). The session mutex is
// taken and released by hand: jumping out of a lock_guard would leave it locked
// and freeze the UI thread that also uses it.
bool RunFrameGuarded(SphynxPlayerHandle *p, int *crash_signal) {
	volatile sig_atomic_t caught = 0;
	volatile bool ok = false;
	p->session_mutex.lock();
	g_fault.armed = 1;
	if (setjmp(g_fault.env) == 0) {
		ok = p->session.RunFrame();
		g_fault.armed = 0;
	} else {
		caught = g_fault.signo;
	}
	p->session_mutex.unlock();
	if (caught) {
		if (crash_signal) *crash_signal = caught;
		return false;
	}
	return ok;
}

// Shutdown, guarded the same way: a core that faults inside deinit() must not
// close the app the user is still looking at.
void StopSessionGuarded(SphynxPlayerHandle *p) {
	p->session_mutex.lock();
	g_fault.armed = 1;
	if (setjmp(g_fault.env) == 0) {
		FlushBatterySave(p);
		// SPHYNX (stage 19): a hardware core releases its GL objects here, while
		// the context still exists, then the context itself goes.
		if (p->session.HardwareRenderActive()) p->session.NotifyHardwareContextDestroy();
		p->session.Stop();
		g_fault.armed = 0;
	} else {
		g_fault.armed = 0;
		sphynx::LogLine("player: the emulator faulted while shutting down");
		p->session.AbandonCore();
	}
	p->session_mutex.unlock();
	if (p->hw) p->hw->Destroy();
}

void PlayerWorker(SphynxPlayerHandle *p, std::string core_path, std::string content,
                  std::string system_dir) {
	const std::string save_dir = p->save_dir;

	std::string error;
	// SPHYNX (stage 18): the core is entered behind a landing point, so a core
	// that faults while loading the game ends this attempt instead of the app.
	PrepareCoreFaultStack();
	InstallCoreFaultHandlers();
	volatile sig_atomic_t crash_signal = 0;
	volatile bool start_ok = false;
	g_fault.armed = 1;
	if (setjmp(g_fault.env) == 0) {
		start_ok = p->session.Start(core_path, content, system_dir, save_dir, &error);
		g_fault.armed = 0;
	} else {
		crash_signal = g_fault.signo;
	}
	if (crash_signal) {
		// The crashed core is left completely alone: unload_game() and deinit()
		// are calls back into the code that just faulted.
		p->session.AbandonCore();
		if (p->hw) p->hw->Destroy();
		sphynx::LogLine(std::string("player: ") + p->system_name + " crashed while starting (" +
		                SignalName(crash_signal) + ")");
		{
			std::lock_guard<std::mutex> lock(p->start_mutex);
			p->start_error = std::string("its emulator crashed while starting (") +
			                 SignalName(crash_signal) + ")";
		}
		p->failed = true;
		p->started = true;
		p->start_cv.notify_all();
		return;
	}
	if (!start_ok) {
		{
			std::lock_guard<std::mutex> lock(p->start_mutex);
			p->start_error = error.empty() ? "the emulator refused this game" : error;
		}
		p->failed = true;
		p->started = true;
		p->start_cv.notify_all();
		return;
	}

	double rate = p->session.FrameRate();
	if (rate <= 20.0 || rate > 200.0) rate = 60.0;
	const double frame_time = 1.0 / rate;

	// The core's battery RAM is read here, on the core thread, immediately after
	// the game is loaded and before the user can save anything.
	LoadBatterySave(p);

	// SPHYNX (stage 16, launch speed): the game is reported as started *before*
	// the sound device is opened. Opening OpenSL means the Android audio HAL
	// comes up, and on some devices that takes a noticeable slice of a second -
	// it used to be spent before the first picture could appear, which is what
	// made "tap a game" feel slow. The audio device has nothing to do with the
	// first frame, so the screen is released first and the device follows.
	{
		std::lock_guard<std::mutex> lock(p->start_mutex);
		p->started = true;
	}
	p->start_cv.notify_all();

	bool device_on = false;
	if (!p->sound.load(std::memory_order_relaxed)) {
		// Sound is off: do not open the device at all (and do not pay for it).
	} else if (!p->audio_out->Start(p->session.SampleRate())) {
		// Silent playback is a fine outcome; refusing to play is not.
		sphynx::LogLine("player: audio device could not be opened, continuing silently");
	} else {
		device_on = true;
	}

	using Clock = std::chrono::steady_clock;
	auto next_frame = Clock::now();
	auto fps_window = Clock::now();
	unsigned frames_in_window = 0;
	double ms_sum = 0.0;
	unsigned ms_count = 0;
	double save_timer = 0.0;

	while (!p->quit.load(std::memory_order_relaxed)) {
		if (p->paused.load(std::memory_order_relaxed)) {
			// Paused (a menu is open): keep the thread parked and cheap. The audio
			// queue is left to drain, which is what stops the "menu music keeps
			// playing one second after the pause" effect.
			std::this_thread::sleep_for(std::chrono::milliseconds(8));
			next_frame = Clock::now();
			fps_window = Clock::now();
			frames_in_window = 0;
			continue;
		}

		// SPHYNX (stage 15 audio): sound on/off is applied here, on the thread
		// that owns the device (see sphynx_player_set_sound). Switching it off
		// stops the device so nothing is drained in the background; switching it
		// on starts it again at the core's own sample rate.
		{
			const bool want = p->sound.load(std::memory_order_relaxed);
			if (want != device_on) {
				if (want) {
					device_on = p->audio_out->Start(p->session.SampleRate());
					if (!device_on) sphynx::LogLine("player: audio device could not be reopened");
				} else {
					p->audio_out->Stop();
					device_on = false;
				}
			}
		}

		// Fast-forward multiplies how many emulated frames one tick runs.
		const int loops = std::max(1, p->fast_forward.load(std::memory_order_relaxed));
		for (int i = 0; i < loops && !p->quit.load(std::memory_order_relaxed); ++i) {
			const auto t0 = Clock::now();
			// SPHYNX (stage 18): one guarded frame (see RunFrameGuarded).
			int frame_crash = 0;
			const bool ok = RunFrameGuarded(p, &frame_crash);
			if (frame_crash) {
				// The core is gone: drop it untouched and tell the screen the
				// truth. No process death, no silent ejection.
				p->session.AbandonCore();
				if (p->hw) p->hw->Destroy();
				p->error = std::string("the emulator crashed (") + SignalName(frame_crash) + ")";
				sphynx::LogLine(std::string("player: ") + p->system_name + ": " + p->error);
				p->failed = true;
				p->quit = true;
				p->start_cv.notify_all();
				break;
			}
			if (ok) ++p->frames_run;
			// SPHYNX (stage 19): a hardware core leaves its picture in the
			// framebuffer instead of returning pixels, so it is fetched here -
			// on the core's thread, with the core's context current - and handed
			// to the same frame queue the software cores fill.
			{
				int hw_w = 0, hw_h = 0;
				if (p->hw && p->frame.TakeHardwareFrame(&hw_w, &hw_h)) {
					if (p->hw->ReadFrame(static_cast<unsigned>(hw_w), static_cast<unsigned>(hw_h),
					                     &p->hw_frame)) {
						p->frame.HardwareFrameReady(p->hw_frame.data(), hw_w, hw_h);
					}
				}
			}
			// A core that has produced a handful of frames is really running.
			if (ok && !p->warm.load(std::memory_order_relaxed) && p->frames_run >= 5) {
				{
					std::lock_guard<std::mutex> lock(p->start_mutex);
					p->warm = true;
				}
				p->start_cv.notify_all();
			}
			const double ms =
				std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
			ms_sum += ms;
			++ms_count;
			if (!ok) {
				p->error = p->session.LastError();
				if (p->error.empty()) p->error = "the emulator stopped";
				sphynx::LogLine(std::string("player: stopped: ") + p->error);
				p->failed = true;
				p->quit = true;
				p->start_cv.notify_all();
				break;
			}
			// Battery save every 20 s, on this thread (it touches core memory).
			save_timer += frame_time * loops;
			if (save_timer > 20.0) {
				save_timer = 0.0;
				std::lock_guard<std::mutex> lock(p->session_mutex);
				FlushBatterySave(p);
			}
		}

		++frames_in_window;
		p->frame_w = static_cast<unsigned>(p->frame.width());
		p->frame_h = static_cast<unsigned>(p->frame.height());

		const auto now = Clock::now();
		const double window = std::chrono::duration<double>(now - fps_window).count();
		if (window >= 0.5) {
			p->measured_fps = frames_in_window / window;
			if (ms_count) p->frame_ms = ms_sum / ms_count;
			frames_in_window = 0;
			ms_sum = 0.0;
			ms_count = 0;
			fps_window = now;
			SetStatus(p);
		}

		// SPHYNX (stage 15 audio): follow the audio clock as well as the frame
		// clock. A 60 Hz picture and a 44100 Hz device are two independent
		// clocks: pacing on the frame clock alone means the queue slowly fills
		// or slowly empties, and a minute into the game that turns into the
		// crackle/pop the owner reported on most cores. Watching how much audio
		// is queued and bending the frame period by up to 3% (16.7 ms -> 16.2
		// or 17.2 ms, invisible) keeps the two clocks together with no
		// resampling and no dropped samples.
		double speed = 1.0;
		if (p->sound.load(std::memory_order_relaxed) && p->audio_out &&
		    p->audio_out->IsRunning()) {
			const size_t queued = p->audio_out->BufferedFrames();
			const double rate = p->session.SampleRate();
			if (rate > 1000.0) {
				const double queued_ms = static_cast<double>(queued) * 1000.0 / rate;
				if (queued_ms > 90.0) speed = 0.97;        // too much queued: slow down
				else if (queued_ms < 20.0) speed = 1.03;   // about to starve: speed up
			}
		}

		// Sleep until the next frame is due. A core that is slower than real time
		// simply never sleeps - it is not our job to drop its frames here.
		next_frame += std::chrono::duration_cast<Clock::duration>(
			std::chrono::duration<double>(frame_time * loops / speed));
		const auto after = Clock::now();
		if (next_frame > after) {
			std::this_thread::sleep_until(next_frame);
		} else if (after - next_frame > std::chrono::milliseconds(250)) {
			// Too far behind to catch up: resynchronise instead of spinning.
			next_frame = after;
		}
	}

	// Leave cleanly: battery save first, then the core, then the audio device.
	// SPHYNX (stage 18): even the shutdown is guarded - a core that faults
	// inside deinit() must not close the app the user is still looking at.
	if (p->session.IsRunning()) {
		StopSessionGuarded(p);
	} else if (!p->failed.load()) {
		std::lock_guard<std::mutex> lock(p->session_mutex);
		FlushBatterySave(p);
	}
	if (p->audio_out) p->audio_out->Stop();
}

// SPHYNX (stage 18): what the emulation thread needs to start, owned by the
// thread itself so a failed pthread_create() cannot leak it either.
struct WorkerArgs {
	SphynxPlayerHandle *handle = nullptr;
	std::string core_path;
	std::string content;
	std::string system_dir;
};

void *PlayerWorkerEntry(void *raw) {
	std::unique_ptr<WorkerArgs> args(static_cast<WorkerArgs *>(raw));
	PlayerWorker(args->handle, args->core_path, args->content, args->system_dir);
	return nullptr;
}

}  // namespace

extern "C" {

SphynxPlayerHandle *sphynx_player_open(const char *path, const char *system_key,
                                      char *err, size_t err_size) {
	auto fail = [&](const char *message) -> SphynxPlayerHandle * {
		if (err && err_size) {
			std::snprintf(err, err_size, "%s", message);
		}
		sphynx::LogLine(std::string("player: ") + message);
		return nullptr;
	};

	if (!path || !*path) return fail("no game file was given");

	std::lock_guard<std::mutex> one_core(g_one_core_mutex);
	// SPHYNX (stage 15): no more "another game is already running". A core left
	// behind by a screen that never closed it is stopped here and marked dead,
	// and the game the user just tapped starts normally. The old handle keeps
	// its memory until its own close() runs, so no pointer goes stale.
	if (SphynxPlayerHandle *stale = g_active_handle.load()) {
		sphynx::LogLine("player: stopping a previous core that was left open (self-repair)");
		StopWorker(stale);
		SphynxPlayerHandle *expected = stale;
		g_active_handle.compare_exchange_strong(expected, nullptr);
	}

	// 1. A core can only fopen() a real file, so an Android content:// URI has
	//    to become one first. This is the step that made every SAF-picked game
	//    fail before.
	const std::string local = sphynx::LocalPathFor(path);
	if (local.empty()) return fail("this file cannot be opened (the storage URI did not resolve)");

	// 2. Which systems can play this file?
	//
	// SPHYNX (stage 17): this used to pick one system and give up when that
	// core refused the file - which is what "core refused this content
	// (unsupported format or missing BIOS)" was: a guess that nobody checked.
	// A .cue/.chd/.bin is claimed by several systems at once, so the host now
	// tries every system that claims the file, in the detector's order, and
	// stops at the first that really starts. Nothing about the player, the
	// buttons or the picture changes; a wrong guess is simply corrected.
	const sphynx::DetectResult detected = sphynx::Detect(local, nullptr, 0);
	std::vector<std::string> keys;
	{
		const std::string named = (system_key && *system_key) ? std::string(system_key) : std::string();
		if (!named.empty()) keys.push_back(named);
		for (const auto &candidate : detected.candidates) {
			const sphynx::SystemInfo *info = sphynx::FindSystem(candidate.id);
			if (!info || !info->key) continue;
			if (std::find(keys.begin(), keys.end(), std::string(info->key)) != keys.end()) continue;
			keys.push_back(info->key);
		}
	}
	if (keys.empty()) {
		if (detected.is_psp_native) return fail("this is a PSP game: PPSSPP plays it itself");
		return fail("this file was not recognised as a game");
	}
	// A PSP disc is PPSSPP's own business: only an explicitly named system makes
	// this layer look at it (that is the "PSP loader refused it" path).
	if (detected.is_psp_native && !(system_key && *system_key))
		return fail("this is a PSP game: PPSSPP plays it itself");

	// What the file itself looks like - used in the message when every system
	// refuses it, and to name the usual mistakes (empty file, missing track).
	const std::string file_ext = ExtensionOf(local);
	const long long file_size = SizeOfFile(local);
	const std::string track_problem = MissingTrackReason(local);
	std::string file_problem;
	if (file_size == 0) file_problem = "the file is empty (0 bytes)";
	else if (file_size < 0) file_problem = "the file cannot be read";
	else if (!track_problem.empty()) file_problem = track_problem;

	// One attempt: build the handle for `key` and let the core start on its own
	// thread. Returns nullptr with a reason when the core refuses the game.
	auto prepare = [&](const std::string &key, bool has_alternatives,
	                   std::string *why) -> SphynxPlayerHandle * {
		const sphynx::SystemInfo *sys = sphynx::FindSystemByKey(key.c_str());
		if (!sys) {
			if (why) *why = "unknown system";
			return nullptr;
		}
		std::unique_ptr<SphynxPlayerHandle> p(new SphynxPlayerHandle());
		p->system_key = key;
		p->system_name = sys->display_name ? sys->display_name : key;
		p->game_tag = SafeFileName(StripExtension(sphynx::DisplayNameOf(local)));

		// 3. Where the core lives and where its BIOS/saves go.
		const std::string core_path =
			sphynx::CoreLibraryPathForSystem(sphynx_ui_native_lib_dir(), sys->key);
		if (core_path.empty()) {
			if (why) *why = "its emulator is not in this build";
			return nullptr;
		}
		p->core_tag = CoreTagFromPath(core_path);
		p->save_dir = sphynx_ui_save_dir();
		if (p->save_dir.empty()) {
			if (why) *why = "no save folder is known";
			return nullptr;
		}

		// SPHYNX (stage 15): before the core is loaded, give its BIOS folder the
		// exact file names it opens (case, system prefix). Nothing is deleted.
		sphynx_ui_prepare_bios(sys->key);
		char *bios_dir = sphynx_ui_bios_dir_for_system(sys->key);
		const std::string system_dir = bios_dir ? bios_dir : std::string(sphynx_ui_system_dir());
		sphynx_ui_free(bios_dir);
		p->bios_status = sphynx_ui_bios_status(sys->key);
		char *bios_report = sphynx_ui_bios_report(sys->key);
		p->bios_report = bios_report ? bios_report : "";
		sphynx_ui_free(bios_report);
		// The folder the core is really given: named in the log, so a "missing
		// BIOS" report can be checked against the truth (stage 17).
		sphynx::Logf("player: trying %s with %s (system dir: %s)",
		             p->system_name.c_str(), p->core_tag.c_str(), system_dir.c_str());

		// 4. Hand the pads outward, and the renderer to a core that wants one.
		p->session.SetFrameSink(&p->frame);
		p->hw.reset(new PlayerHardwareRender());
		p->session.SetHardwareRenderHost(p->hw.get());
		p->audio_out = sphynx::CreateAudioOutput();
		p->audio_push.reset(new AudioPush(p->audio_out.get()));
		p->session.SetAudioSink(p->audio_push.get());

		SphynxPlayerHandle *raw = p.get();
		p->session.SetInputSource([raw](unsigned port, unsigned device, unsigned index, unsigned id) -> int16_t {
			if (port != 0) return 0;
			const unsigned bits = raw->buttons.load(std::memory_order_relaxed);
			if (device == RETRO_DEVICE_JOYPAD) {
				if (id == RETRO_DEVICE_ID_JOYPAD_MASK) {
					unsigned mask = 0;
					for (unsigned bit = 0; bit < 16; ++bit) {
						const unsigned pad_bit = 1u << bit;
						const int joy = PadIdForBit(pad_bit);
						if (joy >= 0 && (bits & pad_bit)) mask |= 1u << joy;
					}
					return static_cast<int16_t>(mask);
				}
				for (unsigned bit = 0; bit < 16; ++bit) {
					const unsigned pad_bit = 1u << bit;
					if (PadIdForBit(pad_bit) == static_cast<int16_t>(id))
						return (bits & pad_bit) ? 1 : 0;
				}
				return 0;
			}
			if (device == RETRO_DEVICE_ANALOG) {
				const float x = index == 0 ? raw->lx.load(std::memory_order_relaxed)
				                           : raw->rx.load(std::memory_order_relaxed);
				const float y = index == 0 ? raw->ly.load(std::memory_order_relaxed)
				                           : raw->ry.load(std::memory_order_relaxed);
				if (id == RETRO_DEVICE_ID_ANALOG_X) return ToAxis(x);
				if (id == RETRO_DEVICE_ID_ANALOG_Y) return ToAxis(-y);   // core wants +up
			}
			return 0;
		});

		// SPHYNX (stage 11): no frame-budget watchdog during playback. A slow
		// frame is not a failure; only the core or the user may stop a game.
		p->session.SetWatchdogMs(0);

		// The user's own values first, then the recommended ones (stage 15).
		p->session.EnvMutable().SetInitialOptions(LoadOptions(OptionsFilePath(p.get())));
		p->session.EnvMutable().SetRecommendations(p->core_tag);

		// 5. Battery save path: the worker writes it while it runs.
		p->sram_path = sphynx::StateFilePath(p->save_dir, p->game_tag + ".sram", 0);
		{
			const size_t dot = p->sram_path.find_last_of('.');
			if (dot != std::string::npos) p->sram_path = p->sram_path.substr(0, dot) + ".srm";
		}

		// 6. Start the core on its own thread and wait for the outcome.
		SphynxPlayerHandle *handle = p.release();
		{
			std::unique_lock<std::mutex> lock(handle->start_mutex);
			// SPHYNX (stage 18): a thread with a real stack (see CoreStackBytes).
			pthread_attr_t attr;
			pthread_attr_init(&attr);
			pthread_attr_setstacksize(&attr, CoreStackBytes);
			WorkerArgs *args = new WorkerArgs{handle, core_path, local, system_dir};
			const int created = pthread_create(&handle->worker, &attr, &PlayerWorkerEntry, args);
			pthread_attr_destroy(&attr);
			if (created != 0) {
				// Nothing was started: fall through to the failure path below,
				// which stops and frees the handle exactly once.
				delete args;
				handle->start_error = "the emulator thread could not be created";
				handle->failed = true;
				handle->started = true;
			}
			handle->worker_valid = true;
			if (!handle->start_cv.wait_for(lock, std::chrono::seconds(30),
			                               [handle] { return handle->started.load(); })) {
				handle->start_error = "the emulator did not start within 30 seconds";
				handle->failed = true;
			}
			// SPHYNX (stage 18): when another system could still play this file,
			// give this core a moment to prove it survives its first frames. A
			// core that dies right after loading used to be accepted (it had
			// "started"), and the user was thrown out of the game a second later.
			// On a timeout the core is accepted: slow is not the same as broken,
			// and a file only one system claims never waits at all.
			if (has_alternatives && !handle->failed.load()) {
				handle->start_cv.wait_for(lock, std::chrono::milliseconds(800), [handle] {
					return handle->warm.load() || handle->failed.load();
				});
			}
		}
		if (handle->failed.load()) {
			// `handle` owns everything now: p was released above and must not be
			// touched again (stage 17 - reading p->bios_status here crashed the
			// whole player the first time this path was taken).
			// SPHYNX (stage 18): a core that started and then failed (it crashed,
			// or it stopped by itself) has no start error - its reason is in
			// handle->error, and that is what belongs on the panel.
			std::string message = handle->start_error;
			if (message.empty()) message = handle->error;
			if (message.empty()) message = "the emulator refused this game";
			// The full text (with the dlopen path, for the log) stays here; the
			// one line that goes on screen is shortened below.
			const std::string full_message = message;
			if (message.find("cannot load") != std::string::npos)
				message = "its emulator could not be loaded";
			// The most useful sentence of the whole program: say what is wrong
			// with *this* system when we can tell (stage 17).
			if (!file_problem.empty())
				message += " — " + file_problem;
			else if (handle->bios_status.rfind("missing", 0) == 0)
				message += " — this system needs its BIOS: " + handle->bios_status;
			sphynx::LogLine("player: " + handle->system_name + " refused it: " + full_message);
			sphynx::LogLine("  bios: " + handle->bios_report);
			// One short line per system is what fits on the failure panel; a
			// reason longer than that is cut, never dropped silently.
			if (message.size() > 90) message = message.substr(0, 88) + "…";
			if (why) *why = message;
			StopWorker(handle);
			delete handle;
			return nullptr;
		}

		handle->fps = handle->session.FrameRate();
		if (handle->fps <= 20.0 || handle->fps > 200.0) handle->fps = 60.0;
		return handle;
	};

	// 7. Try them in order. The first one that starts wins; the failures are kept
	//    so that a file no system can run produces a message that says why.
	std::vector<std::pair<std::string, std::string>> tried;
	SphynxPlayerHandle *handle = nullptr;
	for (size_t key_index = 0; key_index < keys.size(); ++key_index) {
		const std::string &key = keys[key_index];
		std::string why;
		handle = prepare(key, key_index + 1 < keys.size(), &why);
		if (handle) {
			if (!tried.empty())
				sphynx::LogLine("player: " + handle->system_name +
				                " was the system that could run this file");
			break;
		}
		std::string name = key;
		if (const sphynx::SystemInfo *info = sphynx::FindSystemByKey(key.c_str()))
			if (info->display_name) name = info->display_name;
		tried.emplace_back(name, why);
	}
	if (!handle) {
		// A message the user can act on, and a screenshot of it that is worth
		// sending: the file, then one line per system that was tried.
		std::string message = "This file could not be started by any system that claims it.";
		if (file_size >= 0)
			message += "\n" + sphynx::DisplayNameOf(local) + " (" + HumanSize(file_size) +
			           (file_ext.empty() ? "" : ", ." + file_ext) + ")";
		if (!file_problem.empty()) message += "\n" + file_problem;
		for (size_t i = 0; i < tried.size() && i < 4; ++i)
			message += "\n· " + tried[i].first + ": " + tried[i].second;
		if (err && err_size) std::snprintf(err, err_size, "%s", message.c_str());
		sphynx::LogLine("player: no system could run " + local);
		return nullptr;
	}

	// LoadBatterySave() reads core memory, so it happens on the core thread
	// (inside Session::Start's caller) - see PlayerWorker.
	g_active_handle.store(handle);
	handle->status = handle->system_name;
	SetStatus(handle);
	sphynx::LogLine("PLAYER: opened " + local + " (" + handle->system_key + " / " + handle->core_tag + ")");
	sphynx::LogLine("  bios: " + handle->bios_report);
	if (handle->session.Env().RecommendedCount() > 0) {
		sphynx::LogLine("  recommended settings applied: " +
		                std::to_string(handle->session.Env().RecommendedCount()) + " of " +
		                std::to_string(handle->session.Env().Options().size()));
	}
	return handle;
}

void sphynx_player_close(SphynxPlayerHandle *p) {
	if (!p) return;
	// Ask the emulation thread to leave; it saves the battery RAM and stops the
	// core and the audio device itself, because those may only be touched from
	// the thread that started them. Only after it has returned may we delete.
	// A handle that was already stopped (by a reclaim in sphynx_player_open, or
	// by an earlier close) is simply freed: StopWorker() is idempotent.
	StopWorker(p);
	{
		std::lock_guard<std::mutex> one_core(g_one_core_mutex);
		SphynxPlayerHandle *expected = p;
		g_active_handle.compare_exchange_strong(expected, nullptr);
	}
	delete p;
}

int sphynx_player_run(SphynxPlayerHandle *p) {
	// SPHYNX (stage 14): the core is driven by its own thread now, so this call
	// only reports how it is doing. The screen still calls it once per update,
	// which keeps the UI code unchanged: 1 = running, 0 = paused, -1 = stopped
	// with a reason in sphynx_player_status().
	// SPHYNX (stage 15): a handle that a newer game reclaimed (see
	// sphynx_player_open) reports "stopped" here, never a dangling read.
	if (!p) return 0;
	if (p->dead.load()) return -1;
	if (p->failed.load()) return -1;
	if (p->paused.load()) return 0;
	return 1;
}

void sphynx_player_set_paused(SphynxPlayerHandle *p, int paused) {
	if (!p || p->dead.load()) return;
	p->paused = paused != 0;
}

double sphynx_player_measured_fps(SphynxPlayerHandle *p) {
	return (p && !p->dead.load()) ? p->measured_fps.load() : 0.0;
}

double sphynx_player_frame_ms(SphynxPlayerHandle *p) {
	return (p && !p->dead.load()) ? p->frame_ms.load() : 0.0;
}

// Reports the size of the frame waiting in the queue without consuming it, so the
// caller can size its buffer first. Returns 1 when a frame is waiting.
int sphynx_player_frame_size(SphynxPlayerHandle *p, int *width, int *height) {
	if (width) *width = 0;
	if (height) *height = 0;
	if (!p || p->dead.load()) return 0;
	int w = 0, h = 0;
	p->frame.Peek(&w, &h);
	if (width) *width = w;
	if (height) *height = h;
	return (w > 0 && h > 0) ? 1 : 0;
}

int sphynx_player_take_frame(SphynxPlayerHandle *p, unsigned char *dst, int dst_size,
                             int *width, int *height) {
	if (!p || p->dead.load() || !dst || dst_size <= 0) return 0;
	int w = 0, h = 0, bytes = 0;
	const uint8_t *src = p->frame.Take(&w, &h, &bytes);
	if (!src || bytes <= 0) return 0;
	if (bytes > dst_size) return 0;
	std::memcpy(dst, src, static_cast<size_t>(bytes));
	if (width) *width = w;
	if (height) *height = h;
	return 1;
}

void sphynx_player_set_pad(SphynxPlayerHandle *p, unsigned buttons,
                           float lx, float ly, float rx, float ry) {
	if (!p || p->dead.load()) return;
	p->buttons.store(buttons, std::memory_order_relaxed);
	p->lx.store(lx, std::memory_order_relaxed);
	p->ly.store(ly, std::memory_order_relaxed);
	p->rx.store(rx, std::memory_order_relaxed);
	p->ry.store(ry, std::memory_order_relaxed);
}

int sphynx_player_save_state(SphynxPlayerHandle *p, int slot, char *msg, size_t msg_size) {
	if (!p || p->dead.load()) return 0;
	// Serialised with the emulation thread: core memory is not re-entrant.
	std::lock_guard<std::mutex> lock(p->session_mutex);
	std::vector<uint8_t> state;
	if (!p->session.SaveState(&state)) {
		if (msg && msg_size) std::snprintf(msg, msg_size, "This system cannot save state.");
		return 0;
	}
	const std::string path = sphynx::StateFilePath(p->save_dir, p->game_tag, slot);
	std::string error;
	if (!sphynx::WriteStateFile(path, p->core_tag, state, &error)) {
		if (msg && msg_size) std::snprintf(msg, msg_size, "%s", error.c_str());
		return 0;
	}
	sphynx::LogLine("player: saved slot " + std::to_string(slot + 1));
	if (msg && msg_size) std::snprintf(msg, msg_size, "Saved to slot %d", slot + 1);
	return 1;
}

int sphynx_player_load_state(SphynxPlayerHandle *p, int slot, char *msg, size_t msg_size) {
	if (!p || p->dead.load()) return 0;
	// Serialised with the emulation thread: core memory is not re-entrant.
	std::lock_guard<std::mutex> lock(p->session_mutex);
	const std::string path = sphynx::StateFilePath(p->save_dir, p->game_tag, slot);
	std::vector<uint8_t> state;
	std::string error;
	if (!sphynx::ReadStateFile(path, p->core_tag, &state, &error)) {
		if (msg && msg_size) std::snprintf(msg, msg_size, "%s", error.c_str());
		return 0;
	}
	if (!p->session.LoadState(state)) {
		if (msg && msg_size) std::snprintf(msg, msg_size, "The state was refused by the core.");
		return 0;
	}
	sphynx::LogLine("player: loaded slot " + std::to_string(slot + 1));
	if (msg && msg_size) std::snprintf(msg, msg_size, "Loaded slot %d", slot + 1);
	return 1;
}

int sphynx_player_state_exists(SphynxPlayerHandle *p, int slot) {
	if (!p || p->dead.load()) return 0;
	// Serialised with the emulation thread: core memory is not re-entrant.
	std::lock_guard<std::mutex> lock(p->session_mutex);
	return sphynx::StateFileExists(sphynx::StateFilePath(p->save_dir, p->game_tag, slot)) ? 1 : 0;
}

void sphynx_player_flush_save(SphynxPlayerHandle *p) {
	if (!p || p->dead.load()) return;
	// Serialised with the emulation thread: core memory is not re-entrant.
	std::lock_guard<std::mutex> lock(p->session_mutex);
	FlushBatterySave(p);
}

double sphynx_player_fps(SphynxPlayerHandle *p) {
	return (p && !p->dead.load()) ? p->fps : 60.0;
}

const char *sphynx_player_status(SphynxPlayerHandle *p) {
	if (!p) return "";
	if (p->dead.load()) return "this game was closed";
	// SPHYNX (stage 18): a handle that stopped because the core failed answers
	// with the reason itself. The failure panel used to print the "system ·
	// 320x240 · 60 fps" summary here, so the cause - a crash, a refused file -
	// never reached the screen and the game simply ended with no explanation.
	if (p->failed.load() && !p->error.empty()) return p->error.c_str();
	return p->status.c_str();
}

const char *sphynx_player_system_name(SphynxPlayerHandle *p) {
	return p ? p->system_name.c_str() : "";
}

const char *sphynx_player_bios_report(SphynxPlayerHandle *p) {
	// SPHYNX (stage 15): the files the core's own system folder really holds.
	// This is the answer to "I installed the BIOS/firmware and the game still
	// does not start": it names the folder, what was found (with sizes) and what
	// is missing, instead of leaving the user to guess.
	return p ? p->bios_report.c_str() : "";
}

const char *sphynx_player_bios_status(SphynxPlayerHandle *p) {
	return p ? p->bios_status.c_str() : "";
}

int sphynx_player_option_count(SphynxPlayerHandle *p) {
	if (!p || p->dead.load()) return 0;
	return static_cast<int>(p->session.Env().Options().size());
}

const char *sphynx_player_option(SphynxPlayerHandle *p, int index) {
	if (!p) return nullptr;
	if (p->dead.load()) return nullptr;
	const auto &options = p->session.Env().Options();
	if (index < 0 || index >= static_cast<int>(options.size())) return nullptr;
	p->option_line_cache = FlattenOption(options[static_cast<size_t>(index)]);
	return p->option_line_cache.c_str();
}

int sphynx_player_set_option(SphynxPlayerHandle *p, const char *key, const char *value) {
	if (!p || p->dead.load()) return 0;
	// Serialised with the emulation thread: core memory is not re-entrant.
	std::lock_guard<std::mutex> lock(p->session_mutex);
	if (!key || !value) return 0;
	if (!p->session.EnvMutable().SetOption(key, value)) return 0;
	SaveOptions(OptionsFilePath(p), p->session.Env().Options());
	return 1;
}

void sphynx_player_set_sound(SphynxPlayerHandle *p, int on) {
	if (!p || p->dead.load()) return;
	// SPHYNX (stage 15 audio): only the flag is written here. Starting and
	// stopping an OpenSL device from the UI thread while the emulation thread is
	// pushing into it is a data race with a crash at the end of it; the
	// emulation thread now notices the flag and does it, one frame later.
	p->sound.store(on != 0, std::memory_order_relaxed);
}

void sphynx_player_set_fast_forward(SphynxPlayerHandle *p, int multiplier) {
	if (!p || p->dead.load()) return;
	p->fast_forward.store(multiplier > 0 ? multiplier : 1, std::memory_order_relaxed);
}

}  // extern "C"
