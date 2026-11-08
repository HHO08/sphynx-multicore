// ============================================================================
//  Sphynx — extcores/src/session.cpp
//
//  One running external core: load, feed, drain, stop.
//
//  Design notes that matter for the "PSP must never be affected" rule:
//    * The PSP core is not reachable from this file. It has no include of
//      anything under Core/, and it never calls into PPSSPP.
//    * Every fault path (missing core, bad content, watchdog trip, crash of the
//      child process) ends in Stop() + an error string, never in a hard abort.
//    * The watchdog measures per-frame wall time; a hung core is detected even
//      if it never returns from retro_run() (the Android layer runs this in its
//      own process, so the timeout kills that process only).
//
//  SPDX-License-Identifier: GPL-2.0-or-later
// ============================================================================
#include "core_symbols.h"

#include <chrono>
#include <cstring>

namespace sphynx {
namespace {

using Clock = std::chrono::steady_clock;

double MsSince(Clock::time_point t0) {
	const auto d = Clock::now() - t0;
	return std::chrono::duration<double, std::milli>(d).count();
}

// libretro callbacks are plain C function pointers, so the target objects are
// reached through one thread-local slot per role.
thread_local Session *tls_session = nullptr;
thread_local FrameSink *tls_frame = nullptr;
thread_local AudioSink *tls_audio = nullptr;
thread_local Session::InputSource *tls_input = nullptr;
thread_local const Environment *tls_env = nullptr;

void CbVideo(const void *data, unsigned width, unsigned height, size_t pitch) {
	if (!tls_frame) return;
	// SPHYNX (stage 19): a hardware-rendering core sends no pixels at all - it
	// drew into the framebuffer the frontend handed it and signals the frame
	// with RETRO_HW_FRAME_BUFFER_VALID. The player reads that framebuffer back
	// right after the frame and delivers the picture through the usual path.
	if (data == RETRO_HW_FRAME_BUFFER_VALID) {
		tls_frame->OnHardwareFrame(width, height);
		return;
	}
	if (!data) return;   // core asking us to duplicate the previous frame
	// The format is whatever the core negotiated through SET_PIXEL_FORMAT.
	// Assuming RGB565 here made every core that runs in XRGB8888 (Flycast,
	// bsnes, MelonDS...) draw garbage, so ask the environment instead.
	const PixelFormat fmt = tls_env ? tls_env->GetPixelFormat() : PixelFormat::RGB565;
	tls_frame->OnFrame(data, width, height, pitch, fmt);
}

void CbAudioSample(int16_t left, int16_t right) {
	if (!tls_session) return;
	// SPHYNX (stage 15 audio): staged on the core thread and handed to the audio
	// queue once per frame, instead of one lock + one ring write per sample.
	tls_session->QueueAudioSample(left, right);
}

size_t CbAudioBatch(const int16_t *data, size_t frames) {
	if (!tls_audio || !data) return frames;
	tls_audio->OnAudio(data, frames);
	return frames;
}

void CbInputPoll() {
	// PPSSPP polls its own input once per frame; nothing to do here.
}

int16_t CbInputState(unsigned port, unsigned device, unsigned index, unsigned id) {
	if (!tls_input || !*tls_input) return 0;
	return (*tls_input)(port, device, index, id);
}

}  // namespace

Session::Session() = default;
Session::~Session() { Stop(); }

bool Session::Start(const std::string &core_path, const std::string &content_path,
                    const std::string &system_dir, const std::string &save_dir,
                    std::string *error) {
	Stop();
	error_.clear();
	last_run_failed_ = false;
	stats_ = SessionStats();
	content_ = content_path;
	system_dir_ = system_dir;
	save_dir_ = save_dir;

	if (!loader_.Open(core_path)) {
		error_ = loader_.LastError();
		if (error) *error = error_;
		return false;
	}

	env_.SetDirectories(system_dir, save_dir);

	// 1. environment must be installed before init(): cores query it there.
	Environment::BindActiveInstance(&env_);
	loader_.sym->set_environment(&Environment::Trampoline);

	// 2. wire the sinks for this thread before the core starts producing.
	tls_session = this;
	tls_frame = frame_sink_;
	tls_audio = audio_sink_;
	tls_input = &input_;
	tls_env = &env_;

	loader_.sym->set_video_refresh(&CbVideo);
	loader_.sym->set_audio_sample(&CbAudioSample);
	loader_.sym->set_audio_sample_batch(&CbAudioBatch);
	loader_.sym->set_input_poll(&CbInputPoll);
	loader_.sym->set_input_state(&CbInputState);

	loader_.sym->init();

	// 3. load content. A core that needs a full path (DOSBox, ScummVM, Flycast)
	//    asks for it through the environment; we always pass both.
	retro_game_info info{};
	info.path = content_.c_str();
	info.data = nullptr;
	info.size = 0;
	info.meta = nullptr;

	if (!loader_.sym->load_game(&info)) {
		error_ = "core refused this content (unsupported format or missing BIOS)";
		// SPHYNX (stage 19): if the core said why, say it too - that sentence is
		// the difference between "the file is wrong" and "the renderer is
		// missing", and the core is the only one that knows.
		const std::string said = LastCoreMessage();
		if (!said.empty()) error_ += " — " + said;
		if (error) *error = error_;
		loader_.sym->deinit();
		loader_.Close();
		return false;
	}

	// The core's real audio rate matters for the player's AudioTrack.
	{
		retro_system_av_info av{};
		if (loader_.sym->get_system_av_info) {
			loader_.sym->get_system_av_info(&av);
			if (av.timing.sample_rate > 1000.0) sample_rate_ = av.timing.sample_rate;
			// A PAL core reports 50.0, an NTSC one 59.94. Values outside a sane
			// range are ignored so a broken core cannot make the pacer insane.
			if (av.timing.fps > 20.0 && av.timing.fps < 200.0) frame_rate_ = av.timing.fps;
		}
	}

	running_ = true;
	return true;
}

void Session::QueueAudioSample(int16_t left, int16_t right) {
	audio_stage_.push_back(left);
	audio_stage_.push_back(right);
}

void Session::FlushAudioStage() {
	if (audio_stage_.empty()) return;
	if (audio_sink_ && running_) {
		const size_t frames = audio_stage_.size() / 2;
		audio_sink_->OnAudio(audio_stage_.data(), frames);
		stats_.audio_frames += frames;
	}
	// Kept, not freed: the buffer is reused for the next frame, so a 60 Hz game
	// does not allocate 60 times a second.
	audio_stage_.clear();
}

bool Session::RunFrame() {
	if (!running_ || !loader_.sym) return false;
	const auto t0 = Clock::now();
	loader_.sym->run();
	// Everything the core produced for this frame reaches the device here.
	FlushAudioStage();
	const double ms = MsSince(t0);
	stats_.frames++;
	stats_.last_frame_ms = ms;
	if (ms > stats_.worst_frame_ms) stats_.worst_frame_ms = ms;
	if (watchdog_ms_ > 0 && ms > static_cast<double>(watchdog_ms_)) {
		// The frame took longer than the watchdog budget: the core is hung or
		// hopelessly slow for this device. Stop cleanly with a readable reason
		// instead of freezing the UI.
		stats_.watchdog_trips++;
		error_ = "core exceeded the frame budget (possible hang)";
		last_run_failed_ = true;
		Stop();
		return false;
	}
	return true;
}

void Session::AbandonCore() {
	// SPHYNX (stage 18): a core that has just faulted is dropped untouched.
	// Everything below is bookkeeping in *this* process; nothing calls into the
	// crashed library again, so a bad pointer inside it cannot fault twice.
	running_ = false;
	last_run_failed_ = true;
	if (error_.empty()) error_ = "the emulator crashed";
	audio_stage_.clear();
	loader_.ForgetWithoutClosing();
	tls_session = nullptr;
	tls_frame = nullptr;
	tls_audio = nullptr;
	tls_input = nullptr;
	tls_env = nullptr;
}

void Session::Stop() {
	if (!running_) {
		if (loader_.IsOpen()) loader_.Close();
		return;
	}
	running_ = false;
	audio_stage_.clear();   // nothing left worth playing
	if (loader_.sym) {
		loader_.sym->unload_game();
		loader_.sym->deinit();
	}
	loader_.Close();
	tls_session = nullptr;
	tls_frame = nullptr;
	tls_audio = nullptr;
	tls_input = nullptr;
	tls_env = nullptr;
}

void *Session::MemoryData(unsigned id, size_t *size) {
	if (!loader_.sym || !loader_.sym->get_memory_data) return nullptr;
	if (size) *size = loader_.sym->get_memory_size ? loader_.sym->get_memory_size(id) : 0;
	return loader_.sym->get_memory_data(id);
}

size_t Session::StateSize() const {
	if (!loader_.sym || !loader_.sym->serialize_size) return 0;
	return loader_.sym->serialize_size();
}

bool Session::SaveState(std::vector<uint8_t> *out) {
	if (!out) return false;
	const size_t size = StateSize();
	if (size == 0) return false;
	out->resize(size);
	if (!loader_.sym->serialize(out->data(), size)) {
		out->clear();
		return false;
	}
	return true;
}

bool Session::LoadState(const std::vector<uint8_t> &in) {
	if (!loader_.sym || in.empty()) return false;
	if (in.size() != StateSize()) return false;   // incompatible snapshot
	return loader_.sym->unserialize(in.data(), in.size());
}

}  // namespace sphynx
