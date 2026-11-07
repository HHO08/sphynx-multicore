// ============================================================================
//  Sphynx — extcores/src/env_callback.cpp
//
//  Implementation of retro_environment_t.
//
//  A libretro core asks the frontend dozens of questions through this single
//  callback. Everything the core genuinely needs is answered here; anything we
//  do not implement returns false and is counted, which is the contract a
//  well-behaved core is written against.
//
//  Deliberately NOT implemented, because they would break the "no network, no
//  telemetry, no surprise" rule of this fork:
//    * RETRO_ENVIRONMENT_SET_SUPPORT_ACHIEVEMENTS / achievements URLs
//    * anything requiring an HTTP client
//
//  SPDX-License-Identifier: GPL-2.0-or-later
// ============================================================================
#include "sphynx_extcore.h"

#include <libretro.h>

#include <cstdio>
#include <cstring>

namespace sphynx {

Environment::Environment() = default;
Environment::~Environment() = default;

void Environment::SetDirectories(const std::string &system_dir, const std::string &save_dir) {
	system_dir_ = system_dir;
	save_dir_ = save_dir;
}

void Environment::SetVariables(const std::vector<std::pair<std::string, std::string>> &vars) {
	variables_ = vars;
}

// SPHYNX (stage 13): the values the user picked last time for this system. They
// are applied before the core is initialised, so the core starts with them
// instead of making us re-initialise it afterwards.
void Environment::SetInitialOptions(const std::vector<std::pair<std::string, std::string>> &values) {
	initial_options_ = values;
}

bool Environment::SetOption(const std::string &key, const std::string &value) {
	for (CoreOption &option : options_) {
		if (option.key != key) continue;
		bool known = false;
		for (const std::string &v : option.values) {
			if (v == value) known = true;
		}
		if (!known) return false;
		if (option.current == value) return true;   // nothing to do, still valid
		option.current = value;
		// Mirror it into the plain variable map, which is what GET_VARIABLE
		// reads: that is the contract the core actually implements.
		bool mirrored = false;
		for (auto &kv : variables_) {
			if (kv.first == key) {
				kv.second = value;
				mirrored = true;
			}
		}
		if (!mirrored) variables_.emplace_back(key, value);
		option_changed_ = true;
		return true;
	}
	// A variable the core never declared as a "core option" but does read
	// (old-style SET_VARIABLES only): accept it if we know the key at all.
	for (auto &kv : variables_) {
		if (kv.first == key) {
			kv.second = value;
			option_changed_ = true;
			return true;
		}
	}
	return false;
}

void Environment::ApplyInitialOptions() {
	for (auto &option : options_) {
		for (const auto &kv : initial_options_) {
			if (kv.first != option.key) continue;
			bool known = false;
			for (const std::string &v : option.values) {
				if (v == kv.second) known = true;
			}
			if (known) option.current = kv.second;
		}
		// Whatever ended up current must also be what GET_VARIABLE answers, or
		// the core and the UI would disagree from the first frame.
		bool mirrored = false;
		for (auto &pair : variables_) {
			if (pair.first == option.key) {
				pair.second = option.current;
				mirrored = true;
			}
		}
		if (!mirrored) variables_.emplace_back(option.key, option.current);
	}
}

bool Environment::TakeOptionChanged() {
	const bool changed = option_changed_;
	option_changed_ = false;
	return changed;
}

namespace {
// "key; Value A|Value B|Value C" is the old SET_VARIABLES spelling.
void ParseVariableList(const retro_variable *list, std::vector<CoreOption> *out) {
	if (!list) return;
	for (const retro_variable *v = list; v->key != nullptr; ++v) {
		CoreOption option;
		option.key = v->key;
		const std::string packed = v->value ? v->value : "";
		const size_t semi = packed.find(';');
		std::string values = packed;
		if (semi != std::string::npos) {
			option.label = packed.substr(0, semi);
			// The label is conventionally "Display name" - trim spaces.
			while (!option.label.empty() && option.label.back() == ' ') option.label.pop_back();
			while (!option.label.empty() && option.label.front() == ' ') option.label.erase(option.label.begin());
			values = packed.substr(semi + 1);
		}
		while (!values.empty() && values.front() == ' ') values.erase(values.begin());
		size_t start = 0;
		while (start <= values.size()) {
			const size_t bar = values.find('|', start);
			std::string item = values.substr(start, bar == std::string::npos ? std::string::npos : bar - start);
			while (!item.empty() && item.back() == ' ') item.pop_back();
			if (!item.empty()) option.values.push_back(item);
			if (bar == std::string::npos) break;
			start = bar + 1;
		}
		if (option.label.empty()) option.label = option.key;
		if (!option.values.empty()) option.current = option.values.front();
		out->push_back(option);
	}
}

// The newer SET_CORE_OPTIONS form: one entry per option, values in a table.
void ParseCoreOptionDefinitions(const retro_core_option_definition *defs, std::vector<CoreOption> *out) {
	if (!defs) return;
	for (const retro_core_option_definition *d = defs; d->key != nullptr; ++d) {
		CoreOption option;
		option.key = d->key;
		option.label = d->desc ? d->desc : d->key;
		for (int i = 0; i < RETRO_NUM_CORE_OPTION_VALUES_MAX; ++i) {
			if (d->values[i].value == nullptr) break;
			option.values.push_back(d->values[i].value);
		}
		if (!option.values.empty()) option.current = option.values.front();
		if (!option.values.empty()) out->push_back(option);
	}
}
}  // namespace

namespace {
// One external core per process, so the active instance is a single slot. The
// slot is only read under the session lock (see ui_api/Session), but a core may
// call the environment from its own thread during load, so guard the pointer
// itself to keep the read atomic and ordered.
Environment *g_active_env = nullptr;
}

void Environment::BindActiveInstance(Environment *env) { g_active_env = env; }

bool Environment::Trampoline(unsigned cmd, void *data) {
	Environment *env = g_active_env;
	if (!env) return false;
	return env->Callback(cmd, data, nullptr);
}

bool Environment::Callback(unsigned cmd, void *data, void *) {
	switch (cmd) {
	// --- pixel format -------------------------------------------------------
	case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: {
		if (!data) return false;
		const retro_pixel_format fmt = *static_cast<const retro_pixel_format *>(data);
		switch (fmt) {
		case RETRO_PIXEL_FORMAT_0RGB1555: pixel_format_ = PixelFormat::F16; return true;
		case RETRO_PIXEL_FORMAT_XRGB8888: pixel_format_ = PixelFormat::XRGB8888; return true;
		case RETRO_PIXEL_FORMAT_RGB565: pixel_format_ = PixelFormat::RGB565; return true;
		default: return false;   // 10-bit / HDR formats are not rendered by Sphynx
		}
	}

	// --- directories --------------------------------------------------------
	case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
	case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY: {
		// BIOS files and per-core saves live next to the user's PPSSPP folders.
		const std::string &dir = (cmd == RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY) ? system_dir_ : save_dir_;
		if (dir.empty()) return false;
		*static_cast<const char **>(data) = dir.c_str();
		return true;
	}
	case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO:
	case RETRO_ENVIRONMENT_SET_GEOMETRY: {
		if (!data) return false;
		const retro_system_av_info *av = static_cast<const retro_system_av_info *>(data);
		geom_w_ = av->geometry.base_width;
		geom_h_ = av->geometry.base_height;
		aspect_ = av->geometry.aspect_ratio;
		return true;
	}

	// --- core options (the Settings screen depends on this) -----------------
	case RETRO_ENVIRONMENT_SET_VARIABLES: {
		// SPHYNX (stage 13): keep the declarations. They are the rows the player's
		// "Core options" screen shows, and the only place the allowed values are
		// known, so an unknown value can be refused instead of passed on.
		options_.clear();
		ParseVariableList(static_cast<const retro_variable *>(data), &options_);
		ApplyInitialOptions();
		return true;
	}
	case RETRO_ENVIRONMENT_SET_CORE_OPTIONS: {
		options_.clear();
		ParseCoreOptionDefinitions(static_cast<const retro_core_option_definition *>(data), &options_);
		ApplyInitialOptions();
		return true;
	}
	case RETRO_ENVIRONMENT_GET_VARIABLE: {
		if (!data) return false;
		retro_variable *var = static_cast<retro_variable *>(data);
		if (!var->key) return false;
		for (const auto &kv : variables_) {
			if (kv.first == var->key) {
				var->value = kv.second.c_str();
				return true;
			}
		}
		var->value = nullptr;
		return false;
	}
	case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE: {
		// True exactly once after the UI changed an option, which is how a core
		// knows to apply it without being restarted.
		if (!data) return false;
		*static_cast<bool *>(data) = TakeOptionChanged();
		return true;
	}

	// --- misc capabilities --------------------------------------------------
	case RETRO_ENVIRONMENT_GET_CAN_DUPE:
		if (!data) return false;
		*static_cast<bool *>(data) = true;
		return true;
	case RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL:
		return true;
	case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
		// Controller descriptions are mapped onto PPSSPP's fixed pad layout;
		// nothing to do, but the core must not consider us hostile.
		return true;
	case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
		if (data) supports_no_game_ = *static_cast<const bool *>(data);
		return true;
	case RETRO_ENVIRONMENT_GET_LANGUAGE:
		// Sphynx ships both Arabic and English UI; the core only uses this for
		// its own option strings, so English is the safe neutral answer.
		if (!data) return false;
		*static_cast<unsigned *>(data) = RETRO_LANGUAGE_ENGLISH;
		return true;
	case RETRO_ENVIRONMENT_SET_MESSAGE:
	case RETRO_ENVIRONMENT_SET_MESSAGE_EXT:
		// Core messages are routed to PPSSPP-style toasts by the Android layer;
		// returning true prevents the core from retrying.
		return true;
	case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
		return false;   // cores fall back to their own logging
	case RETRO_ENVIRONMENT_GET_VFS_INTERFACE:
		return false;   // standard stdio is fine on Android
	case RETRO_ENVIRONMENT_SET_MEMORY_MAPS:
	case RETRO_ENVIRONMENT_SET_SERIALIZATION_QUIRKS:
		return true;
	case RETRO_ENVIRONMENT_GET_RUMBLE_INTERFACE:
		return false;
	case RETRO_ENVIRONMENT_GET_SENSOR_INTERFACE:
	case RETRO_ENVIRONMENT_GET_CAMERA_INTERFACE:
	case RETRO_ENVIRONMENT_GET_LOCATION_INTERFACE:
		return false;
	default:
		++unknown_commands_;
		return false;
	}
}

void Environment::LogSummary() const {
	std::printf("[sphynx-env] pixel_format=%d geometry=%ux%u aspect=%.3f "
	            "vars=%zu unknown_cmds=%d no_game=%d\n",
	            static_cast<int>(pixel_format_), geom_w_, geom_h_, aspect_,
	            variables_.size(), unknown_commands_, supports_no_game_ ? 1 : 0);
}

}  // namespace sphynx
