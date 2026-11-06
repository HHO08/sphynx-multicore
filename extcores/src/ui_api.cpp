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
#include "sphynx_ui_api.h"
#include "png_write.h"

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
	const size_t dot = path.find_last_of('.');
	if (dot == std::string::npos) return false;
	std::string ext = path.substr(dot + 1);
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

int sphynx_ui_is_external(const char *path) {
	if (!path || !*path) return -1;
	if (IsArchiveExt(path)) return -1;   // let PPSSPP handle archives itself
	std::vector<uint8_t> head;
	const bool have_head = ReadHead(path, &head);
	const sphynx::DetectResult r =
		sphynx::Detect(path, have_head && !head.empty() ? head.data() : nullptr, head.size());
	if (r.is_psp_native) return 0;                      // PSP -> PPSSPP itself
	if (r.candidates.empty()) return -1;
	return 1;
}

int sphynx_ui_make_thumbnail(const char *path, unsigned char **out, int *len) {
	if (!path || !out || !len) return 0;
	*out = nullptr;
	*len = 0;

	const char *key = SystemKeyFor(path);
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
	if (!session.Start(core, path, g_system_dir, g_save_dir, &err)) {
		g_last_error = "core start failed: " + err;
		return 0;
	}

	// Run enough frames for the game to leave its boot screen, but stop early
	// once real content appears: prefer a frame that is neither black nor flat.
	int best_frames = 0;
	std::vector<uint8_t> best;
	int best_score = -1;
	for (int i = 0; i < 240; ++i) {
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
	g_last_error = "ok";
	return 1;
}

void sphynx_ui_free(void *p) { std::free(p); }

// "key|Display name;key|Display name;..." for the ambiguous-disc chooser.
const char *sphynx_ui_candidates(const char *path) {
	static std::string out;
	out.clear();
	if (!path || !*path) return out.c_str();
	std::vector<uint8_t> head;
	const bool have = ReadHead(path, &head);
	const sphynx::DetectResult r =
		sphynx::Detect(path, have && !head.empty() ? head.data() : nullptr, head.size());
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

const char *sphynx_ui_extension_filter(void) {
	// Built once.  Every extension of every Sphynx system, plus PPSSPP's own so
	// that a single listing shows PSP and non-PSP games side by side.
	static std::string filter = [] {
		std::string f = "iso:cso:chd:pbp:elf:prx:ppdmp:";
		int n = 0;
		const sphynx::SystemInfo *all = sphynx::AllSystems(&n);
		auto add = [&f](const char *e) {
			if (!e) return;
			const std::string item = std::string(e) + ":";
			if (f.find(item) == std::string::npos) f += item;
		};
		for (int i = 0; i < n; ++i)
			for (int j = 0; j < all[i].extension_count; ++j) {
				const char *e = all[i].extensions[j];
				if (!e || std::strcmp(e, "zip") == 0 || std::strcmp(e, "7z") == 0) continue;
				add(e);
			}
		return f;
	}();
	return filter.c_str();
}

}  // extern "C"
