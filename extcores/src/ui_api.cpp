// ============================================================================
//  Sphynx — extcores/src/ui_api.cpp
//
//  Implementation of the tiny C surface PPSSPP's UI calls (sphynx_ui_api.h).
//  Everything expensive (booting a core for a thumbnail) happens here so the
//  UI only ever sees a "yes/no + PNG bytes" answer.
//
//  SPDX-License-Identifier: GPL-2.0-or-later
// ============================================================================
#include "sphynx_extcore.h"
#include "libretro.h"   // RetroPad button ids for the synthetic START press
#include "sphynx_ui_api.h"
#include "png_write.h"
#include "android_paths.h"
#include "bios_layout.h"
#include "diag_log.h"

#include <chrono>
#include <mutex>

#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <cstring>
#include <string>
#include <vector>

namespace {

std::string g_native_lib_dir;
std::string g_system_dir;
std::string g_save_dir;
// PPSSPP's memstick root (".../PSP"): the parent of the SYSTEMS_BIOS tree.
std::string g_memstick_dir;
std::string g_last_error = "none";

struct FrameGrabber : sphynx::FrameSink {
	int frames = 0;
	unsigned w = 0, h = 0;
	sphynx::PixelFormat fmt = sphynx::PixelFormat::RGB565;
	std::vector<uint8_t> rgba;      // last frame converted to RGBA8888

	void OnGeometry(unsigned, unsigned, float) override {}

