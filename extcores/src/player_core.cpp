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
#include <sys/stat.h>
#include <sys/types.h>

#include "sphynx_extcore.h"
#include "sphynx_ui_api.h"
#include "android_paths.h"
#include "bios_layout.h"
#include "diag_log.h"
#include "player_audio.h"
#include "state_file.h"

#include "libretro.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
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
// so exactly one external core may run at a time. The screen respects this by
// closing the old player before opening a new one; this flag turns a mistake
// into a readable error instead of two cores fighting over the same globals.
std::mutex g_one_core_mutex;
bool g_core_active = false;

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

// The handle PPSSPP holds. Declared in the header as an opaque forward
// declaration, defined here so the layout stays private.
struct SphynxPlayerHandle {
	sphynx::Session session;
	FrameCapture frame;
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

	std::atomic<unsigned> buttons{0};
	std::atomic<float> lx{0.0f}, ly{0.0f}, rx{0.0f}, ry{0.0f};
	std::atomic<int> fast_forward{1};
	bool sound = true;

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
	std::thread worker;
	std::atomic<bool> quit{false};
	std::atomic<bool> paused{false};
	std::atomic<bool> started{false};
	std::atomic<bool> failed{false};
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
void PlayerWorker(SphynxPlayerHandle *p, std::string core_path, std::string content,
                  std::string system_dir) {
	const std::string save_dir = p->save_dir;

	std::string error;
	if (!p->session.Start(core_path, content, system_dir, save_dir, &error)) {
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

	if (!p->audio_out->Start(p->session.SampleRate())) {
		// Silent playback is a fine outcome; refusing to play is not.
		sphynx::LogLine("player: audio device could not be opened, continuing silently");
	}

	// The core's battery RAM is read here, on the core thread, immediately after
	// the game is loaded and before the user can save anything.
	LoadBatterySave(p);

	{
		std::lock_guard<std::mutex> lock(p->start_mutex);
		p->started = true;
	}
	p->start_cv.notify_all();

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

		// Fast-forward multiplies how many emulated frames one tick runs.
		const int loops = std::max(1, p->fast_forward.load(std::memory_order_relaxed));
		for (int i = 0; i < loops && !p->quit.load(std::memory_order_relaxed); ++i) {
			const auto t0 = Clock::now();
			bool ok = false;
			{
				std::lock_guard<std::mutex> lock(p->session_mutex);
				ok = p->session.RunFrame();
				if (ok) ++p->frames_run;
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

		// Sleep until the next frame is due. A core that is slower than real time
		// simply never sleeps - it is not our job to drop its frames here.
		next_frame += std::chrono::duration_cast<Clock::duration>(
			std::chrono::duration<double>(frame_time * loops));
		const auto after = Clock::now();
		if (next_frame > after) {
			std::this_thread::sleep_until(next_frame);
		} else if (after - next_frame > std::chrono::milliseconds(250)) {
			// Too far behind to catch up: resynchronise instead of spinning.
			next_frame = after;
		}
	}

	// Leave cleanly: battery save first, then the core, then the audio device.
	{
		std::lock_guard<std::mutex> lock(p->session_mutex);
		FlushBatterySave(p);
		p->session.Stop();
	}
	if (p->audio_out) p->audio_out->Stop();
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
	if (g_core_active) return fail("another game is already running");

	// 1. A core can only fopen() a real file, so an Android content:// URI has
	//    to become one first. This is the step that made every SAF-picked game
	//    fail before.
	const std::string local = sphynx::LocalPathFor(path);
	if (local.empty()) return fail("this file cannot be opened (the storage URI did not resolve)");

	// 2. Which system? The caller may already know; otherwise ask the same
	//    tables the game list uses.
	const std::string key = (system_key && *system_key) ? std::string(system_key) : std::string();
	std::string resolved = key;
	if (resolved.empty()) {
		const sphynx::DetectResult r = sphynx::Detect(local, nullptr, 0);
		if (r.is_psp_native) return fail("this is a PSP game: PPSSPP plays it itself");
		if (r.candidates.empty()) return fail("this file was not recognised as a game");
		const sphynx::SystemInfo *info = sphynx::FindSystem(r.candidates.front().id);
		if (!info) return fail("the detected system is not in this build");
		resolved = info->key;
	}
	const sphynx::SystemInfo *sys = sphynx::FindSystemByKey(resolved.c_str());
	if (!sys) return fail("unknown system key");

	std::unique_ptr<SphynxPlayerHandle> p(new SphynxPlayerHandle());
	p->system_key = resolved;
	p->system_name = sys->display_name ? sys->display_name : resolved;
	p->game_tag = SafeFileName(StripExtension(sphynx::DisplayNameOf(local)));

	// 3. Where the core lives and where its BIOS/saves go.
	const std::string core_path =
		sphynx::CoreLibraryPathForSystem(sphynx_ui_native_lib_dir(), sys->key);
	if (core_path.empty()) return fail("the emulator for this system is not in this build");
	p->core_tag = CoreTagFromPath(core_path);

	p->save_dir = sphynx_ui_save_dir();
	if (p->save_dir.empty()) return fail("no save directory is known");

	char *bios_dir = sphynx_ui_bios_dir_for_system(sys->key);
	const std::string system_dir = bios_dir ? bios_dir : std::string(sphynx_ui_system_dir());
	sphynx_ui_free(bios_dir);
	p->bios_status = sphynx_ui_bios_status(sys->key);

	// 4. Hand the pads outward, then start.
	p->session.SetFrameSink(&p->frame);
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

	// SPHYNX (core-running system, stage 11): the frame-budget watchdog is off
	// for playback. It used to stop the core when a single retro_run() took more
	// than 10 s - which a CD-based system (PlayStation, Saturn, Sega CD) or the
	// very first frame of melonDS/DOSBox does routinely on a 32-bit ARM phone.
	// The visible result was "the game closes by itself a few seconds after it
	// starts": the core was killed by our own watchdog, not by the game.
	//
	// V5 has no such watchdog at all: RetroHost::RunFrame() simply runs the core
	// and the screen keeps going. A slow frame is not a failure, so nothing here
	// may ever decide to stop the game. Only the core itself, or the user, may.
	p->session.SetWatchdogMs(0);

	// SPHYNX (stage 13): the core declares its options during retro_init(), so the
	// values chosen last time for this system are handed over now and applied the
	// moment the declaration arrives.
	p->session.EnvMutable().SetInitialOptions(LoadOptions(OptionsFilePath(p.get())));

	// 5. Battery save path first: the worker writes it while it runs.
	p->sram_path = sphynx::StateFilePath(p->save_dir, p->game_tag + ".sram", 0);
	{
		const size_t dot = p->sram_path.find_last_of('.');
		if (dot != std::string::npos) p->sram_path = p->sram_path.substr(0, dot) + ".srm";
	}

	// 6. Start the core on its own thread and wait for the outcome. The wait is
	//    bounded: a core that never reports back must not hang the UI, and a
	//    refusal has to arrive as a readable message, not as a dead screen.
	SphynxPlayerHandle *handle = p.release();
	{
		std::unique_lock<std::mutex> lock(handle->start_mutex);
		handle->worker = std::thread(PlayerWorker, handle, core_path, local, system_dir);
		if (!handle->start_cv.wait_for(lock, std::chrono::seconds(30),
		                               [handle] { return handle->started.load(); })) {
			handle->start_error = "the emulator did not start within 30 seconds";
			handle->failed = true;
		}
	}
	if (handle->failed.load()) {
		const std::string message = handle->start_error.empty()
		                                ? "the emulator refused this game"
		                                : handle->start_error;
		if (err && err_size) std::snprintf(err, err_size, "%s", message.c_str());
		sphynx::LogLine(std::string("player: start failed: ") + message);
		handle->quit = true;
		if (handle->worker.joinable()) handle->worker.join();
		return nullptr;
	}

	handle->fps = handle->session.FrameRate();
	if (handle->fps <= 20.0 || handle->fps > 200.0) handle->fps = 60.0;

	// LoadBatterySave() reads core memory, so it happens on the core thread
	// (inside Session::Start's caller) - see PlayerWorker.

	g_core_active = true;
	handle->status = handle->system_name;
	SetStatus(handle);
	sphynx::LogLine("PLAYER: opened " + local + " (" + handle->system_key + " / " + handle->core_tag + ")");
	return handle;
}

void sphynx_player_close(SphynxPlayerHandle *p) {
	if (!p) return;
	// Ask the emulation thread to leave; it saves the battery RAM and stops the
	// core and the audio device itself, because those may only be touched from
	// the thread that started them. Only after it has returned may we delete.
	p->quit = true;
	if (p->worker.joinable()) p->worker.join();
	{
		std::lock_guard<std::mutex> one_core(g_one_core_mutex);
		g_core_active = false;
	}
	delete p;
}

int sphynx_player_run(SphynxPlayerHandle *p) {
	// SPHYNX (stage 14): the core is driven by its own thread now, so this call
	// only reports how it is doing. The screen still calls it once per update,
	// which keeps the UI code unchanged: 1 = running, 0 = paused, -1 = stopped
	// with a reason in sphynx_player_status().
	if (!p) return 0;
	if (p->failed.load()) return -1;
	if (p->paused.load()) return 0;
	return 1;
}

void sphynx_player_set_paused(SphynxPlayerHandle *p, int paused) {
	if (!p) return;
	p->paused = paused != 0;
}

double sphynx_player_measured_fps(SphynxPlayerHandle *p) {
	return p ? p->measured_fps.load() : 0.0;
}

double sphynx_player_frame_ms(SphynxPlayerHandle *p) {
	return p ? p->frame_ms.load() : 0.0;
}

// Reports the size of the frame waiting in the queue without consuming it, so the
// caller can size its buffer first. Returns 1 when a frame is waiting.
int sphynx_player_frame_size(SphynxPlayerHandle *p, int *width, int *height) {
	if (width) *width = 0;
	if (height) *height = 0;
	if (!p) return 0;
	int w = 0, h = 0;
	p->frame.Peek(&w, &h);
	if (width) *width = w;
	if (height) *height = h;
	return (w > 0 && h > 0) ? 1 : 0;
}

int sphynx_player_take_frame(SphynxPlayerHandle *p, unsigned char *dst, int dst_size,
                             int *width, int *height) {
	if (!p || !dst || dst_size <= 0) return 0;
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
	if (!p) return;
	p->buttons.store(buttons, std::memory_order_relaxed);
	p->lx.store(lx, std::memory_order_relaxed);
	p->ly.store(ly, std::memory_order_relaxed);
	p->rx.store(rx, std::memory_order_relaxed);
	p->ry.store(ry, std::memory_order_relaxed);
}

int sphynx_player_save_state(SphynxPlayerHandle *p, int slot, char *msg, size_t msg_size) {
	if (!p) return 0;
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
	if (!p) return 0;
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
	if (!p) return 0;
	// Serialised with the emulation thread: core memory is not re-entrant.
	std::lock_guard<std::mutex> lock(p->session_mutex);
	return sphynx::StateFileExists(sphynx::StateFilePath(p->save_dir, p->game_tag, slot)) ? 1 : 0;
}

void sphynx_player_flush_save(SphynxPlayerHandle *p) {
	if (!p) return;
	// Serialised with the emulation thread: core memory is not re-entrant.
	std::lock_guard<std::mutex> lock(p->session_mutex);
	FlushBatterySave(p);
}

double sphynx_player_fps(SphynxPlayerHandle *p) {
	return p ? p->fps : 60.0;
}

const char *sphynx_player_status(SphynxPlayerHandle *p) {
	return p ? p->status.c_str() : "";
}

const char *sphynx_player_system_name(SphynxPlayerHandle *p) {
	return p ? p->system_name.c_str() : "";
}

const char *sphynx_player_bios_status(SphynxPlayerHandle *p) {
	return p ? p->bios_status.c_str() : "";
}

int sphynx_player_option_count(SphynxPlayerHandle *p) {
	if (!p) return 0;
	return static_cast<int>(p->session.Env().Options().size());
}

const char *sphynx_player_option(SphynxPlayerHandle *p, int index) {
	if (!p) return nullptr;
	const auto &options = p->session.Env().Options();
	if (index < 0 || index >= static_cast<int>(options.size())) return nullptr;
	p->option_line_cache = FlattenOption(options[static_cast<size_t>(index)]);
	return p->option_line_cache.c_str();
}

int sphynx_player_set_option(SphynxPlayerHandle *p, const char *key, const char *value) {
	if (!p) return 0;
	// Serialised with the emulation thread: core memory is not re-entrant.
	std::lock_guard<std::mutex> lock(p->session_mutex);
	if (!key || !value) return 0;
	if (!p->session.EnvMutable().SetOption(key, value)) return 0;
	SaveOptions(OptionsFilePath(p), p->session.Env().Options());
	return 1;
}

void sphynx_player_set_sound(SphynxPlayerHandle *p, int on) {
	if (!p) return;
	p->sound = on != 0;
	if (p->sound) {
		if (p->audio_out && !p->audio_out->IsRunning())
			p->audio_out->Start(p->session.SampleRate());
	} else if (p->audio_out) {
		p->audio_out->Stop();
	}
}

void sphynx_player_set_fast_forward(SphynxPlayerHandle *p, int multiplier) {
	if (!p) return;
	p->fast_forward.store(multiplier > 0 ? multiplier : 1, std::memory_order_relaxed);
}

}  // extern "C"
