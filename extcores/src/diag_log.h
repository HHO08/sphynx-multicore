// ============================================================================
//  Sphynx — extcores/src/diag_log.h
//
//  A tiny, always-on flight recorder: PSP/sphynx_log.txt
//
//  Why it exists
//  -------------
//  When something goes wrong on a device I cannot see, "it did not work" is
//  not enough to fix it. This log records, in order, what the app decided:
//  which file was tapped, which system the detector guessed, whether the
//  player screen was opened, why a core refused to start, where the BIOS
//  folders were created (or why they could not be).
//
//  It is plain text, capped at 64 KiB (it truncates itself), and never leaves
//  the device on its own: nothing is uploaded anywhere.
//
//  Portable C++17, so the host tests exercise the same code.
//
//  SPDX-License-Identifier: GPL-2.0-or-later
// ============================================================================
#pragma once

#include <string>

namespace sphynx {

// Where the log lives (usually <memstick>/sphynx_log.txt). Empty = disabled.
void SetLogPath(const std::string &path);

// SPHYNX (stage 17): true when a line could really be appended to `path` right
// now. The memstick folder the user picked can be READ by the app but not
// written (for example /storage/emulated/0/Android/media/PSP is another app's
// media folder), and in that case every log line was lost without a word - the
// failure panel then said "(log is empty)", which is the least useful thing it
// can say. With this check the caller can fall back to a folder the app owns.
bool LogPathWritable(const std::string &path);
std::string LogPath();

// Appends one line: "2026-11-06 22:10:03  text".
void LogLine(const std::string &text);

// Same, with printf formatting. Kept simple (512 bytes per line).
void Logf(const char *format, ...);

// Reads the whole log back (used by tests and by the "send me the log" flow).
std::string ReadLog();

}  // namespace sphynx
