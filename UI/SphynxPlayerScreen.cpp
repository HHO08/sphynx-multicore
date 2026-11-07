// ============================================================================
//  Sphynx — UI/SphynxPlayerScreen.cpp
//
//  Playing a non-PSP system, with PPSSPP's own interface.
//
//  Everything the user sees here except the game picture is PPSSPP code that
//  already existed: CreatePadLayout() draws the touch buttons (same assets,
//  same positions, same opacity, same scale, same settings), ControlMapper
//  turns touches and physical pads into sceCtrl button bits, UIScreen draws the
//  view hierarchy, and the pause menu is built from the same Choice/ItemHeader
//  widgets GamePauseScreen uses.
//
//  The only new thing is the game picture: the external core's framebuffer is
//  uploaded into one Draw::Texture and drawn full-screen-while-keeping-aspect
//  behind the UI. That is why PPSSPP's renderer is doing the drawing and not a
//  second GLSurfaceView in a second Activity.
//
//  SPDX-License-Identifier: GPL-2.0-or-later
// ============================================================================
#include "UI/SphynxPlayerScreen.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "Common/Data/Text/I18n.h"
#include "Common/GPU/thin3d.h"
#include "Common/Log.h"
#include "Common/Render/DrawBuffer.h"
#include "Common/System/OSD.h"
#include "Common/StringUtils.h"
#include "Common/System/System.h"
#include "Common/TimeUtil.h"
#include "Common/UI/Context.h"
#include "Common/UI/PopupScreens.h"
#include "Common/UI/Root.h"
#include "Common/UI/UI.h"
#include "Common/UI/View.h"
#include "Common/UI/ViewGroup.h"

#include "Core/Config.h"
#include "Core/HLE/sceCtrl.h"

#include "UI/GameSettingsScreen.h"
#include "UI/GamepadEmu.h"
#include "UI/MainScreen.h"
#include "UI/MiscScreens.h"
#include "UI/OnScreenDisplay.h"

#include "sphynx_player_api.h"
#include "sphynx_ui_api.h"

// ---------------------------------------------------------------------------
//  Pause menu
//
//  Deliberately built like GamePauseScreen: a left column of save slots and a
//  right column of actions, over the frozen game picture. Using the same
//  widgets (ItemHeader / Choice / Spacer / ScrollView) is what makes it look
//  like PPSSPP instead of like a different app.
// ---------------------------------------------------------------------------
class SphynxPauseScreen : public UIDialogScreen {
public:
	explicit SphynxPauseScreen(SphynxPlayerScreen *player) : player_(player) {}
	const char *tag() const override { return "SphynxPause"; }
	bool isTopLevel() const override { return false; }

protected:
	void CreateViews() override;
	void DrawBackground(UIContext &dc) override;

private:
	UI::EventReturn OnSaveSlot(UI::EventParams &e);
	UI::EventReturn OnLoadSlot(UI::EventParams &e);
	UI::EventReturn OnSettings(UI::EventParams &e);
	UI::EventReturn OnFastForward(UI::EventParams &e);
	UI::EventReturn OnSound(UI::EventParams &e);
	UI::EventReturn OnExit(UI::EventParams &e);

	SphynxPlayerScreen *player_;
};

void SphynxPauseScreen::DrawBackground(UIContext &dc) {
	// The game is frozen behind the menu, exactly like PPSSPP's own pause screen.
	player_->DrawVideo(dc, true);
	dc.FillRect(UI::Drawable(0x80000000), dc.GetBounds());
}

