// ============================================================================
//  Sphynx — extcores/tests/host_defaults_test.cpp
//
//  Tests for "the best settings that exist for this core" (stage 15):
//
//    * the explicit table (melonDS' software renderer, the N64 resolution...)
//    * the generic rules (frame skip, resolution, interpolation, threading,
//      recompilers) over keys the core declared itself
//    * the two safety rules: a key the core never declared is never touched,
//      and a value the core never offered is never used
//    * a value the user chose always wins over a recommendation
//
//  Everything runs the same object code the APK ships (core_defaults.cpp and
//  the real Environment), so this is not a description of the behaviour but a
//  measurement of it.
//
//  SPDX-License-Identifier: GPL-2.0-or-later
// ============================================================================
#include "core_defaults.h"
#include "sphynx_extcore.h"

#include <libretro.h>

#include <cstdio>
#include <string>
#include <vector>

namespace {

int g_failures = 0;
int g_checks = 0;

void Check(bool ok, const char *what) {
	++g_checks;
	if (!ok) {
		++g_failures;
		std::printf("  FAIL  %s\n", what);
	} else {
		std::printf("  ok    %s\n", what);
	}
}

std::vector<std::string> Values(std::initializer_list<const char *> list) {
	std::vector<std::string> out;
	for (const char *v : list) out.push_back(v);
	return out;
}

// ---------------------------------------------------------------------------
//  1. The explicit table
// ---------------------------------------------------------------------------
void TestExplicitTable() {
	std::printf("\n[explicit recommendations]\n");

	// The Nintendo DS white-screen fix: the software renderer, when offered.
	const std::string renderer = sphynx::RecommendOptionValue(
		"melonds", "melonds_renderer", Values({"OpenGL", "software"}), "OpenGL");
	Check(renderer == "software", "melonDS is moved to the software renderer");

	// Already on the recommended value: nothing to do, nothing reported.
	Check(sphynx::RecommendOptionValue("melonds", "melonds_renderer",
	                                   Values({"software", "OpenGL"}), "software")
	          .empty(),
	      "a value that is already right is not reported as a change");

	// The core of this version does not offer it: no recommendation, no damage.
	Check(sphynx::RecommendOptionValue("melonds", "melonds_renderer",
	                                   Values({"OpenGL"}), "OpenGL")
	          .empty(),
	      "a value the core does not offer is never used");

	// A different core gets no melonDS recommendation.
	Check(sphynx::RecommendOptionValue("gambatte", "melonds_renderer",
	                                   Values({"OpenGL", "software"}), "OpenGL")
	          .empty(),
	      "one core's recommendation never leaks into another core");

	// The N64 core: native resolution, HLE audio.
	Check(sphynx::RecommendOptionValue("mupen64plus_next_gles2", "mupen64plus-Resolution",
	                                   Values({"640x480", "320x240"}), "640x480") == "320x240",
	      "N64 drops to its native 320x240");
	Check(sphynx::RecommendOptionValue("mupen64plus_next_gles2", "mupen64plus-RSP-Plugin",
	                                   Values({"LLE", "HLE"}), "LLE") == "HLE",
	      "N64 uses high-level RSP audio");

	// PlayStation: recompiler + frame skipping + cheap interpolation.
	Check(sphynx::RecommendOptionValue("pcsx_rearmed", "pcsx_rearmed_drc",
	                                   Values({"disabled", "enabled"}), "disabled") == "enabled",
	      "PlayStation uses the dynamic recompiler");
	Check(sphynx::RecommendOptionValue("pcsx_rearmed", "pcsx_rearmed_spu_interpolation",
	                                   Values({"simple", "gaussian", "cubic"}), "gaussian") == "simple",
	      "PlayStation uses simple SPU interpolation");

	Check(sphynx::DescribeDefaults("melonds").find("melonds_renderer = software") != std::string::npos,
	      "the melonDS defaults are documented for the log");
	Check(sphynx::DescribeDefaults("this-core-does-not-exist").empty(),
	      "a core without a table entry says nothing");
}

// ---------------------------------------------------------------------------
//  2. The generic rules
// ---------------------------------------------------------------------------
void TestGenericRules() {
	std::printf("\n[generic rules over declared options]\n");

	const std::string skip = sphynx::RecommendOptionValue(
		"some_core", "saturn_frameskip", Values({"disabled", "auto", "manual"}), "disabled");
	Check(skip == "auto", "a frame-skip option is set to automatic");

	Check(sphynx::RecommendOptionValue("some_core", "video_resolution",
	                                   Values({"640x480", "320x240", "1280x960"}), "640x480") == "320x240",
	      "the lowest offered internal resolution is chosen");
	Check(sphynx::RecommendOptionValue("some_core", "render_scale",
	                                   Values({"4x", "2x", "1x"}), "4x") == "1x",
	      "a scale multiplier is set to 1x");

	Check(sphynx::RecommendOptionValue("some_core", "audio_interpolation",
	                                   Values({"none", "linear", "gaussian"}), "gaussian") == "linear",
	      "audio interpolation drops to the cheapest smooth one");

	Check(sphynx::RecommendOptionValue("some_core", "threaded_renderer",
	                                   Values({"disabled", "enabled"}), "disabled") == "enabled",
	      "threading is switched on where the core offers it");

	Check(sphynx::RecommendOptionValue("some_core", "cpu_dynarec",
	                                   Values({"interpreter", "dynarec"}), "interpreter") == "dynarec",
	      "a recompiler is preferred over the interpreter");

	// Anything the rules do not recognise must stay exactly as the core wanted.
	Check(sphynx::RecommendOptionValue("some_core", "region", Values({"auto", "ntsc"}), "auto").empty(),
	      "an option with no rule is left alone");
	Check(sphynx::RecommendOptionValue("some_core", "palette", Values({"default", "vivid"}), "default").empty(),
	      "a cosmetic option is left alone");
}

// ---------------------------------------------------------------------------
//  3. Applied to a declared list, and never over a user's own choice
// ---------------------------------------------------------------------------
retro_core_option_definition *MakeDefinitions() {
	// Built fresh each time: the environment copies what it needs.
	static retro_core_option_definition defs[4];
	for (int i = 0; i < 4; ++i) defs[i] = retro_core_option_definition();
	defs[0].key = "melonds_renderer";
	defs[0].desc = "Renderer";
	defs[0].values[0].value = "OpenGL";
	defs[0].values[1].value = "software";
	defs[1].key = "melonds_audio_interpolation";
	defs[1].desc = "Audio interpolation";
	defs[1].values[0].value = "gaussian";
	defs[1].values[1].value = "linear";
	defs[2].key = "melonds_screen_layout";
	defs[2].desc = "Screen layout";
	defs[2].values[0].value = "Top/Bottom";
	defs[2].values[1].value = "Left/Right";
	return defs;
}

void TestThroughTheEnvironment() {
	std::printf("\n[the real Environment: recommendations, then the user's choice]\n");

	// --- no user choice: recommendations apply ------------------------------
	{
		sphynx::Environment env;
		env.SetRecommendations("melonds");
		sphynx::Environment::BindActiveInstance(&env);
		sphynx::Environment::Trampoline(RETRO_ENVIRONMENT_SET_CORE_OPTIONS, MakeDefinitions());
		const auto &options = env.Options();
		Check(options.size() == 3, "the core's three options were kept");
		Check(options[0].current == "software", "the renderer became software without any user action");
		Check(options[1].current == "linear", "interpolation became the cheapest smooth one");
		Check(options[2].current == "Top/Bottom", "an option with no rule kept the core's default");
		Check(env.RecommendedCount() == 2, "exactly two options were changed");
	}

	// --- the user has a saved choice: it always wins ------------------------
	{
		sphynx::Environment env;
		env.SetRecommendations("melonds");
		env.SetInitialOptions({{"melonds_renderer", "OpenGL"}});
		sphynx::Environment::BindActiveInstance(&env);
		sphynx::Environment::Trampoline(RETRO_ENVIRONMENT_SET_CORE_OPTIONS, MakeDefinitions());
		Check(env.Options()[0].current == "OpenGL", "a value the user picked is never overruled");
	}

	// --- a saved value this core cannot use is ignored, not applied ---------
	{
		sphynx::Environment env;
		env.SetRecommendations("melonds");
		env.SetInitialOptions({{"melonds_renderer", "SomethingFromAnOlderVersion"}});
		sphynx::Environment::BindActiveInstance(&env);
		sphynx::Environment::Trampoline(RETRO_ENVIRONMENT_SET_CORE_OPTIONS, MakeDefinitions());
		Check(env.Options()[0].current == "software",
		      "an invalid saved value falls back to the recommendation");
	}

	// --- no recommendations configured: stock behaviour remains ------------
	{
		sphynx::Environment env;
		sphynx::Environment::BindActiveInstance(&env);
		sphynx::Environment::Trampoline(RETRO_ENVIRONMENT_SET_CORE_OPTIONS, MakeDefinitions());
		Check(env.Options()[0].current == "OpenGL", "without a core tag nothing is changed");
		Check(env.RecommendedCount() == 0, "and nothing is reported as recommended");
	}
}

// ---------------------------------------------------------------------------
//  4. The vector-level helper
// ---------------------------------------------------------------------------
void TestApplyToVector() {
	std::printf("\n[ApplyRecommendedDefaults]\n");
	std::vector<sphynx::CoreOption> options(3);
	options[0].key = "melonds_renderer";
	options[0].values = Values({"OpenGL", "software"});
	options[0].current = "OpenGL";
	options[1].key = "melonds_region";
	options[1].values = Values({"Auto", "USA"});
	options[1].current = "Auto";
	options[2].key = "melonds_renderer";   // a duplicate must not confuse the count
	options[2].values = Values({"OpenGL", "software"});
	options[2].current = "software";

	const int changed = sphynx::ApplyRecommendedDefaults("melonds", &options);
	Check(changed == 1, "only the option that needed it was changed");
	Check(options[0].current == "software", "the first option got the recommendation");
	Check(options[1].current == "Auto", "the untouched option stayed as it was");
	Check(sphynx::ApplyRecommendedDefaults("melonds", nullptr) == 0, "a null list is answered, not crashed on");
}

// ---------------------------------------------------------------------------
//  5. The display-quality setting (stage 20)
// ---------------------------------------------------------------------------
void TestDisplayQuality() {
	std::printf("\n[DisplayQuality]\n");
	const sphynx::DeviceProfile modest{3000, 6};
	const sphynx::DeviceProfile strong{6000, 8};

	Check(sphynx::StrongDevice(strong) && !sphynx::StrongDevice(modest),
	      "8 cores and 4 GiB is the strong-device line");
	Check(sphynx::IsValidQuality("auto") && sphynx::IsValidQuality("high") &&
	          sphynx::IsValidQuality("balanced") && sphynx::IsValidQuality("fast") &&
	          !sphynx::IsValidQuality("ultra"),
	      "the four quality words and nothing else are valid");

	Check(sphynx::QualityResolutionKey("mupen64plus_next_gles2") == "mupen64plus-EnableNativeResFactor",
	      "N64 quality lands on GLideN64's resolution factor");
	Check(sphynx::QualityResolutionKey("flycast") == "reicast_internal_resolution",
	      "Dreamcast quality lands on the internal resolution option");
	Check(sphynx::QualityResolutionKey("gambatte").empty(),
	      "a system without a resolution knob gets no quality mapping");

	const std::vector<std::string> n64 = {"0", "1", "2", "3", "4", "5", "6", "7", "8"};
	const std::vector<std::string> dc = {"1x", "2x", "4x", "8x"};

	Check(sphynx::QualityPick("fast", "mupen64plus_next_gles2", n64, strong, nullptr) == "1",
	      "N64 fast is the native factor");
	Check(sphynx::QualityPick("balanced", "mupen64plus_next_gles2", n64, modest, nullptr) == "2",
	      "N64 balanced is one step above native");
	Check(sphynx::QualityPick("high", "mupen64plus_next_gles2", n64, strong, nullptr) == "4",
	      "N64 high is four times the native pixels");
	Check(sphynx::QualityPick("auto", "mupen64plus_next_gles2", n64, modest, nullptr) == "1",
	      "N64 auto on a modest device stays native");
	Check(sphynx::QualityPick("auto", "mupen64plus_next_gles2", n64, strong, nullptr) == "2",
	      "N64 auto on a strong device steps up");
	Check(sphynx::QualityPick("", "mupen64plus_next_gles2", n64, strong, nullptr) == "2",
	      "an empty stored setting behaves like auto");

	Check(sphynx::QualityPick("fast", "flycast", dc, modest, nullptr) == "1x",
	      "Dreamcast fast is native");
	Check(sphynx::QualityPick("balanced", "flycast", dc, modest, nullptr) == "2x",
	      "Dreamcast balanced doubles the resolution");
	Check(sphynx::QualityPick("high", "flycast", dc, strong, nullptr) == "4x",
	      "Dreamcast high is 4x");
	Check(sphynx::QualityPick("auto", "flycast", dc, strong, nullptr) == "2x",
	      "Dreamcast auto on a strong device doubles");
	Check(sphynx::QualityPick("high", "gambatte", {"enabled", "disabled"}, strong, nullptr).empty(),
	      "a system without the knob resolves to nothing");
	Check(sphynx::QualityPick("high", "mupen64plus_next_gles2", {"enabled", "disabled"}, strong, nullptr)
	          .empty(),
	      "a core that offers no sizes is left alone (fail-open)");
	Check(sphynx::QualityPick("high", "mupen64plus_next_gles2", {"0", "1"}, strong, nullptr) == "1",
	      "a short list tops out at its biggest step instead of failing");
}

}  // namespace

int main() {
	std::printf("Sphynx core-defaults test (%s)\n", sphynx::BuildId());
	TestExplicitTable();
	TestGenericRules();
	TestThroughTheEnvironment();
	TestApplyToVector();
	TestDisplayQuality();
	std::printf("\n%d checks, %d failure(s) — %s\n", g_checks, g_failures,
	            g_failures ? "FAILED" : "all good");
	return g_failures ? 1 : 0;
}
