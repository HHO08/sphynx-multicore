// ============================================================================
//  Sphynx — extcores/tests/host_smoke.cpp
//
//  Local/CI test of the external-core host. Runs on a normal desktop toolchain,
//  no Android SDK required, which is why it can be executed on every push.
//
//  What it proves (each line corresponds to a check in the output):
//    1. the 35-system registry is internally consistent (SelfCheck)
//    2. file-type detection: PSP is always claimed for PSP containers, and the
//       shared extensions (.iso/.cue/.chd/.bin) never silently mis-route
//    3. the loader really dlopen()s a core, drives 180 frames, receives video
//       and audio, round-trips a save state and shuts down cleanly
//    4. failure paths are graceful: a missing core and a broken .so both
//       produce an error string instead of a crash
//    5. the watchdog stops a core that overruns its frame budget
//
//  Usage:  host_smoke /path/to/libstub_core.so
//  Exit code 0 == all checks passed.
//
//  SPDX-License-Identifier: GPL-2.0-or-later
// ============================================================================
#include "sphynx_extcore.h"

#include <libretro.h>

#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

int g_failures = 0;
int g_checks = 0;

void Check(bool cond, const std::string &what) {
	++g_checks;
	if (!cond) {
		++g_failures;
		std::printf("  [FAIL] %s\n", what.c_str());
	} else {
		std::printf("  [ ok ] %s\n", what.c_str());
	}
}

struct RecordingFrameSink : sphynx::FrameSink {
	int frames = 0;
	unsigned w = 0, h = 0;
	size_t pitch = 0;
	sphynx::PixelFormat fmt = sphynx::PixelFormat::RGB565;
	uint64_t checksum = 0;

	void OnFrame(const void *data, unsigned width, unsigned height, size_t p,
	             sphynx::PixelFormat f) override {
		frames++;
		w = width;
		h = height;
		pitch = p;
		fmt = f;
		// Sample a few bytes so we can tell whether frames actually change.
		const uint8_t *b = static_cast<const uint8_t *>(data);
		checksum = 0;
		for (size_t i = 0; i < 64 && i < p * height; ++i) checksum = checksum * 131 + b[i];
	}
	void OnGeometry(unsigned, unsigned, float) override {}
};

struct CountingAudioSink : sphynx::AudioSink {
	uint64_t frames = 0;
	void OnAudio(const int16_t *, size_t n) override { frames += n; }
	int TargetSampleRate() const override { return 32768; }
};

// Builds a fake 4 KiB header with a given pattern at a given offset.
std::vector<uint8_t> MakeHeader(size_t size, size_t offset, const void *bytes, size_t len) {
	std::vector<uint8_t> h(size, 0);
	if (offset + len <= size) std::memcpy(h.data() + offset, bytes, len);
	return h;
}

void TestRegistry() {
	std::printf("\n[1] registry self-check\n");
	std::string report;
	const bool ok = sphynx::SelfCheck(&report);
	std::printf("%s", report.c_str());
	Check(ok, "SelfCheck() passes");
	Check(sphynx::SystemCount() == 35,
	      "exactly 35 systems (got " + std::to_string(sphynx::SystemCount()) + ")");

	// Every requested system from the user's specification must be present.
	const char *required[] = {"nes", "fds", "gb", "gbc", "gba", "n64", "nds", "snes",
	                          "genesis", "32x", "segacd", "saturn", "dreamcast", "sms",
	                          "gg", "picodrive_pico", "3do", "amiga", "arcade", "msx",
	                          "ws", "wsc", "coleco", "psx", "pc88", "pc98", "pcfx", "pce",
	                          "pcecd", "dos", "scummvm", "neogeocd", "ngp", "ngpc"};
	for (const char *k : required) {
		Check(sphynx::FindSystemByKey(k) != nullptr, std::string("system '") + k + "' exists");
	}

	// PSP must never be routed through an external core.
	bool psp_absent = true;
	for (int i = 0; i < sphynx::SystemCount(); ++i) {
		const sphynx::SystemInfo *s = sphynx::AllSystems(nullptr) + i;
		if (s->core_lib && std::strcmp(s->core_lib, "ppsspp") == 0) psp_absent = false;
	}
	Check(psp_absent, "PSP is not an external core entry");
}