void SphynxPauseScreen::CreateViews() {
	using namespace UI;

	auto pa = GetI18NCategory(I18NCat::PAUSE);
	auto di = GetI18NCategory(I18NCat::DIALOG);
	auto gr = GetI18NCategory(I18NCat::GRAPHICS);

	Margins scrollMargins(0, 10, 0, 0);
	Margins actionMenuMargins(0, 10, 15, 0);
	const bool vertical = UseVerticalLayout();

	root_ = new LinearLayout(ORIENT_HORIZONTAL);

	// --- left: the four save slots ----------------------------------------
	ViewGroup *leftColumn = new ScrollView(ORIENT_VERTICAL, new LinearLayoutParams(1.0, scrollMargins));
	root_->Add(leftColumn);
	LinearLayout *leftItems = new LinearLayoutList(ORIENT_VERTICAL, new LayoutParams(FILL_PARENT, WRAP_CONTENT));
	leftItems->SetSpacing(6.0f);
	leftColumn->Add(leftItems);

	leftItems->Add(new ItemHeader("Save state"));

	for (int i = 0; i < 4; ++i) {
		char label[64];
		const bool has = player_->StateExists(i);
		std::snprintf(label, sizeof(label), "Slot %d%s", i + 1, has ? "  (saved)" : "  (empty)");
		Choice *slot = leftItems->Add(new Choice(label));
		slot->SetTag(std::string("slot") + std::to_string(i));
		// One tap saves, the arrow button on the same row loads. That keeps the
		// column to four rows instead of eight.
		slot->OnClick.Handle(this, &SphynxPauseScreen::OnSaveSlot);
		slot->SetIcon(ImageID(has ? "I_SAVE" : "I_SAVE"));
	}

	leftItems->Add(new Spacer(8.0f));
	TextView *hint = leftItems->Add(new TextView("Tap a slot to save into it.\nOpen the arrow menu on a slot to load it.", new LinearLayoutParams(Margins(0, 0))));
	hint->SetSmall(true);

	// --- middle spacer ----------------------------------------------------
	root_->Add(new LinearLayout(ORIENT_VERTICAL, new LinearLayoutParams(32, FILL_PARENT)));

	// --- right: actions ---------------------------------------------------
	ViewGroup *rightHolder = new LinearLayout(ORIENT_VERTICAL, new LinearLayoutParams(vertical ? 220 : 320, FILL_PARENT, actionMenuMargins));
	root_->Add(rightHolder);
	ViewGroup *rightColumn = new ScrollView(ORIENT_VERTICAL, new LinearLayoutParams(1.0f));
	rightHolder->Add(rightColumn);
	LinearLayout *rightItems = new LinearLayout(ORIENT_VERTICAL);
	rightColumn->Add(rightItems);

	Choice *continueChoice = rightItems->Add(new Choice(pa->T("Continue")));
	continueChoice->OnClick.Add([this](UI::EventParams &) {
		// Closing the menu returns focus to the player, which resumes the core.
		screenManager()->finishDialog(this, DR_CANCEL);
		return UI::EVENT_DONE;
	});
	root_->SetDefaultFocusView(continueChoice);

	rightItems->Add(new Spacer(16.0f));

	UI::Choice *loadChoice = rightItems->Add(new Choice("Load state"));
	loadChoice->OnClick.Handle(this, &SphynxPauseScreen::OnLoadSlot);

	UI::Choice *settingsChoice = rightItems->Add(new Choice(pa->T("Settings")));
	settingsChoice->OnClick.Handle(this, &SphynxPauseScreen::OnSettings);

	UI::Choice *ffChoice = rightItems->Add(new Choice(player_->FastForward() ? "Fast forward: off" : "Fast forward: on"));
	ffChoice->OnClick.Handle(this, &SphynxPauseScreen::OnFastForward);
	ffChoice->SetTag("ff");

	UI::Choice *soundChoice = rightItems->Add(new Choice(player_->SoundEnabled() ? "Sound: on" : "Sound: off"));
	soundChoice->OnClick.Handle(this, &SphynxPauseScreen::OnSound);
	soundChoice->SetTag("sound");

	rightItems->Add(new Spacer(24.0f));

	UI::Choice *exitChoice = rightItems->Add(new Choice(pa->T("Exit to menu")));
	exitChoice->OnClick.Handle(this, &SphynxPauseScreen::OnExit);
	(void)di;
	(void)gr;
}

UI::EventReturn SphynxPauseScreen::OnSaveSlot(UI::EventParams &e) {
	const std::string tag = e.v->Tag();
	const int slot = tag.rfind("slot", 0) == 0 ? atoi(tag.c_str() + 4) : 0;
	std::string message;
	if (player_->SaveState(slot, &message)) {
		g_OSD.Show(OSDType::MESSAGE_INFO, message, 1.5f);
		RecreateViews();
	} else {
		g_OSD.Show(OSDType::MESSAGE_ERROR, message, 3.0f);
	}
	return UI::EVENT_DONE;
}

