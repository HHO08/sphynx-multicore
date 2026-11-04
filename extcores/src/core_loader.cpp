// ============================================================================
//  Sphynx — extcores/src/core_loader.cpp
//
//  dlopen()-based loader for libretro cores.
//
//  Why dynamic loading and not static linking: a core that fails to build, or
//  that faults at run time, must never affect the PSP core or the UI. Every
//  core ships as its own .so and is loaded on demand.
//
//  SPDX-License-Identifier: GPL-2.0-or-later
// ============================================================================
#include "core_symbols.h"

#include <dlfcn.h>

#include <cstring>

namespace sphynx {

namespace {
const char *const kNames[] = {
	"retro_init",
	"retro_deinit",
	"retro_api_version",
	"retro_get_system_info",
	"retro_get_system_av_info",
	"retro_set_controller_port_device",
	"retro_reset",
	"retro_run",
	"retro_serialize_size",
	"retro_serialize",
	"retro_unserialize",
	"retro_cheat_reset",
	"retro_cheat_set",
	"retro_load_game",
	"retro_load_game_special",
	"retro_unload_game",
	"retro_get_region",
	"retro_get_memory_data",
	"retro_get_memory_size",
	"retro_set_environment",
	"retro_set_video_refresh",
	"retro_set_audio_sample",
	"retro_set_audio_sample_batch",
	"retro_set_input_poll",
	"retro_set_input_state",
};
constexpr size_t kNameCount = sizeof(kNames) / sizeof(kNames[0]);
}  // namespace

const char *const *CoreSymbols::Names() { return kNames; }
size_t CoreSymbols::Count() { return kNameCount; }

CoreLoader::~CoreLoader() { Close(); }

namespace {
// Converting void* from dlsym() into a typed function pointer without relying
// on implementation-defined pointer casts.
template <typename Fn>
inline void Assign(Fn &target, void *p) {
	static_assert(sizeof(Fn) == sizeof(void *), "function pointer size mismatch");
	std::memcpy(&target, &p, sizeof(void *));
}
}  // namespace

bool CoreLoader::Open(const std::string &library_path) {
	Close();
	error_.clear();

	handle_ = dlopen(library_path.c_str(), RTLD_NOW | RTLD_LOCAL);
	if (!handle_) {
		const char *e = dlerror();
		error_ = std::string("cannot load ") + library_path + ": " +
		         (e ? e : "unknown dlopen error");
		return false;
	}

	// A partial core fails unpredictably later, so every entry point is
	// required. Missing any of them aborts the load with a clear message.
	CoreSymbols *s = new CoreSymbols();
	bool ok = true;
	std::string missing;

	// Written out one by one so a missing symbol names itself in the error.
	Assign(s->init, dlsym(handle_, "retro_init"));                           if (!s->init) { ok = false; missing = "retro_init"; }
	Assign(s->deinit, dlsym(handle_, "retro_deinit"));                       if (ok && !s->deinit) { ok = false; missing = "retro_deinit"; }
	Assign(s->api_version, dlsym(handle_, "retro_api_version"));             if (ok && !s->api_version) { ok = false; missing = "retro_api_version"; }
	Assign(s->get_system_info, dlsym(handle_, "retro_get_system_info"));     if (ok && !s->get_system_info) { ok = false; missing = "retro_get_system_info"; }
	Assign(s->get_system_av_info, dlsym(handle_, "retro_get_system_av_info")); if (ok && !s->get_system_av_info) { ok = false; missing = "retro_get_system_av_info"; }
	Assign(s->set_controller_port_device, dlsym(handle_, "retro_set_controller_port_device"));
	Assign(s->reset, dlsym(handle_, "retro_reset"));                         if (ok && !s->reset) { ok = false; missing = "retro_reset"; }
	Assign(s->run, dlsym(handle_, "retro_run"));                             if (ok && !s->run) { ok = false; missing = "retro_run"; }
	Assign(s->serialize_size, dlsym(handle_, "retro_serialize_size"));      if (ok && !s->serialize_size) { ok = false; missing = "retro_serialize_size"; }
	Assign(s->serialize, dlsym(handle_, "retro_serialize"));                 if (ok && !s->serialize) { ok = false; missing = "retro_serialize"; }
	Assign(s->unserialize, dlsym(handle_, "retro_unserialize"));             if (ok && !s->unserialize) { ok = false; missing = "retro_unserialize"; }
	Assign(s->cheat_reset, dlsym(handle_, "retro_cheat_reset"));
	Assign(s->cheat_set, dlsym(handle_, "retro_cheat_set"));
	Assign(s->load_game, dlsym(handle_, "retro_load_game"));                 if (ok && !s->load_game) { ok = false; missing = "retro_load_game"; }
	Assign(s->load_game_special, dlsym(handle_, "retro_load_game_special"));
	Assign(s->unload_game, dlsym(handle_, "retro_unload_game"));             if (ok && !s->unload_game) { ok = false; missing = "retro_unload_game"; }
	Assign(s->get_region, dlsym(handle_, "retro_get_region"));
	Assign(s->get_memory_data, dlsym(handle_, "retro_get_memory_data"));
	Assign(s->get_memory_size, dlsym(handle_, "retro_get_memory_size"));
	Assign(s->set_environment, dlsym(handle_, "retro_set_environment"));     if (ok && !s->set_environment) { ok = false; missing = "retro_set_environment"; }
	Assign(s->set_video_refresh, dlsym(handle_, "retro_set_video_refresh")); if (ok && !s->set_video_refresh) { ok = false; missing = "retro_set_video_refresh"; }
	Assign(s->set_audio_sample, dlsym(handle_, "retro_set_audio_sample"));
	Assign(s->set_audio_sample_batch, dlsym(handle_, "retro_set_audio_sample_batch")); if (ok && !s->set_audio_sample_batch) { ok = false; missing = "retro_set_audio_sample_batch"; }
	Assign(s->set_input_poll, dlsym(handle_, "retro_set_input_poll"));       if (ok && !s->set_input_poll) { ok = false; missing = "retro_set_input_poll"; }
	Assign(s->set_input_state, dlsym(handle_, "retro_set_input_state"));     if (ok && !s->set_input_state) { ok = false; missing = "retro_set_input_state"; }

	if (!ok) {
		error_ = std::string("missing symbol ") + missing + " in " + library_path;
		delete s;
		dlclose(handle_);
		handle_ = nullptr;
		return false;
	}

	sym = s;
	api_version_ = s->api_version();
	if (api_version_ != RETRO_API_VERSION) {
		// Not fatal (ABI is stable within a major version), but surfaced to the
		// UI rather than silently misbehaving.
		error_ = std::string("core ABI ") + std::to_string(api_version_) +
		         " differs from host ABI " + std::to_string(RETRO_API_VERSION);
	}
	return true;
}

void CoreLoader::Close() {
	if (sym) {
		delete sym;
		sym = nullptr;
	}
	if (handle_) {
		dlclose(handle_);
		handle_ = nullptr;
	}
	api_version_ = 0;
}

bool CoreLoader::SystemInfoText(std::string *name, std::string *version,
                                std::string *valid_extensions) const {
	if (!sym || !sym->get_system_info) return false;
	retro_system_info info{};
	sym->get_system_info(&info);
	if (name) *name = info.library_name ? info.library_name : "";
	if (version) *version = info.library_version ? info.library_version : "";
	if (valid_extensions) *valid_extensions = info.valid_extensions ? info.valid_extensions : "";
	return true;
}

}  // namespace sphynx
