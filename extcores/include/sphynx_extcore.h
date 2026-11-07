// ============================================================================
//  Sphynx — multi-system fork of PPSSPP v1.18.1
//  extcores/include/sphynx_extcore.h
//
//  Public API of the Sphynx external-core host.
//
//  RULES OF THIS PROJECT (enforced by review, see docs):
//    * Nothing in PPSSPP's Core/, GPU/, HLE/ is ever included from here.
//    * This layer knows nothing about PSP. The PSP core is handled by PPSSPP
//      itself and is *never* routed through this code path.
//    * Everything here is additive: removing extcores/ returns the tree to
//      stock PPSSPP v1.18.1.
//
//  SPDX-License-Identifier: GPL-2.0-or-later
// ============================================================================
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace sphynx {

// ---------------------------------------------------------------------------
// 1. Systems
// ---------------------------------------------------------------------------

// One entry per system the user asked for (35 total, see docs/04-*.md for the
// mapping between the 31 requested lines and the 35 concrete systems).
enum class SystemId : int {
	Unknown = 0,

	// Nintendo
	Nes,
	Fds,
	Gb,
	Gbc,
	Gba,
	N64,
	Nds,
	Snes,

	// SEGA
	Sg1000,
	MasterSystem,
	GameGear,
	Genesis,
	Genesis32X,
	SegaCd,
	Saturn,
	Dreamcast,
	SegaPico,

	// Other
	ThreeDo,
	Amiga,
	Arcade,
	Msx,
	ColecoVision,
	WonderSwan,
	WonderSwanColor,

	// Sony  (PSP itself is NOT here — it never goes through this layer)
	Psx,

	// NEC
	Pc88,
	Pc98,
	PcFx,
	TurboGrafx16,
	TurboGrafxCd,

	// PC
	MsDos,
	ScummVm,

	// SNK
	NeoGeoCd,
	NeoGeoPocket,
	NeoGeoPocketColor,

	Count
};

// How well the licence of the backing core combines with PPSSPP's GPL-2.0+.
enum class LicenceCompat {
	Compatible,     // GPLv2 / GPLv2+ / GPLv3 / MPL-2.0 / LGPL — can be linked
	RequiresGpl3,   // GPLv3-only: the whole distributed work must move to GPLv3
	NeedsReview,    // mixed/ambiguous licensing — must be audited before shipping
	Incompatible,   // non-commercial / closed: must NOT be linked
};

// Honest capability statement, used by the UI to warn the user up front
// instead of letting them discover it as a crash or a black screen.
enum class SupportLevel {
	NotBuilt,       // core binary not shipped in this build
	Experimental,   // builds, runs, but not validated on low-end hardware
	Supported,      // validated on reference hardware (see the test matrix)
	NativePsp,      // handled by PPSSPP itself, never by this layer
};

struct SystemInfo {
	SystemId id = SystemId::Unknown;
	const char *key = nullptr;          // stable id used by the UI and settings
	const char *display_name = nullptr; // shown in the game list
	const char *core_lib = nullptr;     // "gambatte" -> libsphynx_core_gambatte.so
	const char *const *extensions = nullptr;
	int extension_count = 0;
	const char *bios_hint = nullptr;    // nullptr => no BIOS required
	int ram_hint_mb = 0;                // rough working-set hint for the watchdog
	bool dual_screen = false;           // NDS: two screens must be rendered
	SupportLevel support = SupportLevel::NotBuilt;
};

// Core-level metadata (shared by every system a core covers).
struct CoreInfo {
	const char *lib = nullptr;
	const char *upstream = nullptr;   // upstream project of the core
	const char *licence = nullptr;
	LicenceCompat compat = LicenceCompat::NeedsReview;
	bool needs_gles3 = false;
	int est_size_kb = 0;              // measured, filled in per stage
};

// ---------------------------------------------------------------------------
// 2. Registry / database
// ---------------------------------------------------------------------------

const SystemInfo *AllSystems(int *count);           // 35 entries, stable order
const SystemInfo *FindSystem(SystemId id);
const SystemInfo *FindSystemByKey(const char *key); // "gb", "snes", ...
const CoreInfo *FindCore(const char *core_lib);
int SystemCount();                                   // == 35, asserted by tests

// ---------------------------------------------------------------------------
// 2b. Bundled core binaries
// ---------------------------------------------------------------------------
// The .so files are shipped in the APK's native library directory and named
// libsphynx_core_<buildbot name>.so. Names follow the libretro buildbot so the
// download script and the registry can never drift apart.
int BundledCoreCount();                       // 23 in this build
const char *const *BundledCoreLibs();         // array of buildbot names
// Registry name -> shipped file name ("mupen64plus_next" -> "..._gles2.so").
const char *CoreLibraryFile(const char *core_lib);
// Empty string when the system is known but its core is not shipped.
std::string CoreLibraryPath(const std::string &native_library_dir, const char *core_lib);
std::string CoreLibraryPathForSystem(const std::string &native_library_dir,
                                     const char *system_key);

// ---------------------------------------------------------------------------
// 3. Content detection
// ---------------------------------------------------------------------------

struct Candidate {
	SystemId id = SystemId::Unknown;
	int confidence = 0;   // 100 = certain, 50 = extension only, 10 = guess
	bool ambiguous = false;
};

struct DetectResult {
	// Ordered, best first. Empty => nothing matched (the UI must ask the user).
	std::vector<Candidate> candidates;
	// True when two or more systems are equally plausible: the UI is expected
	// to show ONE question in PPSSPP's own style and remember the answer.
	bool needs_user_choice = false;
	bool is_psp_native = false;   // true => hand the file to PPSSPP itself
	const char *reason = "";      // human-readable, for logging only
};

// `head` may be a 4 KiB prefix of the file; nullptr is allowed (extension-only
// detection). Never reads the file itself: the caller owns IO.
DetectResult Detect(const std::string &path, const uint8_t *head, size_t head_size);

// ---------------------------------------------------------------------------
// 4. Core loading + lifetime
// ---------------------------------------------------------------------------

struct CoreSymbols;   // opaque

class CoreLoader {
public:
	CoreLoader() = default;
	~CoreLoader();
	CoreLoader(const CoreLoader &) = delete;
	CoreLoader &operator=(const CoreLoader &) = delete;

	// Resolves every libretro entry point. Returns false and fills LastError()
	// when the library is missing or does not expose the full API.
	bool Open(const std::string &library_path);
	void Close();
	bool IsOpen() const { return handle_ != nullptr; }

	const char *LastError() const { return error_.c_str(); }
	unsigned ApiVersion() const { return api_version_; }
	// Fills name/version/extensions advertised by the core itself.
	bool SystemInfoText(std::string *name, std::string *version,
	                    std::string *valid_extensions) const;

	CoreSymbols *sym = nullptr;   // internal; do not touch

private:
	void *handle_ = nullptr;
	std::string error_;
	unsigned api_version_ = 0;
};

// ---------------------------------------------------------------------------
// 5. Sinks — the only way pixels/sound leave this layer
// ---------------------------------------------------------------------------

enum class PixelFormat { F16, XRGB8888, RGB565 };

// Implemented by the Android GLES sink. Must be callable from the core thread.
class FrameSink {
public:
	virtual ~FrameSink() = default;
	virtual void OnFrame(const void *data, unsigned width, unsigned height,
	                     size_t pitch, PixelFormat format) = 0;
	virtual void OnGeometry(unsigned base_w, unsigned base_h, float aspect) = 0;
};

class AudioSink {
public:
	virtual ~AudioSink() = default;
	virtual void OnAudio(const int16_t *frames, size_t frame_count) = 0;
	virtual int TargetSampleRate() const = 0;
};

// ---------------------------------------------------------------------------
// 6. Environment callback (retro_environment)
// ---------------------------------------------------------------------------

// Translates libretro environment calls into Sphynx state. Unknown commands are
// logged and answered with `false`, which is what a well-behaved core expects.
class Environment {
public:
	Environment();
	~Environment();

	void SetDirectories(const std::string &system_dir, const std::string &save_dir);
	void SetVariables(const std::vector<std::pair<std::string, std::string>> &vars);
	void SetPixelFormat(PixelFormat fmt) { pixel_format_ = fmt; }
	PixelFormat GetPixelFormat() const { return pixel_format_; }
	int UnknownCommands() const { return unknown_commands_; }
	unsigned LastGeometryWidth() const { return geom_w_; }
	unsigned LastGeometryHeight() const { return geom_h_; }
	float LastAspect() const { return aspect_; }
	bool SupportsNoGame() const { return supports_no_game_; }
	bool NeedsFullpath() const { return need_fullpath_; }
	void LogSummary() const;   // prints what the core asked for (dev builds)

	// Entry point handed to retro_set_environment(). libretro passes no
	// userdata, so the instance is held in a file-static slot set by Session
	// (one external core per process, by design).
	static bool Trampoline(unsigned cmd, void *data);
	// Called by Session right before retro_init().
	static void BindActiveInstance(Environment *env);

private:
	bool Callback(unsigned cmd, void *data, void *userdata);
	std::string system_dir_, save_dir_;
	std::vector<std::pair<std::string, std::string>> variables_;
	PixelFormat pixel_format_ = PixelFormat::RGB565;
	unsigned geom_w_ = 0, geom_h_ = 0;
	float aspect_ = 0.0f;
	int unknown_commands_ = 0;
	bool supports_no_game_ = false;
	bool need_fullpath_ = false;
};

// ---------------------------------------------------------------------------
// 7. Session — one running external core
// ---------------------------------------------------------------------------

struct SessionStats {
	uint64_t frames = 0;
	uint64_t audio_frames = 0;
	double last_frame_ms = 0.0;
	double worst_frame_ms = 0.0;
	uint64_t watchdog_trips = 0;
};

class Session {
public:
	Session();
	~Session();
	Session(const Session &) = delete;
	Session &operator=(const Session &) = delete;

	// `content_path` is the game file/dir; `core_path` the plug-in .so.
	bool Start(const std::string &core_path, const std::string &content_path,
	           const std::string &system_dir, const std::string &save_dir,
	           std::string *error);
	// Runs exactly one frame. Returns false when the core asks to stop or when
	// the watchdog fired.
	bool RunFrame();
	void Stop();
	bool IsRunning() const { return running_; }

	// Save states go through the core, never through PPSSPP's PSP savestates.
	size_t StateSize() const;
	bool SaveState(std::vector<uint8_t> *out);
	bool LoadState(const std::vector<uint8_t> &in);

	void SetFrameSink(FrameSink *sink) { frame_sink_ = sink; }
	void SetAudioSink(AudioSink *sink) { audio_sink_ = sink; }
	void SetWatchdogMs(int ms) { watchdog_ms_ = ms; }

	// Where button state comes from. The Android layer forwards PPSSPP's own
	// pad (on-screen + physical) through this hook, so every external system
	// inherits PPSSPP's button mapping unchanged.
	using InputSource = std::function<int16_t(unsigned port, unsigned device,
	                                          unsigned index, unsigned id)>;
	void SetInputSource(InputSource src) { input_ = std::move(src); }
	const SessionStats &Stats() const { return stats_; }
	// Sample rate advertised by the core (Hz). Valid after Start().
	double SampleRate() const { return sample_rate_; }
	// Frame rate advertised by the core (Hz). Valid after Start(). The player
	// screen paces retro_run() with this, so a 50 Hz PAL title does not run 20%
	// too fast just because the UI happens to tick at 60 Hz.
	double FrameRate() const { return frame_rate_; }
	// Battery-backed memory (libretro RETRO_MEMORY_SAVE_RAM). The player copies
	// this to disk so in-game saves survive closing the app. nullptr when the
	// core has none.
	void *MemoryData(unsigned id, size_t *size);
	const Environment &Env() const { return env_; }
	bool LastRunFailed() const { return last_run_failed_; }
	const char *LastError() const { return error_.c_str(); }

private:
	CoreLoader loader_;
	Environment env_;
	FrameSink *frame_sink_ = nullptr;
	AudioSink *audio_sink_ = nullptr;
	InputSource input_;
	SessionStats stats_;
	std::string system_dir_, save_dir_, content_;
	std::string error_;
	bool running_ = false;
	bool last_run_failed_ = false;
	int watchdog_ms_ = 3000;
	double sample_rate_ = 44100.0;
	double frame_rate_ = 60.0;
};

// ---------------------------------------------------------------------------
// 8. Diagnostics
// ---------------------------------------------------------------------------

// One-line build banner, printed by the host test and by the Android log.
const char *BuildId();
// Self-check used by CI: verifies registry integrity (35 systems, unique keys,
// every system maps to a known core, PSP excluded from routing).
bool SelfCheck(std::string *report);

}  // namespace sphynx
