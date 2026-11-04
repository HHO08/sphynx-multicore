// ============================================================================
//  Sphynx — extcores/src/core_symbols.h  (internal)
//
//  The complete set of libretro entry points a core must expose. Keeping them
//  in one struct means the loader fails loudly at load time instead of
//  crashing later on a missing symbol.
//
//  SPDX-License-Identifier: GPL-2.0-or-later
// ============================================================================
#pragma once

#include "sphynx_extcore.h"

#include <libretro.h>

namespace sphynx {

struct CoreSymbols {
	void (*init)();
	void (*deinit)();
	unsigned (*api_version)();
	void (*get_system_info)(retro_system_info *);
	void (*get_system_av_info)(retro_system_av_info *);
	void (*set_controller_port_device)(unsigned, unsigned);
	void (*reset)();
	void (*run)();
	size_t (*serialize_size)();
	bool (*serialize)(void *, size_t);
	bool (*unserialize)(const void *, size_t);
	void (*cheat_reset)();
	void (*cheat_set)(unsigned, bool, const char *);
	bool (*load_game)(const retro_game_info *);
	bool (*load_game_special)(unsigned, const retro_game_info *, size_t);
	void (*unload_game)();
	unsigned (*get_region)();
	void *(*get_memory_data)(unsigned);
	size_t (*get_memory_size)(unsigned);
	void (*set_environment)(retro_environment_t);
	void (*set_video_refresh)(retro_video_refresh_t);
	void (*set_audio_sample)(retro_audio_sample_t);
	void (*set_audio_sample_batch)(retro_audio_sample_batch_t);
	void (*set_input_poll)(retro_input_poll_t);
	void (*set_input_state)(retro_input_state_t);

	// Symbol names, in the same order, for the loader.
	static const char *const *Names();
	static size_t Count();
};

}  // namespace sphynx