	void OnFrame(const void *data, unsigned width, unsigned height, size_t pitch,
	             sphynx::PixelFormat format) override {
		if (!data || !width || !height) return;
		frames++;
		w = width;
		h = height;
		fmt = format;
		rgba.resize(size_t(width) * height * 4);
		for (unsigned y = 0; y < height; ++y) {
			const uint8_t *row = static_cast<const uint8_t *>(data) + y * pitch;
			uint8_t *out = rgba.data() + size_t(y) * width * 4;
			switch (format) {
			case sphynx::PixelFormat::RGB565: {
				const uint16_t *p = reinterpret_cast<const uint16_t *>(row);
				for (unsigned x = 0; x < width; ++x) {
					const uint16_t c = p[x];
					// 5-6-5 -> 8-8-8 with bit replication for full range.
					const uint8_t r = uint8_t(((c >> 11) & 0x1F) * 255 / 31);
					const uint8_t g = uint8_t(((c >> 5) & 0x3F) * 255 / 63);
					const uint8_t b = uint8_t((c & 0x1F) * 255 / 31);
					out[x * 4 + 0] = r;
					out[x * 4 + 1] = g;
					out[x * 4 + 2] = b;
					out[x * 4 + 3] = 255;
				}
				break;
			}
			case sphynx::PixelFormat::XRGB8888: {
				const uint32_t *p = reinterpret_cast<const uint32_t *>(row);
				for (unsigned x = 0; x < width; ++x) {
					const uint32_t c = p[x];
					out[x * 4 + 0] = uint8_t(c & 0xFF);
					out[x * 4 + 1] = uint8_t((c >> 8) & 0xFF);
					out[x * 4 + 2] = uint8_t((c >> 16) & 0xFF);
					out[x * 4 + 3] = 255;
				}
				break;
			}
			default: {   // 0RGB1555
				const uint16_t *p = reinterpret_cast<const uint16_t *>(row);
				for (unsigned x = 0; x < width; ++x) {
					const uint16_t c = p[x];
					out[x * 4 + 0] = uint8_t(((c >> 10) & 0x1F) * 255 / 31);
					out[x * 4 + 1] = uint8_t(((c >> 5) & 0x1F) * 255 / 31);
					out[x * 4 + 2] = uint8_t((c & 0x1F) * 255 / 31);
					out[x * 4 + 3] = 255;
				}
				break;
			}
			}
		}
	}
};

std::vector<uint8_t> DownscaleRgba(const std::vector<uint8_t> &src, int sw, int sh,
                                   int dw, int dh) {
	std::vector<uint8_t> dst(size_t(dw) * dh * 4, 0);
	if (sw <= 0 || sh <= 0) return dst;
	for (int y = 0; y < dh; ++y) {
		// Box filter: average the source pixels covering this destination pixel.
		const int y0 = y * sh / dh;
		const int y1 = std::max(y0 + 1, (y + 1) * sh / dh);
		for (int x = 0; x < dw; ++x) {
			const int x0 = x * sw / dw;
			const int x1 = std::max(x0 + 1, (x + 1) * sw / dw);
			uint32_t r = 0, g = 0, b = 0, n = 0;
			for (int sy = y0; sy < y1; ++sy) {
				const uint8_t *row = src.data() + (size_t(sy) * sw + x0) * 4;
				for (int sx = x0; sx < x1; ++sx) {
					r += row[0]; g += row[1]; b += row[2];
					row += 4; ++n;
				}
			}
			if (!n) n = 1;
			uint8_t *o = dst.data() + (size_t(y) * dw + x) * 4;
			o[0] = uint8_t(r / n);
			o[1] = uint8_t(g / n);
			o[2] = uint8_t(b / n);
			o[3] = 255;
		}
	}
	return dst;
}

bool ReadHead(const std::string &path, std::vector<uint8_t> *out, size_t want = 4096) {
	FILE *f = std::fopen(path.c_str(), "rb");
	if (!f) return false;
	out->resize(want);
	const size_t got = std::fread(out->data(), 1, want, f);
	std::fclose(f);
	out->resize(got);
	return true;
}

bool IsArchiveExt(const std::string &path) {
	// Look at the file NAME, so content:// URIs (whose path is percent-encoded
	// and may carry query junk) are classified the same way as plain paths.
	const std::string name = sphynx::DisplayNameOf(path);
	const size_t dot = name.find_last_of('.');
	if (dot == std::string::npos) return false;
	std::string ext = name.substr(dot + 1);
	for (char &c : ext) c = char(tolower((unsigned char)c));
	// Archives keep PPSSPP's own behaviour (install prompt); they are never
	// routed to an arcade/DOS core by guessing from the container.
	return ext == "zip" || ext == "7z" || ext == "rar";
}

const char *SystemKeyFor(const std::string &path) {
	const sphynx::DetectResult r = sphynx::Detect(path, nullptr, 0);
	if (r.is_psp_native || r.candidates.empty()) return nullptr;
	if (r.needs_user_choice) {
		// Ambiguous extension: still pick the best candidate so the file is not
		// hidden from the user; the player screen offers a system picker.
	}
	const sphynx::SystemInfo *info = sphynx::FindSystem(r.candidates.front().id);
	return info ? info->key : nullptr;
}

}  // namespace

