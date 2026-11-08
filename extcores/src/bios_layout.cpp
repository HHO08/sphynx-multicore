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
#include <unordered_map>
#include <utility>
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
	{"psx", "PlayStation", kPsxFiles, 3, true,
	 "One of the three files is enough. PAL games use scph5502, NTSC-U scph5501, NTSC-J scph5500."},
	{"saturn", "Sega Saturn", kSaturnFiles, 2, true,
	 "sega_101.bin = Japanese Saturn, mpr-17933.bin = USA/Europe Saturn. Pick the one matching the disc."},
	{"segacd", "Sega CD", kSegaCdFiles, 3, true,
	 "Choose by the disc region: _U = USA, _E = Europe, _J = Japan."},
	{"pcecd", "PC Engine CD", kPceCdFiles, 3, true,
	 "syscard3.pce is the Super CD-ROM2 card and covers nearly every CD game. Hucard games need nothing."},
	{"neogeocd", "Neo Geo CD", kNeoGeoCdFiles, 1, true,
	 "The Neo Geo CD BIOS (neocd.bin), taken from the front-loading console."},
	{"3do", "3DO", kThreeDoFiles, 2, true,
	 "panafz10.bin is the Panasonic FZ-10 BIOS; panafz1.bin is the older FZ-1 one."},
	{"pcfx", "PC-FX", kPcFxFiles, 2, true,
	 "The single PC-FX BIOS file."},
	{"amiga", "Amiga", kAmigaFiles, 4, true,
	 "Kickstart ROMs. kick34005.A500 covers most A500 games; keep the others if a game asks for them."},
	{"pc88", "PC-8801", kPc88Files, 3, true,
	 "PC-88 ROM set (main ROM, font ROM, disk ROM). Names accepted by the NP2kai core."},
	{"pc98", "PC-9801", kPc98Files, 3, true,
	 "PC-98 ROM set plus the font ROM; the emulator refuses to start without the font."},
	{"fds", "Famicom Disk System", kFdsFiles, 1, true,
	 "The Famicom Disk System BIOS (disksys.rom). Cartridge NES games need nothing."},
	{"coleco", "ColecoVision", kColecoFiles, 1, true,
	 "coleco.rom, the ColecoVision BIOS (8 KiB)."},

	// ---- recommended: runs without, clearly better with --------------------
	{"dreamcast", "Dreamcast", kDreamcastFiles, 2, false,
	 "Optional: the built-in replacement BIOS works, but the real one fixes boot and logo problems."},
	{"nds", "Nintendo DS", kNdsFiles, 3, false,
	 "bios7.bin + bios9.bin + firmware.bin belong together: the DS needs all three, and without them it shows two white screens. A game can start without them, but almost none runs.", true},
	{"msx", "MSX", kMsxFiles, 4, false,
	 "Optional: the built-in C-BIOS only runs a handful of games; the real ROM set runs almost all."},
	{"sms", "Master System", kMasterSystemFiles, 1, false,
	 "Optional: only needed for the Sega boot screen and for SG-1000 games on a Master System."},
};

// --- tiny POSIX helpers ----------------------------------------------------

std::string JoinPath(const std::string &a, const std::string &b) {
	if (a.empty()) return b;
	if (a.back() == '/') return a + b;
	return a + "/" + b;
}

std::string BaseName(const std::string &path) {
	const size_t slash = path.find_last_of('/');
	return slash == std::string::npos ? path : path.substr(slash + 1);
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

// --- case- and alias-tolerant file lookup (stage 15) -----------------------
//
// A core opens its BIOS with fopen("bios7.bin"), which is case sensitive on
// Android. The files were therefore "in the folder" and still invisible to the
// core: the Nintendo DS booted to two white screens with a full BIOS set in
// place. Everything below looks a file up the way a person means it, and the
// caller then gives it the exact name the core wants.
std::string Lowered(const std::string &in) {
	std::string out;
	out.reserve(in.size());
	for (char c : in) out.push_back(static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c));
	return out;
}

bool IsNameSeparator(char c) {
	return c == '_' || c == '-' || c == '.' || c == ' ';
}

// True when `file` is the file `expected` means:
//   * the same name in any case ("BIOS7.BIN" == "bios7.bin"), or
//   * the same name behind a system prefix ("nds_bios7.bin", "NDS-Bios7.bin").
// A name that merely *ends* with the same letters without a separator
// ("neobios.rom" vs "bios.rom") is deliberately NOT a match: adopting the wrong
// file would be worse than reporting it missing.
bool NameMatches(const std::string &file, const std::string &expected) {
	const std::string f = Lowered(file);
	const std::string e = Lowered(expected);
	if (f == e) return true;
	if (f.size() <= e.size()) return false;
	if (f.compare(f.size() - e.size(), e.size(), e) != 0) return false;
	return IsNameSeparator(f[f.size() - e.size() - 1]);
}

