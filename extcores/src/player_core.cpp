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

#include "sphynx_extcore.h"
#include "sphynx_ui_api.h"
#include "android_paths.h"
#include "bios_layout.h"
#include "diag_log.h"
#include "player_audio.h"
#include "state_file.h"

#include "libretro.h"

#include <atomic>
#include <cmath>
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
		for (unsigned y = 0; y < height; ++y) {
			const uint8_t *row = static_cast<const uint8_t *>(data) + y * pitch;
			uint8_t *out = pixels_.data() + static_cast<size_t>(y) * width * 4;
			if (format == sphynx::PixelFormat::RGB565) {
				const uint16_t *src = reinterpret_cast<const uint16_t *>(row);
				for (unsigned x = 0; x < width; ++x) {
					const uint16_t c = src[x];
					const uint8_t r = static_cast<uint8_t>(((c >> 11) & 0x1F) << 3);
					const uint8_t g = static_cast<uint8_t>(((c >> 5) & 0x3F) << 2);
					const uint8_t b = static_cast<uint8_t>((c & 0x1F) << 3);
					out[x * 4 + 0] = r;
					out[x * 4 + 1] = g;
					out[x * 4 + 2] = b;
					out[x * 4 + 3] = 0xFF;
				}
			} else {
				// XRGB8888: the fourth byte is padding, not alpha.
				for (unsigned x = 0; x < width; ++x) {
					out[x * 4 + 0] = row[x * 4 + 2];   // R  (core is BGRX)
					out[x * 4 + 1] = row[x * 4 + 1];   // G
					out[x * 4 + 2] = row[x * 4 + 0];   // B
					out[x * 4 + 3] = 0xFF;
				}
			}
		}
		fresh_ = true;
	}

	// Copies the newest frame out for the renderer. Returns false when there is
	// nothing new, which lets the screen skip a texture upload entirely.
	bool Take(std::vector<uint8_t> *out, int *w, int *h) {
		std::lock_guard<std::mutex> lock(mutex_);
		if (!fresh_ || pixels_.empty()) return false;
		out->assign(pixels_.begin(), pixels_.end());
		*w = width_;
		*h = height_;
		fresh_ = false;
		return true;
	}

	int width() const { return width_; }
	int height() const { return height_; }

private:
	std::mutex mutex_;
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

void SetStatus(SphynxPlayerHandle *p) {
	char buf[192];
	std::snprintf(buf, sizeof(buf), "%s · %ux%u · %.2f fps%s", p->system_name.c_str(),
	              p->frame_w, p->frame_h, p->fps,
	              p->session.Stats().watchdog_trips ? " · slow" : "");
	p->status = buf;
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

	// A Saturn or Dreamcast core can spend a second or two on its first frame
	// (BIOS checks, shader-ish init). The watchdog is there to catch a genuine
	// hang, not to turn a slow phone into an error message, so it is generous.
	p->session.SetWatchdogMs(10000);

	std::string start_error;
	if (!p->session.Start(core_path, local, system_dir, p->save_dir, &start_error)) {
		const std::string message = start_error.empty() ? "the emulator refused this game" : start_error;
		if (err && err_size) std::snprintf(err, err_size, "%s", message.c_str());
		sphynx::LogLine(std::string("player: start failed: ") + message);
		return nullptr;
	}

	p->fps = p->session.FrameRate();
	if (p->fps <= 20.0 || p->fps > 200.0) p->fps = 60.0;

	// 5. Battery save next to the states, then audio.
	//    The path is built through StateFilePath() only to reuse its mkdir -p;
	//    the file itself is raw save RAM, not a snapshot, so it has its own
	//    extension and is never parsed by ReadStateFile().
	p->sram_path = sphynx::StateFilePath(p->save_dir, p->game_tag + ".sram", 0);
	{
		const size_t dot = p->sram_path.find_last_of('.');
		if (dot != std::string::npos) p->sram_path = p->sram_path.substr(0, dot) + ".srm";
	}
	LoadBatterySave(p.get());

	if (!p->audio_out->Start(p->session.SampleRate())) {
		// Silent is a fine outcome; refusing to play is not.
		sphynx::LogLine("player: audio device could not be opened, continuing silently");
	}

	g_core_active = true;
	p->status = p->system_name;
	SetStatus(p.get());
	sphynx::LogLine("PLAYER: opened " + local + " (" + p->system_key + " / " + p->core_tag + ")");
	return p.release();
}

void sphynx_player_close(SphynxPlayerHandle *p) {
	if (!p) return;
	FlushBatterySave(p);
	if (p->audio_out) p->audio_out->Stop();
	p->session.Stop();
	{
		std::lock_guard<std::mutex> one_core(g_one_core_mutex);
		g_core_active = false;
	}
	delete p;
}

int sphynx_player_run(SphynxPlayerHandle *p) {
	if (!p) return 0;
	const int loops = p->fast_forward.load(std::memory_order_relaxed);
	const int count = loops > 0 ? loops : 1;
	int result = 1;
	for (int i = 0; i < count; ++i) {
		if (!p->session.RunFrame()) {
			p->error = p->session.LastError();
			if (p->error.empty()) p->error = "the emulator stopped";
			sphynx::LogLine(std::string("player: stopped: ") + p->error);
			result = p->session.LastRunFailed() ? -1 : 0;
			break;
		}
		++p->frames_run;
	}
	p->frame_w = static_cast<unsigned>(p->frame.width());
	p->frame_h = static_cast<unsigned>(p->frame.height());
	if ((p->frames_run % 120) == 0) SetStatus(p);

	// Battery save twice a minute: cheap, and it bounds what a kill can lose.
	const double now = static_cast<double>(time(nullptr));
	if (now - p->last_save_check > 20.0) {
		p->last_save_check = now;
		FlushBatterySave(p);
	}
	return result;
}

int sphynx_player_take_frame(SphynxPlayerHandle *p, unsigned char *dst, int dst_size,
                             int *width, int *height) {
	if (!p || !dst || dst_size <= 0) return 0;
	std::vector<uint8_t> copy;
	int w = 0, h = 0;
	if (!p->frame.Take(&copy, &w, &h)) return 0;
	const int needed = static_cast<int>(copy.size());
	if (needed > dst_size) return 0;
	std::memcpy(dst, copy.data(), copy.size());
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
	return sphynx::StateFileExists(sphynx::StateFilePath(p->save_dir, p->game_tag, slot)) ? 1 : 0;
}

void sphynx_player_flush_save(SphynxPlayerHandle *p) {
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
