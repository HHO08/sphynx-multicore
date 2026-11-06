// ============================================================================
//  Sphynx — extcores/src/state_file.h
//
//  Save / load state, on disk, for every external system.
//
//  The state itself comes from the emulator core (retro_serialize /
//  retro_unserialize). This file only wraps it in something that can be
//  written to storage and validated when it comes back:
//
//    "SPHYNXST" | version | core tag | payload size | crc32 | payload
//
//  The core tag (for example "gambatte") and the payload size are checked
//  before the snapshot is handed back to a core, so loading a Game Boy state
//  while a SNES game is running fails with a readable message instead of
//  corrupting the emulation.
//
//  Portable C++17: the same code runs on Android and in the host tests.
//
//  SPDX-License-Identifier: GPL-2.0-or-later
// ============================================================================
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace sphynx {

// <save_dir>/states/<game_tag>.slot<N>.sav
std::string StateFilePath(const std::string &save_dir, const std::string &game_tag, int slot);

// Writes a snapshot. Returns false and fills `error` when the file cannot be
// written (read-only storage, no space, ...).
bool WriteStateFile(const std::string &path, const std::string &core_tag,
                    const std::vector<uint8_t> &state, std::string *error);

// Reads a snapshot back. `expected_core` must match the tag stored in the file;
// pass an empty string to skip that check. Returns false and fills `error`
// with a human readable reason on any mismatch.
bool ReadStateFile(const std::string &path, const std::string &expected_core,
                   std::vector<uint8_t> *out, std::string *error);

bool StateFileExists(const std::string &path);

// Exposed for the host tests.
uint32_t Crc32(const uint8_t *data, size_t size);

}  // namespace sphynx
