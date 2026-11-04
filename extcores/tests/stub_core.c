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

void retro_run(void) {
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

bool retro_load_game(const struct retro_game_info *game) {
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
