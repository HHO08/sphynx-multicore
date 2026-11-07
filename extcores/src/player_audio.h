// ============================================================================
//  Sphynx — extcores/src/player_audio.h  (internal)
//
//  A very small stereo PCM queue that the player pushes audio into and that a
//  platform backend drains. On Android the backend is OpenSL ES (see
//  player_audio.cpp); everywhere else it is a no-op, so the host tests can link
//  the player without a sound card.
//
//  SPDX-License-Identifier: GPL-2.0-or-later
// ============================================================================
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace sphynx {

class AudioOutput {
public:
	virtual ~AudioOutput() = default;

	// Starts output at `sample_rate` Hz (interleaved stereo, 16 bit). Returns
	// false when the device cannot be opened: the player keeps running, silent.
	virtual bool Start(double sample_rate) = 0;
	virtual void Stop() = 0;
	// Called from the emulation thread with interleaved stereo frames.
	virtual void Push(const int16_t *frames, size_t frame_count) = 0;
	virtual bool IsRunning() const = 0;
};

// Returns a backend for this platform. Never null.
std::unique_ptr<AudioOutput> CreateAudioOutput();

}  // namespace sphynx