extern "C" {

void sphynx_ui_set_paths(const char *native_library_dir, const char *system_dir,
                         const char *save_dir) {
	g_native_lib_dir = native_library_dir ? native_library_dir : "";
	g_system_dir = system_dir ? system_dir : "";
	g_save_dir = save_dir ? save_dir : "";
}

// Same, but also records the process JavaVM: the thumbnail generator runs on a
// PPSSPP worker thread and still has to ask Java for a content:// copy.
void sphynx_ui_set_paths_and_vm(const char *native_library_dir, const char *system_dir,
                                const char *save_dir, void *jni_env) {
	sphynx_ui_set_paths(native_library_dir, system_dir, save_dir);
	sphynx::CaptureJavaVM(jni_env);
}

int sphynx_ui_is_external(const char *path) {
	if (!path || !*path) return -1;
	if (IsArchiveExt(path)) return -1;   // let PPSSPP handle archives itself

	// Android hands us content:// URIs for anything opened through the Storage
	// Access Framework. Detection must see the real bytes, so resolve first;
	// when nothing can be resolved we still answer from the extension alone
	// (an unreadable file keeps its row in the list instead of vanishing).
	// No copying here: the list asks this for every file it shows.
	const std::string local = sphynx::LocalPathFor(path, /*allow_copy=*/false);
	const std::string &probe = local.empty() ? std::string(path) : local;
	std::vector<uint8_t> head;
	const bool have_head = ReadHead(probe, &head);
	const sphynx::DetectResult r =
		sphynx::Detect(probe, have_head && !head.empty() ? head.data() : nullptr, head.size());
	if (r.is_psp_native) return 0;                      // PSP -> PPSSPP itself
	if (r.candidates.empty()) return -1;
	return 1;
}

int sphynx_ui_make_thumbnail(const char *path, unsigned char **out, int *len) {
	if (!path || !out || !len) return 0;
	*out = nullptr;
	*len = 0;

	// One core at a time: libretro's environment callback has a single active
	// instance per process, so two thumbnail jobs must never overlap.
	static std::mutex job_mutex;
	std::lock_guard<std::mutex> job_lock(job_mutex);

	const std::string local = sphynx::LocalPathFor(path);
	if (local.empty()) {
		g_last_error = "file is not readable (cannot resolve the Android URI)";
		return 0;
	}

	const char *key = SystemKeyFor(local);
	if (!key) {
		g_last_error = "no external system for this file";
		return 0;
	}
	const sphynx::SystemInfo *info = sphynx::FindSystemByKey(key);
	const std::string core = sphynx::CoreLibraryPath(g_native_lib_dir, info->core_lib);
	if (core.empty()) {
		g_last_error = std::string("core path unknown for ") + key;
		return 0;
	}

	FrameGrabber grabber;
	sphynx::Session session;
	session.SetFrameSink(&grabber);
	session.SetWatchdogMs(0);   // thumbnailing may be slow on weak devices
	session.SetInputSource([](unsigned, unsigned, unsigned, unsigned id) -> int16_t {
		// Hold Start so most cores leave their title/intro screen quickly.
		return id == RETRO_DEVICE_ID_JOYPAD_START ? 1 : 0;
	});

	std::string err;
	if (!session.Start(core, local, g_system_dir, g_save_dir, &err)) {
		g_last_error = "core start failed: " + err;
		return 0;
	}

	// Run enough frames for the game to leave its boot screen, but stop early
	// once real content appears: prefer a frame that is neither black nor flat.
	int best_frames = 0;
	std::vector<uint8_t> best;
	int best_score = -1;
	// Hard wall-clock budget: a slow core must never stall the game list for
	// long. 6 s is enough for a title screen on every supported system.
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(6);
	for (int i = 0; i < 240; ++i) {
		if (std::chrono::steady_clock::now() > deadline) break;
		if (!session.RunFrame()) break;
		if (grabber.frames == 0) continue;
		if (i < 30) continue;   // let it settle
		// Score: variance-ish measure so a black/blank frame loses.
		int score = 0;
		const auto &px = grabber.rgba;
		for (size_t p = 0; p + 3 < px.size(); p += 4 * 97) {
			score += px[p] + px[p + 1] + px[p + 2];
		}
		if (score > best_score) {
			best_score = score;
			best = grabber.rgba;
			best_frames = i;
		}
		if ((i % 30) == 0 && best_score > 4000) break;
	}
	session.Stop();

	if (best.empty() || grabber.w == 0) {
		g_last_error = "core produced no frames";
		return 0;
	}

	const int tw = 144, th = 80;   // PPSSPP icon slot; keeps the PNG small
	const std::vector<uint8_t> small =
		DownscaleRgba(best, int(grabber.w), int(grabber.h), tw, th);
	const std::vector<uint8_t> png = sphynx::EncodePngRgba(small.data(), tw, th);
	if (png.empty()) {
		g_last_error = "png encode failed";
		return 0;
	}

	*out = static_cast<unsigned char *>(std::malloc(png.size()));
	if (!*out) return 0;
	std::memcpy(*out, png.data(), png.size());
	*len = int(png.size());
	g_last_error = "ok (frame " + std::to_string(best_frames) + ")";
	return 1;
}

void sphynx_ui_free(void *p) { std::free(p); }

char *sphynx_ui_local_path(const char *path) {
	if (!path || !*path) return nullptr;
	const std::string local = sphynx::LocalPathFor(path);
	if (local.empty()) return nullptr;
	char *out = static_cast<char *>(std::malloc(local.size() + 1));
	if (!out) return nullptr;
	std::memcpy(out, local.c_str(), local.size() + 1);
	return out;
}

// "key|Display name;key|Display name;..." for the ambiguous-disc chooser.
const char *sphynx_ui_candidates(const char *path) {
	static std::string out;
	out.clear();
	if (!path || !*path) return out.c_str();
	const std::string local = sphynx::LocalPathFor(path);
	const std::string &probe = local.empty() ? std::string(path) : local;
	std::vector<uint8_t> head;
	const bool have = ReadHead(probe, &head);
	const sphynx::DetectResult r =
		sphynx::Detect(probe, have && !head.empty() ? head.data() : nullptr, head.size());
	bool first = true;
	for (const auto &c : r.candidates) {
		const sphynx::SystemInfo *info = sphynx::FindSystem(c.id);
		if (!info) continue;
		if (!first) out += ';';
		out += info->key;
		out += '|';
		out += info->display_name;
		first = false;
	}
	return out.c_str();
}

const char *sphynx_ui_last_error(void) { return g_last_error.c_str(); }

// ---------------------------------------------------------------------------
//  BIOS tree (PSP/SYSTEMS_BIOS)
// ---------------------------------------------------------------------------

void sphynx_ui_set_memstick(const char *memstick_dir) {
	g_memstick_dir = memstick_dir ? memstick_dir : "";
}

int sphynx_ui_ensure_bios_tree(const char *memstick_dir) {
	if (memstick_dir && *memstick_dir) g_memstick_dir = memstick_dir;
	if (g_memstick_dir.empty()) {
		g_last_error = "no memstick folder: the BIOS tree was not created";
		return -1;
	}
	// The flight recorder lives next to the folders the user can find.
	sphynx::SetLogPath(g_memstick_dir + "/sphynx_log.txt");
	std::string report;
	const int made = sphynx::EnsureBiosTree(g_memstick_dir, &report);
	if (made < 0) {
		g_last_error = "BIOS tree: " + report;
		return -1;
	}
	// Anything the user already keeps in PSP/SYSTEM is copied into the matching
	// per-system folder, so an existing PPSSPP setup needs no manual work.
	std::string adopted_report;
	const int adopted = sphynx::AdoptLegacyBios(g_memstick_dir, &adopted_report);
	g_last_error = "bios tree ready (" + std::to_string(made) + " new folders, " +
	               std::to_string(adopted) + " files adopted)";
	sphynx::Logf("bios tree: %s (%d new folders, %d adopted)", sphynx::BiosRoot(g_memstick_dir).c_str(),
	             made, adopted);
	if (!adopted_report.empty()) sphynx::LogLine("adopted from PSP/SYSTEM:" + adopted_report);
	return made;
}

const char *sphynx_ui_bios_root(void) {
	static std::string root;
	root = sphynx::BiosRoot(g_memstick_dir);
	return root.c_str();
}

char *sphynx_ui_bios_dir_for_system(const char *system_key) {
	if (!system_key || !*system_key || g_memstick_dir.empty()) return nullptr;
	std::string dir = sphynx::BiosDirForSystem(g_memstick_dir, system_key);
	if (dir.empty()) return nullptr;   // this system needs no BIOS
	// Create it on demand: the user may have deleted the folder, or the app may
	// have been reinstalled while the memory card stayed in place.
	std::string report;
	sphynx::EnsureBiosTree(g_memstick_dir, &report);
	if (!sphynx::DirectoryExists(dir)) return nullptr;
	char *out = static_cast<char *>(std::malloc(dir.size() + 1));
	if (!out) return nullptr;
	std::memcpy(out, dir.c_str(), dir.size() + 1);
	return out;
}

char *sphynx_ui_bios_missing(const char *system_key) {
	const std::string missing = sphynx::MissingBiosFiles(g_memstick_dir, system_key);
	char *out = static_cast<char *>(std::malloc(missing.size() + 1));
	if (!out) return nullptr;
	std::memcpy(out, missing.c_str(), missing.size() + 1);
	return out;
}

// ---------------------------------------------------------------------------
//  Launching the player + diagnostics
// ---------------------------------------------------------------------------

#if !defined(SPHYNX_ANDROID)
// Desktop builds have no Activity to open: the real implementation lives in
// extcores/jni/sphynx_launch_jni.cpp, which only exists in the Android build.
void sphynx_ui_set_context(void *, void *) {}
int sphynx_ui_launch_player(const char *) { return 0; }
#endif

void sphynx_ui_set_log_path(const char *path) { sphynx::SetLogPath(path ? path : ""); }

void sphynx_ui_log_line(const char *text) { sphynx::LogLine(text ? text : ""); }

const char *sphynx_ui_log_path(void) {
	static std::string path;
	path = sphynx::LogPath();
	return path.c_str();
}

int sphynx_ui_is_known_extension(const char *path) {
	if (!path || !*path) return 0;
	std::string name = sphynx::DisplayNameOf(path);
	const size_t dot = name.find_last_of('.');
	// A dot in a folder name is not an extension: compare against the file name
	// only, which DisplayNameOf already isolated.
	if (dot == std::string::npos) return 0;
	std::string ext = name.substr(dot + 1);
	for (char &c : ext) c = char(std::tolower(static_cast<unsigned char>(c)));
	if (ext.empty()) return 0;
	// Same token list the game list uses, so "is this a Sphynx extension?" can
	// never disagree with what the list shows.
	const std::string filter = sphynx_ui_extension_filter();
	size_t pos = 0;
	while (pos <= filter.size()) {
		const size_t sep = filter.find(':', pos);
		const std::string token = filter.substr(pos, sep == std::string::npos ? std::string::npos : sep - pos);
		if (!token.empty() && token == ext) return 1;
		if (sep == std::string::npos) break;
		pos = sep + 1;
	}
	return 0;
}

const char *sphynx_ui_bios_status(const char *system_key) {
	static std::string status;
	status = sphynx::BiosStatusForSystem(g_memstick_dir, system_key);
	return status.c_str();
}

const char *sphynx_ui_extension_filter(void) {
	// Built once: PPSSPP's own extensions plus every extension of every Sphynx
	// system, so one listing shows PSP and non-PSP games side by side.
	//
	// Tokens are deduplicated as whole entries (not by substring) because
	// several systems share suffixes like "88"/"d88" — a naive find() would let
	// "d88" swallow "88" and silently hide those games.
	//
	// Archives stay out on purpose: PPSSPP installs .zip/.7z through its own
	// flow, and routing them to a core would break that.
	static std::string filter = [] {
		std::vector<std::string> tokens;
		auto add = [&tokens](const char *e) {
			if (!e || !*e) return;
			if (std::strcmp(e, "zip") == 0 || std::strcmp(e, "7z") == 0) return;
			std::string item(e);
			for (char &c : item) c = char(std::tolower(static_cast<unsigned char>(c)));
			for (const std::string &have : tokens)
				if (have == item) return;
			tokens.push_back(item);
		};
		for (const char *psp : {"iso", "cso", "chd", "pbp", "elf", "prx", "ppdmp"}) add(psp);
		int n = 0;
		const sphynx::SystemInfo *all = sphynx::AllSystems(&n);
		for (int i = 0; i < n; ++i)
			for (int j = 0; j < all[i].extension_count; ++j) add(all[i].extensions[j]);
		std::string f;
		for (const std::string &t : tokens) {
			if (!f.empty()) f += ':';
			f += t;
		}
		f += ':';   // trailing separator, same shape as PPSSPP's own list
		return f;
	}();
	return filter.c_str();
}

}  // extern "C"
