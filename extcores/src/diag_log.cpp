// ============================================================================
//  Sphynx — extcores/src/diag_log.cpp  (see diag_log.h)
//  SPDX-License-Identifier: GPL-2.0-or-later
// ============================================================================
#include "diag_log.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>

#include <sys/stat.h>
#include <sys/types.h>

namespace sphynx {
namespace {

constexpr long kMaxLogBytes = 64 * 1024;   // keeps the file readable

std::string g_path;

bool Exists(const std::string &p) {
	struct stat st;
	return ::stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

long SizeOf(const std::string &p) {
	struct stat st;
	return ::stat(p.c_str(), &st) == 0 ? long(st.st_size) : -1;
}

}  // namespace

void SetLogPath(const std::string &path) { g_path = path; }

bool LogPathWritable(const std::string &path) {
	if (path.empty()) return false;
	// The real test, not a guess: open for append and write nothing meaningful.
	FILE *f = std::fopen(path.c_str(), "ab");
	if (!f) return false;
	std::fclose(f);
	return true;
}

std::string LogPath() { return g_path; }

void LogLine(const std::string &text) {
	if (g_path.empty()) return;
	// Self-truncating: a runaway log must never fill the user's storage.
	if (SizeOf(g_path) > kMaxLogBytes) ::remove(g_path.c_str());

	FILE *f = std::fopen(g_path.c_str(), "ab");
	if (!f) return;
	const std::time_t now = std::time(nullptr);
	char stamp[32];
	std::tm tm_buf{};
#if defined(_WIN32)
	localtime_s(&tm_buf, &now);
#else
	localtime_r(&now, &tm_buf);
#endif
	std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &tm_buf);
	std::fprintf(f, "%s  %s\n", stamp, text.c_str());
	std::fclose(f);
}

void Logf(const char *format, ...) {
	if (g_path.empty() || !format) return;
	char buffer[512];
	va_list args;
	va_start(args, format);
	std::vsnprintf(buffer, sizeof(buffer), format, args);
	va_end(args);
	LogLine(buffer);
}

std::string ReadLog() {
	if (g_path.empty() || !Exists(g_path)) return std::string();
	FILE *f = std::fopen(g_path.c_str(), "rb");
	if (!f) return std::string();
	std::string out;
	char buffer[4096];
	size_t n = 0;
	while ((n = std::fread(buffer, 1, sizeof(buffer), f)) > 0) out.append(buffer, n);
	std::fclose(f);
	return out;
}

}  // namespace sphynx