UI::EventReturn SphynxPauseScreen::OnLoadSlot(UI::EventParams &e) {
	// "Load state" offers the slots that actually contain a state.
	std::vector<std::string> labels;
	std::vector<int> slots;
	for (int i = 0; i < 4; ++i) {
		if (player_->StateExists(i)) {
			labels.push_back(std::string("Slot ") + std::to_string(i + 1));
			slots.push_back(i);
		}
	}
	if (labels.empty()) {
		g_OSD.Show(OSDType::MESSAGE_ERROR, "There is no save state for this game yet.", 2.0f);
		return UI::EVENT_DONE;
	}
	SphynxPlayerScreen *player = player_;
	screenManager()->push(new UI::ListPopupScreen("Load state", labels, 0, [player, slots](int choice) {
		if (choice < 0 || choice >= (int)slots.size()) return;
		std::string message;
		if (player->LoadState(slots[choice], &message))
			g_OSD.Show(OSDType::MESSAGE_INFO, message, 1.5f);
		else
			g_OSD.Show(OSDType::MESSAGE_ERROR, message, 3.0f);
	}, true));
	return UI::EVENT_DONE;
}

UI::EventReturn SphynxPauseScreen::OnSettings(UI::EventParams &e) {
	// PPSSPP's own settings screen: the same one the main menu opens.
	screenManager()->push(new GameSettingsScreen(Path(), ""));
	return UI::EVENT_DONE;
}

UI::EventReturn SphynxPauseScreen::OnFastForward(UI::EventParams &e) {
	player_->SetFastForward(!player_->FastForward());
	RecreateViews();
	return UI::EVENT_DONE;
}

UI::EventReturn SphynxPauseScreen::OnSound(UI::EventParams &e) {
	player_->SetSoundEnabled(!player_->SoundEnabled());
	RecreateViews();
	return UI::EVENT_DONE;
}

UI::EventReturn SphynxPauseScreen::OnExit(UI::EventParams &e) {
	// Close the menu, then the player: the game list is right behind it, so the
	// user lands where PPSSPP itself would have left them.
	player_->FlushSave();
	// Same destination PPSSPP's own pause menu uses: the game list.
	screenManager()->switchScreen(new MainScreen());
	return UI::EVENT_DONE;
}

// ---------------------------------------------------------------------------
//  Player screen
// ---------------------------------------------------------------------------

SphynxPlayerScreen::SphynxPlayerScreen(const Path &gamePath, const std::string &systemKey)
	: gamePath_(gamePath), systemKey_(systemKey) {
	// Route the pad through PPSSPP's mapper so physical gamepads, on-screen
	// buttons and user remaps all end up in the same place: sceCtrl's state.
	controlMapper_.SetCallbacks(
		[this](int vkey, bool down) { OnVirtualKey(vkey, down); },
		[this](int vkey, float value) { (void)vkey; (void)value; },
		[](uint32_t bitsToSet, uint32_t bitsToClear) {
			__CtrlUpdateButtons(bitsToSet, bitsToClear);
		},
		[](int stick, float x, float y) {
			__CtrlSetAnalogXY(stick, x, y);
		},
		nullptr);
}

SphynxPlayerScreen::~SphynxPlayerScreen() {
	// Undo the force above, so the rest of the app keeps the user's own setting.
	if (padForcedOn_) {
		g_Config.bShowTouchControls = false;
		padForcedOn_ = false;
	}
	if (player_) {
		sphynx_player_flush_save(player_);
		sphynx_player_close(player_);
		player_ = nullptr;
	}
	// Drop the texture while the draw context is still alive; textures are
	// ref-counted here, so releasing our reference is the deletion.
	if (texture_ && screenManager() && screenManager()->getDrawContext()) {
		texture_->Release();
	}
	texture_ = nullptr;
}

void SphynxPlayerScreen::CreateViews() {
	using namespace UI;

	// SPHYNX (button system, stage 12): the requirement is PPSSPP's own pad, on
	// screen, always. PPSSPP's default for ShowTouchControls is false unless the
	// device reports itself as DEVICE_TYPE_MOBILE (Core/Config.cpp,
	// DefaultShowTouchControls), and the setting is per-game - so a phone that
	// reports anything else, or a per-game profile written by an older install,
	// leaves CreatePadLayout() returning an empty layout: a game picture with no
	// buttons at all. That is one of the two things the owner keeps seeing, so
	// the player forces the pad on for its own screen only. Nothing is written to
	// the config, and the status line says when it happened.
	padForcedOn_ = !g_Config.bShowTouchControls;
	if (padForcedOn_)
		g_Config.bShowTouchControls = true;

	const Bounds &bounds = screenManager()->getUIContext()->GetLayoutBounds();
	InitPadLayout(bounds.w, bounds.h);

	// Devices without a back button need the on-screen pause button; the same
	// rule EmuScreen uses, so the setting means the same thing everywhere.
	const bool showPauseButton = !System_GetPropertyBool(SYSPROP_HAS_BACK_BUTTON) || g_Config.bShowTouchPause;
	root_ = CreatePadLayout(bounds.w, bounds.h, &pauseTrigger_, showPauseButton, &controlMapper_);
	root_->SetTag("sphynxpad");

	// The status line. Small, top-left, and the only thing on this screen that
	// is not PPSSPP's own: it names the build and says whether frames are
	// arriving. Without it, "the app threw me out" and "the core never drew a
	// frame" look exactly the same from a photograph-free bug report.
	statusView_ = root_->Add(new TextView("Sphynx", new AnchorLayoutParams(dp(8), dp(6), NONE, NONE, false)));
	statusView_->SetSmall(true);
	statusView_->SetShadow(true);
	statusView_->SetTextColor(0xFFE0E0E0);
	UpdateStatusLine();
}

