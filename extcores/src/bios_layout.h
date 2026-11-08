// ============================================================================
//  Sphynx — extcores/src/bios_layout.h
//
//  The BIOS folder the user asked for: PSP/SYSTEMS_BIOS
//
//  Layout
//  ------
//    <memstick>/                     <- PPSSPP's own memstick root (".../PSP")
//      SYSTEMS_BIOS/
//        00-READ-ME-FIRST.txt        <- what this tree is, in Arabic + English
//        01-PlayStation/  README.txt
//        02-Sega Saturn/  README.txt
//        ...
//
//  Only systems that actually need a BIOS get a folder:
//    * 12 systems that cannot run without one,
//    * 4 systems that run without one but work much better with it.
//  The other 19 systems get no folder at all, exactly as requested.
//
//  Rules this file follows
//  -----------------------
//    * never deletes, never overwrites a file the user put there;
//    * every folder is created again on each start if the user removed it;
//    * BIOS files already sitting in PPSSPP's own PSP/SYSTEM are copied into
//      the matching system folder, so an existing PPSSPP setup keeps working;
//    * the emulator core of a system is given *its own* folder as the libretro
//      "system directory", which is what every core looks in for its BIOS.
//
//  SPDX-License-Identifier: GPL-2.0-or-later
// ============================================================================
#pragma once

#include <string>

namespace sphynx {

// One system that has a BIOS folder.
struct BiosSystem {
	const char *system_key = nullptr;      // registry key: "psx", "saturn", ...
	const char *folder = nullptr;          // folder name inside SYSTEMS_BIOS
	const char *const *files = nullptr;    // expected file names (NULL-terminated)
	int file_count = 0;
	// true  = the system cannot run without BIOS (folder is mandatory)
	// false = it runs without BIOS, but works much better with it
	bool mandatory = false;
	const char *note = nullptr;            // one line shown in the README
	// SPHYNX (stage 15): most systems accept *one* of several dumps - three
	// PlayStation regions, two Saturn regions, ... - and any single one is
	// enough. The Nintendo DS is the exception: bios7.bin, bios9.bin and
	// firmware.bin are three different things that are all needed, and reporting
	// it as "complete" because one of them was there is what made a white screen
	// look like a mystery. true = every file is required.
	bool all_required = false;
};

// Root folder name, fixed by the request: PSP/SYSTEMS_BIOS
constexpr const char *kSphynxBiosFolderName = "SYSTEMS_BIOS";

int BiosSystemCount();                          // 16: 12 mandatory + 4 recommended
const BiosSystem *AllBiosSystems();
const BiosSystem *FindBiosSystem(const char *system_key);   // NULL when no BIOS

int MandatoryBiosCount();
int RecommendedBiosCount();

// <memstick>/SYSTEMS_BIOS  (no trailing slash)
std::string BiosRoot(const std::string &memstick_dir);

// Folder of one system, e.g. <memstick>/SYSTEMS_BIOS/03-Sega Saturn.
// Empty string when the system needs no BIOS at all.
std::string BiosDirForSystem(const std::string &memstick_dir, const char *system_key);

// Creates the whole tree (folder + README.txt for every BIOS system) and the
// top-level READ-ME-FIRST.txt. Idempotent and safe to call on every start.
// Returns the number of folders that had to be created, -1 on a hard error.
int EnsureBiosTree(const std::string &memstick_dir, std::string *report);

// Copies expected BIOS files from <memstick>/SYSTEM and from the .zip-free
// root of <memstick> into the system folders, only when the destination file
// does not exist yet. Returns how many files were adopted.
int AdoptLegacyBios(const std::string &memstick_dir, std::string *report);

// SPHYNX (stage 15): a core opens its BIOS with a plain, case-sensitive
// fopen("<exact name>"). A user who drops "BIOS7.BIN" or "NDS_Bios7.bin" into
// the folder therefore had the file *right there* and the core still refused to
// boot (the Nintendo DS white screens). This pass finds such a file and gives it
// the exact name the core asks for, by renaming it inside its own folder.
//   * it never touches a file that already has the exact name;
//   * it never deletes anything: a rename is tried first, a copy second;
//   * returns how many names were corrected (the report lists them).
int NormalizeBiosNames(const std::string &memstick_dir, std::string *report);

// One folder + which expected files are really present, with their sizes, e.g.
//   "nds · .../SYSTEMS_BIOS/Nintendo DS · bios7.bin (16 KiB), firmware.bin
//    (256 KiB) · missing: bios9.bin"
// This is what the player shows when a core starts but never draws a picture:
// it tells "the file is not where the core looks" apart from "the file is
// there and the core still refuses it".
std::string BiosReport(const std::string &memstick_dir, const char *system_key);

// Same report for an explicit folder (stage 17): the player already knows which
// folder it will hand the core, so it does not have to rebuild the path - and
// with several PSP roots on the device the two could disagree.
std::string BiosReportInDir(const std::string &dir, const char *system_key);

// ---------------------------------------------------------------------------
//  Start-up cost (stage 16)
//
//  The full pass above (create + adopt + rename) is what the app ran on every
//  start *and* on every game launch, and it is the reason the app began with a
//  black screen for seconds: it reads every candidate source folder once per
//  expected file, and those folders live behind Android's FUSE layer. Two
//  additions make it cost nothing when there is nothing to do:
//
//    * BiosTreeLooksReady() - a stamp file plus one stat per folder: when the
//      tree is already complete, the whole pass is skipped;
//    * PrepareBiosForSystem() - the cheap per-system version used when a game
//      is opened: only that system's folder is touched.
//
//  Neither changes what ends up in the tree: the full pass still runs the first
//  time (install, update, or a folder the user deleted), and the per-system
//  pass still adopts and renames inside the folder the game is about to use.
// ---------------------------------------------------------------------------

// True when the tree exists, every system folder is there, and the stamp file
// written by MarkBiosTreeReady() is present. A few stats, no directory reads.
bool BiosTreeLooksReady(const std::string &memstick_dir);

// Records that the full pass completed for this tree (one small file).
void MarkBiosTreeReady(const std::string &memstick_dir);

// The cheap per-game pass: ensures this system's folder exists, adopts its
// files from PSP/SYSTEM and from PSP/ itself, and gives them the exact names
// the core opens. Touches nothing else. Returns how many names were corrected.
int PrepareBiosForSystem(const std::string &memstick_dir, const char *system_key,
                         std::string *report);

// Comma separated list of the expected files that are missing ("" = complete).
// An empty BIOS folder therefore reports every file of that system.
std::string MissingBiosFiles(const std::string &memstick_dir, const char *system_key);

// Same question, asked about an explicit folder. The player already holds the
// per-system folder, so it does not have to rebuild the path.
std::string MissingBiosFilesInDir(const std::string &bios_dir, const char *system_key);

// Human readable status of one system: "complete", "missing A, B" or
// "no BIOS needed". Used by the player's failure screen.
std::string BiosStatusForSystem(const std::string &memstick_dir, const char *system_key);

// SPHYNX (stage 17): true when `dir` holds at least one of the files this
// system needs (any capitalisation). Used to decide *which* PSP folder is the
// one the user actually filled, when several PSP folders exist on the device.
bool BiosDirHasAnyFile(const std::string &dir, const char *system_key);

// True when the path exists and is a regular file (small helper, also used by
// the tests; exposed so callers do not repeat the stat dance).
bool FileExists(const std::string &path);
bool DirectoryExists(const std::string &path);

}  // namespace sphynx
