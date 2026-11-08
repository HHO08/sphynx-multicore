/* ===========================================================================
 *  Sphynx — extcores/tests/stub_core.c
 *
 *  A deliberately tiny, fully conformant libretro core used to test the host
 *  bridge (loader, environment, video/audio pumping, input, save states,
 *  watchdog) without shipping or emulating anything copyrighted.
 *
 *  It is NOT part of the shipped APK: it exists only for CI and local tests.
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 * ===========================================================================*/
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <libretro.h>

#define STUB_W 160
#define STUB_H 144
#define STUB_FPS 59.727500569606
#define STUB_SAMPLE_RATE 32768.0
#define STUB_MAX_FRAMES 1000000

static retro_environment_t env_cb;
static retro_video_refresh_t video_cb;
static retro_audio_sample_t sample_cb;
static retro_audio_sample_batch_t batch_cb;
static retro_input_poll_t poll_cb;
static retro_input_state_t state_cb;

static uint16_t *frame_buf;

/* Test hook (stage 19): SPHYNX_STUB_HW=1 makes this core behave like a real
 * hardware renderer - it asks the frontend for a GL context with
 * RETRO_ENVIRONMENT_SET_HW_RENDER, draws into the framebuffer it is given, and
 * presents the frame with RETRO_HW_FRAME_BUFFER_VALID and no pixels. That is
 * exactly what mupen64plus_next (N64) and flycast (Dreamcast) do, and it is the
 * path that used to be refused, so this is what the host test drives. */
static bool hw_mode;
static uintptr_t hw_framebuffer;
static unsigned hw_w = 320, hw_h = 240;
static void (*hw_glClearColor)(float, float, float, float);
static void (*hw_glClear)(unsigned);
static void (*hw_glBindFramebuffer)(unsigned, unsigned);
static void (*hw_glViewport)(int, int, int, int);
static void (*hw_glEnable)(unsigned);
static void (*hw_glDisable)(unsigned);
static void (*hw_glScissor)(int, int, int, int);
static uint32_t frame_counter;
static uint32_t last_buttons;
static bool game_loaded;

unsigned retro_api_version(void) { return RETRO_API_VERSION; }

void retro_set_environment(retro_environment_t cb) {
	env_cb = cb;
	bool no_game = false;
	if (env_cb) env_cb(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &no_game);
}

void retro_set_video_refresh(retro_video_refresh_t cb) { video_cb = cb; }
void retro_set_audio_sample(retro_audio_sample_t cb) { sample_cb = cb; }
void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { batch_cb = cb; }
void retro_set_input_poll(retro_input_poll_t cb) { poll_cb = cb; }
void retro_set_input_state(retro_input_state_t cb) { state_cb = cb; }

void retro_init(void) {
	if (!frame_buf) {
		frame_buf = (uint16_t *)calloc(STUB_W * STUB_H, sizeof(uint16_t));
	}
	frame_counter = 0;
	last_buttons = 0;
}

void retro_deinit(void) {
	hw_mode = false;
	free(frame_buf);
	frame_buf = NULL;
	game_loaded = false;
}

void retro_get_system_info(struct retro_system_info *info) {
	memset(info, 0, sizeof(*info));
	info->library_name = "Sphynx Stub";
	info->library_version = "1.0.0";
	info->valid_extensions = "stub|gb";
	info->need_fullpath = false;
	info->block_extract = false;
}

void retro_get_system_av_info(struct retro_system_av_info *info) {
	memset(info, 0, sizeof(*info));
	info->geometry.base_width = STUB_W;
	info->geometry.base_height = STUB_H;
	info->geometry.max_width = STUB_W;
	info->geometry.max_height = STUB_H;
	info->geometry.aspect_ratio = (float)STUB_W / (float)STUB_H;
	info->timing.fps = STUB_FPS;
	info->timing.sample_rate = STUB_SAMPLE_RATE;
}

void retro_set_controller_port_device(unsigned port, unsigned device) {
	(void)port;
	(void)device;
}

void retro_reset(void) { frame_counter = 0; }