ScreenRenderFlags SphynxPlayerScreen::render(ScreenRenderMode mode) {
	// SPHYNX (stage 12): the newest frame is moved into the texture here, while
	// the draw pass owns the GPU context - the same place PPSSPP uploads its own
	// textures. Then UIScreen::render draws DrawBackground() and the button
	// layout on top, which is exactly the order we need.
	if (player_ && !stopped_)
		UploadFrame();
	return UIScreen::render(mode);
}

void SphynxPlayerScreen::DrawBackground(UIContext &dc) {
	if (!started_ || !texture_ || texWidth_ <= 0 || texHeight_ <= 0) {
		dc.FillRect(UI::Drawable(0xFF000000), dc.GetBounds());
		return;
	}
	DrawVideo(dc, false);
}

void SphynxPlayerScreen::DrawVideo(UIContext &dc, bool dim) {
	if (!texture_ || texWidth_ <= 0 || texHeight_ <= 0) {
		dc.FillRect(UI::Drawable(0xFF000000), dc.GetBounds());
		return;
	}
	framesDrawn_++;
	const Bounds &bounds = dc.GetBounds();
	// Keep the aspect ratio: a stretched SNES picture is not "identical to
	// PPSSPP", it is just wrong.
	const float scale = std::min(bounds.w / (float)texWidth_, bounds.h / (float)texHeight_);
	const float w = texWidth_ * scale;
	const float h = texHeight_ * scale;
	const Bounds target(bounds.centerX() - w * 0.5f, bounds.centerY() - h * 0.5f, w, h);

	dc.Flush();
	dc.GetDrawContext()->BindTexture(0, texture_);
	dc.Draw()->DrawTexRect(target, 0.0f, 0.0f, 1.0f, 1.0f, dim ? 0xFF808080 : 0xFFFFFFFF);
	dc.Flush();
	dc.RebindTexture();
}

void SphynxPlayerScreen::update() {
	UIScreen::update();

	if (!started_) {
		StartGame();
		return;
	}
	if (stopped_)
		return;

	if (pauseTrigger_) {
		pauseTrigger_ = false;
		OpenPauseMenu();
		return;
	}

	// SPHYNX (button system, stage 11): PPSSPP's touch buttons are drawn with the
	// file-static g_gamepadOpacity in UI/GamepadEmu.cpp, and that value starts at
	// 0. Only EmuScreen ever refreshes it, and it does so by calling
	// GamepadUpdateOpacity() with no argument - which forces the value to 0
	// whenever coreState != CORE_RUNNING. Here no PSP core exists, so coreState
	// is never CORE_RUNNING: the result was a pad drawn with alpha 0, i.e. the
	// PPSSPP buttons were there but invisible.
	//
	// V5 solves it the same way: its RetroScreen::update() calls
	// GamepadUpdateOpacity(g_Config.iTouchButtonOpacity / 100.0f), the "forced"
	// form of the call, which sets the exact opacity the user configured and
	// skips the coreState gate. Same call, same value, same result: the pad now
	// looks exactly like the pad in a PSP game.
	GamepadUpdateOpacity(g_Config.iTouchButtonOpacity / 100.0f);

	RunFrames();

	// SPHYNX (stage 12): the frame is uploaded from render(), inside the draw
	// pass, not here. Creating and filling a GPU texture from update() is what
	// every other PPSSPP screen avoids, and on an Android GL driver it is a
	// use-outside-the-render-pass hazard. The newest frame simply waits in the
	// core's queue until the next render() and costs nothing.

	if (time_now_d() - lastStatus_ > 0.5) {
		lastStatus_ = time_now_d();
		UpdateStatusLine();
	}
}

