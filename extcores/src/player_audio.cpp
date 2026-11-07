// ============================================================================
//  Sphynx — extcores/src/player_audio.cpp
//
//  Sound for the external systems, inside PPSSPP's own process.
//
//  Stage 9 moved the player into PPSSPP's UI, so the old scheme (a Java
//  AudioTrack drained by a helper thread in a separate Activity) had to go: it
//  was the last piece that could outlive the screen and keep the microphone of
//  the audio HAL busy after the game was closed.
//
//  What is left is one OpenSL ES buffer queue fed by a small lock-protected
//  ring buffer. The emulation thread pushes, OpenSL's callback thread pulls.
//  Underruns are filled with silence rather than blocking, because blocking the
//  emulation thread on the audio device is exactly how a "black screen then
//  kicked out" bug happens.
//
//  On non-Android builds this file compiles to a silent stub, so the host tests
//  link the player without needing a sound card.
//
//  SPDX-License-Identifier: GPL-2.0-or-later
// ============================================================================
#include "player_audio.h"

#include <algorithm>
#include <cstring>

#if defined(SPHYNX_ANDROID)

#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>

#include <android/log.h>

#define ALOG(...) __android_log_print(ANDROID_LOG_INFO, "SphynxAudio", __VA_ARGS__)

namespace sphynx {
namespace {

// SPHYNX (audio, stage 13): four buffers of 16 ms plus a ring that is often
// *empty* is what "crackling, popping, choppy sound" literally is. OpenSL asks
// for the next 16 ms and Fill() has nothing: this file used to pad the gap with
// silence, so every scheduler hiccup became an audible click and every slow
// emulated frame became a hole in the sound.
//
// Three changes, all of them the standard cure:
//   * deeper queue - 8 buffers of 16 ms, so a 100 ms stall costs nothing;
//   * a prefill - the queue is started with enough silence to cover the
//     latency of the first emulated frames instead of underrunning instantly;
//   * an underrun is bridged by repeating the last delivered sample instead of
//     jumping to zero (a click), and it is counted so it can be seen.
constexpr int kBufferCount = 8;
constexpr int kRingMs = 500;      // how much audio the ring can hold
constexpr int kPrefillBuffers = 3;  // ~48 ms of lead at start-up

class OpenSLAudio : public AudioOutput {
public:
	~OpenSLAudio() override { Stop(); }

	bool Start(double sample_rate) override {
		Stop();
		int rate = static_cast<int>(sample_rate + 0.5);
		if (rate < 8000 || rate > 192000) rate = 44100;
		rate_ = rate;
		frames_ = std::max(256, rate / 60);
		ring_.assign(static_cast<size_t>(rate) * kRingMs / 1000 * 2, 0);
		read_ = write_ = 0;
		for (int i = 0; i < kBufferCount; ++i)
			buffers_[i].assign(static_cast<size_t>(frames_) * 2, 0);

		SLresult r = slCreateEngine(&engine_obj_, 0, nullptr, 0, nullptr, nullptr);
		if (r != SL_RESULT_SUCCESS) return false;
		if ((*engine_obj_)->Realize(engine_obj_, SL_BOOLEAN_FALSE) != SL_RESULT_SUCCESS) { Stop(); return false; }
		if ((*engine_obj_)->GetInterface(engine_obj_, SL_IID_ENGINE, &engine_) != SL_RESULT_SUCCESS) { Stop(); return false; }

		if ((*engine_)->CreateOutputMix(engine_, &mix_obj_, 0, nullptr, nullptr) != SL_RESULT_SUCCESS) { Stop(); return false; }
		if ((*mix_obj_)->Realize(mix_obj_, SL_BOOLEAN_FALSE) != SL_RESULT_SUCCESS) { Stop(); return false; }

		SLDataLocator_AndroidSimpleBufferQueue q = { SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE, kBufferCount };
		SLDataFormat_PCM fmt = {};
		fmt.formatType = SL_DATAFORMAT_PCM;
		fmt.numChannels = 2;
		// OpenSL wants milliHertz. Android's mixer converts whatever we ask for
		// to the device rate, so a 32768 Hz Game Boy + Advance core is fine.
		fmt.samplesPerSec = static_cast<SLuint32>(rate) * 1000;
		fmt.bitsPerSample = SL_PCMSAMPLEFORMAT_FIXED_16;
		fmt.containerSize = SL_PCMSAMPLEFORMAT_FIXED_16;
		fmt.channelMask = SL_SPEAKER_FRONT_LEFT | SL_SPEAKER_FRONT_RIGHT;
		fmt.endianness = SL_BYTEORDER_LITTLEENDIAN;

		SLDataSource src = { &q, &fmt };
		SLDataLocator_OutputMix out = { SL_DATALOCATOR_OUTPUTMIX, mix_obj_ };
		SLDataSink sink = { &out, nullptr };

		const SLInterfaceID ids[1] = { SL_IID_ANDROIDSIMPLEBUFFERQUEUE };
		const SLboolean req[1] = { SL_BOOLEAN_TRUE };
		if ((*engine_)->CreateAudioPlayer(engine_, &player_obj_, &src, &sink, 1, ids, req) != SL_RESULT_SUCCESS) { Stop(); return false; }
		if ((*player_obj_)->Realize(player_obj_, SL_BOOLEAN_FALSE) != SL_RESULT_SUCCESS) { Stop(); return false; }
		if ((*player_obj_)->GetInterface(player_obj_, SL_IID_PLAY, &play_) != SL_RESULT_SUCCESS) { Stop(); return false; }
		if ((*player_obj_)->GetInterface(player_obj_, SL_IID_ANDROIDSIMPLEBUFFERQUEUE, &bq_) != SL_RESULT_SUCCESS) { Stop(); return false; }
		if ((*bq_)->RegisterCallback(bq_, &OpenSLAudio::BqCallback, this) != SL_RESULT_SUCCESS) { Stop(); return false; }

		running_ = true;
		// Prime the queue with silence so the callback chain starts.
		// Start with a little silence: the first emulated frames have not produced
		// audio yet, and starting empty guarantees an immediate underrun (a pop)
		// on every launch.
		for (int i = 0; i < kPrefillBuffers; ++i) {
			std::fill(buffers_[i].begin(), buffers_[i].end(), 0);
			(*bq_)->Enqueue(bq_, buffers_[i].data(), buffers_[i].size() * sizeof(int16_t));
		}
		next_buffer_ = kPrefillBuffers % kBufferCount;
		(*play_)->SetPlayState(play_, SL_PLAYSTATE_PLAYING);
		ALOG("OpenSL started at %d Hz", rate_);
		return true;
	}