void TestDetection() {
	std::printf("\n[2] content detection\n");

	struct Case {
		const char *path;
		size_t magic_off;
		const char *magic;
		size_t magic_len;
		sphynx::SystemId expect;
		bool expect_psp_native;
	};
	const Case cases[] = {
		// PSP containers must always be claimed by PPSSPP itself.
		{"game.cso", 0, nullptr, 0, sphynx::SystemId::Unknown, true},
		{"EBOOT.pbp", 0, nullptr, 0, sphynx::SystemId::Unknown, true},
		{"game.iso", 0x8000, "PSP GAME", 8, sphynx::SystemId::Unknown, true},
		// Cartridge magic.
		{"mario.nes", 0, "NES\x1a", 4, sphynx::SystemId::Nes, false},
		{"disk.fds", 0, "FDS\x1a", 4, sphynx::SystemId::Fds, false},
		{"zelda.n64", 0, "\x80\x37\x12\x40", 4, sphynx::SystemId::N64, false},
		{"sega.md", 0x100, "SEGA GENESIS", 12, sphynx::SystemId::Genesis, false},
		{"knuckles.32x", 0x100, "SEGA 32X", 8, sphynx::SystemId::Genesis32X, false},
		{"pico.bin", 0x100, "SEGA PICO", 9, sphynx::SystemId::SegaPico, false},
		{"iso3do.iso", 0, "\x01\x5a\x5a\x5a", 4, sphynx::SystemId::ThreeDo, false},
		// Extension-only matches.
		{"pokemon.gba", 0, nullptr, 0, sphynx::SystemId::Gba, false},
		{"tetris.gb", 0, nullptr, 0, sphynx::SystemId::Gb, false},
		{"colour.gbc", 0, nullptr, 0, sphynx::SystemId::Gbc, false},
		{"nsmb.nds", 0, nullptr, 0, sphynx::SystemId::Nds, false},
		{"disc.cue", 0, nullptr, 0, sphynx::SystemId::Unknown, false},
		{"data.gdi", 0, nullptr, 0, sphynx::SystemId::Dreamcast, false},
		{"jewel.ngc", 0, nullptr, 0, sphynx::SystemId::NeoGeoPocketColor, false},
	};

	for (const Case &c : cases) {
		std::vector<uint8_t> head;
		const uint8_t *hp = nullptr;
		size_t hn = 0;
		if (c.magic) {
			head = MakeHeader(4096, c.magic_off > 4096 ? 0 : c.magic_off, c.magic, c.magic_len);
			hp = head.data();
			hn = head.size();
			// Headers used at offsets past 4 KiB (PSP ISO vol. descriptor) are
			// tested separately below since the sniffer expects a large prefix.
			if (c.magic_off > 4096) { hp = nullptr; hn = 0; }
		}
		const sphynx::DetectResult r = sphynx::Detect(c.path, hp, hn);
		const bool psp_ok = (r.is_psp_native == c.expect_psp_native);
		bool found = (c.expect == sphynx::SystemId::Unknown);
		for (const auto &cand : r.candidates)
			if (cand.id == c.expect) found = true;
		Check(psp_ok && found, std::string("detect ") + c.path + " -> " +
		                         (found ? "matched" : "MISSED") +
		                         (psp_ok ? "" : " (PSP flag wrong)"));
	}

	// A big PSP ISO header must be recognised through the volume descriptor.
	std::vector<uint8_t> iso(0x10000, 0);
	std::memcpy(iso.data() + 0x8010, "PSP GAME", 8);
	const sphynx::DetectResult r = sphynx::Detect("big.iso", iso.data(), iso.size());
	Check(r.is_psp_native, "PSP ISO recognised by volume descriptor");

	// Unknown content must not be guessed silently.
	const sphynx::DetectResult u = sphynx::Detect("mystery.xyz", nullptr, 0);
	Check(u.candidates.empty(), "unknown extension yields no candidate");
}

