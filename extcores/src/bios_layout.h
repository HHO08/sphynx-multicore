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

// Comma separated list of the expected files that are missing ("" = complete).
// An empty BIOS folder therefore reports every file of that system.
std::string MissingBiosFiles(const std::string &memstick_dir, const char *system_key);

// Same question, asked about an explicit folder. The player already holds the
// per-system folder, so it does not have to rebuild the path.
std::string MissingBiosFilesInDir(const std::string &bios_dir, const char *system_key);

// Human readable status of one system: "complete", "missing A, B" or
// "no BIOS needed". Used by the player's failure screen.
std::string BiosStatusForSystem(const std::string &memstick_dir, const char *system_key);

// True when the path exists and is a regular file (small helper, also used by
// the tests; exposed so callers do not repeat the stat dance).
bool FileExists(const std::string &path);
bool DirectoryExists(const std::string &path);

}  // namespace sphynx