	void Stop() override {
		running_ = false;
		if (player_obj_) {
			if (play_) (*play_)->SetPlayState(play_, SL_PLAYSTATE_STOPPED);
			if (bq_) (*bq_)->Clear(bq_);
			(*player_obj_)->Destroy(player_obj_);
			player_obj_ = nullptr;
		}
		play_ = nullptr;
		bq_ = nullptr;
		if (mix_obj_) {
			(*mix_obj_)->Destroy(mix_obj_);
			mix_obj_ = nullptr;
		}
		if (engine_obj_) {
			(*engine_obj_)->Destroy(engine_obj_);
			engine_obj_ = nullptr;
		}
		engine_ = nullptr;
	}

	void Push(const int16_t *frames, size_t frame_count) override {
		if (!running_ || !frames || frame_count == 0) return;
		std::lock_guard<std::mutex> lock(mutex_);
		const size_t samples = frame_count * 2;
		const size_t capacity = ring_.size();
		if (samples >= capacity) {
			// More than the whole ring: keep the newest part only.
			std::memcpy(ring_.data(), frames + (samples - capacity), capacity * sizeof(int16_t));
			read_ = 0;
			write_ = 0;
			return;
		}
		for (size_t i = 0; i < samples; ++i) {
			ring_[write_] = frames[i];
			write_ = (write_ + 1) % capacity;
			if (write_ == read_) read_ = (read_ + 1) % capacity;   // drop oldest
		}
	}

	bool IsRunning() const override { return running_; }

private:
	static void BqCallback(SLAndroidSimpleBufferQueueItf bq, void *ctx) {
		static_cast<OpenSLAudio *>(ctx)->OnBufferDone(bq);
	}

	void OnBufferDone(SLAndroidSimpleBufferQueueItf bq) {
		if (!running_) return;
		const int index = next_buffer_;
		next_buffer_ = (next_buffer_ + 1) % kBufferCount;
		Fill(buffers_[index]);
		(*bq)->Enqueue(bq, buffers_[index].data(), buffers_[index].size() * sizeof(int16_t));
	}

	// Pull as much as the ring holds, pad the rest with silence.
	void Fill(std::vector<int16_t> &dst) {
		std::lock_guard<std::mutex> lock(mutex_);
		const size_t capacity = ring_.size();
		size_t available = (write_ + capacity - read_) % capacity;
		const size_t wanted = dst.size();
		size_t copied = std::min(available, wanted);
		for (size_t i = 0; i < copied; ++i) {
			dst[i] = ring_[read_];
			read_ = (read_ + 1) % capacity;
		}
		if (copied < wanted) {
			// Underrun: hold the last sample instead of snapping to silence. A
			// step to zero in the middle of a waveform is exactly the click that
			// was audible; holding the level is inaudible at this length.
			++underruns_;
			const int16_t last_l = copied > 1 ? dst[copied - 2] : last_l_;
			const int16_t last_r = copied > 0 ? dst[copied - 1] : last_r_;
			for (size_t i = copied; i + 1 < wanted; i += 2) {
				dst[i] = last_l;
				dst[i + 1] = last_r;
			}
			last_l_ = last_l;
			last_r_ = last_r;
		} else if (copied > 1) {
			last_l_ = dst[copied - 2];
			last_r_ = dst[copied - 1];
		}
	}

	SLObjectItf engine_obj_ = nullptr;
	SLEngineItf engine_ = nullptr;
	SLObjectItf mix_obj_ = nullptr;
	SLObjectItf player_obj_ = nullptr;
	SLPlayItf play_ = nullptr;
	SLAndroidSimpleBufferQueueItf bq_ = nullptr;

	int rate_ = 44100;
	int frames_ = 735;
	int next_buffer_ = 0;
	std::vector<int16_t> buffers_[kBufferCount];
	std::vector<int16_t> ring_;
	size_t read_ = 0, write_ = 0;
	std::mutex mutex_;
	volatile bool running_ = false;
	// Last delivered sample, used to bridge an underrun without a click.
	int16_t last_l_ = 0;
	int16_t last_r_ = 0;
	unsigned long underruns_ = 0;
};

}  // namespace

std::unique_ptr<AudioOutput> CreateAudioOutput() {
	return std::unique_ptr<AudioOutput>(new OpenSLAudio());
}

}  // namespace sphynx

#else   // !SPHYNX_ANDROID

namespace sphynx {
namespace {
class NullAudio : public AudioOutput {
public:
	bool Start(double) override { return true; }
	void Stop() override {}
	void Push(const int16_t *, size_t) override {}
	bool IsRunning() const override { return false; }
};
}  // namespace

std::unique_ptr<AudioOutput> CreateAudioOutput() {
	return std::unique_ptr<AudioOutput>(new NullAudio());
}

}  // namespace sphynx

#endif
