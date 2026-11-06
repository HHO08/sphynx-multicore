// ============================================================================
//  Sphynx — host test for the UI bridge (no Android, no PPSSPP headers).
//
//  Verifies the parts of the game list that were broken before stage 4:
//    * every system extension reaches PPSSPP's directory listing
//    * archives stay with PPSSPP (they must not be routed to a core)
//    * ambiguous disc formats produce a chooser list
//    * the PNG encoder emits a well-formed image (IHDR/IEND, right size)
//
//  SPDX-License-Identifier: GPL-2.0-or-later
// ============================================================================
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "sphynx_extcore.h"
#include "sphynx_ui_api.h"
#include "../src/png_write.h"

static int g_failures = 0;

static void Check(bool cond, const char *what) {
	if (cond) {
		std::printf("  PASS %s\n", what);
	} else {
		std::printf("  FAIL %s\n", what);
		++g_failures;
	}
}

// Every extension the registry knows must appear in the listing filter.
static void TestExtensionFilter() {
	const char *filter = sphynx_ui_extension_filter();
	Check(filter && *filter, "extension filter is not empty");
	Check(std::strstr(filter, ":nes:") != nullptr, "filter lists .nes");
	Check(std::strstr(filter, ":gba:") != nullptr, "filter lists .gba");
	Check(std::strstr(filter, ":md:") != nullptr, "filter lists .md (Genesis)");
	Check(std::strstr(filter, ":sfc:") != nullptr, "filter lists .sfc (SNES)");
	// The PSP extensions are first, so match them at the start of the list.
	Check(std::strncmp(filter, "iso:", 4) == 0, "filter still lists .iso (PSP)");
	Check(std::strstr(filter, ":pbp:") != nullptr, "filter still lists .pbp (PSP)");

	int systems = 0;
	const sphynx::SystemInfo *all = sphynx::AllSystems(&systems);
	int missing = 0;
	for (int i = 0; i < systems; ++i) {
		for (int e = 0; e < all[i].extension_count; ++e) {
			const char *ext = all[i].extensions[e];
			// Archives are routed to PPSSPP's installer, never to a core.
			if (std::strcmp(ext, "zip") == 0 || std::strcmp(ext, "7z") == 0) continue;
			std::string needle = std::string(ext) + ":";
			if (!std::strstr(filter, needle.c_str())) {
				std::printf("  .. missing extension %s (system %s)\n", ext, all[i].key);
				++missing;
			}
		}
	}
	Check(missing == 0, "filter contains every extension of every system");
}

static void TestArchiveRouting() {
	// Archives belong to PPSSPP's own install-zip flow.
	Check(sphynx_ui_is_external("/sdcard/game.zip") == -1, ".zip is not routed to a core");
	Check(sphynx_ui_is_external("/sdcard/game.7z") == -1, ".7z is not routed to a core");
	Check(sphynx_ui_is_external("") == -1, "empty path is rejected");
}

static void TestCandidates() {
	// A .cue could be Mega CD, Saturn, TurboGrafx-CD or PSX: the player screen
	// must offer the choice instead of guessing silently.
	const char *cands = sphynx_ui_candidates("test.cue");
	Check(cands && *cands, "ambiguous .cue yields candidates");
	Check(std::strchr(cands, '|') != nullptr, "candidate strings carry a display name");
	const char *none = sphynx_ui_candidates("");
	Check(none && !*none, "empty path yields no candidates");
}

static void TestPng() {
	std::vector<uint8_t> px(16 * 16 * 4);
	for (int y = 0; y < 16; ++y)
		for (int x = 0; x < 16; ++x) {
			uint8_t *p = &px[(y * 16 + x) * 4];
			p[0] = uint8_t(x * 16);
			p[1] = uint8_t(y * 16);
			p[2] = 0x40;
			p[3] = 0xFF;
		}
	const std::vector<uint8_t> png = sphynx::EncodePngRgba(px.data(), 16, 16);
	Check(png.size() > 60, "PNG encoder produced data");
	static const uint8_t kSig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
	Check(png.size() > 8 && std::memcmp(png.data(), kSig, 8) == 0, "PNG signature");
	Check(png.size() >= 24 && std::memcmp(png.data() + 12, "IHDR", 4) == 0, "IHDR first chunk");
	// Width/height are big-endian 32-bit at offsets 16 and 20.
	auto be32 = [&](size_t off) {
		return (uint32_t(png[off]) << 24) | (uint32_t(png[off + 1]) << 16) |
		       (uint32_t(png[off + 2]) << 8) | uint32_t(png[off + 3]);
	};
	Check(png.size() > 24 && be32(16) == 16, "PNG width is 16");
	Check(png.size() > 24 && be32(20) == 16, "PNG height is 16");
	Check(sphynx::EncodePngRgba(nullptr, 0, 0).empty(), "PNG encoder rejects empty input");
}

int main() {
	std::printf("Sphynx UI bridge host test (%s)\n", sphynx::BuildId());
	std::printf("[1] extension filter\n");
	TestExtensionFilter();
	std::printf("[2] archive routing\n");
	TestArchiveRouting();
	std::printf("[3] ambiguous formats\n");
	TestCandidates();
	std::printf("[4] PNG encoder\n");
	TestPng();
	std::printf(g_failures ? "\nFAILED: %d check(s)\n" : "\nall checks passed\n", g_failures);
	return g_failures ? 1 : 0;
}