/* Test hook (stage 18): a real fault at a chosen point, so the host's crash
 * containment is tested with an actual SIGSEGV instead of a simulated answer.
 *   SPHYNX_STUB_CRASH_ON_LOAD=1  -> faults inside retro_load_game()
 *   SPHYNX_STUB_CRASH_AFTER=<n>  -> faults on frame n+1 of retro_run()
 * Writing through a null pointer cannot be optimised away and is exactly what a
 * desktop core does when it is handed content it does not understand. */
static void stub_crash_now(void) {
	volatile unsigned *nowhere = (volatile unsigned *)0;
	*nowhere = 0x1234u;
}

void retro_run(void) {
	if (hw_mode) {
		/* Draw one solid frame into the frontend's framebuffer and present it
		 * without any pixels: the frontend has to read it back itself. */
		const float r = (float)((frame_counter >> 0) & 0xFF) / 255.0f;
		hw_glBindFramebuffer(0x8D40 /* GL_FRAMEBUFFER */, (unsigned)hw_framebuffer);
		hw_glViewport(0, 0, (int)hw_w, (int)hw_h);
		/* A two-colour picture: the GL bottom half red, the top half blue. That
		 * is how the orientation of the read-back is checked - a test that only
		 * clears one colour cannot tell "the right way up" from "upside down". */
		hw_glClearColor(0.0f, 0.25f, 0.75f, 1.0f);
		hw_glClear(0x4000 /* GL_COLOR_BUFFER_BIT */);
		if (hw_glEnable && hw_glScissor && hw_glDisable) {
			hw_glEnable(0x0C11 /* GL_SCISSOR_TEST */);
			hw_glScissor(0, 0, (int)hw_w, (int)(hw_h / 2));   /* GL y=0 is the bottom */
			hw_glClearColor(0.9f, 0.1f, 0.0f, 1.0f);
			hw_glClear(0x4000 /* GL_COLOR_BUFFER_BIT */);
			hw_glDisable(0x0C11);
		}
		(void)r;
		++frame_counter;
		if (video_cb) video_cb((const void *)(intptr_t)-1 /* RETRO_HW_FRAME_BUFFER_VALID */, hw_w, hw_h, 0);
		return;
	}
	{
		const char *crash = getenv("SPHYNX_STUB_CRASH_AFTER");
		if (crash && *crash && frame_counter >= (uint32_t)atoi(crash)) stub_crash_now();
	}

	/* Test hook: SPHYNX_STUB_DELAY_MS makes one frame take a known amount of
	 * time so the host watchdog can be tested deterministically. */
	const char *delay = getenv("SPHYNX_STUB_DELAY_MS");
	if (delay) {
		const int ms = atoi(delay);
		if (ms > 0) usleep((useconds_t)ms * 1000u);
	}

	enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_RGB565;
	if (env_cb) env_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt);

	if (poll_cb) poll_cb();
	if (state_cb) {
		last_buttons = 0;
		last_buttons |= state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A) ? 1u : 0u;
		last_buttons |= state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B) ? 2u : 0u;
	}

	/* A moving gradient: any renderer bug shows up immediately as a frozen or
	 * incorrectly-coloured picture rather than as an invisible failure. */
	if (frame_buf) {
		for (unsigned y = 0; y < STUB_H; ++y) {
			for (unsigned x = 0; x < STUB_W; ++x) {
				const uint16_t r = (uint16_t)((x + frame_counter) & 0x1F);
				const uint16_t g = (uint16_t)((y + frame_counter) & 0x3F);
				const uint16_t b = (uint16_t)(last_buttons ? 0x1F : (frame_counter & 0x1F));
				frame_buf[y * STUB_W + x] = (uint16_t)((r << 11) | (g << 5) | b);
			}
		}
		if (video_cb) video_cb(frame_buf, STUB_W, STUB_H, STUB_W * sizeof(uint16_t));
	}

	/* 735 stereo frames ≈ 60 Hz at 32768 Hz (what a GB-ish core produces). */
	{
		enum { N = 735 };
		static int16_t audio[N * 2];
		for (int i = 0; i < N; ++i) {
			const int16_t v = (int16_t)((frame_counter + (uint32_t)i) & 0x0FFF);
			audio[i * 2 + 0] = v;
			audio[i * 2 + 1] = (int16_t)(v ^ 0x0100);
		}
		if (batch_cb) {
			batch_cb(audio, N);
		} else if (sample_cb) {
			for (int i = 0; i < N; ++i) sample_cb(audio[i * 2], audio[i * 2 + 1]);
		}
	}

	frame_counter++;
}