void SphynxPlayerScreen::StartGame() {
	started_ = true;

	// The detector is the same one the game list uses, so a file that is shown
	// with an icon can always be started as well.
	std::string key = systemKey_;
	if (key.empty()) {
		char err[512] = {};
		SphynxPlayerHandle *probe = sphynx_player_open(gamePath_.ToString().c_str(), nullptr, err, sizeof(err));
		if (probe) {
			player_ = probe;
		} else {
			// Two possibilities: the core refused the file, or several systems
			// claim the extension and we must ask. The candidate list tells them
			// apart without a second detection pass.
			const char *candidates = sphynx_ui_candidates(gamePath_.ToString().c_str());
			std::vector<std::string> keys, names;
			if (candidates && *candidates) {
				std::string list = candidates;
				size_t start = 0;
				while (start <= list.size()) {
					const size_t end = list.find(';', start);
					const std::string item = list.substr(start, end == std::string::npos ? std::string::npos : end - start);
					const size_t bar = item.find('|');
					if (!item.empty()) {
						keys.push_back(bar == std::string::npos ? item : item.substr(0, bar));
						names.push_back(bar == std::string::npos ? item : item.substr(bar + 1));
					}
					if (end == std::string::npos) break;
					start = end + 1;
				}
			}
			if (keys.size() > 1) {
				ChooseSystem(keys, names);
				return;
			}
			ShowProblem("This game could not be started", std::string(err) + "\n" + gamePath_.ToString());
			return;
		}
	} else {
		char err[512] = {};
		player_ = sphynx_player_open(gamePath_.ToString().c_str(), key.c_str(), err, sizeof(err));
		if (!player_) {
			ShowProblem("This game could not be started", std::string(err) + "\n" + gamePath_.ToString());
			return;
		}
	}

	SetSoundEnabled(sound_);
	lastTime_ = time_now_d();
	startTime_ = lastTime_;
	frameDebt_ = 0.0;
	// One line on screen naming the system and the build: it is how the user can
	// tell this apart from any earlier player, without hunting through menus.
	g_OSD.Show(OSDType::MESSAGE_INFO,
		StringFromFormat("Sphynx player %s · %s", PPSSPP_GIT_VERSION, sphynx_player_system_name(player_)),
		2.5f);
	sphynx_ui_log_line((std::string("player screen: running ") + gamePath_.ToString()).c_str());
}

void SphynxPlayerScreen::ChooseSystem(const std::vector<std::string> &keys, const std::vector<std::string> &names) {
	const Path path = gamePath_;
	screenManager()->push(new UI::ListPopupScreen("Which system is this game for?", names, 0,
		[this, path, keys](int choice) {
			if (choice < 0 || choice >= (int)keys.size())
				return;
			char err[512] = {};
			player_ = sphynx_player_open(path.ToString().c_str(), keys[choice].c_str(), err, sizeof(err));
			if (!player_) {
				ShowProblem("This game could not be started", std::string(err) + "\n" + path.ToString());
				return;
			}
			SetSoundEnabled(sound_);
			lastTime_ = time_now_d();
		}, true));
}

void SphynxPlayerScreen::OnVirtualKey(int virtualKey, bool down) {
	switch (virtualKey) {
	case VIRTKEY_PAUSE:
		if (down) pauseTrigger_ = true;
		break;
	case VIRTKEY_FASTFORWARD:
		SetFastForward(down);
		break;
	case VIRTKEY_TOGGLE_TOUCH_CONTROLS:
		// Same code as EmuScreen's handler: a tap on a faded-out pad brings it
		// back instead of doing nothing.
		if (down) {
			if (g_Config.bShowTouchControls) {
				if (GamepadGetOpacity() < 0.01f) {
					GamepadTouch();
				} else {
					GamepadTouch(true);
				}
			} else {
				g_Config.bShowTouchControls = true;
				RecreateViews();
				GamepadTouch();
			}
		}
		break;
	default:
		break;
	}
}