// SPHYNX (stage 16, start-up speed): every lookup used to open and read the
// source directory again, so one adoption pass cost
//     (files of all systems) x (source folders)  =  38 x 18
// = 684 opendir/readdir sequences - and the Java side runs that pass for up to
// three PSP folders at launch. On a phone those directories live behind a FUSE
// layer (Android/media), where one readdir of a folder holding a few dozen BIOS
// files costs milliseconds: that is the ten second black screen at start-up.
//
// The cache below reads each directory exactly once per pass. Same answers, a
// handful of syscalls instead of hundreds - the cost no longer depends on how
// many BIOS folders and files exist.
class DirCache {
public:
	// Names of `dir`, lower-cased, with their full path. Read once, then reused.
	const std::vector<std::pair<std::string, std::string>> &Get(const std::string &dir) {
		auto it = entries_.find(dir);
		if (it != entries_.end()) return it->second;
		std::vector<std::pair<std::string, std::string>> list;
		DIR *d = ::opendir(dir.c_str());
		if (d) {
			while (struct dirent *entry = ::readdir(d)) {
				const std::string name = entry->d_name;
				if (name == "." || name == "..") continue;
				list.emplace_back(Lowered(name), JoinPath(dir, name));
			}
			::closedir(d);
		}
		return entries_.emplace(dir, std::move(list)).first->second;
	}

	// A rename inside `dir` makes the cached listing stale; dropping it is
	// cheaper than reasoning about which entries are still valid.
	void Forget(const std::string &dir) { entries_.erase(dir); }

private:
	std::unordered_map<std::string, std::vector<std::pair<std::string, std::string>>> entries_;
};

// Full path of the file `expected` inside a listing that is already in memory,
// or "". This is the only lookup left: reading the directory itself now happens
// once per folder (see DirCache), never once per expected file.
std::string FindLooseIn(const std::vector<std::pair<std::string, std::string>> &listing,
                        const char *expected) {
	if (!expected || !*expected) return std::string();
	for (const auto &entry : listing) {
		// NameMatches() is the one definition of "this file is the one the core
		// means": same name in any case, or the same name behind a system prefix.
		if (NameMatches(entry.first, expected)) return entry.second;
	}
	return std::string();
}

std::vector<std::pair<std::string, std::string>> ReadListing(const std::string &dir) {
	std::vector<std::pair<std::string, std::string>> list;
	DIR *d = ::opendir(dir.c_str());
	if (!d) return list;
	while (struct dirent *entry = ::readdir(d)) {
		const std::string name = entry->d_name;
		if (name == "." || name == "..") continue;
		list.emplace_back(Lowered(name), JoinPath(dir, name));
	}
	::closedir(d);
	return list;
}

long long FileSizeOf(const std::string &path) {
	struct stat st;
	if (::stat(path.c_str(), &st) != 0) return -1;
	return static_cast<long long>(st.st_size);
}