size_t retro_serialize_size(void) { return 64; }

bool retro_serialize(void *data, size_t size) {
	if (!data || size < 64) return false;
	memset(data, 0, 64);
	memcpy(data, "SPHYNXSTUB01", 12);
	memcpy((char *)data + 12, &frame_counter, sizeof(frame_counter));
	return true;
}

bool retro_unserialize(const void *data, size_t size) {
	if (!data || size < 64) return false;
	if (memcmp(data, "SPHYNXSTUB01", 12) != 0) return false;
	memcpy(&frame_counter, (const char *)data + 12, sizeof(frame_counter));
	return true;
}

void retro_cheat_reset(void) {}
void retro_cheat_set(unsigned index, bool enabled, const char *code) {
	(void)index; (void)enabled; (void)code;
}

/* --- hardware renderer mode (stage 19 test hook) ---------------------------
 * The two callbacks in the request are written by the *frontend*, so this core
 * leaves them NULL and reads them back after SET_HW_RENDER returns - which is
 * exactly what mupen64plus_next and flycast do. */
bool retro_load_game(const struct retro_game_info *game) {
	{
		const char *crash = getenv("SPHYNX_STUB_CRASH_ON_LOAD");
		if (crash && *crash && atoi(crash) != 0) stub_crash_now();
	}
	{
		const char *hw = getenv("SPHYNX_STUB_HW");
		hw_mode = (hw && *hw && atoi(hw) != 0);
	}
	if (hw_mode) {
		/* Ask for a GLES2 context exactly like the real hardware cores do. */
		struct retro_hw_render_callback hw;
		memset(&hw, 0, sizeof(hw));
		hw.context_type = RETRO_HW_CONTEXT_OPENGLES2;
		hw.version_major = 2;
		hw.version_minor = 0;
		hw.depth = true;
		hw.stencil = false;
		hw.bottom_left_origin = true;
		if (!env_cb || !env_cb(RETRO_ENVIRONMENT_SET_HW_RENDER, &hw)) {
			/* What the real cores do when the frontend has no renderer. */
			return false;
		}
		/* The frontend filled get_current_framebuffer/get_proc_address in. */
		hw_framebuffer = hw.get_current_framebuffer();
		hw_glClearColor = (void (*)(float, float, float, float))(void *)(hw.get_proc_address("glClearColor"));
		hw_glClear = (void (*)(unsigned))(void *)(hw.get_proc_address("glClear"));
		hw_glBindFramebuffer = (void (*)(unsigned, unsigned))(void *)(hw.get_proc_address("glBindFramebuffer"));
		hw_glViewport = (void (*)(int, int, int, int))(void *)(hw.get_proc_address("glViewport"));
		hw_glEnable = (void (*)(unsigned))(void *)(hw.get_proc_address("glEnable"));
		hw_glDisable = (void (*)(unsigned))(void *)(hw.get_proc_address("glDisable"));
		hw_glScissor = (void (*)(int, int, int, int))(void *)(hw.get_proc_address("glScissor"));
		if (!hw_glClearColor || !hw_glClear || !hw_glBindFramebuffer) return false;
	}
	if (env_cb) {
		enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_RGB565;
		if (!env_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt)) return false;
		struct retro_system_av_info av;
		retro_get_system_av_info(&av);
		env_cb(RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO, &av);
	}
	game_loaded = (game != NULL);
	frame_counter = 0;
	return game_loaded;
}

bool retro_load_game_special(unsigned type, const struct retro_game_info *info, size_t num) {
	(void)type; (void)info; (void)num;
	return false;
}

void retro_unload_game(void) { game_loaded = false; }

unsigned retro_get_region(void) { return RETRO_REGION_NTSC; }

void *retro_get_memory_data(unsigned id) {
	(void)id;
	return NULL;
}

size_t retro_get_memory_size(unsigned id) {
	(void)id;
	return 0;
}
