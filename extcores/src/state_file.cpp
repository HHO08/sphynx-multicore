// ============================================================================
//  Sphynx — extcores/src/state_file.cpp
//
//  See state_file.h. Kept free of Android headers so the exact shipped code is
//  what the CI host test runs.
//
//  SPDX-License-Identifier: GPL-2.0-or-later
// ============================================================================
#include "state_file.h"

#include <cerrno>
#include <cstdio>
#include <cstring>

#include <sys/stat.h>
#include <sys/types.h>

namespace sphynx {
namespace {

constexpr char kMagic[8] = {'S', 'P', 'H', 'Y', 'N', 'X', 'S', 'T'};
constexpr uint32_t kVersion = 1;

bool DirectoryExists(const std::string &path) {
	struct stat st;
	return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

// mkdir -p (only two levels are ever needed here, but the loop is harmless).
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
			cur = (cur.empty() || cur.back() == '/') ? cur + part : cur + "/" + part;
			if (!DirectoryExists(cur) && ::mkdir(cur.c_str(), 0777) != 0 && errno != EEXIST) return false;
		}
		if (slash == std::string::npos) break;
		i = slash + 1;
	}
	return DirectoryExists(path);
}

std::string DirName(const std::string &path) {
	const size_t slash = path.find_last_of('/');
	if (slash == std::string::npos) return std::string();
	if (slash == 0) return "/";
	return path.substr(0, slash);
}

void PutU32(std::vector<uint8_t> *out, uint32_t v) {
	out->push_back(uint8_t(v & 0xFF));
	out->push_back(uint8_t((v >> 8) & 0xFF));
	out->push_back(uint8_t((v >> 16) & 0xFF));
	out->push_back(uint8_t((v >> 24) & 0xFF));
}

bool GetU32(const std::vector<uint8_t> &in, size_t *pos, uint32_t *v) {
	if (*pos + 4 > in.size()) return false;
	*v = uint32_t(in[*pos]) | (uint32_t(in[*pos + 1]) << 8) | (uint32_t(in[*pos + 2]) << 16) |
	     (uint32_t(in[*pos + 3]) << 24);
	*pos += 4;
	return true;
}

}  // namespace

uint32_t Crc32(const uint8_t *data, size_t size) {
	static uint32_t table[256];
	static bool ready = false;
	if (!ready) {
		for (uint32_t i = 0; i < 256; ++i) {
			uint32_t c = i;
			for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
			table[i] = c;
		}
		ready = true;
	}
	uint32_t crc = 0xFFFFFFFFu;
	for (size_t i = 0; i < size; ++i) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
	return crc ^ 0xFFFFFFFFu;
}

std::string StateFilePath(const std::string &save_dir, const std::string &game_tag, int slot) {
	if (save_dir.empty()) return std::string();
	if (slot < 0) slot = 0;
	std::string path = save_dir;
	if (path.back() != '/') path += '/';
	path += "states/";
	path += game_tag;
	path += ".slot";
	path += std::to_string(slot);
	path += ".sav";
	return path;
}

bool StateFileExists(const std::string &path) {
	struct stat st;
	return !path.empty() && ::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

bool WriteStateFile(const std::string &path, const std::string &core_tag,
                    const std::vector<uint8_t> &state, std::string *error) {
	if (path.empty()) {
		if (error) *error = "no save directory";
		return false;
	}
	if (state.empty()) {
		if (error) *error = "the emulator produced an empty save state";
		return false;
	}
	if (!MkdirP(DirName(path))) {
		if (error) *error = "cannot create the save state folder";
		return false;
	}

	std::vector<uint8_t> blob;
	blob.insert(blob.end(), kMagic, kMagic + sizeof(kMagic));
	PutU32(&blob, kVersion);
	PutU32(&blob, uint32_t(core_tag.size()));
	PutU32(&blob, uint32_t(state.size()));
	PutU32(&blob, Crc32(state.data(), state.size()));
	blob.insert(blob.end(), core_tag.begin(), core_tag.end());
	blob.insert(blob.end(), state.begin(), state.end());

	// Write to a temporary file and rename: a save interrupted by the system
	// (battery, task kill) must never leave a half-written state behind.
	const std::string tmp = path + ".part";
	FILE *f = std::fopen(tmp.c_str(), "wb");
	if (!f) {
		if (error) *error = "cannot write " + path;
		return false;
	}
	const size_t wrote = std::fwrite(blob.data(), 1, blob.size(), f);
	std::fclose(f);
	if (wrote != blob.size()) {
		::remove(tmp.c_str());
		if (error) *error = "storage is full";
		return false;
	}
	::remove(path.c_str());
	if (::rename(tmp.c_str(), path.c_str()) != 0) {
		::remove(tmp.c_str());
		if (error) *error = "cannot finalise the save state file";
		return false;
	}
	return true;
}

bool ReadStateFile(const std::string &path, const std::string &expected_core,
                   std::vector<uint8_t> *out, std::string *error) {
	if (!out) return false;
	out->clear();
	if (!StateFileExists(path)) {
		if (error) *error = "there is no save state in this slot yet";
		return false;
	}
	FILE *f = std::fopen(path.c_str(), "rb");
	if (!f) {
		if (error) *error = "cannot open the save state file";
		return false;
	}
	std::vector<uint8_t> blob;
	char buffer[64 * 1024];
	size_t got = 0;
	while ((got = std::fread(buffer, 1, sizeof(buffer), f)) > 0) blob.insert(blob.end(), buffer, buffer + got);
	std::fclose(f);

	if (blob.size() < sizeof(kMagic) + 16 || std::memcmp(blob.data(), kMagic, sizeof(kMagic)) != 0) {
		if (error) *error = "this file is not a Sphynx save state";
		return false;
	}
	size_t pos = sizeof(kMagic);
	uint32_t version = 0, tag_len = 0, payload_len = 0, crc = 0;
	if (!GetU32(blob, &pos, &version) || !GetU32(blob, &pos, &tag_len) ||
	    !GetU32(blob, &pos, &payload_len) || !GetU32(blob, &pos, &crc)) {
		if (error) *error = "this save state is damaged";
		return false;
	}
	if (version != kVersion) {
		if (error) *error = "this save state comes from another Sphynx version";
		return false;
	}
	if (pos + tag_len + payload_len != blob.size()) {
		if (error) *error = "this save state is truncated or damaged";
		return false;
	}
	const std::string tag(reinterpret_cast<const char *>(blob.data() + pos), tag_len);
	pos += tag_len;
	out->assign(blob.begin() + pos, blob.begin() + pos + payload_len);
	if (Crc32(out->data(), out->size()) != crc) {
		out->clear();
		if (error) *error = "this save state is corrupt (checksum mismatch)";
		return false;
	}
	if (!expected_core.empty() && tag != expected_core) {
		out->clear();
		if (error) *error = "this save state belongs to another system (" + tag + ")";
		return false;
	}
	return true;
}

}  // namespace sphynx