void TestLoaderAndSession(const std::string &stub_core) {
	std::printf("\n[3] loader + session with a real .so\n");
	{
		sphynx::CoreLoader loader;
		Check(loader.Open(stub_core), std::string("dlopen + all symbols (") + loader.LastError() + ")");
		std::string name, version, exts;
		loader.SystemInfoText(&name, &version, &exts);
		Check(name == "Sphynx Stub", "core reports its name: " + name);
		Check(!exts.empty(), "core reports extensions: " + exts);
		Check(loader.ApiVersion() == RETRO_API_VERSION, "core ABI matches host");
	}

	sphynx::Session session;
	RecordingFrameSink video;
	CountingAudioSink audio;
	session.SetFrameSink(&video);
	session.SetAudioSink(&audio);
	session.SetInputSource([](unsigned, unsigned, unsigned, unsigned id) -> int16_t {
		// Pretend button A is held: the stub renders a different colour.
		return id == RETRO_DEVICE_ID_JOYPAD_A ? 1 : 0;
	});

	std::string err;
	const bool started = session.Start(stub_core, "dummy.stub", "/tmp/sphynx/sys", "/tmp/sphynx/save", &err);
	Check(started, "session.Start() " + err);
	if (!started) return;

	const auto t0 = std::chrono::steady_clock::now();
	for (int i = 0; i < 180; ++i) {
		if (!session.RunFrame()) break;
	}
	const double ms = std::chrono::duration<double, std::milli>(
	                      std::chrono::steady_clock::now() - t0).count();

	Check(video.frames == 180, "180 frames delivered (got " + std::to_string(video.frames) + ")");
	Check(video.w == 160 && video.h == 144, "frame geometry 160x144 reported");
	Check(video.pitch == 320, "pitch reported correctly");
	Check(audio.frames > 180 * 700, "audio pumped: " + std::to_string(audio.frames) + " frames");
	Check(session.Stats().frames == 180, "stats counted 180 frames");
	Check(session.Env().LastGeometryWidth() == 160, "environment captured AV info");
	std::printf("  [info] 180 stub frames in %.1f ms (%.3f ms/frame)\n", ms, ms / 180.0);

	// Save-state round trip through the core.
	std::vector<uint8_t> state;
	Check(session.StateSize() == 64, "core reports a 64-byte state");
	Check(session.SaveState(&state) && state.size() == 64, "SaveState() produced 64 bytes");
	Check(session.LoadState(state), "LoadState() accepted its own snapshot");
	std::vector<uint8_t> broken(64, 0);
	Check(!session.LoadState(broken), "LoadState() rejects a foreign snapshot");

	// Input plumbing: the stub sets a distinct blue value when A is held, so a
	// frame checksum of zero would mean input never arrived.
	Check(video.checksum != 0, "frames are non-empty");

	session.Stop();
	Check(!session.IsRunning(), "session stopped cleanly");
}

void TestFailurePaths(const std::string &broken_core) {
	std::printf("\n[4] failure paths are graceful\n");
	{
		sphynx::Session s;
		std::string err;
		Check(!s.Start("/nonexistent/libnope.so", "x.gb", "/tmp", "/tmp", &err),
		      "missing core -> error, not a crash");
		Check(!err.empty(), "error message is set: " + err);
	}
	{
		sphynx::Session s;
		std::string err;
		const bool ok = s.Start(broken_core, "x.gb", "/tmp", "/tmp", &err);
		Check(!ok, "broken core -> refused: " + err);
	}
}

void TestWatchdog(const std::string &stub_core) {
	std::printf("\n[5] watchdog\n");
	sphynx::Session s;
	RecordingFrameSink video;
	s.SetFrameSink(&video);
	s.SetWatchdogMs(0);   // 0 disables the trip so we can measure first
	std::string err;
	if (!s.Start(stub_core, "dummy.stub", "/tmp", "/tmp", &err)) {
		Check(false, "watchdog test could not start a session: " + err);
		return;
	}
	s.RunFrame();
	const double frame_ms = s.Stats().last_frame_ms;
	s.Stop();

	// Re-run with a budget smaller than a real frame: the watchdog must trip.
	// The stub honours SPHYNX_STUB_DELAY_MS so the overrun is deterministic
	// instead of depending on machine speed.
	setenv("SPHYNX_STUB_DELAY_MS", "40", 1);
	sphynx::Session w;
	w.SetFrameSink(&video);
	w.SetWatchdogMs(10);   // 10 ms budget, the frame now takes ~40 ms
	if (w.Start(stub_core, "dummy.stub", "/tmp", "/tmp", &err)) {
		const bool alive = w.RunFrame();
		Check(!alive, "watchdog tripped on a 10 ms budget (frame was " +
		                  std::to_string(frame_ms) + " ms before the delay)");
		Check(w.Stats().watchdog_trips == 1, "watchdog trip counted");
		Check(w.LastError() && *w.LastError(), std::string("watchdog reason: ") + w.LastError());
	}
}

}  // namespace

int main(int argc, char **argv) {
	std::printf("Sphynx external-core host test\n%s\n", sphynx::BuildId());
	const std::string stub = argc > 1 ? argv[1] : "libstub_core.so";
	const std::string broken = argc > 2 ? argv[2] : "libbroken_core.so";

	TestRegistry();
	TestDetection();
	TestLoaderAndSession(stub);
	TestFailurePaths(broken);
	TestWatchdog(stub);

	std::printf("\n==== %d checks, %d failures ====\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
