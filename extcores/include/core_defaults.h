// ============================================================================
//  Sphynx — extcores/include/core_defaults.h
//
//  "The best settings that exist for this core", applied automatically.
//
//  The owner asked for the best possible configuration for *every* bundled
//  core: the smoothest picture and sound this phone can produce, without
//  trading away so much quality that the game looks wrong. Two layers do that:
//
//    1. an explicit table - a handful of options whose right value for this
//       hardware is not a matter of taste (melonDS' OpenGL renderer producing
//       white screens on many phone GPUs, the N64 core rendering at 640x480 on
//       a device that cannot do it at full speed, ...);
//    2. generic rules over the options the core itself declared - any option
//       whose key talks about frame skipping, resolution, interpolation,
//       threading or a recompiler gets the fast-but-fair value.
//
//  Two safety rules make this impossible to break a core:
//    * only options the core declared are touched, never invented keys;
//    * only values the core itself offered are used, so a recommendation that
//      does not fit this core's version is simply ignored (fail-open).
//  Anything the user picks in "Core options" is written to the per-system file
//  and always wins over a recommendation.
//
//  SPDX-License-Identifier: GPL-2.0-or-later
// ============================================================================
#pragma once

#include <string>
#include <vector>

#include "sphynx_extcore.h"

namespace sphynx {

// The value this build recommends for one option, or "" when it has none.
// `current` is the core's own default and is returned unchanged when the
// recommendation would be the same (so nothing is reported as "changed").
std::string RecommendOptionValue(const std::string &core_tag, const std::string &option_key,
                                 const std::vector<std::string> &values,
                                 const std::string &current);

// Applies the recommendations to a freshly declared option list.
// Returns how many options were changed; the reasons are logged by the caller.
int ApplyRecommendedDefaults(const std::string &core_tag, std::vector<CoreOption> *options);

// ---------------------------------------------------------------------------
//  SPHYNX (stage 20): display quality for the cores that can render above
//  their console's own resolution (Nintendo 64, Dreamcast). One setting in
//  Core Settings ("sphynx-quality"), default "auto": the value is chosen for
//  this device (a strong phone steps up, a modest one keeps full speed at the
//  native resolution), and any level the user picks wins over it.
// ---------------------------------------------------------------------------
struct DeviceProfile {
	int ram_mb;      // physical memory, rounded down to MiB
	int cpu_cores;   // cores the kernel reports
};

// A device that can afford rendering above the console's native resolution
// without trading frames away. Deliberately conservative: 8 cores and 4 GiB.
bool StrongDevice(DeviceProfile profile);

// The quality words the setting accepts. "auto" is the default.
bool IsValidQuality(const std::string &quality);

// The core option key this setting drives for a system, or "" when the system
// has no real resolution knob (everything but N64 and Dreamcast stays native).
std::string QualityResolutionKey(const std::string &core_tag);

// The value the auto/baseline resolution recommendation produces for a system
// ("1" for N64, "1x" for Dreamcast), so a value the user chose by hand can be
// told apart from one the engine wrote.
std::string QualityAutoBaseline(const std::string &core_tag);

// Resolves a quality level to a concrete value of the core's resolution
// option, chosen from the values the core itself offers (fail-open: a level
// that maps to nothing this core offers returns ""). `why` gets one line.
std::string QualityPick(const std::string &quality, const std::string &core_tag,
                        const std::vector<std::string> &values, DeviceProfile profile,
                        std::string *why);

// One line per recommendation that exists for `core_tag`, for the log and for
// the player's diagnostic panel: "melonds_renderer = software (software...)".
// Empty when this core has no recommendation.
std::string DescribeDefaults(const std::string &core_tag);

// Same, but for one key: the reason text (empty when there is no rule).
std::string DefaultReason(const std::string &core_tag, const std::string &option_key);

}  // namespace sphynx