// "16 KiB" / "256 KiB" / "1.2 MiB": a BIOS dump has a known size, and seeing it
// is how a user notices that a 0-byte or truncated file was copied.
std::string HumanSize(long long bytes) {
	if (bytes < 0) return "?";
	char buf[48];
	if (bytes < 1024)
		std::snprintf(buf, sizeof(buf), "%lld B", bytes);
	else if (bytes < 1024 * 1024)
		std::snprintf(buf, sizeof(buf), "%lld KiB", (bytes + 512) / 1024);
	else
		std::snprintf(buf, sizeof(buf), "%.1f MiB", static_cast<double>(bytes) / (1024.0 * 1024.0));
	return buf;
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
	std::vector<std::string> sources = {
		JoinPath(memstick_dir, "SYSTEM"),   // PPSSPP's traditional BIOS folder
		memstick_dir,                       // files dumped straight into PSP/
	};
	// Also every existing folder of the tree itself. That is how a user who
	// already filled a folder with an older layout — for example a numbered one
	// from a previous build — keeps those files after the layout changed.
	const std::string root = BiosRoot(memstick_dir);
	if (DirectoryExists(root)) {
		DIR *dir = ::opendir(root.c_str());
		if (dir) {
			while (struct dirent *entry = ::readdir(dir)) {
				const std::string name = entry->d_name;
				if (name == "." || name == "..") continue;
				const std::string path = JoinPath(root, name);
				if (DirectoryExists(path)) sources.push_back(path);
			}
			::closedir(dir);
		}
	}
	int adopted = 0;
	std::string detail;
	// SPHYNX (stage 16): each source folder is read once, then every lookup is a
	// string search in memory. The filesystem is asked two questions per system
	// folder (does it exist, does this file already exist) instead of one per
	// file per source, which is what the old code did: 38 files x 18 folders.
	DirCache cache;
	std::vector<const std::vector<std::pair<std::string, std::string>> *> listings;
	for (const std::string &src_dir : sources) listings.push_back(&cache.Get(src_dir));
	for (const BiosSystem &s : kSystems) {
		const std::string dst_dir = BiosDirForSystem(memstick_dir, s.system_key);
		if (dst_dir.empty() || !DirectoryExists(dst_dir)) continue;
		for (int i = 0; i < s.file_count; ++i) {
			const std::string dst = JoinPath(dst_dir, s.files[i]);
			if (FileExists(dst)) continue;
			for (size_t si = 0; si < sources.size(); ++si) {
				if (sources[si] == dst_dir) continue;   // never copy onto itself
				// SPHYNX (stage 15): the source may be "BIOS7.BIN" or
				// "nds_bios7.bin"; it is adopted under the exact name the core
				// opens, so a file that was already on the card starts working
				// instead of being reported as missing.
				const std::string src = FindLooseIn(*listings[si], s.files[i]);
				if (src.empty()) continue;
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
	// SPHYNX (stage 16): one listing for the whole answer.
	const auto listing = ReadListing(dir);
	for (int i = 0; i < s->file_count; ++i) {
		// SPHYNX (stage 15): "present" means the file the user put there, even
		// when the capital letters differ - NormalizeBiosNames() then gives it
		// the exact name the core opens, so the two answers agree.
		if (!FindLooseIn(listing, s->files[i]).empty()) {
			any_present = true;
			continue;
		}
		if (!missing.empty()) missing += ", ";
		missing += s->files[i];
	}
	// Several BIOS files of a system are usually alternatives (regions,
	// revisions): finding ONE of them is enough, so only report "missing" when
	// the folder holds none of them.
	// SPHYNX (stage 15): the Nintendo DS is not like that - bios7, bios9 and
	// firmware are three different parts and all are required. Reporting it as
	// complete because one was present is how "the BIOS is installed and it is
	// still white" happened.
	if (s->all_required) return missing;
	if (any_present) return std::string();
	return missing;
}

// ---------------------------------------------------------------------------
//  Start-up cost (stage 16) - see bios_layout.h
// ---------------------------------------------------------------------------

namespace {
// The stamp lives inside the tree, so moving or deleting the tree invalidates
// it by definition. The content carries the layout version: if a future build
// changes the folder set, an old stamp is ignored instead of trusted.
constexpr const char *kStampName = ".sphynx-tree-v1";
}  // namespace

bool BiosTreeLooksReady(const std::string &memstick_dir) {
	if (memstick_dir.empty()) return false;
	const std::string root = BiosRoot(memstick_dir);
	if (root.empty() || !DirectoryExists(root)) return false;
	if (!FileExists(JoinPath(root, kStampName))) return false;
	// One stat per folder: if the user removed one, the full pass is needed to
	// put it back - that is the behaviour that existed before, kept.
	for (const BiosSystem &s : kSystems) {
		if (!DirectoryExists(JoinPath(root, s.folder))) return false;
	}
	return true;
}

void MarkBiosTreeReady(const std::string &memstick_dir) {
	const std::string root = BiosRoot(memstick_dir);
	if (root.empty() || !DirectoryExists(root)) return;
	WriteIfMissing(JoinPath(root, kStampName), "Sphynx BIOS tree, layout 1\n");
}

int PrepareBiosForSystem(const std::string &memstick_dir, const char *system_key,
                         std::string *report) {
	const BiosSystem *s = FindBiosSystem(system_key);
	if (!s) return 0;   // this system needs no BIOS: nothing to prepare
	const std::string dir = BiosDirForSystem(memstick_dir, system_key);
	if (dir.empty()) return 0;
	// The folder may have been deleted, or the app reinstalled while the memory
	// card stayed in place.
	if (!DirectoryExists(dir) && !MkdirP(dir)) return 0;

	// Two possible sources, read once each: PPSSPP's own SYSTEM folder and the
	// root of the memory stick (where people drop files).
	std::vector<std::string> sources = {JoinPath(memstick_dir, "SYSTEM"), memstick_dir};
	DirCache cache;
	int fixed = 0;
	std::string detail;
	for (const std::string &src_dir : sources) {
		if (src_dir == dir) continue;
		const auto &listing = cache.Get(src_dir);
		for (int i = 0; i < s->file_count; ++i) {
			const std::string dst = JoinPath(dir, s->files[i]);
			if (FileExists(dst)) continue;
			const std::string src = FindLooseIn(listing, s->files[i]);
			if (src.empty()) continue;
			if (!CopyIfMissing(src, dst)) continue;
			++fixed;
			detail += "  ";
			detail += BaseName(src);
			detail += " -> ";
			detail += s->folder;
			detail += "/";
			detail += s->files[i];
			detail += "\n";
		}
	}

	// Then the exact names, inside this folder only.
	const auto listing = ReadListing(dir);
	for (int i = 0; i < s->file_count; ++i) {
		const std::string want = s->files[i];
		const std::string dst = JoinPath(dir, want);
		if (FileExists(dst)) continue;
		const std::string src = FindLooseIn(listing, s->files[i]);
		if (src.empty() || src == dst) continue;
		bool ok = ::rename(src.c_str(), dst.c_str()) == 0;
		if (!ok) ok = CopyIfMissing(src, dst);
		if (!ok) continue;
		++fixed;
		detail += "  ";
		detail += BaseName(src);
		detail += " -> ";
		detail += want;
		detail += "\n";
	}
	if (report) *report = detail;
	return fixed;
}

int NormalizeBiosNames(const std::string &memstick_dir, std::string *report) {
	int fixed = 0;
	std::string detail;
	// SPHYNX (stage 16): one directory read per system folder instead of one
	// per expected file (16 instead of 38 on every start and every launch).
	DirCache cache;
	for (const BiosSystem &s : kSystems) {
		const std::string dir = BiosDirForSystem(memstick_dir, s.system_key);
		if (dir.empty() || !DirectoryExists(dir)) continue;
		for (int i = 0; i < s.file_count; ++i) {
			const std::string want = s.files[i];
			const std::string dst = JoinPath(dir, want);
			if (FileExists(dst)) continue;            // already exactly right
			const std::string src = FindLooseIn(cache.Get(dir), s.files[i]);
			if (src.empty() || src == dst) continue;  // nothing to correct
			// Rename keeps the user's single copy; if the filesystem refuses
			// (a read-only card, an odd provider) a copy of the same bytes is
			// just as good and the original stays where it was.
			bool ok = ::rename(src.c_str(), dst.c_str()) == 0;
			if (!ok) ok = CopyIfMissing(src, dst);
			if (!ok) continue;
			cache.Forget(dir);   // the folder changed under the cached listing
			++fixed;
			detail += "  ";
			detail += BaseName(src);
			detail += " -> ";
			detail += want;
			detail += "\n";
		}
	}
	if (report) *report = detail;
	return fixed;
}

bool BiosDirHasAnyFile(const std::string &dir, const char *system_key) {
	const BiosSystem *s = FindBiosSystem(system_key);
	if (!s || dir.empty() || !DirectoryExists(dir)) return false;
	const auto listing = ReadListing(dir);
	for (int i = 0; i < s->file_count; ++i) {
		if (!FindLooseIn(listing, s->files[i]).empty()) return true;
	}
	return false;
}

std::string BiosReportInDir(const std::string &dir, const char *system_key) {
	const BiosSystem *s = FindBiosSystem(system_key);
	if (!s) return std::string(system_key ? system_key : "?") + " · no BIOS needed";
	std::string out = s->system_key;
	out += " · ";
	out += dir.empty() ? "(no folder)" : dir;
	out += " · ";
	std::string found, missing;
	// SPHYNX (stage 16): one listing for the whole report.
	const auto listing = ReadListing(dir);
	for (int i = 0; i < s->file_count; ++i) {
		const std::string path = FindLooseIn(listing, s->files[i]);
		if (path.empty()) {
			if (!missing.empty()) missing += ", ";
			missing += s->files[i];
			continue;
		}
		if (!found.empty()) found += ", ";
		found += s->files[i];
		found += " (";
		found += HumanSize(FileSizeOf(path));
		found += ")";
	}
	out += found.empty() ? "no BIOS file found" : ("found: " + found);
	if (!missing.empty()) out += " · missing: " + missing;
	if (!s->mandatory && found.empty())
		out += " · optional, but this system works far better with it";
	return out;
}

std::string BiosReport(const std::string &memstick_dir, const char *system_key) {
	return BiosReportInDir(BiosDirForSystem(memstick_dir, system_key), system_key);
}

std::string BiosStatusForSystem(const std::string &memstick_dir, const char *system_key) {
	if (!FindBiosSystem(system_key)) return "no BIOS needed";
	const std::string missing = MissingBiosFiles(memstick_dir, system_key);
	if (missing.empty()) return "complete";
	return "missing " + missing;
}

}  // namespace sphynx