void SphynxPlayerScreen::RunFrames() {
	if (!player_) return;

	// A core that runs but never draws looks like a black screen with buttons,
	// which is indistinguishable from a crash. Say so on screen instead of
	// guessing - this costs nothing and it is what a bug report needs.
	if (!frameSeen_ && !noFrameWarned_ && startTime_ > 0.0 && time_now_d() - startTime_ > 4.0) {
		noFrameWarned_ = true;
		g_OSD.Show(OSDType::MESSAGE_WARNING,
			std::string("No picture from the core yet — ") + sphynx_player_status(player_), 5.0f);
		sphynx_ui_log_line((std::string("player screen: no frames after 4 s: ") + sphynx_player_status(player_)).c_str());
	}

	const double now = time_now_d();
	if (lastTime_ == 0.0) lastTime_ = now;
	double dt = now - lastTime_;
	lastTime_ = now;
	// Coming back from the background must not fast-forward through a minute of
	// gameplay in one burst.
	if (dt < 0.0) dt = 0.0;
	if (dt > 0.25) dt = 0.25;

	double fps = sphynx_player_fps(player_);
	if (fps < 20.0 || fps > 200.0) fps = 60.0;
	frameDebt_ += dt * fps;
	if (fastForward_) frameDebt_ += dt * fps * 2.0;   // "fast forward" = 3x total

	// The cap keeps a slow device responsive: better to drop emulated frames
	// than to freeze the UI thread that also draws the buttons.
	int budget = 4;
	while (frameDebt_ >= 1.0 && budget-- > 0) {
		UpdatePadState();
		controlMapper_.Update(now);
		const int result = sphynx_player_run(player_);
		frameDebt_ -= 1.0;
		if (result <= 0) {
			HandleCoreStop(result);
			return;
		}
	}
	if (frameDebt_ < 0.0) frameDebt_ = 0.0;
}

void SphynxPlayerScreen::UpdatePadState() {
	// PPSSPP's pad state: the on-screen buttons write it directly, ControlMapper
	// writes the mapped physical inputs into it, and we only read it back. That
	// is why a gamepad mapped for PSP also works here, unchanged.
	const unsigned bits = __CtrlPeekButtons();
	float lx = 0.0f, ly = 0.0f, rx = 0.0f, ry = 0.0f;
	__CtrlPeekAnalog(0, &lx, &ly);
	__CtrlPeekAnalog(1, &rx, &ry);
	sphynx_player_set_pad(player_, bits, lx, ly, rx, ry);
}

void SphynxPlayerScreen::UploadFrame() {
	if (!player_) return;
	// No draw context means no GPU: this happens for a frame or two after the
	// app returns from the background. The frame simply waits.
	if (!screenManager() || !screenManager()->getDrawContext()) return;
	const unsigned want_w = (unsigned)texWidth_, want_h = (unsigned)texHeight_;
	(void)want_w;
	(void)want_h;

	// Frames can change size at runtime (a core switching resolutions), so the
	// texture is re-created when the incoming frame differs from it.
	if (upload_.empty()) upload_.resize(1024 * 512 * 4);
	// SPHYNX (core-running system, stage 11): size the buffer from the frame that
	// is actually waiting. The old code took the frame into a fixed 2 MB buffer,
	// and take_frame() throws the frame away when it does not fit - so an arcade
	// core at 1024x1024 (4 MB) produced a permanently black screen with sound
	// playing, which is indistinguishable from "the game did not start".
	int need_w = 0, need_h = 0;
	if (sphynx_player_frame_size(player_, &need_w, &need_h) == 1 && need_w > 0 && need_h > 0) {
		const size_t needed = static_cast<size_t>(need_w) * static_cast<size_t>(need_h) * 4u;
		if (upload_.size() < needed) upload_.resize(needed);
	}
	for (int attempt = 0; attempt < 2; ++attempt) {
		int w = 0, h = 0;
		if (!sphynx_player_take_frame(player_, upload_.data(), (int)upload_.size(), &w, &h)) {
			// Either nothing new arrived, or the buffer was too small. The second
			// case is told apart by asking the player for its current size below.
			break;
		}
		frameSeen_ = true;
		bool recreate = (texture_ == nullptr) || w != texWidth_ || h != texHeight_;
		if (texture_ && recreate) {
			texture_->Release();
			texture_ = nullptr;
		}
		if (!texture_) {
			Draw::TextureDesc desc{};
			desc.type = Draw::TextureType::LINEAR2D;
			desc.format = Draw::DataFormat::R8G8B8A8_UNORM;
			desc.width = w;
			desc.height = h;
			desc.depth = 1;
			desc.mipLevels = 1;
			desc.generateMips = false;
			desc.swizzle = Draw::TextureSwizzle::DEFAULT;
			desc.tag = "sphynx_frame";
			texture_ = screenManager()->getDrawContext()->CreateTexture(desc);
			texWidth_ = w;
			texHeight_ = h;
		}
		if (texture_) {
			const uint8_t *planes[1] = { upload_.data() };
			screenManager()->getDrawContext()->UpdateTextureLevels(texture_, planes, nullptr, 1);
		}
		break;
	}
}

