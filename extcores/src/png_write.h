// Sphynx — internal PNG writer interface.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <vector>

namespace sphynx {
// Encodes 8-bit RGBA pixels as a PNG. Empty vector on invalid input.
std::vector<uint8_t> EncodePngRgba(const uint8_t *rgba, int width, int height);
}  // namespace sphynx
