// ============================================================================
//  Sphynx — extcores/src/bios_layout.cpp
//
//  Builds and maintains PSP/SYSTEMS_BIOS (see bios_layout.h for the contract).
//
//  This file is deliberately plain C++17 + POSIX: it is compiled for Android
//  *and* for the host test binaries, so the exact same code that ships is the
//  code the CI test exercises. No JNI, no Android headers.
//
//  SPDX-License-Identifier: GPL-2.0-or-later
// ============================================================================
#include "bios_layout.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <vector>

#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <unistd.h>

namespace sphynx {
namespace {

// --- the 16 systems that get a folder --------------------------------------
//
// 12 mandatory entries first (the system refuses to run without these files),
// then the 4 recommended ones (runs without, far better with). Every other
// system of the 35 gets NO folder, because its core contains what it needs.
const char *kPsxFiles[] = {"scph5500.bin", "scph5501.bin", "scph5502.bin", nullptr};
const char *kSaturnFiles[] = {"sega_101.bin", "mpr-17933.bin", nullptr};
const char *kSegaCdFiles[] = {"bios_CD_U.bin", "bios_CD_E.bin", "bios_CD_J.bin", nullptr};
const char *kPceCdFiles[] = {"syscard3.pce", "syscard2.pce", "syscard1.pce", nullptr};
const char *kNeoGeoCdFiles[] = {"neocd.bin", nullptr};
const char *kThreeDoFiles[] = {"panafz10.bin", "panafz1.bin", nullptr};
const char *kPcFxFiles[] = {"pcfx.rom", "pcfx-bios.rom", nullptr};
const char *kAmigaFiles[] = {"kick34005.A500", "kick13.rom", "kick20.rom", "kick31.rom", nullptr};
const char *kPc88Files[] = {"pc88.rom", "font.rom", "disk.rom", nullptr};
const char *kPc98Files[] = {"pc98.rom", "font.rom", "bios.rom", nullptr};
const char *kFdsFiles[] = {"disksys.rom", nullptr};
const char *kColecoFiles[] = {"coleco.rom", nullptr};

const char *kDreamcastFiles[] = {"dc_boot.bin", "dc_flash.bin", nullptr};
const char *kNdsFiles[] = {"bios7.bin", "bios9.bin", "firmware.bin", nullptr};
const char *kMsxFiles[] = {"MSX.ROM", "MSX2.ROM", "MSX2EXT.ROM", "MSX2P.ROM", nullptr};
const char *kMasterSystemFiles[] = {"bios.sms", nullptr};

const BiosSystem kSystems[] = {
	// ---- mandatory: nothing runs without one of these files ----------------
	{"psx", "01-PlayStation", kPsxFiles, 3, true,
	 "One of the three files is enough. PAL games use scph5502, NTSC-U scph5501, NTSC-J scph5500."},
	{"saturn", "02-Sega Saturn", kSaturnFiles, 2, true,
	 "sega_101.bin = Japanese Saturn, mpr-17933.bin = USA/Europe Saturn. Pick the one matching the disc."},
	{"segacd", "03-Sega CD", kSegaCdFiles, 3, true,
	 "Choose by the disc region: _U = USA, _E = Europe, _J = Japan."},
	{"pcecd", "04-PC Engine CD", kPceCdFiles, 3, true,
	 "syscard3.pce is the Super CD-ROM2 card and covers nearly every CD game. Hucard games need nothing."},
	{"neogeocd", "05-Neo Geo CD", kNeoGeoCdFiles, 1, true,
	 "The Neo Geo CD BIOS (neocd.bin), taken from the front-loading console."},
	{"3do", "06-3DO", kThreeDoFiles, 2, true,
	 "panafz10.bin is the Panasonic FZ-10 BIOS; panafz1.bin is the older FZ-1 one."},
	{"pcfx", "07-PC-FX", kPcFxFiles, 2, true,
	 "The single PC-FX BIOS file."},
	{"amiga", "08-Amiga", kAmigaFiles, 4, true,
	 "Kickstart ROMs. kick34005.A500 covers most A500 games; keep the others if a game asks for them."},
	{"pc88", "09-PC-8801", kPc88Files, 3, true,
	 "PC-88 ROM set (main ROM, font ROM, disk ROM). Names accepted by the NP2kai core."},
	{"pc98", "10-PC-9801", kPc98Files, 3, true,
	 "PC-98 ROM set plus the font ROM; the emulator refuses to start without the font."},
	{"fds", "11-Famicom Disk System", kFdsFiles, 1, true,
	 "The Famicom Disk System BIOS (disksys.rom). Cartridge NES games need nothing."},
	{"coleco", "12-ColecoVision", kColecoFiles, 1, true,
	 "coleco.rom, the ColecoVision BIOS (8 KiB)."},

	// ---- recommended: runs without, clearly better with --------------------
	{"dreamcast", "13-Dreamcast", kDreamcastFiles, 2, false,
	 "Optional: the built-in replacement BIOS works, but the real one fixes boot and logo problems."},
	{"nds", "14-Nintendo DS", kNdsFiles, 3, false,
	 "Optional: without firmware a few games refuse to boot. bios7/bios9/firmware belong together."},
	{"msx", "15-MSX", kMsxFiles, 4, false,
	 "Optional: the built-in C-BIOS only runs a handful of games; the real ROM set runs almost all."},
	{"sms", "16-Master System", kMasterSystemFiles, 1, false,
	 "Optional: only needed for the Sega boot screen and for SG-1000 games on a Master System."},
};

// --- tiny POSIX helpers ----------------------------------------------------

std::string JoinPath(const std::string &a, const std::string &b) {
	if (a.empty()) return b;
	if (a.back() == '/') return a + b;
	return a + "/" + b;
}

}  // namespace

bool FileExists(const std::string &path) {
	struct stat st;
	if (::stat(path.c_str(), &st) != 0) return false;
	return S_ISREG(st.st_mode);
}

bool DirectoryExists(const std::string &path) {
	struct stat st;
	if (::stat(path.c_str(), &st) != 0) return false;
	return S_ISDIR(st.st_mode);
}

namespace {

// mkdir -p. Returns true when the directory exists afterwards.
bool MkdirP(const std::string &path) {
	if (path.empty()) return false;
	if (DirectoryExists(path)) return true;
	std::string cur;
	size_t i = 0;
	if (path[0] == '/') {
		cur = "/";
		i = 1;
	}
	while (i <= path.size()) {
		const size_t slash = path.find('/', i);
		const std::string part = path.substr(i, slash == std::string::npos ? std::string::npos : slash - i);
		if (!part.empty()) {
			if (cur.empty() || cur.back() == '/')
				cur += part;
			else
				cur += "/" + part;
			if (!DirectoryExists(cur)) {
				// 0777 + the process umask: the same permissions PPSSPP uses for
				// its own folders, so the user can manage these files with any
				// file manager.
				if (::mkdir(cur.c_str(), 0777) != 0 && errno != EEXIST) return false;
			}
		}
		if (slash == std::string::npos) break;
		i = slash + 1;
	}
	return DirectoryExists(path);
}

bool WriteIfMissing(const std::string &path, const std::string &text) {
	if (FileExists(path)) return false;   // never overwrite the user's notes
	FILE *f = std::fopen(path.c_str(), "wb");
	if (!f) return false;
	const size_t n = std::fwrite(text.data(), 1, text.size(), f);
	std::fclose(f);
	return n == text.size();
}

bool CopyIfMissing(const std::string &from, const std::string &to) {
	if (!FileExists(from) || FileExists(to)) return false;
	FILE *in = std::fopen(from.c_str(), "rb");
	if (!in) return false;
	FILE *out = std::fopen(to.c_str(), "wb");
	if (!out) {
		std::fclose(in);
		return false;
	}
	char buffer[64 * 1024];
	size_t got = 0;
	bool ok = true;
	while ((got = std::fread(buffer, 1, sizeof(buffer), in)) > 0) {
		if (std::fwrite(buffer, 1, got, out) != got) {
			ok = false;
			break;
		}
	}
	std::fclose(in);
	std::fclose(out);
	if (!ok) ::remove(to.c_str());
	return ok;
}

std::string FileList(const BiosSystem &s) {
	std::string out;
	for (int i = 0; i < s.file_count; ++i) {
		if (i) out += (i + 1 == s.file_count) ? " or " : ", ";
		out += s.files[i];
	}
	return out;
}

// Per-system README.txt. Bilingual on purpose: the app's UI is English, the
// owner of this build reads Arabic.
std::string SystemReadme(const BiosSystem &s) {
	std::string t;
	t += "Sphynx | ";
	t += s.folder;
	t += "\n";
	t += "====================================================\n\n";
	t += s.mandatory ? "STATUS: BIOS is REQUIRED. No game of this system runs without it.\n"
	                 : "STATUS: BIOS is OPTIONAL. Games run without it, but work much better with it.\n";
	t += "\n";
	t += "Put the BIOS file(s) in THIS folder, with these exact names:\n";
	t += "  ";
	t += FileList(s);
	t += "\n\n";
	t += s.note ? s.note : "";
	t += "\n\n";
	t += "The emulator reads this folder every time you start a game, so you can\n"
	     "add, replace or remove files at any moment. No restart is needed.\n\n";
	t += "----------------------------------------------------\n";
	t += "الحالة: ";
	t += s.mandatory ? "هذا النظام لا يعمل بدون ملف بايوس.\n" : "هذا النظام يعمل بدون بايوس، لكن يعمل أفضل بكثير معه.\n";
	t += "ضع ملف البايوس داخل هذا المجلد بهذه الأسماء بالضبط:\n  ";
	t += FileList(s);
	t += "\n";
	t += "التطبيق يقرأ هذا المجلد في كل مرة تشغّل فيها لعبة، فتستطيع الإضافة أو\n"
	     "التبديل أو الحذف في أي وقت بدون إعادة تشغيل التطبيق.\n\n";
	t += "Sphynx does not ship BIOS files: they are copyrighted by their owners.\n";
	return t;
}

std::string RootReadme() {
	std::string t;
	t += "Sphynx | SYSTEMS_BIOS\n";
	t += "====================================================\n\n";
	t += "One folder per system that uses a BIOS. Everything else works out of the box.\n\n";
	t += "REQUIRED (a game cannot start without one of these files):\n";
	for (const BiosSystem &s : kSystems) {
		if (!s.mandatory) continue;
		t += "  ";
		t += s.folder;
		t += "  ->  ";
		t += FileList(s);
		t += "\n";
	}
	t += "\nOPTIONAL (games run without it, better with it):\n";
	for (const BiosSystem &s : kSystems) {
		if (s.mandatory) continue;
		t += "  ";
		t += s.folder;
		t += "  ->  ";
		t += FileList(s);
		t += "\n";
	}
	t += "\n";
	t += "The other 19 systems (NES, SNES, Game Boy, GBA, N64, Genesis, 32X, SG-1000,\n"
	     "Game Gear, Sega Pico, WonderSwan, Neo Geo Pocket, MS-DOS, ScummVM, Arcade,\n"
	     "TurboGrafx-16 Hucard, ...) need no BIOS at all and have no folder here.\n"
	     "PSP games are handled by the original PPSSPP engine and need no BIOS either.\n\n";
	t += "Files already placed in PSP/SYSTEM are picked up automatically the next time\n"
	     "the app starts: they are copied into the matching folder here.\n\n";
	t += "----------------------------------------------------\n";
	t += "مجلد لكل نظام يستخدم بايوس. باقي الأنظمة تعمل مباشرة بدون أي ملف.\n";
	t += "المجلدات من 01 إلى 12 إجبارية، ومن 13 إلى 16 مستحسنة (تعمل الألعاب بدونها).\n";
	t += "ضع الملفات بالأسماء المكتوبة داخل README.txt في كل مجلد، ويمكنك التبديل\n"
	     "أو الحذف في أي وقت، والتطبيق يقرأها عند كل تشغيل للعبة.\n";
	t += "أي ملف بايوس موجود في مجلد PSP/SYSTEM يتم نسخه تلقائياً إلى مجلده هنا.\n";
	return t;
}

}  // namespace

int BiosSystemCount() { return static_cast<int>(sizeof(kSystems) / sizeof(kSystems[0])); }

const BiosSystem *AllBiosSystems() { return kSystems; }

const BiosSystem *FindBiosSystem(const char *system_key) {
	if (!system_key) return nullptr;
	for (const BiosSystem &s : kSystems)
		if (std::strcmp(s.system_key, system_key) == 0) return &s;
	return nullptr;
}

int MandatoryBiosCount() {
	int n = 0;
	for (const BiosSystem &s : kSystems)
		if (s.mandatory) ++n;
	return n;
}

int RecommendedBiosCount() { return BiosSystemCount() - MandatoryBiosCount(); }

std::string BiosRoot(const std::string &memstick_dir) {
	if (memstick_dir.empty()) return std::string();
	std::string root = memstick_dir;
	while (root.size() > 1 && root.back() == '/') root.pop_back();
	return JoinPath(root, kSphynxBiosFolderName);
}

std::string BiosDirForSystem(const std::string &memstick_dir, const char *system_key) {
	const BiosSystem *s = FindBiosSystem(system_key);
	if (!s) return std::string();
	const std::string root = BiosRoot(memstick_dir);
	if (root.empty()) return std::string();
	return JoinPath(root, s->folder);
}

int EnsureBiosTree(const std::string &memstick_dir, std::string *report) {
	if (memstick_dir.empty() || !DirectoryExists(memstick_dir)) {
		if (report) *report = "memstick folder does not exist";
		return -1;
	}
	const std::string root = BiosRoot(memstick_dir);
	if (!MkdirP(root)) {
		if (report) *report = "cannot create " + root;
		return -1;
	}
	int made = 0;
	for (const BiosSystem &s : kSystems) {
		const std::string dir = JoinPath(root, s.folder);
		if (!DirectoryExists(dir)) {
			if (!MkdirP(dir)) continue;
			++made;
		}
		// The README is recreated when the user deleted it, but never replaced.
		WriteIfMissing(JoinPath(dir, "README.txt"), SystemReadme(s));
	}
	WriteIfMissing(JoinPath(root, "00-READ-ME-FIRST.txt"), RootReadme());
	if (report) *report = root;
	return made;
}

int AdoptLegacyBios(const std::string &memstick_dir, std::string *report) {
	const std::vector<std::string> sources = {
		JoinPath(memstick_dir, "SYSTEM"),   // PPSSPP's traditional BIOS folder
		memstick_dir,                       // files dumped straight into PSP/
	};
	int adopted = 0;
	std::string detail;
	for (const BiosSystem &s : kSystems) {
		const std::string dst_dir = BiosDirForSystem(memstick_dir, s.system_key);
		if (dst_dir.empty() || !DirectoryExists(dst_dir)) continue;
		for (int i = 0; i < s.file_count; ++i) {
			const std::string dst = JoinPath(dst_dir, s.files[i]);
			if (FileExists(dst)) continue;
			for (const std::string &src_dir : sources) {
				const std::string src = JoinPath(src_dir, s.files[i]);
				if (CopyIfMissing(src, dst)) {
					++adopted;
					detail += "  ";
					detail += s.files[i];
					detail += " -> ";
					detail += s.folder;
					detail += "\n";
					break;
				}
			}
		}
	}
	if (report) *report = detail;
	return adopted;
}

std::string MissingBiosFiles(const std::string &memstick_dir, const char *system_key) {
	const std::string dir = BiosDirForSystem(memstick_dir, system_key);
	if (dir.empty()) return std::string();
	return MissingBiosFilesInDir(dir, system_key);
}

std::string MissingBiosFilesInDir(const std::string &dir, const char *system_key) {
	const BiosSystem *s = FindBiosSystem(system_key);
	if (!s || dir.empty()) return std::string();
	std::string missing;
	bool any_present = false;
	for (int i = 0; i < s->file_count; ++i) {
		if (FileExists(JoinPath(dir, s->files[i]))) {
			any_present = true;
			continue;
		}
		if (!missing.empty()) missing += ", ";
		missing += s->files[i];
	}
	// Several BIOS files of a system are alternatives (regions, revisions):
	// finding ONE of them is enough, so only report "missing" when the folder
	// holds none of them.
	if (any_present) return std::string();
	return missing;
}

std::string BiosStatusForSystem(const std::string &memstick_dir, const char *system_key) {
	if (!FindBiosSystem(system_key)) return "no BIOS needed";
	const std::string missing = MissingBiosFiles(memstick_dir, system_key);
	if (missing.empty()) return "complete";
	return "missing " + missing;
}

}  // namespace sphynx