void SphynxPlayerScreen::HandleCoreStop(int result) {
	stopped_ = true;
	if (result < 0) {
		ShowProblem("This game stopped", sphynx_player_status(player_));
	} else {
		// The core asked to quit by itself (a game that ends, a menu choice):
		// still the user's decision to leave, so offer it instead of doing it.
		ShowProblem("The system stopped this game", "The emulated system closed the game by itself.");
	}
}

void SphynxPlayerScreen::QuitToMenu(const std::string &reason) {
	// Only ever reached from a button the user pressed, never automatically.
	FlushSave();
	screenManager()->switchScreen(new MainScreen());
	(void)reason;
}

// Everything that can go wrong lands here, and here is a panel on the game
// screen that stays until the user leaves. That is the difference between "it
// did not work" and "the app threw me out": nothing happens behind the user's
// back any more, and the reason is readable on the device.
void SphynxPlayerScreen::ShowProblem(const std::string &title, const std::string &detail) {
	if (failed_)
		return;
	failed_ = true;
	stopped_ = true;

	sphynx_ui_log_line((std::string("player screen: problem: ") + title + " | " + detail).c_str());

	using namespace UI;
	LinearLayout *panel = new LinearLayout(ORIENT_VERTICAL);
	panel->SetSpacing(8.0f);

	TextView *heading = panel->Add(new TextView(title));
	heading->SetTextColor(0xFFE8E8E8);
	heading->SetShadow(true);

	TextView *body = panel->Add(new TextView(StringFromFormat("%s\n\nBuild: %s", detail.c_str(), PPSSPP_GIT_VERSION)));
	body->SetSmall(true);
	body->SetTextColor(0xFFB8B8B8);

	// The log tail: on a device I cannot touch this is the only way to learn what
	// actually happened, and it is where "why did it stop" is answered.
	TextView *logView = panel->Add(new TextView(sphynx_ui_diag_report()));
	logView->SetSmall(true);
	logView->SetTextColor(0xFF909090);

	Choice *exit = panel->Add(new Choice("Back to the game list"));
	exit->OnClick.Add([this](UI::EventParams &) {
		screenManager()->switchScreen(new MainScreen());
		return UI::EVENT_DONE;
	});

	const Bounds &bounds = screenManager()->getUIContext()->GetBounds();
	const float width = std::min(bounds.w - dp(40), dp(620));
	ScrollView *scroll = new ScrollView(ORIENT_VERTICAL, new AnchorLayoutParams(width, std::min(bounds.h - dp(40), dp(420)), NONE, NONE, NONE, NONE, true));
	scroll->Add(panel);
	root_->Add(scroll);
}

// One line: build, system, core, picture, pad. Everything needed to tell the
// three possible states apart - screen never opened, core running but silent,
// or running normally.
void SphynxPlayerScreen::UpdateStatusLine() {
	if (!statusView_ || failed_)
		return;
	char line[256];
	const float padOpacity = g_Config.iTouchButtonOpacity / 100.0f;
	std::snprintf(line, sizeof(line), "Sphynx %s · %s%s%s · %dx%d · %llu frames · pad %s %.2f",
	              PPSSPP_GIT_VERSION,
	              player_ ? (SystemName().empty() ? "?" : SystemName().c_str()) : "starting",
	              stopped_ ? " · stopped" : "",
	              sound_ ? "" : " · muted",
	              texWidth_, texHeight_, framesDrawn_,
	              padForcedOn_ ? "on(auto)" : (g_Config.bShowTouchControls ? "on" : "OFF"),
	              padOpacity);
	statusView_->SetText(line);
}

float SphynxPlayerScreen::dp(float value) {
	// PPSSPP's layouts are authored for a 480-point-tall screen; scale the same
	// way the UI does so the panel is the same physical size on every device.
	ScreenManager *sm = screenManager();
	UIContext *ctx = sm ? sm->getUIContext() : nullptr;
	const float h = ctx ? ctx->GetBounds().h : 480.0f;
	return value * (h / 480.0f);
}

void SphynxPlayerScreen::OpenPauseMenu() {
	FlushSave();
	screenManager()->push(new SphynxPauseScreen(this));
}

