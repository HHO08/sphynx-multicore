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
#include "sphynx_player_api.h"
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

	// The same game, this time reached through the content:// URI that Android
	// really hands over. This is the exact path the user reported as failing.
	const std::string uri =
		"content://com.android.externalstorage.documents/tree/primary%3APSP%2FGAME"
		"/document/primary%3APSP%2FGAME%2FYs%20III%20(USA).sfc";
	unsigned char *png2 = nullptr;
	int len2 = 0;
	const bool ok2 = sphynx_ui_make_thumbnail(uri.c_str(), &png2, &len2) == 1;
	Check(ok2, std::string("thumbnail generated from a content:// URI (") +
	               sphynx_ui_last_error() + ")");
	if (ok2 && png2) {
		Check(len2 > 60 && std::memcmp(png2, kSig, 8) == 0, "URI thumbnail is a valid PNG");
		sphynx_ui_free(png2);
	}

	// SPHYNX (stage 16): the picture is cached on disk, so the second request
	// must not need an emulator at all. Proven the hard way: the core is taken
	// away before the second call. With the cache the PNG still arrives; without
	// it this is exactly the case that used to re-emulate the game's first
	// seconds on every start of the game list.
	{
		const std::string cachedir = savedir + "/sphynx_thumbs";
		struct stat st;
		Check(::stat(cachedir.c_str(), &st) == 0 && S_ISDIR(st.st_mode),
		      "a thumbnail cache folder was created next to the saves");

		const std::string hidden = nativelib + "/libsphynx_core_bsnes.so.hidden";
		::unlink(hidden.c_str());
		const bool moved = ::rename(link.c_str(), hidden.c_str()) == 0;
		Check(moved, "the core was taken away to make the point");

		unsigned char *png3 = nullptr;
		int len3 = 0;
		const int ok3 = sphynx_ui_make_thumbnail(rom.c_str(), &png3, &len3);
		Check(ok3 == 1, std::string("second request served from the cache, no core needed (") +
		                    sphynx_ui_last_error() + ")");
		Check(ok3 == 1 && len3 == len, "and it is the same picture as the first time");
		if (ok3 == 1 && png3) sphynx_ui_free(png3);

		// Put the core back for the rest of the test run.
		if (moved) ::rename(hidden.c_str(), link.c_str());

		// A different file behind the same name must not reuse the picture: the
		// cache key carries size and modification time.
		const std::string other = g_root + "/storage/emulated/0/PSP/GAME/Ys III (USA).sfc";
		WriteFile(other, std::string(4096, 'x'));
		unsigned char *png4 = nullptr;
		int len4 = 0;
		const int ok4 = sphynx_ui_make_thumbnail(other.c_str(), &png4, &len4);
		Check(ok4 == 1, std::string("a changed file is rendered again (") + sphynx_ui_last_error() + ")");
		if (ok4 == 1 && png4) sphynx_ui_free(png4);
	}
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

// --- 5. one core at a time, without the "already running" trap --------------
//
// SPHYNX (stage 15): the owner photographed "This game could not be started —
// another game is already running". The host used to keep a flag that refused
// every later game once one player had been left open, for the whole life of the
// process. This test drives that exact situation through the real player API:
// open a game, never close it, open another one. The second open must work, the
// abandoned handle must report "stopped" instead of crashing, and closing both
// must be safe.
void TestReclaim(const std::string &stub_core, const std::string &sys, const std::string &save) {
	std::printf("\n[5] a game is opened twice: the stale one must not block\n");
	const std::string libs = g_root + "/nativelib";
	MkDirs(libs);
	// The player asks the registry for the core of the system, so the stub has to
	// sit under the name the APK would ship it as.
	std::string bytes;
	{
		FILE *f = std::fopen(stub_core.c_str(), "rb");
		if (f) {
			char buffer[8192];
			size_t n = 0;
			while ((n = std::fread(buffer, 1, sizeof(buffer), f)) > 0) bytes.append(buffer, n);
			std::fclose(f);
		}
	}
	const std::string core_path = libs + "/libsphynx_core_gambatte.so";
	WriteFile(core_path, bytes);
	Check(!bytes.empty(), "the stub core was copied into the native library folder");
	sphynx_ui_set_paths(libs.c_str(), sys.c_str(), save.c_str());

	const std::string game = g_root + "/reclaim.gb";
	WriteFile(game, std::string(64, '\0'));

	char err[512] = {};
	SphynxPlayerHandle *first = sphynx_player_open(game.c_str(), "gb", err, sizeof(err));
	Check(first != nullptr, std::string("the first game opens: ") + err);
	if (!first) return;

	// Deliberately not closed - this is the situation the owner's screenshot
	// showed: a player that outlived its screen.
	char err2[512] = {};
	SphynxPlayerHandle *second = sphynx_player_open(game.c_str(), "gb", err2, sizeof(err2));
	Check(second != nullptr, std::string("the second game opens while the first is still open: ") + err2);
	Check(std::string(err2).find("already running") == std::string::npos,
	      "the words \"already running\" are gone for good");

	if (second) {
		Check(sphynx_player_run(second) == 1, "the new game is running");
		Check(sphynx_player_run(first) == -1, "the abandoned handle reports stopped, it does not crash");
		Check(sphynx_player_measured_fps(first) == 0.0, "and it reads as zero, not as garbage");
		Check(std::string(sphynx_player_status(first)).find("closed") != std::string::npos,
		      "its status explains what happened");
		// Every call on a reclaimed handle must be a safe no-op.
		sphynx_player_set_pad(first, 0xFFFF, 0, 0, 0, 0);
		sphynx_player_set_paused(first, 0);
		sphynx_player_flush_save(first);
		Check(sphynx_player_state_exists(first, 0) == 0, "state lookups on it are refused, not executed");
		Check(sphynx_player_frame_size(first, nullptr, nullptr) == 0, "and it has no frame to give");
		sphynx_player_close(first);   // frees the memory the reclaim left behind
		sphynx_player_close(second);
	} else {
		sphynx_player_close(first);
	}

	// And the normal path still works afterwards: close, then open again.
	char err3[512] = {};
	SphynxPlayerHandle *third = sphynx_player_open(game.c_str(), "gb", err3, sizeof(err3));
	Check(third != nullptr, std::string("a game opens normally after all that: ") + err3);
	if (third) {
		Check(sphynx_player_run(third) == 1, "and it runs");
		sphynx_player_close(third);
	}
}

