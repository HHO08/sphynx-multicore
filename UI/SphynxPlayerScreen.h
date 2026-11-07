// ============================================================================
//  Sphynx — UI/SphynxPlayerScreen.h
//
//  The screen that plays every non-PSP system, *inside* PPSSPP.
//
//  Until stage 8 this was a separate Android Activity with its own GLSurfaceView
//  and its own hand-drawn buttons. That is why the buttons did not look like
//  PPSSPP's (they were plain Android widgets), why the settings dialogs looked
//  foreign, and - more importantly - why starting a game could throw the user
//  back to a restarted PPSSPP with a black screen.
//
//  This screen is a normal PPSSPP screen, so:
//    * the game picture is drawn by PPSSPP's own renderer, behind the UI;
//    * the buttons are the very same CreatePadLayout() the PSP games use, with
//      the user's own layout, opacity, scale and mappings;
//    * the pause menu is PPSSPP's pause-menu layout, with Save state /
//      Load state / Settings / Exit;
//    * there is no second Activity, no second EGL context and no second audio
//      thread, so nothing can "kick" the user out.
//
//  Only the emulated core changes.
//
//  SPDX-License-Identifier: GPL-2.0-or-later
// ============================================================================
#pragma once

#include <string>
#include <vector>

#include "Common/File/Path.h"
#include "Common/UI/UIScreen.h"
#include "Core/ControlMapper.h"
#include "Core/KeyMap.h"

struct SphynxPlayerHandle;

namespace Draw {
class Texture;
}

class SphynxPauseScreen;

class SphynxPlayerScreen : public UIScreen {
public:
	SphynxPlayerScreen(const Path &gamePath, const std::string &systemKey = "");
	~SphynxPlayerScreen() override;

	const char *tag() const override { return "SphynxPlayer"; }

	ScreenRenderFlags render(ScreenRenderMode mode) override;
	void update() override;
	bool key(const KeyInput &key) override;
	bool UnsyncTouch(const TouchInput &touch) override;
	bool UnsyncKey(const KeyInput &key) override;
	void UnsyncAxis(const AxisInput *axes, size_t count) override;
	void resized() override;
	void deviceLost() override;
	void focusChanged(ScreenFocusChange focusChange) override;

	// --- used by the pause menu ---------------------------------------------
	// Draws the frozen game picture. `dim` darkens it, which is what PPSSPP's
	// own pause menu does behind its columns.
	void DrawVideo(UIContext &dc, bool dim);
	bool SaveState(int slot, std::string *message);
	bool LoadState(int slot, std::string *message);
	bool StateExists(int slot);
	void SetSoundEnabled(bool on);
	bool SoundEnabled() const { return sound_; }
	void SetFastForward(bool on);
	bool FastForward() const { return fastForward_; }
	std::string SystemName();
	void OpenPauseMenu();
	// Writes battery saves right now (called when the menu opens and on exit).
	void FlushSave();

protected:
	void CreateViews() override;
	void DrawBackground(UIContext &dc) override;

private:
	void StartGame();
	void ChooseSystem(const std::vector<std::string> &keys, const std::vector<std::string> &names);
	void RunFrames();
	void UpdatePadState();
	void UploadFrame();
	void HandleCoreStop(int result);
	void QuitToMenu(const std::string &reason);
	void OnVirtualKey(int virtualKey, bool down);

	Path gamePath_;
	std::string systemKey_;      // empty: let the detector decide
	SphynxPlayerHandle *player_ = nullptr;

	Draw::Texture *texture_ = nullptr;
	std::vector<unsigned char> upload_;
	int texWidth_ = 0;
	int texHeight_ = 0;

	ControlMapper controlMapper_;
	bool pauseTrigger_ = false;
	bool started_ = false;
	bool stopped_ = false;
	bool fastForward_ = false;
	bool sound_ = true;

	double lastTime_ = 0.0;
	double frameDebt_ = 0.0;
};