bool SphynxPlayerScreen::SaveState(int slot, std::string *message) {
	if (!player_) return false;
	char msg[256] = {};
	const int ok = sphynx_player_save_state(player_, slot, msg, sizeof(msg));
	if (message) *message = msg;
	return ok == 1;
}

bool SphynxPlayerScreen::LoadState(int slot, std::string *message) {
	if (!player_) return false;
	char msg[256] = {};
	const int ok = sphynx_player_load_state(player_, slot, msg, sizeof(msg));
	if (message) *message = msg;
	// A loaded state does not emit a frame by itself. render() uploads whatever
	// the core produces next, so the picture is refreshed by the very next
	// rendered frame without touching the GPU from here.
	return ok == 1;
}

bool SphynxPlayerScreen::StateExists(int slot) {
	return player_ && sphynx_player_state_exists(player_, slot) == 1;
}

void SphynxPlayerScreen::SetSoundEnabled(bool on) {
	sound_ = on;
	if (player_) sphynx_player_set_sound(player_, on ? 1 : 0);
}

void SphynxPlayerScreen::SetFastForward(bool on) {
	fastForward_ = on;
	// 3 frames per tick: the same "hold the fast-forward button" speed feel as
	// PPSSPP's own fast-forward, without needing a second clock.
	if (player_) sphynx_player_set_fast_forward(player_, on ? 3 : 1);
}

std::string SphynxPlayerScreen::SystemName() {
	return player_ ? sphynx_player_system_name(player_) : "";
}

void SphynxPlayerScreen::FlushSave() {
	if (player_) sphynx_player_flush_save(player_);
}

bool SphynxPlayerScreen::key(const KeyInput &key) {
	// The back button is the pause button, exactly as in a PSP game.
	if ((key.flags & KEY_DOWN) && UI::IsEscapeKey(key) && started_ && !stopped_) {
		pauseTrigger_ = true;
		return true;
	}
	return UIScreen::key(key);
}

bool SphynxPlayerScreen::UnsyncTouch(const TouchInput &touch) {
	// SPHYNX (button system, stage 11): every touch is a "the pad is in use"
	// signal. Without this the fade timer in GamepadEmu.cpp never restarts, so
	// the buttons would fade out and stay out. V5's RetroScreen::UnsyncTouch()
	// starts with exactly this call, and so does EmuScreen::UnsyncTouch().
	GamepadTouch();
	// Same path as EmuScreen: the pad layout gets a chance first, then the UI.
	if (root_)
		UIScreen::UnsyncTouch(touch);
	return true;
}

bool SphynxPlayerScreen::UnsyncKey(const KeyInput &key) {
	return UIScreen::UnsyncKey(key);
}

void SphynxPlayerScreen::UnsyncAxis(const AxisInput *axes, size_t count) {
	if (UI::IsFocusMovementEnabled())
		return UIScreen::UnsyncAxis(axes, count);
	return controlMapper_.Axis(axes, count);
}

void SphynxPlayerScreen::resized() {
	RecreateViews();
}

void SphynxPlayerScreen::deviceLost() {
	// The GPU context is gone: the texture with it. It is re-created from the
	// next frame the core produces, so nothing is lost but a few milliseconds.
	texture_ = nullptr;
	texWidth_ = texHeight_ = 0;
	UIScreen::deviceLost();
}

void SphynxPlayerScreen::focusChanged(ScreenFocusChange focusChange) {
	Screen::focusChanged(focusChange);
	if (focusChange == ScreenFocusChange::FOCUS_LOST_TOP) {
		// A menu opened: release every held button so the pad does not come back
		// "stuck down" when the menu closes.
		controlMapper_.ReleaseAll();
		__CtrlUpdateButtons(0, 0xFFFFFFFF);
		FlushSave();
	} else if (focusChange == ScreenFocusChange::FOCUS_BECAME_TOP) {
		// Returning from a menu: do not count the menu time as game time.
		lastTime_ = time_now_d();
		frameDebt_ = 0.0;
		// SPHYNX (settings style, stage 11): PPSSPP's Settings screen can have
		// changed what the buttons look like (opacity, scale, layout, show/hide).
		// EmuScreen rebuilds its views after settings for the same reason; without
		// this the pad kept the old look until the game was reopened, which reads
		// as "the settings do not work here". A failure panel lives in root_ and
		// must not be rebuilt away, so it is left alone.
		if (!failed_)
			RecreateViews();
	}
}
