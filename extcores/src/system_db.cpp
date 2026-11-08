// ============================================================================
//  Sphynx — extcores/src/system_db.cpp
//
//  The single source of truth for: which systems exist, which core backs each
//  one, what licences those cores carry, and how a file on disk is recognised.
//
//  Size/behaviour policy baked into this table (decided by the project owner):
//    * SNES uses bsnes (GPLv3) because Snes9x's licence is non-commercial and
//      cannot be combined with GPL code.
//    * SEGA 8-bit uses Gearsystem, Genesis/32X/SegaCD use PicoDrive (Genesis
//      Plus GX is non-commercial -> excluded).
//    * PSX uses PCSX-ReARMed (much lighter on ARM than Beetle PSX).
//    * No BIOS and no ROM is ever bundled: every entry that needs BIOS records
//      a *hint* so the UI can tell the user which file to import.
//
//  SPDX-License-Identifier: GPL-2.0-or-later
// ============================================================================
#include "sphynx_extcore.h"

#include <libretro.h>

#include <algorithm>
#include <cstring>

namespace sphynx {
namespace {

// --- extension lists (lower-case, no dot) ----------------------------------
const char *kExtNes[] = {"nes", "unf", "unif"};
const char *kExtFds[] = {"fds"};
const char *kExtGb[] = {"gb"};
const char *kExtGbc[] = {"gbc"};
const char *kExtGba[] = {"gba"};
const char *kExtN64[] = {"n64", "z64", "v64", "ndd"};
const char *kExtNds[] = {"nds"};
const char *kExtSnes[] = {"sfc", "smc", "fig", "swc"};
const char *kExtSg1000[] = {"sg"};
const char *kExtSms[] = {"sms"};
const char *kExtGg[] = {"gg"};
const char *kExtGenesis[] = {"md", "gen", "smd"};
const char *kExt32X[] = {"32x"};
// Sega CD / Saturn / PCE-CD / 3DO / PSX / Dreamcast / Neo Geo CD all share
// .cue/.chd/.iso; the magic sniffer and the user's answer decide between them.
// SPHYNX (stage 17): a raw disc image is just as often .bin/.img as it is .cue
// or .iso, and those two extensions used to be claimed by the Sega Pico core
// alone - so a PlayStation or Saturn .bin was handed to a core that cannot read
// it and the answer was "core refused this content". They are now offered to
// every disc system, and the player tries them in order until one really starts.
const char *kExtDiscImage[] = {"cue", "chd", "iso", "bin", "img"};
const char *kExtDreamcast[] = {"gdi", "cdi", "chd"};
const char *kExtPico[] = {"bin", "md"};
const char *kExt3do[] = {"iso", "cue", "chd", "bin", "img"};
const char *kExtAmiga[] = {"adf", "adz", "dms", "ipf", "hdf", "lha", "uae"};
const char *kExtArcade[] = {"zip", "7z"};
const char *kExtMsx[] = {"rom", "mx1", "mx2", "ri", "col", "dsk", "cas", "sg"};
const char *kExtColeco[] = {"col"};   // blueMSX also emulates ColecoVision
const char *kExtWs[] = {"ws"};
const char *kExtWsc[] = {"wsc"};
const char *kExtPsx[] = {"cue", "chd", "iso", "bin", "img", "exe", "psexe"};
const char *kExtPc88[] = {"d88", "d98", "88", "98", "fdi", "hdf", "t88"};
const char *kExtPc98[] = {"hdi", "hdd", "fdd", "d88", "d98", "fdi"};
const char *kExtPcfx[] = {"cue", "chd", "bin", "img"};
const char *kExtPce[] = {"pce"};
const char *kExtPceCd[] = {"cue", "chd", "bin", "img"};
const char *kExtDos[] = {"zip", "dosz", "conf", "exe", "com", "bat"};
const char *kExtScumm[] = {"scummvm"};
const char *kExtNeoGeoCd[] = {"cue", "chd", "bin", "img"};
const char *kExtNgp[] = {"ngp"};
const char *kExtNgpc[] = {"ngc"};

#define EXTS(x) x, static_cast<int>(sizeof(x) / sizeof(x[0]))

const SystemInfo kSystems[] = {
	// ---------------- Nintendo ----------------
	{SystemId::Nes, "nes", "Nintendo Entertainment System", "fceumm", EXTS(kExtNes), nullptr, 8, false, SupportLevel::NotBuilt},
	{SystemId::Fds, "fds", "Famicom Disk System", "fceumm", EXTS(kExtFds), "disksys.rom (FDS BIOS)", 8, false, SupportLevel::NotBuilt},
	{SystemId::Gb, "gb", "Game Boy", "gambatte", EXTS(kExtGb), nullptr, 4, false, SupportLevel::NotBuilt},
	{SystemId::Gbc, "gbc", "Game Boy Color", "gambatte", EXTS(kExtGbc), nullptr, 4, false, SupportLevel::NotBuilt},
	{SystemId::Gba, "gba", "Game Boy Advance", "mgba", EXTS(kExtGba), nullptr, 8, false, SupportLevel::NotBuilt},
	{SystemId::N64, "n64", "Nintendo 64", "mupen64plus_next", EXTS(kExtN64), nullptr, 96, false, SupportLevel::NotBuilt},
	{SystemId::Nds, "nds", "Nintendo DS", "melonds", EXTS(kExtNds), "bios7.bin / bios9.bin / firmware.bin (optional)", 96, true, SupportLevel::NotBuilt},
	{SystemId::Snes, "snes", "Super Nintendo", "bsnes", EXTS(kExtSnes), nullptr, 32, false, SupportLevel::NotBuilt},

	// ---------------- SEGA ----------------
	{SystemId::Sg1000, "sg1000", "SG-1000", "gearsystem", EXTS(kExtSg1000), nullptr, 4, false, SupportLevel::NotBuilt},
	{SystemId::MasterSystem, "sms", "Master System", "gearsystem", EXTS(kExtSms), "bios.sms (optional)", 4, false, SupportLevel::NotBuilt},
	{SystemId::GameGear, "gg", "Game Gear", "gearsystem", EXTS(kExtGg), nullptr, 4, false, SupportLevel::NotBuilt},
	{SystemId::Genesis, "genesis", "Genesis / Mega Drive", "picodrive", EXTS(kExtGenesis), nullptr, 8, false, SupportLevel::NotBuilt},
	{SystemId::Genesis32X, "32x", "32X", "picodrive", EXTS(kExt32X), nullptr, 16, false, SupportLevel::NotBuilt},
	{SystemId::SegaCd, "segacd", "Sega CD", "picodrive", EXTS(kExtDiscImage), "bios_CD_U.bin / E / J", 24, false, SupportLevel::NotBuilt},
	{SystemId::Saturn, "saturn", "Sega Saturn", "mednafen_saturn", EXTS(kExtDiscImage), "sega_101.bin / mpr-17933.bin", 96, false, SupportLevel::NotBuilt},
	{SystemId::Dreamcast, "dreamcast", "Dreamcast", "flycast", EXTS(kExtDreamcast), "dc_boot.bin / dc_flash.bin (recommended)", 96, false, SupportLevel::NotBuilt},
	{SystemId::SegaPico, "picodrive_pico", "Sega Pico", "picodrive", EXTS(kExtPico), nullptr, 8, false, SupportLevel::NotBuilt},

	// ---------------- Other ----------------
	{SystemId::ThreeDo, "3do", "3DO Interactive Multiplayer", "opera", EXTS(kExt3do), "panafz10.bin (3DO BIOS)", 64, false, SupportLevel::NotBuilt},
	{SystemId::Amiga, "amiga", "Amiga", "puae", EXTS(kExtAmiga), "kick34005.A500 (Kickstart)", 32, false, SupportLevel::NotBuilt},
	{SystemId::Arcade, "arcade", "Arcade (FinalBurn Neo)", "fbneo", EXTS(kExtArcade), nullptr, 64, false, SupportLevel::NotBuilt},
	{SystemId::Msx, "msx", "MSX / MSX2", "bluemsx", EXTS(kExtMsx), "MSX.ROM / MSX2.ROM", 16, false, SupportLevel::NotBuilt},
	// ColecoVision rides the same blueMSX core: a 35th system at zero size cost.
	{SystemId::ColecoVision, "coleco", "ColecoVision", "bluemsx", EXTS(kExtColeco), "coleco.rom (ColecoVision BIOS)", 8, false, SupportLevel::NotBuilt},
	{SystemId::WonderSwan, "ws", "WonderSwan", "mednafen_wswan", EXTS(kExtWs), nullptr, 8, false, SupportLevel::NotBuilt},
	{SystemId::WonderSwanColor, "wsc", "WonderSwan Color", "mednafen_wswan", EXTS(kExtWsc), nullptr, 8, false, SupportLevel::NotBuilt},

	// ---------------- Sony ----------------
	{SystemId::Psx, "psx", "PlayStation", "pcsx_rearmed", EXTS(kExtPsx), "scph5501.bin (PS1 BIOS)", 48, false, SupportLevel::NotBuilt},

	// ---------------- NEC ----------------
	{SystemId::Pc88, "pc88", "PC-8801", "np2kai", EXTS(kExtPc88), "PC-88 ROM set", 24, false, SupportLevel::NotBuilt},
	{SystemId::Pc98, "pc98", "PC-9801", "np2kai", EXTS(kExtPc98), "PC-98 ROM set + fonts", 32, false, SupportLevel::NotBuilt},
	{SystemId::PcFx, "pcfx", "PC-FX", "mednafen_pcfx", EXTS(kExtPcfx), "pcfx.rom", 48, false, SupportLevel::NotBuilt},
	{SystemId::TurboGrafx16, "pce", "TurboGrafx-16 / PCE", "mednafen_pce_fast", EXTS(kExtPce), "syscard3.pce (CD only)", 8, false, SupportLevel::NotBuilt},
	{SystemId::TurboGrafxCd, "pcecd", "TurboGrafx-CD", "mednafen_pce", EXTS(kExtPceCd), "syscard3.pce", 24, false, SupportLevel::NotBuilt},

	// ---------------- PC ----------------
	{SystemId::MsDos, "dos", "MS-DOS", "dosbox_pure", EXTS(kExtDos), nullptr, 48, false, SupportLevel::NotBuilt},
	{SystemId::ScummVm, "scummvm", "ScummVM", "scummvm", EXTS(kExtScumm), nullptr, 64, false, SupportLevel::NotBuilt},

	// ---------------- SNK ----------------
	{SystemId::NeoGeoCd, "neogeocd", "Neo Geo CD", "fbneo", EXTS(kExtNeoGeoCd), "neocd.bin", 48, false, SupportLevel::NotBuilt},
	{SystemId::NeoGeoPocket, "ngp", "Neo Geo Pocket", "mednafen_ngp", EXTS(kExtNgp), nullptr, 4, false, SupportLevel::NotBuilt},
	{SystemId::NeoGeoPocketColor, "ngpc", "Neo Geo Pocket Color", "mednafen_ngp", EXTS(kExtNgpc), nullptr, 4, false, SupportLevel::NotBuilt},
};

const CoreInfo kCores[] = {
	{"fceumm", "libretro/fceumm", "GPL-2.0-or-later", LicenceCompat::Compatible, false, 1100},
	{"gambatte", "libretro/gambatte", "GPL-2.0-or-later", LicenceCompat::Compatible, false, 700},
	{"mgba", "libretro/mgba", "MPL-2.0", LicenceCompat::Compatible, false, 2000},
	{"mupen64plus_next", "libretro/mupen64plus-next", "GPL-2.0-or-later", LicenceCompat::Compatible, true, 9000},
	{"melonds", "libretro/melonDS", "GPL-3.0-or-later", LicenceCompat::RequiresGpl3, true, 4000},
	{"bsnes", "libretro/bsnes", "GPL-3.0-or-later", LicenceCompat::RequiresGpl3, false, 3500},
	{"gearsystem", "libretro/Gearsystem", "GPL-3.0-or-later", LicenceCompat::RequiresGpl3, false, 600},
	{"picodrive", "libretro/picodrive", "mixed MPL/GPL", LicenceCompat::NeedsReview, false, 1300},
	{"mednafen_saturn", "libretro/beetle-saturn", "GPL-2.0-or-later", LicenceCompat::Compatible, false, 4500},
	{"flycast", "libretro/flycast", "GPL-2.0-or-later", LicenceCompat::Compatible, true, 7500},
	{"opera", "libretro/opera", "LGPL-2.1+/GPL mixed", LicenceCompat::NeedsReview, false, 1400},
	{"puae", "libretro/libretro-uae", "GPL-2.0-or-later", LicenceCompat::Compatible, false, 2800},
	{"fbneo", "libretro/FBNeo", "GPL-2.0+ (driver-level audit needed)", LicenceCompat::NeedsReview, false, 14000},
	{"bluemsx", "libretro/blueMSX", "GPL-2.0-or-later", LicenceCompat::Compatible, false, 2500},
	{"mednafen_wswan", "libretro/beetle-cygne", "GPL-2.0-or-later", LicenceCompat::Compatible, false, 500},
	{"pcsx_rearmed", "libretro/pcsx_rearmed", "GPL-2.0-or-later", LicenceCompat::Compatible, false, 3500},
	{"np2kai", "libretro/NP2kai", "unclear — audit required", LicenceCompat::NeedsReview, false, 1500},
	{"mednafen_pcfx", "libretro/beetle-pcfx", "GPL-2.0-or-later", LicenceCompat::Compatible, false, 2500},
	{"mednafen_pce_fast", "libretro/beetle-pce-fast", "GPL-2.0-or-later", LicenceCompat::Compatible, false, 1200},
	{"mednafen_pce", "libretro/beetle-pce", "GPL-2.0-or-later", LicenceCompat::Compatible, false, 1600},
	{"dosbox_pure", "libretro/dosbox-pure", "GPL-2.0-or-later", LicenceCompat::Compatible, false, 4500},
	{"scummvm", "libretro/scummvm", "GPL-2.0-or-later", LicenceCompat::Compatible, false, 16000},
	{"mednafen_ngp", "libretro/beetle-ngp", "GPL-2.0-or-later", LicenceCompat::Compatible, false, 600},
};

// --- small helpers ---------------------------------------------------------
std::string LowerExt(const std::string &path) {
	size_t slash = path.find_last_of("/\\");
	size_t dot = path.find_last_of('.');
	if (dot == std::string::npos) return std::string();
	if (slash != std::string::npos && dot < slash) return std::string();
	std::string ext = path.substr(dot + 1);
	for (char &c : ext) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
	return ext;
}

bool HasExt(const SystemInfo *s, const std::string &ext) {
	for (int i = 0; i < s->extension_count; ++i)
		if (ext == s->extensions[i]) return true;
	return false;
}

bool HeadIs(const uint8_t *head, size_t n, size_t off, const char *bytes, size_t len) {
	if (n < off + len) return false;
	return std::memcmp(head + off, bytes, len) == 0;
}

bool HeadEq32(const uint8_t *head, size_t n, size_t off, uint32_t be_value) {
	if (n < off + 4) return false;
	uint32_t v = (uint32_t(head[off]) << 24) | (uint32_t(head[off + 1]) << 16) |
	             (uint32_t(head[off + 2]) << 8) | uint32_t(head[off + 3]);
	return v == be_value;
}

// The PSP container formats belong to PPSSPP itself. Checked before anything
// else so a PSP game can never be hijacked by an external core.
// 3DO discs start with 0x01 0x5A 0x5A 0x5A ... (big-endian magic 0x015A5A5A).
bool LooksLikeThreeDo(const uint8_t *head, size_t n) {
	return n > 8 && head[0] == 0x01 && head[1] == 0x5A && head[2] == 0x5A && head[3] == 0x5A;
}

// PlayStation discs carry the "PLAYSTATION" licence string in the volume
// descriptor area (sector 4 / 0x8000 onwards).
bool LooksLikePlayStation(const uint8_t *head, size_t n) {
	if (n < 0x9000) return false;
	for (size_t off = 0x8000; off + 11 < 0x20000 && off + 11 < n; off += 2048) {
		if (HeadIs(head, n, off, "PLAYSTATION", 11)) return true;
		if (HeadIs(head, n, off, "PLAYSTATION ", 12)) return true;
	}
	return false;
}

bool IsPspContainer(const std::string &ext, const uint8_t *head, size_t n) {
	if (ext == "cso" || ext == "pbp" || ext == "elf" || ext == "prx") return true;
	if (ext == "iso") {
		// Never hijack a disc we can positively identify as another system.
		if (LooksLikeThreeDo(head, n)) return false;
		if (LooksLikePlayStation(head, n)) return false;
		return true;   // PSP first; ambiguous .iso never breaks PSP playback
	}
	return false;
}

void AddUnique(DetectResult *r, SystemId id, int confidence, bool ambiguous) {
	for (auto &c : r->candidates) {
		if (c.id == id) {
			if (confidence > c.confidence) { c.confidence = confidence; c.ambiguous = ambiguous; }
			return;
		}
	}
	r->candidates.push_back({id, confidence, ambiguous});
}

}  // namespace

const SystemInfo *AllSystems(int *count) {
	if (count) *count = static_cast<int>(sizeof(kSystems) / sizeof(kSystems[0]));
	return kSystems;
}

int SystemCount() { return static_cast<int>(sizeof(kSystems) / sizeof(kSystems[0])); }

const SystemInfo *FindSystem(SystemId id) {
	for (const auto &s : kSystems)
		if (s.id == id) return &s;
	return nullptr;
}

const SystemInfo *FindSystemByKey(const char *key) {
	if (!key) return nullptr;
	for (const auto &s : kSystems)
		if (std::strcmp(s.key, key) == 0) return &s;
	return nullptr;
}

const CoreInfo *FindCore(const char *core_lib) {
	if (!core_lib) return nullptr;
	for (const auto &c : kCores)
		if (std::strcmp(c.lib, core_lib) == 0) return &c;
	return nullptr;
}

DetectResult Detect(const std::string &path, const uint8_t *head, size_t head_size) {
	DetectResult r;
	const size_t n = head ? head_size : 0;
	const std::string ext = LowerExt(path);

	// ---- 1. PSP always wins --------------------------------------------------
	if (IsPspContainer(ext, head, n)) {
		// ".iso" without a header might still be a PSX/3DO disc; keep them as
		// lower-confidence candidates so the UI can ask *once* if PSP fails.
		r.is_psp_native = true;
		r.reason = "PSP container — handled by PPSSPP's own core";
		if (ext == "iso") {
			AddUnique(&r, SystemId::Psx, 20, true);
			AddUnique(&r, SystemId::ThreeDo, 10, true);
			r.needs_user_choice = false;   // PSP first, never block the user
		}
		r.candidates.insert(r.candidates.begin(), {SystemId::Unknown, 100, false});
		return r;
	}

	// ---- 2. magic bytes -----------------------------------------------------
	if (HeadIs(head, n, 0, "NES\x1a", 4)) { AddUnique(&r, SystemId::Nes, 100, false); }
	if (HeadIs(head, n, 0, "FDS\x1a", 4)) { AddUnique(&r, SystemId::Fds, 100, false); }
	if (HeadEq32(head, n, 0, 0x80371240u) || HeadEq32(head, n, 0, 0x37804012u) ||
	    HeadEq32(head, n, 0, 0x40123780u)) {
		AddUnique(&r, SystemId::N64, 100, false);
	}
	if (HeadIs(head, n, 0, "\x00PBP", 4)) {
		// A PBP container is a PSP EBOOT *or* a PSX EBOOT; PPSSPP owns the
		// former, the PSX core the latter — the disc type inside decides.
		r.is_psp_native = true;
		r.reason = "PBP container — PSP Eboot path first";
		r.candidates.insert(r.candidates.begin(), {SystemId::Unknown, 100, false});
		AddUnique(&r, SystemId::Psx, 30, true);
		return r;
	}
	// Game Boy / Game Boy Color: Nintendo logo at 0x104, CGB flag at 0x143.
	if (n > 0x150 && std::memcmp(head + 0x104, "\xCE\xED\x66\x66\xCC\x0D\x00\x0B", 8) == 0) {
		const uint8_t cgb = head[0x143];
		if (cgb == 0x80 || cgb == 0xC0) AddUnique(&r, SystemId::Gbc, 100, false);
		else AddUnique(&r, SystemId::Gb, 100, false);
	}
	if (LooksLikeThreeDo(head, n)) AddUnique(&r, SystemId::ThreeDo, 95, false);
	if (LooksLikePlayStation(head, n)) AddUnique(&r, SystemId::Psx, 95, false);
	if (HeadIs(head, n, 0, "PK\x03\x04", 4)) {
		AddUnique(&r, SystemId::Arcade, 60, true);
		AddUnique(&r, SystemId::MsDos, 40, true);
		r.needs_user_choice = true;
	}
	// Sega header: "SEGA" at 0x100 selects the 8/16-bit family.
	if (HeadIs(head, n, 0x100, "SEGA", 4)) {
		const bool is32x = HeadIs(head, n, 0x100, "SEGA 32X", 8);
		const bool isPico = HeadIs(head, n, 0x100, "SEGA PICO", 9);
		if (is32x) AddUnique(&r, SystemId::Genesis32X, 95, false);
		else if (isPico) AddUnique(&r, SystemId::SegaPico, 95, false);
		else {
			AddUnique(&r, SystemId::Genesis, 80, false);
			AddUnique(&r, SystemId::MasterSystem, 40, true);
		}
	}

	// ---- 3. extensions ------------------------------------------------------
	if (r.candidates.empty()) {
		int count = 0;
		const SystemInfo *all = AllSystems(&count);
		for (int i = 0; i < count; ++i) {
			if (HasExt(&all[i], ext)) AddUnique(&r, all[i].id, 50, false);
		}
	}

	// ---- 4. ambiguity bookkeeping ------------------------------------------
	if (r.candidates.size() > 1) {
		// Sort by confidence, keep ties marked as ambiguous.
		std::stable_sort(r.candidates.begin(), r.candidates.end(),
		                 [](const Candidate &a, const Candidate &b) {
			                 return a.confidence > b.confidence;
		                 });
		const int top = r.candidates[0].confidence;
		int ties = 0;
		for (const auto &c : r.candidates)
			if (c.confidence == top) ++ties;
		if (ties > 1) {
			r.needs_user_choice = true;
			for (auto &c : r.candidates)
				if (c.confidence == top) c.ambiguous = true;
		}
	}
	if (r.candidates.empty()) r.reason = "unrecognised content — ask the user";
	else if (!r.reason[0]) r.reason = "matched";
	return r;
}

// --- bundled core binaries -------------------------------------------------
// The value is the libretro buildbot name, which is what Tools/sphynx/
// fetch_cores.sh downloads and renames to libsphynx_core_<name>.so.
const char *const kBundledCoreLibs[] = {
	"fceumm", "gambatte", "mgba", "mupen64plus_next_gles2", "melonds", "bsnes",
	"gearsystem", "picodrive", "mednafen_saturn", "flycast", "opera", "puae",
	"fbneo", "bluemsx", "mednafen_wswan", "pcsx_rearmed", "np2kai",
	"mednafen_pcfx", "mednafen_pce_fast", "mednafen_pce", "dosbox_pure",
	"scummvm", "mednafen_ngp",
};

// Registry name -> bundled file name (only where they differ).
const char *BundledNameFor(const char *core_lib) {
	if (!core_lib) return nullptr;
	// Nintendo 64 rides the GLES2 build on purpose: on 32-bit ARM the GLES2
	// renderer is the only one that reaches playable speed.
	if (std::strcmp(core_lib, "mupen64plus_next") == 0) return "mupen64plus_next_gles2";
	for (const char *name : kBundledCoreLibs)
		if (std::strcmp(name, core_lib) == 0) return name;
	return nullptr;
}

int BundledCoreCount() {
	return static_cast<int>(sizeof(kBundledCoreLibs) / sizeof(kBundledCoreLibs[0]));
}

const char *const *BundledCoreLibs() { return kBundledCoreLibs; }

const char *CoreLibraryFile(const char *core_lib) {
	// Static per-core strings so callers can keep the pointer.
	static std::string files[sizeof(kBundledCoreLibs) / sizeof(kBundledCoreLibs[0])];
	const char *name = BundledNameFor(core_lib);
	if (!name) return nullptr;
	for (size_t i = 0; i < sizeof(kBundledCoreLibs) / sizeof(kBundledCoreLibs[0]); ++i) {
		if (std::strcmp(kBundledCoreLibs[i], name) == 0) {
			if (files[i].empty()) files[i] = std::string("libsphynx_core_") + name + ".so";
			return files[i].c_str();
		}
	}
	return nullptr;
}

std::string CoreLibraryPath(const std::string &native_library_dir, const char *core_lib) {
	const char *file = CoreLibraryFile(core_lib);
	if (!file || native_library_dir.empty()) return std::string();
	std::string path = native_library_dir;
	if (path.back() != '/') path += '/';
	path += file;
	return path;
}

std::string CoreLibraryPathForSystem(const std::string &native_library_dir, const char *system_key) {
	const SystemInfo *info = FindSystemByKey(system_key);
	if (!info) return std::string();
	return CoreLibraryPath(native_library_dir, info->core_lib);
}

bool SelfCheck(std::string *report) {
	std::string out;
	bool ok = true;

	if (SystemCount() != 35) {
		out += "FAIL: expected 35 systems, found " + std::to_string(SystemCount()) + "\n";
		ok = false;
	}
	for (int i = 0; i < SystemCount(); ++i) {
		const SystemInfo &s = kSystems[i];
		if (!s.key || !s.display_name || !s.core_lib) {
			out += std::string("FAIL: system #") + std::to_string(i) + " has null fields\n";
			ok = false;
			continue;
		}
		if (!FindCore(s.core_lib)) {
			out += std::string("FAIL: ") + s.key + " -> unknown core " + s.core_lib + "\n";
			ok = false;
		}
		if (!BundledNameFor(s.core_lib)) {
			out += std::string("FAIL: ") + s.key + " -> core " + s.core_lib +
			       " is not bundled in this build\n";
			ok = false;
		}
		if (s.extension_count <= 0) {
			out += std::string("FAIL: ") + s.key + " has no extensions\n";
			ok = false;
		}
		for (int j = i + 1; j < SystemCount(); ++j) {
			if (std::strcmp(kSystems[j].key, s.key) == 0) {
				out += std::string("FAIL: duplicate key ") + s.key + "\n";
				ok = false;
			}
		}
	}
	// PSP must never be reachable through a core library: it is PPSSPP's own.
	for (const auto &c : kCores) {
		if (std::strcmp(c.lib, "ppsspp") == 0) {
			out += "FAIL: PSP must not appear as an external core\n";
			ok = false;
		}
		if (c.compat == LicenceCompat::Incompatible) {
			out += std::string("WARN: core ") + c.lib + " is licence-incompatible\n";
		}
	}
	out += "systems=" + std::to_string(SystemCount()) + " cores=" +
	       std::to_string(static_cast<int>(sizeof(kCores) / sizeof(kCores[0]))) +
	       " bundled=" + std::to_string(BundledCoreCount()) + "\n";
	if (report) *report = out;
	return ok;
}

const char *BuildId() {
	// Static storage: the caller may keep the pointer for the process lifetime.
	// SPHYNX (stage 15): the stage number is part of the string on purpose - a
	// screenshot of the failure panel then names the build exactly, which is how
	// a screenshot from a device becomes a usable bug report.
	static std::string id = std::string("sphynx-extcore-host 1.18.1-stage19 (hardware renderer for N64 and Dreamcast, libretro ABI ") +
	                        std::to_string(RETRO_API_VERSION) + ")";
	return id.c_str();
}

}  // namespace sphynx
