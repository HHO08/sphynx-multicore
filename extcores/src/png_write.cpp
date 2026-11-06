// ============================================================================
//  Sphynx — extcores/src/png_write.cpp
//
//  Minimal PNG encoder (8-bit RGBA).  Written instead of pulling a dependency:
//  the only thing Sphynx writes is a thumbnail, and a stored-deflate PNG is a
//  few dozen lines with no external library, no licence question and no build
//  cost.  Files are larger than a zlib-compressed PNG, but they are written
//  once per game and cached.
//
//  SPDX-License-Identifier: GPL-2.0-or-later
// ============================================================================
#include "png_write.h"

#include <cstdlib>
#include <cstring>
#include <vector>

namespace sphynx {
namespace {

uint32_t Crc32(const uint8_t *data, size_t len, uint32_t crc = 0xFFFFFFFFu) {
	static uint32_t table[256];
	static bool init = false;
	if (!init) {
		for (uint32_t i = 0; i < 256; ++i) {
			uint32_t c = i;
			for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
			table[i] = c;
		}
		init = true;
	}
	for (size_t i = 0; i < len; ++i) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
	return crc;
}

void Put32(std::vector<uint8_t> &v, uint32_t x) {
	v.push_back(uint8_t(x >> 24));
	v.push_back(uint8_t(x >> 16));
	v.push_back(uint8_t(x >> 8));
	v.push_back(uint8_t(x));
}

void Chunk(std::vector<uint8_t> &out, const char *type, const std::vector<uint8_t> &data) {
	Put32(out, uint32_t(data.size()));
	std::vector<uint8_t> crcBuf;
	crcBuf.reserve(4 + data.size());
	const uint8_t *t = reinterpret_cast<const uint8_t *>(type);
	crcBuf.insert(crcBuf.end(), t, t + 4);
	crcBuf.insert(crcBuf.end(), data.begin(), data.end());
	out.insert(out.end(), t, t + 4);
	out.insert(out.end(), data.begin(), data.end());
	Put32(out, Crc32(crcBuf.data(), crcBuf.size()) ^ 0xFFFFFFFFu);
}

// zlib stream made of stored (uncompressed) deflate blocks — valid per RFC1951.
std::vector<uint8_t> ZlibStored(const uint8_t *raw, size_t len) {
	std::vector<uint8_t> out;
	out.push_back(0x78);  // CMF: deflate, 32K window
	out.push_back(0x01);  // FLG: no dict, fastest; (0x78*256+0x01) % 31 == 0
	size_t pos = 0;
	while (pos < len) {
		const size_t block = std::min<size_t>(65535, len - pos);
		const bool last = (pos + block == len);
		out.push_back(last ? 1 : 0);            // BFINAL + BTYPE=00
		out.push_back(uint8_t(block & 0xFF));   // LEN
		out.push_back(uint8_t(block >> 8));
		out.push_back(uint8_t((~block) & 0xFF));  // NLEN
		out.push_back(uint8_t(((~block) >> 8) & 0xFF));
		out.insert(out.end(), raw + pos, raw + pos + block);
		pos += block;
	}
	if (len == 0) {
		out.push_back(1);
		out.push_back(0);
		out.push_back(0);
		out.push_back(0xFF);
		out.push_back(0xFF);
	}
	// Adler-32 of the uncompressed data.
	uint32_t a = 1, b = 0;
	for (size_t i = 0; i < len; ++i) {
		a = (a + raw[i]) % 65521;
		b = (b + a) % 65521;
	}
	const uint32_t adler = (b << 16) | a;
	out.push_back(uint8_t(adler >> 24));
	out.push_back(uint8_t(adler >> 16));
	out.push_back(uint8_t(adler >> 8));
	out.push_back(uint8_t(adler));
	return out;
}

}  // namespace

std::vector<uint8_t> EncodePngRgba(const uint8_t *rgba, int width, int height) {
	std::vector<uint8_t> png;
	if (!rgba || width <= 0 || height <= 0) return png;

	static const uint8_t kSig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
	png.insert(png.end(), kSig, kSig + 8);

	std::vector<uint8_t> ihdr;
	Put32(ihdr, uint32_t(width));
	Put32(ihdr, uint32_t(height));
	ihdr.push_back(8);  // bit depth
	ihdr.push_back(6);  // colour type RGBA
	ihdr.push_back(0);  // deflate
	ihdr.push_back(0);  // adaptive filtering
	ihdr.push_back(0);  // no interlace
	Chunk(png, "IHDR", ihdr);

	// Scanlines with filter type 0 (None).
	std::vector<uint8_t> raw;
	raw.reserve(size_t(height) * (1 + size_t(width) * 4));
	for (int y = 0; y < height; ++y) {
		raw.push_back(0);
		const uint8_t *row = rgba + size_t(y) * width * 4;
		raw.insert(raw.end(), row, row + size_t(width) * 4);
	}
	Chunk(png, "IDAT", ZlibStored(raw.data(), raw.size()));
	Chunk(png, "IEND", {});
	return png;
}

}  // namespace sphynx
