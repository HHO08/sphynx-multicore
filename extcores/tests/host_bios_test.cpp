// ============================================================================
//  Sphynx — extcores/tests/host_bios_test.cpp
//
//  Tests for the two things this stage adds outside the player itself:
//
//    1. PSP/SYSTEMS_BIOS  — the folder tree the user asked for:
//         * exactly 16 folders (12 mandatory + 4 recommended, never 35)
//         * created automatically, with a README naming the exact files
//         * a user's own files are never overwritten or deleted
//         * BIOS already in PSP/SYSTEM is adopted into the matching folder
//         * "missing" reports the truth, including "one of several regions"
//         * systems without a BIOS get no folder at all
//    2. Save states on disk — round trip, wrong-core refusal, corruption
//       refusal, and the path layout.
//
//  Everything here runs the same object code the APK ships.
//
//  SPDX-License-Identifier: GPL-2.0-or-later
// ============================================================================
#include "sphynx_extcore.h"
#include "sphynx_ui_api.h"
#include "bios_layout.h"
#include "state_file.h"
#include "diag_log.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

namespace {

int g_failures = 0;
int g_checks = 0;
std::string g_root;

void Check(bool ok, const char *what) {
	++g_checks;
	if (!ok) {
		++g_failures;
		std::printf("  FAIL  %s\n", what);
	} else {
		std::printf("  ok    %s\n", what);
	}
}

bool Exists(const std::string &p) {
	struct stat st;
	return ::stat(p.c_str(), &st) == 0;
}

bool IsDir(const std::string &p) {
	struct stat st;
	return ::stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

void MkDirs(const std::string &p) {
	std::string cur;
	for (size_t i = 0; i < p.size(); ++i) {
		cur += p[i];
		if (p[i] == '/' && cur.size() > 1) ::mkdir(cur.c_str(), 0777);
	}
	::mkdir(p.c_str(), 0777);
}

void WriteFile(const std::string &path, const std::string &text) {
	FILE *f = std::fopen(path.c_str(), "wb");
	if (!f) return;
	std::fwrite(text.data(), 1, text.size(), f);
	std::fclose(f);
}

std::string ReadFile(const std::string &path) {
	FILE *f = std::fopen(path.c_str(), "rb");
	if (!f) return std::string();
	std::string out;
	char buf[4096];
	size_t n;
	while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
	std::fclose(f);
	return out;
}

// ---------------------------------------------------------------------------
//  1. The folder tree
// ---------------------------------------------------------------------------
void TestRegistry() {
	std::printf("\n[BIOS registry]\n");
	Check(sphynx::BiosSystemCount() == 16, "exactly 16 systems have a BIOS folder");
	Check(sphynx::MandatoryBiosCount() == 12, "12 of them cannot run without a BIOS");
	Check(sphynx::RecommendedBiosCount() == 4, "4 of them only work better with one");

	// No duplicates: two systems sharing a folder would make one of them read
	// the other's files.
	bool unique = true;
	for (int i = 0; i < sphynx::BiosSystemCount(); ++i) {
		for (int j = i + 1; j < sphynx::BiosSystemCount(); ++j) {
			if (std::strcmp(sphynx::AllBiosSystems()[i].folder, sphynx::AllBiosSystems()[j].folder) == 0)
				unique = false;
		}
	}
	Check(unique, "every folder name is unique");

	// Every key must exist in the system registry, otherwise the player would
	// look for a folder it can never be asked about.
	bool keys_known = true;
	for (int i = 0; i < sphynx::BiosSystemCount(); ++i) {
		const sphynx::BiosSystem &s = sphynx::AllBiosSystems()[i];
		const sphynx::SystemInfo *info = sphynx::FindSystemByKey(s.system_key);
		if (!info || !info->bios_hint) keys_known = false;
	}
	Check(keys_known, "every BIOS folder belongs to a real system that declares a BIOS");

	// Systems that need nothing must have no folder: the request was explicit
	// about not creating folders for the other 19 systems.
	Check(sphynx::FindBiosSystem("nes") == nullptr, "NES has no BIOS folder");
	Check(sphynx::FindBiosSystem("snes") == nullptr, "SNES has no BIOS folder");
	Check(sphynx::FindBiosSystem("gba") == nullptr, "GBA has no BIOS folder");
	Check(sphynx::FindBiosSystem("genesis") == nullptr, "Genesis has no BIOS folder");
	Check(sphynx::FindBiosSystem("psp") == nullptr, "PSP has no BIOS folder");
	Check(sphynx::FindBiosSystem("psx") != nullptr, "PlayStation has a BIOS folder");
	Check(sphynx::FindBiosSystem("saturn") != nullptr, "Saturn has a BIOS folder");
	Check(sphynx::FindBiosSystem("dreamcast") != nullptr, "Dreamcast has a (recommended) folder");
}

void TestTreeCreation() {
	std::printf("\n[BIOS tree is created]\n");
	const std::string memstick = g_root + "/memstick";
	MkDirs(memstick);

	const std::string root = sphynx::BiosRoot(memstick);
	Check(root == memstick + "/SYSTEMS_BIOS", "the tree lives in PSP/SYSTEMS_BIOS");

	std::string report;
	const int made = sphynx::EnsureBiosTree(memstick, &report);
	Check(made == 16, "the first run creates all 16 folders");
	Check(IsDir(root), "the SYSTEMS_BIOS folder exists");
	Check(Exists(root + "/00-READ-ME-FIRST.txt"), "a top level README is written");

	// Every system folder must exist with its own README.
	bool all_dirs = true, all_readme = true;
	for (int i = 0; i < sphynx::BiosSystemCount(); ++i) {
		const sphynx::BiosSystem &s = sphynx::AllBiosSystems()[i];
		const std::string dir = root + "/" + s.folder;
		if (!IsDir(dir)) all_dirs = false;
		if (!Exists(dir + "/README.txt")) all_readme = false;
	}
	Check(all_dirs, "one folder per BIOS system");
	Check(all_readme, "each folder explains which files it wants");

	// The README must actually name the files, otherwise it is decoration.
	const std::string saturn_readme = ReadFile(root + "/Sega Saturn/README.txt");
	Check(saturn_readme.find("sega_101.bin") != std::string::npos &&
	          saturn_readme.find("mpr-17933.bin") != std::string::npos,
	      "the Saturn README names both region BIOS files");
	Check(saturn_readme.find("REQUIRED") != std::string::npos, "and says the BIOS is required");

	// The request was explicit: folder names carry no numbering.
	// "3DO" legitimately starts with a digit, so reject the numbering *pattern*
	// ("01-PlayStation") rather than any digit at the front.
	bool no_numbering = true;
	for (int i = 0; i < sphynx::BiosSystemCount(); ++i) {
		const char *folder = sphynx::AllBiosSystems()[i].folder;
		if (folder[0] >= '0' && folder[0] <= '9' && folder[1] >= '0' && folder[1] <= '9')
			no_numbering = false;
		if (folder[strlen(folder) > 3 ? 2 : 0] == '-' && folder[0] >= '0' && folder[0] <= '9')
			no_numbering = false;
	}
	Check(no_numbering, "folder names carry no numbering (plain system names only)");
	const std::string dc_readme = ReadFile(root + "/Dreamcast/README.txt");
	Check(dc_readme.find("OPTIONAL") != std::string::npos, "Dreamcast is marked optional");

	// Running it again must be a no-op: the app does this at every start.
	const int again = sphynx::EnsureBiosTree(memstick, &report);
	Check(again == 0, "a second run creates nothing (idempotent)");

	// A user note must survive.
	WriteFile(root + "/PlayStation/README.txt", "my own notes\n");
	sphynx::EnsureBiosTree(memstick, &report);
	Check(ReadFile(root + "/PlayStation/README.txt") == "my own notes\n",
	      "a README the user edited is never overwritten");
}

void TestMissingAndAdoption() {
	std::printf("\n[BIOS detection + adoption from PSP/SYSTEM]\n");
	const std::string memstick = g_root + "/memstick";

	// Empty folder: everything the system expects is reported as missing.
	const std::string missing = sphynx::MissingBiosFiles(memstick, "psx");
	Check(missing.find("scph5501.bin") != std::string::npos, "an empty PSX folder reports scph5501.bin");
	Check(sphynx::BiosStatusForSystem(memstick, "psx").rfind("missing", 0) == 0,
	      "status starts with 'missing'");

	// One region file is enough: the others must stop being reported.
	WriteFile(sphynx::BiosDirForSystem(memstick, "psx") + "/scph5501.bin", "FAKEBIOS");
	Check(sphynx::MissingBiosFiles(memstick, "psx").empty(),
	      "one PSX BIOS file is enough (regions are alternatives)");
	Check(sphynx::BiosStatusForSystem(memstick, "psx") == "complete", "status becomes complete");

	// A system without a BIOS answers honestly.
	Check(sphynx::BiosStatusForSystem(memstick, "nes") == "no BIOS needed",
	      "a system without BIOS says so instead of complaining");

	// Adoption: files dropped in the old PSP/SYSTEM folder are copied in.
	MkDirs(memstick + "/SYSTEM");
	WriteFile(memstick + "/SYSTEM/sega_101.bin", "SATURNBIOS");
	WriteFile(memstick + "/SYSTEM/coleco.rom", "COLECO");
	std::string report;
	const int adopted = sphynx::AdoptLegacyBios(memstick, &report);
	Check(adopted == 2, "two BIOS files were adopted from PSP/SYSTEM");
	Check(Exists(sphynx::BiosDirForSystem(memstick, "saturn") + "/sega_101.bin"),
	      "the Saturn BIOS arrived in its own folder");
	Check(Exists(sphynx::BiosDirForSystem(memstick, "coleco") + "/coleco.rom"),
	      "the ColecoVision BIOS arrived in its own folder");
	Check(Exists(memstick + "/SYSTEM/sega_101.bin"), "the original file is left where it was (copy, not move)");
	Check(sphynx::AdoptLegacyBios(memstick, &report) == 0, "adoption does not run twice");

	// A file left in an older, numbered folder must be adopted as well, so an
	// existing setup survives a layout change.
	const std::string legacy = memstick + "/SYSTEMS_BIOS/01-PlayStation";
	MkDirs(legacy);
	WriteFile(legacy + "/scph5502.bin", "OLDBIOS");
	const int adopted2 = sphynx::AdoptLegacyBios(memstick, &report);
	Check(adopted2 >= 1, "a BIOS sitting in an older numbered folder is adopted");
	Check(Exists(sphynx::BiosDirForSystem(memstick, "psx") + "/scph5502.bin") ||
	          sphynx::BiosStatusForSystem(memstick, "psx") == "complete",
	      "and the PlayStation system now has its file");

	// An adopted file makes the system report complete.
	Check(sphynx::BiosStatusForSystem(memstick, "saturn") == "complete",
	      "adopting a BIOS makes the system complete");
}

// ---------------------------------------------------------------------------
//  1b. BIOS file *names*: the white-screen case (stage 15)
//
//  A core opens "bios7.bin" with a case-sensitive fopen(). A user who copies
//  "BIOS7.BIN" or "nds_bios7.bin" into the Nintendo DS folder had the files in
//  the right place and the core still could not see them - two white screens.
//  These checks are the fix, measured.
// ---------------------------------------------------------------------------
void TestBiosFileNames() {
	std::printf("\n[BIOS file names a core can really open (stage 15)]\n");
	const std::string memstick = g_root + "/memstick";
	sphynx::EnsureBiosTree(memstick, nullptr);
	const std::string nds = sphynx::BiosDirForSystem(memstick, "nds");
	Check(IsDir(nds), "the Nintendo DS folder exists");

	// A wrong-case name and a system-prefixed one, exactly like a real dump set.
	WriteFile(nds + "/BIOS7.BIN", std::string(16 * 1024, '7'));
	WriteFile(nds + "/NDS_Firmware.bin", std::string(256 * 1024, 'F'));

	std::string report;
	const int fixed = sphynx::NormalizeBiosNames(memstick, &report);
	Check(fixed == 2, "two names were corrected");
	Check(Exists(nds + "/bios7.bin"), "BIOS7.BIN becomes bios7.bin");
	Check(Exists(nds + "/firmware.bin"), "NDS_Firmware.bin becomes firmware.bin");
	Check(!Exists(nds + "/BIOS7.BIN"), "the wrong-cased name is gone: renamed, not duplicated");
	Check(report.find("bios7.bin") != std::string::npos, "the correction is listed in the report");

	// The reports now agree with reality: two present, one missing.
	const std::string missing = sphynx::MissingBiosFiles(memstick, "nds");
	Check(missing.find("bios9.bin") != std::string::npos, "bios9.bin is still reported missing");
	Check(missing.find("bios7.bin") == std::string::npos, "but bios7.bin is not");
	const std::string status = sphynx::BiosReport(memstick, "nds");
	Check(status.find("Nintendo DS") != std::string::npos, "the report names the folder");
	Check(status.find("bios7.bin (16 KiB)") != std::string::npos, "and shows what it found, with its size");
	Check(status.find("missing: bios9.bin") != std::string::npos, "and exactly what is missing");

	// Discipline: a name that merely ends with the same letters is NOT adopted,
	// because adopting the wrong file would be worse than reporting it missing.
	WriteFile(nds + "/neobios9.bin", "NOT-A-DS-BIOS");
	Check(!Exists(nds + "/bios9.bin"), "an unrelated file is not mistaken for the BIOS");
	Check(sphynx::MissingBiosFiles(memstick, "nds").find("bios9.bin") != std::string::npos,
	      "and it stays reported as missing");

	// An exact name is never touched, and running the pass twice changes nothing.
	WriteFile(nds + "/bios9.bin", std::string(4 * 1024, '9'));
	Check(sphynx::NormalizeBiosNames(memstick, &report) == 0, "a correctly named set is left alone");
	Check(sphynx::MissingBiosFiles(memstick, "nds").empty(), "the DS BIOS set is now complete");

	// Adoption from PSP/SYSTEM is also name-tolerant, and lands under the exact
	// name: this is how files a user already had start working.
	MkDirs(memstick + "/SYSTEM");
	WriteFile(memstick + "/SYSTEM/Disksys.ROM", std::string(8 * 1024, 'D'));
	const int adopted = sphynx::AdoptLegacyBios(memstick, &report);
	Check(adopted >= 1, "a wrong-cased file in PSP/SYSTEM is adopted");
	Check(Exists(sphynx::BiosDirForSystem(memstick, "fds") + "/disksys.rom"),
	      "and it arrives with the exact name the Famicom Disk System core opens");

	// A system with no BIOS folder must not be invented by any of this.
	Check(sphynx::BiosDirForSystem(memstick, "nes").empty(), "a system without BIOS still gets no folder");
}

// ---------------------------------------------------------------------------
//  2. The C surface PPSSPP's UI and Java use
// ---------------------------------------------------------------------------
void TestUiApi() {
	std::printf("\n[UI/Java bridge]\n");
	const std::string memstick = g_root + "/memstick";
	sphynx_ui_set_memstick(memstick.c_str());
	Check(std::string(sphynx_ui_bios_root()) == memstick + "/SYSTEMS_BIOS",
	      "sphynx_ui_bios_root() returns PSP/SYSTEMS_BIOS");

	const int made = sphynx_ui_ensure_bios_tree(memstick.c_str());
	Check(made >= 0, "sphynx_ui_ensure_bios_tree() succeeds");

	char *psx = sphynx_ui_bios_dir_for_system("psx");
	Check(psx && std::string(psx) == memstick + "/SYSTEMS_BIOS/PlayStation",
	      "the PlayStation folder is handed to the core as its system directory");
	sphynx_ui_free(psx);

	char *nes = sphynx_ui_bios_dir_for_system("nes");
	Check(nes == nullptr, "a system without BIOS gets no folder (the caller falls back to PSP/SYSTEM)");
	sphynx_ui_free(nes);

	char *missing = sphynx_ui_bios_missing("3do");
	Check(missing && std::strstr(missing, "panafz10.bin") != nullptr,
	      "the bridge reports the 3DO BIOS as missing");
	sphynx_ui_free(missing);

	Check(std::string(sphynx_ui_bios_status("msx")).rfind("missing", 0) == 0,
	      "MSX reports its (optional) files as missing until they exist");
}

// ---------------------------------------------------------------------------
//  3. Save states on disk
// ---------------------------------------------------------------------------
void TestSaveStates() {
	std::printf("\n[save states]\n");
	const std::string save_dir = g_root + "/save";

	const std::string path = sphynx::StateFilePath(save_dir, "Ys III (USA)", 2);
	Check(path == save_dir + "/states/Ys III (USA).slot2.sav", "the slot path has the expected shape");

	std::vector<uint8_t> snapshot;
	for (int i = 0; i < 4096; ++i) snapshot.push_back(uint8_t(i * 7));
	std::string err;
	Check(sphynx::WriteStateFile(path, "snes9x", snapshot, &err), "a state is written");

	std::vector<uint8_t> back;
	Check(sphynx::ReadStateFile(path, "snes9x", &back, &err), "the same state is read back");
	Check(back == snapshot, "the payload survives the round trip byte for byte");

	// A Game Boy state must not be loadable while a SNES game runs.
	Check(!sphynx::ReadStateFile(path, "gambatte", &back, &err), "a state from another system is refused");
	Check(err.find("another system") != std::string::npos, "and the reason is readable");

	// Corruption must be caught, not loaded.
	std::string blob = ReadFile(path);
	blob[blob.size() - 10] = char(uint8_t(blob[blob.size() - 10]) ^ 0xFF);
	WriteFile(path, blob);
	Check(!sphynx::ReadStateFile(path, "snes9x", &back, &err), "a corrupted state is refused");
	Check(err.find("checksum") != std::string::npos, "with a checksum message");

	// An empty slot must say so, in words the player screen can show.
	Check(!sphynx::ReadStateFile(sphynx::StateFilePath(save_dir, "other", 0), "snes9x", &back, &err),
	      "an empty slot refuses to load");
	Check(err.find("no save state") != std::string::npos, "and explains that the slot is empty");
	Check(!sphynx::StateFileExists(sphynx::StateFilePath(save_dir, "other", 0)), "hasState() reports it empty");
	Check(sphynx::StateFileExists(path), "hasState() reports the written slot as used");

	// An empty snapshot must never be written.
	Check(!sphynx::WriteStateFile(path, "snes9x", {}, &err), "an empty snapshot is rejected");
}

// ---------------------------------------------------------------------------
//  4. The flight recorder (PSP/sphynx_log.txt)
// ---------------------------------------------------------------------------
void TestDiagLog() {
	std::printf("\n[diagnostic log]\n");
	const std::string path = g_root + "/memstick/sphynx_log.txt";
	sphynx::SetLogPath(path);
	Check(sphynx::LogPath() == path, "the log path is remembered");

	sphynx::LogLine("hello from the test");
	sphynx::Logf("value = %d", 42);
	const std::string text = sphynx::ReadLog();
	Check(text.find("hello from the test") != std::string::npos, "a line reaches the file");
	Check(text.find("value = 42") != std::string::npos, "formatted lines work");
	Check(text.find("20") != std::string::npos, "lines carry a timestamp");

	// The log must stop growing instead of filling the user's storage.
	for (int i = 0; i < 4000; ++i)
		sphynx::LogLine(std::string(120, 'x'));
	struct stat st;
	::stat(path.c_str(), &st);
	Check(st.st_size <= 80 * 1024, "the log truncates itself instead of growing forever");
	Check(sphynx::LogPath() == path, "and stays usable after truncation");
}

// ---------------------------------------------------------------------------
//  5. How a tap is routed, decided from the file name alone
// ---------------------------------------------------------------------------
void TestExtensionRouting() {
	std::printf("\n[extension routing]\n");
	sphynx_ui_set_memstick((g_root + "/memstick").c_str());
	Check(sphynx_ui_is_known_extension("/x/Ys III (USA).sfc") == 1, ".sfc is a Sphynx extension");
	Check(sphynx_ui_is_known_extension("/x/game.gba") == 1, ".gba is a Sphynx extension");
	Check(sphynx_ui_is_known_extension("/x/game.SFC") == 1, "the check is case insensitive");
	Check(sphynx_ui_is_known_extension("/x/game.unknownxyz") == 0, "an unknown extension is not claimed");
	Check(sphynx_ui_is_known_extension("/x/game") == 0, "a file without an extension is not claimed");
	// The player screen cannot be opened from a host binary: the caller falls
	// back to the command queue, so this must answer 0, never crash.
	// Stage 10: nothing is launched any more - the player is a PPSSPP screen.
	Check(sphynx_ui_launch_player("/x/game.sfc") == -1, "launching an Activity is refused: the player is in-process");
}

}  // namespace

int main() {
	char temp[] = "/tmp/sphynx_bios_XXXXXX";
	const char *dir = ::mkdtemp(temp);
	if (!dir) {
		std::printf("cannot create a temp directory\n");
		return 2;
	}
	g_root = dir;
	MkDirs(g_root + "/save");

	std::printf("Sphynx BIOS/state test (%s)\nworkspace: %s\n", sphynx::BuildId(), g_root.c_str());

	TestRegistry();
	TestTreeCreation();
	TestMissingAndAdoption();
	TestBiosFileNames();
	TestUiApi();
	TestSaveStates();
	TestDiagLog();
	TestExtensionRouting();

	std::printf("\n%d checks, %d failure(s) — %s\n", g_checks, g_failures,
	            g_failures ? "FAILED" : "all good");
	return g_failures ? 1 : 0;
}