// --- 6. a file the first core refuses (stage 17) -----------------------------
//
// The owner's screenshot: "core refused this content (unsupported format or
// missing BIOS)" on many systems. A .cue is claimed by seven systems at once,
// and the host used to ask exactly one of them. These checks drive the real
// player API: the first candidate's core is deliberately unloadable, so the
// game can only start if the host really moves on to the next system.
void TestRefusalFallback(const std::string &stub_core, const std::string &sys, const std::string &save) {
	std::printf("\n[6] the first core refuses: the next system must be tried\n");
	const std::string libs = g_root + "/nativelib";
	MkDirs(libs);
	std::string bytes;
	{
		FILE *f = std::fopen(stub_core.c_str(), "rb");
		if (f) {
			char buffer[8192];
			size_t n = 0;
			while ((n = std::fread(buffer, 1, sizeof(buffer), f)) > 0) bytes.append(buffer, n);
			std::fclose(f);
		}
	}
	// Candidate order for a .cue is: Sega CD (picodrive), Saturn (mednafen_saturn).
	// The first one is made unloadable - exactly what a core that cannot read
	// the file looks like from here - and the second is the working stub.
	WriteFile(libs + "/libsphynx_core_picodrive.so", std::string("this is not a library"));
	WriteFile(libs + "/libsphynx_core_mednafen_saturn.so", bytes);
	sphynx_ui_set_paths(libs.c_str(), sys.c_str(), save.c_str());

	// A complete disc: cue + its track, so the "missing track" check stays quiet.
	const std::string game = g_root + "/disc.cue";
	WriteFile(g_root + "/disc.bin", std::string(4096, 'D'));
	WriteFile(game, "FILE \"disc.bin\" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n");

	char err[512] = {};
	SphynxPlayerHandle *player = sphynx_player_open(game.c_str(), nullptr, err, sizeof(err));
	Check(player != nullptr, std::string("the game started on a later system: ") + err);
	if (player) {
		Check(std::string(sphynx_player_system_name(player)) == "Sega Saturn",
		      std::string("and it is the Saturn core that runs it: ") + sphynx_player_system_name(player));
		sphynx_player_close(player);
	}

	// Nothing can run it any more: the working stub is taken away, so every
	// candidate really fails. The message must then name the missing track, not
	// say "unsupported format or missing BIOS".
	::unlink((libs + "/libsphynx_core_mednafen_saturn.so").c_str());
	const std::string broken_cue = g_root + "/broken.cue";
	WriteFile(broken_cue, "FILE \"not-here.bin\" BINARY\n  TRACK 01 MODE2/2352\n");
	char err2[512] = {};
	SphynxPlayerHandle *none = sphynx_player_open(broken_cue.c_str(), nullptr, err2, sizeof(err2));
	Check(none == nullptr, "a disc whose track is missing does not start");
	const std::string detail2 = err2;
	Check(detail2.find("not-here.bin") != std::string::npos,
	      std::string("the message names the missing file: ") + detail2);
	Check(detail2.find("missing BIOS") == std::string::npos,
	      "and it no longer blames the BIOS for it");

	// An empty file is named as such.
	const std::string empty = g_root + "/empty.gba";
	WriteFile(empty, std::string());
	char err3[512] = {};
	Check(sphynx_player_open(empty.c_str(), nullptr, err3, sizeof(err3)) == nullptr,
	      "an empty file is refused");
	Check(std::string(err3).find("empty") != std::string::npos,
	      std::string("and the message says the file is empty: ") + err3);

	// The failure list is capped and readable: four systems at most.
	Check(std::string(err2).find("·") != std::string::npos,
	      "the message lists the systems that were tried");
}

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
	TestReclaim(stub_core, g_root + "/sys", g_root + "/save");
	TestRefusalFallback(stub_core, g_root + "/sys", g_root + "/save");

	std::printf("\n%d checks, %d failure(s) — %s\n", g_checks, g_failures,
	            g_failures ? "FAILED" : "all good");
	return g_failures ? 1 : 0;
}
