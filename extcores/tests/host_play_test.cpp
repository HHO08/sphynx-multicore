// ============================================================================
//  Sphynx — extcores/tests/host_play_test.cpp
//
//  End-to-end test of "press the game in the list and it plays", using the same
//  code paths the device runs, but driven from a desktop toolchain:
//
//    1. Android storage resolution
//         content://…/primary%3APSP%2Fgame.sfc  ->  /storage/emulated/0/PSP/game.sfc
//         (this is the bug that made every non-PSP game fail to start)
//    2. game-list classification of a URI, a plain path and an archive
//    3. thumbnail generation, driven through a real dlopen()ed core, producing a
//       PNG whose header is valid
//    4. the player session: frames, audio, input, save state
//
//  Only the Android GL surface / Activity glue is untested here — everything
//  below it is the same object code that ships in the APK.
//
//  Usage:  host_play_test /path/to/libstub_core.so
//  SPDX-License-Identifier: GPL-2.0-or-later
// ============================================================================
#include "sphynx_extcore.h"
#include "sphynx_ui_api.h"
#include "../src/android_paths.h"

#include <libretro.h>

#include <cstdio>
#include <cstdlib>
#include <limits.h>
#include <cstring>
#include <string>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

namespace {

int g_failures = 0;
int g_checks = 0;

void Check(bool cond, const std::string &what) {
	++g_checks;
	if (cond) {
		std::printf("  PASS %s\n", what.c_str());
	} else {
		std::printf("  FAIL %s\n", what.c_str());
		++g_failures;
	}
}

std::string g_root;   // temp workspace

std::string TempDir() {
	char tmpl[] = "/tmp/sphynx_play_XXXXXX";
	const char *d = mkdtemp(tmpl);
	return d ? std::string(d) : std::string("/tmp/sphynx_play");
}

void MkDirs(const std::string &path) {
	std::string acc;
	for (size_t i = 0; i <= path.size(); ++i) {
		if (i == path.size() || path[i] == '/') {
			if (!acc.empty()) ::mkdir(acc.c_str(), 0777);
		}
		if (i < path.size()) acc.push_back(path[i]);
	}
}

void WriteFile(const std::string &path, const std::string &bytes) {
	FILE *f = std::fopen(path.c_str(), "wb");
	if (!f) return;
	std::fwrite(bytes.data(), 1, bytes.size(), f);
	std::fclose(f);
}

// --- 1. storage resolution ---------------------------------------------------
void TestStorageResolution() {
	std::printf("[1] Android URI -> file resolution\n");
	::setenv("SPHYNX_STORAGE_ROOT", (g_root + "/storage").c_str(), 1);

	// What PPSSPP actually hands the game list when the folder was picked with
	// the system file picker.
	const std::string uri =
		"content://com.android.externalstorage.documents/tree/primary%3APSP%2FGAME"
		"/document/primary%3APSP%2FGAME%2FYs%20III%20(USA).sfc";
	std::string docid;
	Check(sphynx::DocumentIdFromUri(uri, &docid), "document id extracted from a tree URI");
	Check(docid == "primary:PSP/GAME/Ys III (USA).sfc", "document id decoded (id=" + docid + ")");

	std::string mapped;
	Check(sphynx::MapDocumentIdToPath(docid, &mapped), "document id mapped");
	Check(mapped == g_root + "/storage/emulated/0/PSP/GAME/Ys III (USA).sfc",
	      "primary: volume maps to emulated/0 (got " + mapped + ")");

	std::string raw;
	Check(sphynx::MapDocumentIdToPath("raw:/sdcard/roms/x.gba", &raw) &&
	          raw == "/sdcard/roms/x.gba", "raw: document id maps through");

	std::string sdcard;
	Check(sphynx::MapDocumentIdToPath("12D4-3F2A:roms/x.gba", &sdcard) &&
	          sdcard == g_root + "/storage/12D4-3F2A/roms/x.gba",
	      "SD-card volume maps to its UUID mount");

	std::string nope;
	Check(!sphynx::MapDocumentIdToPath("msf:1000001095", &nope),
	      "Downloads/media ids are reported as unmappable (they need a copy)");

	Check(sphynx::DisplayNameOf(uri) == "Ys III (USA).sfc", "display name from a URI");
	Check(sphynx::DisplayNameOf("/a/b/game.gba") == "game.gba", "display name from a path");

	// A real file behind a URI must resolve to a readable path.
	const std::string rom = g_root + "/storage/emulated/0/PSP/GAME/Ys III (USA).sfc";
	MkDirs(rom.substr(0, rom.rfind('/')));
	WriteFile(rom, std::string(4096, '\0'));
	Check(sphynx::LocalPathFor(uri) == rom, "LocalPathFor() resolves the whole URI");
}

// --- 2. game-list classification --------------------------------------------
void TestClassification() {
	std::printf("[2] game-list classification\n");
	const std::string rom = g_root + "/storage/emulated/0/PSP/GAME/Ys III (USA).sfc";
	const std::string uri =
		"content://com.android.externalstorage.documents/tree/primary%3APSP%2FGAME"
		"/document/primary%3APSP%2FGAME%2FYs%20III%20(USA).sfc";

	Check(sphynx_ui_is_external(rom.c_str()) == 1, "a plain .sfc file is an external system");
	Check(sphynx_ui_is_external(uri.c_str()) == 1, "the same game via content:// is recognised");
	Check(sphynx_ui_is_external("/sdcard/x.zip") == -1, "archives stay with PPSSPP");
	Check(sphynx_ui_is_external("/sdcard/EBOOT.PBP") == 0, "PSP files are never hijacked");

	const std::string filter = sphynx_ui_extension_filter();
	Check(filter.find(":sfc:") != std::string::npos, "listing filter contains .sfc");
}

// --- 3. thumbnails through a real core --------------------------------------
void TestThumbnail(const std::string &stub_core, const std::string &nativelib,
                   const std::string &sysdir, const std::string &savedir) {
	std::printf("[3] thumbnail rendered by the game's own core\n");

	// The host looks cores up as <nativeLibraryDir>/libsphynx_core_<name>.so,
	// exactly as on the device. The stub stands in for bsnes here.
	const std::string link = nativelib + "/libsphynx_core_bsnes.so";
	::unlink(link.c_str());
	// Absolute target: CI passes a relative "./out/libstub_core.so" and a
	// relative symlink would resolve against the temp directory instead.
	char resolved[4096];
	const std::string target = realpath(stub_core.c_str(), resolved) ? std::string(resolved) : stub_core;
	if (::symlink(target.c_str(), link.c_str()) != 0) {
		Check(false, "could not place the stub core in the native lib dir");
		return;
	}

	sphynx_ui_set_paths(nativelib.c_str(), sysdir.c_str(), savedir.c_str());
	const std::string rom = g_root + "/storage/emulated/0/PSP/GAME/Ys III (USA).sfc";

	unsigned char *png = nullptr;
	int len = 0;
	const bool ok = sphynx_ui_make_thumbnail(rom.c_str(), &png, &len) == 1;
	Check(ok, std::string("thumbnail generated (") + sphynx_ui_last_error() + ")");
	if (!ok || !png) return;

	static const unsigned char kSig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
	Check(len > 60, "thumbnail has data");
	Check(std::memcmp(png, kSig, 8) == 0, "thumbnail is a valid PNG");
	Check(len > 24 && std::memcmp(png + 12, "IHDR", 4) == 0, "thumbnail has an IHDR chunk");
	sphynx_ui_free(png);
}

// --- 4. the player session ---------------------------------------------------
struct CountingSink : sphynx::FrameSink {
	int frames = 0;
	unsigned w = 0, h = 0;
	void OnGeometry(unsigned, unsigned, float) override {}
	void OnFrame(const void *, unsigned width, unsigned height, size_t,
	             sphynx::PixelFormat) override {
		++frames;
		w = width;
		h = height;
	}
};

struct AudioCounter : sphynx::AudioSink {
	size_t frames = 0;
	void OnAudio(const int16_t *, size_t count) override { frames += count; }
	int TargetSampleRate() const override { return 44100; }
};

void TestPlayerSession(const std::string &stub_core, const std::string &sysdir,
                       const std::string &savedir) {
	std::printf("[4] player session (frames, audio, input, save state)\n");
	const std::string rom = g_root + "/storage/emulated/0/PSP/GAME/Ys III (USA).sfc";

	CountingSink frames;
	AudioCounter audio;
	sphynx::Session session;
	session.SetFrameSink(&frames);
	session.SetAudioSink(&audio);
	session.SetWatchdogMs(5000);
	int16_t start_held = 0;
	session.SetInputSource([&](unsigned port, unsigned device, unsigned, unsigned id) -> int16_t {
		if (port || device != RETRO_DEVICE_JOYPAD) return 0;
		if (id == RETRO_DEVICE_ID_JOYPAD_START) return start_held;
		return 0;
	});

	std::string err;
	Check(session.Start(stub_core, rom, sysdir, savedir, &err),
	      "session starts on a resolved file path" + (err.empty() ? "" : " (" + err + ")"));

	for (int i = 0; i < 60; ++i) session.RunFrame();
	Check(frames.frames == 60, "60 frames reached the frame sink");
	Check(frames.w == 160 && frames.h == 144, "frame size is the core's geometry");
	Check(audio.frames > 0, "audio reached the audio sink");

	// Input plumbing: the frontend must be able to raise a button for the core.
	start_held = 1;
	session.RunFrame();
	start_held = 0;

	const size_t state_size = session.StateSize();
	Check(state_size > 0, "core exposes a save state");
	std::vector<uint8_t> state;
	Check(session.SaveState(&state) && state.size() == state_size, "save state written");
	Check(session.LoadState(state), "save state restored");

	session.Stop();
	Check(!session.IsRunning(), "session stops cleanly");
}

}  // namespace

int main(int argc, char **argv) {
	if (argc < 2) {
		std::printf("usage: %s /path/to/libstub_core.so\n", argv[0]);
		return 2;
	}
	const std::string stub_core = argv[1];
	g_root = TempDir();
	MkDirs(g_root + "/nativelib");
	MkDirs(g_root + "/sys");
	MkDirs(g_root + "/save");
	MkDirs(g_root + "/storage/emulated/0/PSP/GAME");

	std::printf("Sphynx play test (%s)\nworkspace: %s\n", sphynx::BuildId(), g_root.c_str());

	TestStorageResolution();
	TestClassification();
	TestThumbnail(stub_core, g_root + "/nativelib", g_root + "/sys", g_root + "/save");
	TestPlayerSession(stub_core, g_root + "/sys", g_root + "/save");

	std::printf("\n%d checks, %d failure(s) — %s\n", g_checks, g_failures,
	            g_failures ? "FAILED" : "all good");
	return g_failures ? 1 : 0;
}
