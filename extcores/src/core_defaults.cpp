// ============================================================================
//  Sphynx — extcores/src/core_defaults.cpp  (see core_defaults.h)
//
//  Two layers, in this order:
//    * an explicit table for the options whose right value is not a matter of
//      taste on this hardware (white-screen renderers, lazy recompilers...);
//    * generic rules over keys the core declared itself (frame skip,
//      resolution, interpolation, threading, recompilers).
//
//  Everything is fail-open by construction: a key the core never declared is
//  never touched, and a value the core never offered is never used. A wrong
//  guess therefore costs nothing; a right one shows up as a smoother game.
//
//  SPDX-License-Identifier: GPL-2.0-or-later
// ============================================================================
#include "core_defaults.h"

#include <cctype>
#include <cstdlib>
#include <cstring>

namespace sphynx {
namespace {

std::string Lower(const std::string &in) {
	std::string out;
	out.reserve(in.size());
	for (char c : in) out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
	return out;
}

bool Has(const std::string &haystack, const char *needle) {
	return Lower(haystack).find(Lower(needle)) != std::string::npos;
}

bool HasAny(const std::string &haystack, const char *const *needles, int count) {
	for (int i = 0; i < count; ++i)
		if (Has(haystack, needles[i])) return true;
	return false;
}

// First offered value that equals one of `prefer` (ignoring case), or "".
std::string Pick(const std::vector<std::string> &values, const std::vector<const char *> &prefer) {
	for (const char *want : prefer) {
		const std::string w = Lower(want);
		for (const std::string &value : values)
			if (Lower(value) == w) return value;
	}
	return std::string();
}

// "640x480" / "2x" -> area, so the smallest offered resolution can be chosen.
long ResolutionArea(const std::string &value) {
	long w = 0, h = 0;
	const char *s = value.c_str();
	while (*s && !std::isdigit(static_cast<unsigned char>(*s))) ++s;
	if (!*s) return -1;
	w = std::strtol(s, nullptr, 10);
	while (*s && std::isdigit(static_cast<unsigned char>(*s))) ++s;
	if (*s == 'x' || *s == 'X' || *s == '*' || *s == ' ') {
		++s;
		h = std::strtol(s, nullptr, 10);
	}
	if (h <= 0) return w;   // a plain multiplier such as "2x"
	return w * h;
}

// ---------------------------------------------------------------------------
//  Layer 1: the explicit table
// ---------------------------------------------------------------------------
struct Explicit {
	const char *core;    // core tag, i.e. the file name without libsphynx_core_
	const char *key;     // option key as the core declares it
	const char *value;   // recommended value
	const char *why;     // why, in one line (log + documentation)
};

const Explicit kExplicit[] = {
	// --- Nintendo DS -------------------------------------------------------
	// The OpenGL renderer is the single most common reason a DS game starts and
	// shows two white screens: it depends on GLES features several phone drivers
	// get wrong, and the software renderer is both correct everywhere and
	// *faster* on a 32-bit ARM core. This is the first thing to try when any
	// melonDS title stays white.
	{"melonds", "melonds_renderer", "software",
	 "software renderer: the OpenGL one draws white screens on many phone GPUs, and software is faster here"},
	{"melonds", "melonds_boot_directly", "disabled",
	 "boot through the DS firmware so the BIOS/firmware the user installed is actually used"},

	// --- Nintendo 64 -------------------------------------------------------
	// 640x480 (the core's default) is 4x the pixels of the console's own
	// 320x240. On this ABI the difference between "playable" and "slideshow".
	{"mupen64plus_next_gles2", "mupen64plus-Resolution", "320x240",
	 "native N64 resolution: the only setting that holds full speed on 32-bit ARM"},
	{"mupen64plus_next_gles2", "mupen64plus-RSP-Plugin", "HLE",
	 "high-level RSP audio: a fraction of the CPU cost of LLE with the same music and effects"},
	{"mupen64plus_next_gles2", "mupen64plus-FrameDuping", "True",
	 "repeat the last frame instead of dropping it: steady picture at the same emulation speed"},

	// --- PlayStation -------------------------------------------------------
	{"pcsx_rearmed", "pcsx_rearmed_drc", "enabled",
	 "the dynamic recompiler instead of the interpreter: several times faster"},
	{"pcsx_rearmed", "pcsx_rearmed_frameskip", "enabled",
	 "automatic frame skipping keeps the game at full speed instead of slowing it down"},
	{"pcsx_rearmed", "pcsx_rearmed_spu_interpolation", "simple",
	 "simple SPU interpolation: audibly identical on a phone speaker, much cheaper"},
	{"pcsx_rearmed", "pcsx_rearmed_show_bios_bootlogo", "disabled",
	 "skip the BIOS boot logo: one less step before the game starts"},

	// --- Dreamcast ---------------------------------------------------------
	{"flycast", "reicast_threaded_rendering", "enabled",
	 "threaded rendering: spreads the GPU work off the emulation thread"},

	// --- Mega Drive / 32X / Sega CD ---------------------------------------
	{"picodrive", "picodrive_drc", "enabled",
	 "dynamic recompiler instead of the interpreter"},

	// --- Game Boy ----------------------------------------------------------
	{"gambatte", "gambatte_mix_frames", "disabled",
	 "no frame mixing: same picture, half the work"},

	// --- Game Boy Advance --------------------------------------------------
	{"mgba", "mgba_interframe_blending", "disabled",
	 "no interframe blending: it only blurs the picture and costs a full extra pass"},

	// --- SNES --------------------------------------------------------------
	{"bsnes", "bsnes_blargg_ntsc_filter", "disabled",
	 "no NTSC filter: it is the most expensive option in the core and this screen is not a CRT"},
};

const Explicit *FindExplicit(const std::string &core_tag, const std::string &key) {
	for (const Explicit &e : kExplicit)
		if (core_tag == e.core && key == e.key) return &e;
	return nullptr;
}

// The engine of layer 2. `why` receives a one-line reason when a value is
// returned, so the same code serves both the decision and the explanation.
std::string GenericRecommendation(const std::string &key, const std::vector<std::string> &values,
                                  const std::string &current, std::string *why) {
	// 1. Frame skipping: "auto" keeps the emulated speed correct and only
	//    repeats frames; "enabled" is what the cores call the same thing.
	static const char *const kSkipKeys[] = {"frameskip", "frame_skip", "skip_frame", "frame_duping"};
	if (HasAny(key, kSkipKeys, 4)) {
		const std::string pick = Pick(values, {"auto", "enabled", "true", "on"});
		if (!pick.empty() && pick != current) {
			if (why) *why = "automatic frame skipping: keeps the game at full speed";
			return pick;
		}
	}

	// 2. Audio interpolation: "simple" is the cheapest setting that is still
	//    smooth; nobody hears the difference on a phone speaker.
	if (Has(key, "interpolation")) {
		const std::string pick = Pick(values, {"simple", "fast", "linear"});
		if (!pick.empty() && pick != current) {
			if (why) *why = "simple interpolation: same sound on a phone, much cheaper";
			return pick;
		}
	}

	// 3. Internal resolution: the smallest one the core offers. Anything higher
	//    is paid for in frames per second on this hardware.
	if (Has(key, "resolution") || Has(key, "upscale") || Has(key, "scale")) {
		std::string best;
		long best_area = -1;
		for (const std::string &value : values) {
			const long area = ResolutionArea(value);
			if (area <= 0) continue;
			if (best_area < 0 || area < best_area) {
				best_area = area;
				best = value;
			}
		}
		if (!best.empty() && best != current) {
			if (why) *why = "lowest internal resolution: the native picture, at full speed";
			return best;
		}
	}

	// 4. Threading: "enabled" wherever a core offers to move work off the
	//    emulation thread.
	if (Has(key, "thread")) {
		const std::string pick = Pick(values, {"enabled", "on", "true", "yes"});
		if (!pick.empty() && pick != current) {
			if (why) *why = "threaded: moves the work off the emulation thread";
			return pick;
		}
	}

	// 5. Recompilers: a dynarec/JIT is always faster than the interpreter the
	//    same core also offers.
	static const char *const kDynarecKeys[] = {"dynarec", "recompiler", "drc", "_jit", "cpu_core"};
	if (HasAny(key, kDynarecKeys, 5)) {
		const std::string pick =
			Pick(values, {"enabled", "dynamic", "dynarec", "recompiler", "yes", "on", "true"});
		if (!pick.empty() && pick != current) {
			if (why) *why = "recompiler instead of the interpreter";
			return pick;
		}
	}
	return std::string();
}

}  // namespace

std::string RecommendOptionValue(const std::string &core_tag, const std::string &option_key,
                                 const std::vector<std::string> &values,
                                 const std::string &current) {
	// Layer 1: explicit, and only when the core really offers that value.
	if (const Explicit *e = FindExplicit(core_tag, option_key)) {
		for (const std::string &value : values) {
			if (Lower(value) == Lower(e->value)) {
				return value == current ? std::string() : value;
			}
		}
		// Not offered by this version of the core: fall through to the rules so
		// a renamed option still gets a sensible value.
	}
	std::string why;
	return GenericRecommendation(option_key, values, current, &why);
}

int ApplyRecommendedDefaults(const std::string &core_tag, std::vector<CoreOption> *options) {
	if (!options) return 0;
	int changed = 0;
	for (CoreOption &option : *options) {
		const std::string recommended =
			RecommendOptionValue(core_tag, option.key, option.values, option.current);
		if (recommended.empty() || recommended == option.current) continue;
		option.current = recommended;
		++changed;
	}
	return changed;
}

std::string DefaultReason(const std::string &core_tag, const std::string &option_key) {
	if (const Explicit *e = FindExplicit(core_tag, option_key)) return e->why;
	std::string why;
	// The generic rules need values to decide; for the explanation only the
	// key matters, so ask with an empty list and a fresh flag.
	GenericRecommendation(option_key, {}, std::string(), &why);
	if (why.empty()) {
		// GenericRecommendation writes `why` only when it returns a value; the
		// explanation for a matched key is therefore rebuilt here.
		static const char *const kSkipKeys[] = {"frameskip", "frame_skip", "skip_frame", "frame_duping"};
		if (HasAny(option_key, kSkipKeys, 4)) why = "automatic frame skipping: keeps the game at full speed";
		else if (Has(option_key, "interpolation")) why = "simple interpolation: same sound on a phone, much cheaper";
		else if (Has(option_key, "resolution") || Has(option_key, "upscale") || Has(option_key, "scale"))
			why = "lowest internal resolution: the native picture, at full speed";
		else if (Has(option_key, "thread")) why = "threaded: moves the work off the emulation thread";
		else {
			static const char *const kDynarecKeys[] = {"dynarec", "recompiler", "drc", "_jit", "cpu_core"};
			if (HasAny(option_key, kDynarecKeys, 5)) why = "recompiler instead of the interpreter";
		}
	}
	return why;
}

std::string DescribeDefaults(const std::string &core_tag) {
	std::string out;
	for (const Explicit &e : kExplicit) {
		if (core_tag != e.core) continue;
		if (!out.empty()) out += "; ";
		out += e.key;
		out += " = ";
		out += e.value;
		out += " (";
		out += e.why;
		out += ")";
	}
	return out;
}

}  // namespace sphynx
