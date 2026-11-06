// ============================================================================
//  Sphynx — playback screen for every non-PSP system.
//
//  Own GLSurfaceView + own EGL context + own AudioTrack: nothing here can
//  disturb PPSSPP's PSP renderer, which keeps its own state underneath.
//
//  Hard-won details that are easy to get wrong (each one was a bug):
//    * content:// URIs must be resolved to a real file before a core sees them
//    * onResume() must call surface.onResume(), otherwise the picture freezes
//      for good after the first time the app goes to the background
//    * audio must be paused with the activity, or it keeps playing behind the
//      home screen
//    * a failure must be *visible*: an error that only goes to logcat looks
//      exactly like a black screen to the user
//
//  Added in this stage:
//    * a Menu button with Save state / Load state / Settings / Close
//    * four save-state slots per game, written next to the app's save data
//    * Settings: sound, touch controls, fast forward, frame-rate display,
//      sharp/smooth filtering and screen fit — remembered across launches
//    * the game is given its own BIOS folder (PSP/SYSTEMS_BIOS/<system>)
//
//  SPDX-License-Identifier: GPL-2.0-or-later
// ============================================================================
package org.sphynx.emulator;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.SharedPreferences;
import android.graphics.Color;
import android.media.AudioFormat;
import android.media.AudioManager;
import android.media.AudioTrack;
import android.opengl.GLSurfaceView;
import android.os.Bundle;
import android.util.Log;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewGroup;
import android.view.ViewGroup.LayoutParams;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.TextView;
import android.widget.Toast;

import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.util.concurrent.atomic.AtomicBoolean;

import javax.microedition.khronos.egl.EGLConfig;
import javax.microedition.khronos.opengles.GL10;

public class SphynxPlayerActivity extends Activity {
	private static final String TAG = "SphynxPlayer";

	public static final String EXTRA_PATH = "sphynx.path";

	// libretro joypad ids (retropad layout, identical across every core).
	private static final int B = 0, Y = 1, SELECT = 2, START = 3,
			UP = 4, DOWN = 5, LEFT = 6, RIGHT = 7, A = 8, X = 9, L = 10, R = 11;

	// How many save-state slots the menu offers, per game.
	private static final int STATE_SLOTS = 4;

	private static final String PREFS = "sphynx_player";

	private GLSurfaceView surface;
	private AudioThread audio;
	private TextView overlay;
	private View controls;
	private final AtomicBoolean running = new AtomicBoolean(false);
	private int frameCounter;

	/** Set once the native session exists, so onResume knows to restart audio. */
	private boolean sessionReady;

	private String systemKey = "";
	private SharedPreferences prefs;

	@Override
	protected void onCreate(Bundle savedInstanceState) {
		super.onCreate(savedInstanceState);
		getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
		SphynxExtCore.attachContext(this);
		prefs = getSharedPreferences(PREFS, MODE_PRIVATE);
		if (!SphynxExtCore.isAvailable()) {
			fail("The emulator engine is missing from this build (libsphynx_extcore.so).");
			return;
		}
		// Make sure the BIOS tree exists even when the player is opened without
		// going through PPSSPP's own main screen first.
		SphynxExtCore.ensureBiosTree(this);

		String requested = getIntent() != null ? getIntent().getStringExtra(EXTRA_PATH) : null;
		if (requested == null || requested.isEmpty()) {
			fail("No game file was passed to the player.");
			return;
		}

		// Android may hand us a content:// URI. Cores cannot open those, so turn
		// it into a real file first — this is the single most important step.
		String path = SphynxExtCore.resolvePath(requested);
		if (path == null || path.isEmpty()) {
			fail("This file cannot be opened.\nIf it lives in a cloud or Downloads "
					+ "folder, copy it into the device storage (PSP/GAME) and try again.");
			return;
		}
		Log.i(TAG, "playing " + path);

		String detected = detectSystem(path);
		if (detected == null || detected.isEmpty()) {
			// Shared extensions (.cue / .chd / .iso) can belong to several
			// systems. Ask exactly once, then play.
			final String[] choices = candidateSystems(path);
			if (choices.length > 0) {
				askSystem(choices, path);
				return;
			}
			fail("This game file was not recognised.\n" + path);
			return;
		}
		startWithSystem(path, detected);
	}

	private void startWithSystem(String path, String key) {
		systemKey = key;
		String corePath = SphynxExtCore.corePath(key, getApplicationInfo().nativeLibraryDir);
		if (corePath == null || corePath.isEmpty()) {
			fail("The emulator for this system is not in this build.\n(" + key + ")");
			return;
		}
		File core = new File(corePath);
		if (!core.isFile()) {
			fail("The emulator file is missing:\n" + corePath);
			return;
		}
		launch(path, corePath);
	}

	private void launch(String path, String corePath) {
		// Each system gets its own BIOS folder (PSP/SYSTEMS_BIOS/<system>); systems
		// without a BIOS fall back to PPSSPP's normal PSP/SYSTEM folder.
		File sysDir = new File(SphynxExtCore.systemDirForSystem(this, systemKey));
		File saveDir = new File(SphynxExtCore.saveDir(this));

		SphynxPlayer.nativeSetSystem(systemKey);
		applySetting("sound", prefs.getBoolean("sound", true));
		applySetting("pad", prefs.getBoolean("pad", true));
		applySetting("fastforward", prefs.getBoolean("fastforward", false));
		applySetting("smooth", prefs.getBoolean("smooth", false));
		applySetting("showfps", prefs.getBoolean("showfps", true));
		applySetting("aspect", prefs.getInt("aspect", 0));

		String err;
		try {
			err = SphynxPlayer.nativeStart(corePath, path, sysDir.getAbsolutePath(),
					saveDir.getAbsolutePath());
		} catch (Throwable t) {
			// A missing or unloadable native library lands here instead of
			// killing the process with UnsatisfiedLinkError.
			Log.e(TAG, "nativeStart threw", t);
			fail("The emulator engine could not be loaded on this device.\n" + t);
			return;
		}
		if (err != null && !err.isEmpty()) {
			Log.e(TAG, "start failed: " + err);
			fail(startFailureMessage(err, sysDir));
			return;
		}
		sessionReady = true;

		FrameLayout root = new FrameLayout(this);
		root.setBackgroundColor(Color.BLACK);

		surface = new GLSurfaceView(this);
		surface.setEGLContextClientVersion(2);
		surface.setRenderer(new Renderer());
		surface.setRenderMode(GLSurfaceView.RENDERMODE_CONTINUOUSLY);
		root.addView(surface, new FrameLayout.LayoutParams(
				ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));

		controls = buildControls();
		root.addView(controls, new FrameLayout.LayoutParams(
				ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));

		overlay = new TextView(this);
		overlay.setTextColor(Color.WHITE);
		overlay.setTextSize(10f);
		overlay.setAlpha(0.45f);
		FrameLayout.LayoutParams op = new FrameLayout.LayoutParams(
				ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT);
		op.gravity = Gravity.TOP | Gravity.START;
		op.setMargins(dp(10), dp(8), 0, 0);
		root.addView(overlay, op);

		root.addView(buildMenuButton(), menuButtonParams());

		setContentView(root);
		applyVisibilitySettings();
		startAudio();
		running.set(true);
	}

	/** The message shown when a core refuses to start: never a bare "failed". */
	private String startFailureMessage(String err, File sysDir) {
		StringBuilder message = new StringBuilder("This game could not be started:\n");
		message.append(err).append("\n\nSystem: ").append(systemKey);
		String bios = SphynxPlayer.nativeBiosStatus();
		if (bios != null && !bios.isEmpty() && !bios.equals("not needed by this system")) {
			message.append("\nBIOS: ").append(bios);
		}
		message.append("\nBIOS folder:\n").append(sysDir.getAbsolutePath());
		message.append("\n\nSave states: ").append(SphynxExtCore.saveDir(this)).append("/states");
		return message.toString();
	}

	/** Shows the reason on screen. A toast alone was invisible in practice. */
	private void fail(String message) {
		Log.e(TAG, message.replace('\n', ' '));
		running.set(false);
		LinearLayout box = new LinearLayout(this);
		box.setOrientation(LinearLayout.VERTICAL);
		box.setBackgroundColor(Color.BLACK);
		box.setPadding(dp(24), dp(64), dp(24), dp(24));

		TextView title = new TextView(this);
		title.setText("Sphynx — could not play this game");
		title.setTextColor(Color.WHITE);
		title.setTextSize(18f);
		box.addView(title);

		TextView body = new TextView(this);
		body.setText(message);
		body.setTextColor(0xFFCCCCCC);
		body.setTextSize(13f);
		body.setPadding(0, dp(16), 0, dp(16));
		box.addView(body);

		Button close = new Button(this);
		close.setText("Back");
		close.setOnClickListener(v -> finish());
		box.addView(close);

		setContentView(box);
	}

	/** "key|Display name;..." -> {"key|Display name", ...} */
	private String[] candidateSystems(String path) {
		String raw = SphynxExtCore.candidates(path);
		if (raw == null || raw.isEmpty()) return new String[0];
		return raw.split(";");
	}

	private void askSystem(final String[] choices, final String path) {
		final String[] labels = new String[choices.length];
		for (int i = 0; i < choices.length; ++i) {
			int bar = choices[i].indexOf('|');
			labels[i] = bar >= 0 ? choices[i].substring(bar + 1) : choices[i];
		}
		new AlertDialog.Builder(this)
				.setTitle(org.ppsspp.ppsspp.R.string.sphynxChooseSystem)
				.setItems(labels, (dialog, which) -> {
					String key = choices[which];
					int bar = key.indexOf('|');
					if (bar >= 0) key = key.substring(0, bar);
					startWithSystem(path, key);
				})
				.setOnCancelListener(dialog -> finish())
				.show();
	}

	private String detectSystem(String path) {
		byte[] head = new byte[4096];
		int n = 0;
		try (FileInputStream in = new FileInputStream(path)) {
			n = in.read(head);
		} catch (IOException e) {
			Log.w(TAG, "cannot read " + path, e);
		}
		if (n > 0) {
			byte[] exact = new byte[n];
			System.arraycopy(head, 0, exact, 0, n);
			head = exact;
		} else {
			head = null;
		}
		return SphynxExtCore.detect(path, head);
	}

	// --- menu: save state / load state / settings --------------------------
	private View buildMenuButton() {
		Button menu = new Button(this);
		menu.setText("☰");
		menu.setAlpha(0.45f);
		menu.setBackgroundColor(Color.TRANSPARENT);
		menu.setTextColor(Color.WHITE);
		menu.setOnClickListener(v -> showMenu());
		return menu;
	}

	private FrameLayout.LayoutParams menuButtonParams() {
		FrameLayout.LayoutParams lp = new FrameLayout.LayoutParams(dp(52), dp(44));
		lp.gravity = Gravity.TOP | Gravity.END;
		lp.setMargins(0, dp(6), dp(6), 0);
		return lp;
	}

	private void showMenu() {
		final String[] items = {
				"Save state",
				"Load state",
				"Settings",
				"Close game",
		};
		new AlertDialog.Builder(this)
				.setTitle("Sphynx menu")
				.setItems(items, (dialog, which) -> {
					switch (which) {
						case 0:
							showSlots(true);
							break;
						case 1:
							showSlots(false);
							break;
						case 2:
							showSettings();
							break;
						default:
							finish();
							break;
					}
				})
				.show();
	}

	/** Slot chooser for saving (save=true) or loading (save=false). */
	private void showSlots(final boolean save) {
		final String[] labels = new String[STATE_SLOTS];
		for (int i = 0; i < STATE_SLOTS; ++i) {
			boolean has = hasState(i);
			labels[i] = "Slot " + (i + 1) + (has ? "  (saved)" : "  (empty)");
		}
		new AlertDialog.Builder(this)
				.setTitle(save ? "Save state to…" : "Load state from…")
				.setItems(labels, (dialog, which) -> {
					final int slot = which;
					if (!save && !hasState(slot)) {
						toast("Slot " + (slot + 1) + " is empty.");
						return;
					}
					String result;
					try {
						result = save ? SphynxPlayer.nativeSaveState(slot)
								: SphynxPlayer.nativeLoadState(slot);
					} catch (Throwable t) {
						Log.e(TAG, "state operation failed", t);
						toast("The save state operation failed.");
						return;
					}
					if (result == null || result.isEmpty()) {
						toast((save ? "Saved to slot " : "Loaded slot ") + (slot + 1));
					} else {
						toast(result);
					}
				})
				.show();
	}

	private boolean hasState(int slot) {
		try {
			return SphynxPlayer.nativeHasState(slot);
		} catch (Throwable t) {
			return false;
		}
	}

	/** Settings dialog: everything applies instantly and is remembered. */
	private void showSettings() {
		LinearLayout box = new LinearLayout(this);
		box.setOrientation(LinearLayout.VERTICAL);
		int pad = dp(20);
		box.setPadding(pad, pad, pad, pad);

		final CheckBox sound = checkBox("Sound", prefs.getBoolean("sound", true));
		final CheckBox touch = checkBox("Touch controls", prefs.getBoolean("pad", true));
		final CheckBox fast = checkBox("Fast forward", prefs.getBoolean("fastforward", false));
		final CheckBox smooth = checkBox("Smooth filter (off = sharp pixels)",
				prefs.getBoolean("smooth", false));
		final CheckBox fps = checkBox("Show frame time", prefs.getBoolean("showfps", true));
		box.addView(sound);
		box.addView(touch);
		box.addView(fast);
		box.addView(smooth);
		box.addView(fps);

		// Screen fit: 0 fit, 1 stretch, 2 = 4:3.
		final int[] aspect = {prefs.getInt("aspect", 0)};
		final String[] aspectNames = {"Fit (keep proportions)", "Stretch to screen", "Force 4:3"};
		final Button aspectButton = new Button(this);
		aspectButton.setText("Screen: " + aspectNames[aspect[0]]);
		aspectButton.setOnClickListener(v -> {
			aspect[0] = (aspect[0] + 1) % aspectNames.length;
			aspectButton.setText("Screen: " + aspectNames[aspect[0]]);
		});
		box.addView(aspectButton);

		// Informational: which BIOS folder this game reads and what is missing.
		TextView biosInfo = new TextView(this);
		String status = SphynxPlayer.nativeBiosStatus();
		biosInfo.setText("BIOS: " + status + "\nFolder: "
				+ SphynxExtCore.systemDirForSystem(this, systemKey));
		biosInfo.setTextSize(TypedValue.COMPLEX_UNIT_SP, 11f);
		biosInfo.setTextColor(0xFFAAAAAA);
		biosInfo.setPadding(0, dp(12), 0, 0);
		box.addView(biosInfo);

		new AlertDialog.Builder(this)
				.setTitle("Settings")
				.setView(box)
				.setPositiveButton("Done", (dialog, which) -> {
					prefs.edit()
							.putBoolean("sound", sound.isChecked())
							.putBoolean("pad", touch.isChecked())
							.putBoolean("fastforward", fast.isChecked())
							.putBoolean("smooth", smooth.isChecked())
							.putBoolean("showfps", fps.isChecked())
							.putInt("aspect", aspect[0])
							.apply();
					applySetting("sound", sound.isChecked());
					applySetting("pad", touch.isChecked());
					applySetting("fastforward", fast.isChecked());
					applySetting("smooth", smooth.isChecked());
					applySetting("showfps", fps.isChecked());
					applySetting("aspect", aspect[0]);
					applyVisibilitySettings();
				})
				.show();
	}

	private CheckBox checkBox(String label, boolean value) {
		CheckBox box = new CheckBox(this);
		box.setText(label);
		box.setTextColor(Color.WHITE);
		box.setChecked(value);
		return box;
	}

	private void applySetting(String key, boolean value) {
		try {
			SphynxPlayer.nativeSetOption(key, value ? 1 : 0);
		} catch (Throwable t) {
			Log.w(TAG, "cannot apply " + key, t);
		}
	}

	private void applySetting(String key, int value) {
		try {
			SphynxPlayer.nativeSetOption(key, value);
		} catch (Throwable t) {
			Log.w(TAG, "cannot apply " + key, t);
		}
	}

	/** Touch pad + frame-time line follow the Settings switches. */
	private void applyVisibilitySettings() {
		if (controls != null) {
			controls.setVisibility(prefs.getBoolean("pad", true) ? View.VISIBLE : View.GONE);
		}
		if (overlay != null) {
			overlay.setVisibility(prefs.getBoolean("showfps", true) ? View.VISIBLE : View.GONE);
		}
	}

	private void toast(String message) {
		Toast.makeText(this, message, Toast.LENGTH_SHORT).show();
	}

	// --- on-screen pad -----------------------------------------------------
	private View buildControls() {
		FrameLayout layer = new FrameLayout(this);

		LinearLayout dpad = new LinearLayout(this);
		dpad.setOrientation(LinearLayout.VERTICAL);
		dpad.addView(padButton("▲", UP));
		LinearLayout row = new LinearLayout(this);
		row.addView(padButton("◀", LEFT));
		row.addView(padButton("▶", RIGHT));
		dpad.addView(row);
		dpad.addView(padButton("▼", DOWN));
		FrameLayout.LayoutParams dl = new FrameLayout.LayoutParams(dp(LayoutParams.WRAP_CONTENT),
				dp(LayoutParams.WRAP_CONTENT));
		dl.gravity = Gravity.BOTTOM | Gravity.START;
		dl.setMargins(dp(12), 0, 0, dp(12));
		layer.addView(dpad, dl);

		LinearLayout face = new LinearLayout(this);
		face.setOrientation(LinearLayout.VERTICAL);
		LinearLayout faceRow = new LinearLayout(this);
		faceRow.addView(padButton("B", B));
		faceRow.addView(padButton("A", A));
		face.addView(faceRow);
		LinearLayout faceRow2 = new LinearLayout(this);
		faceRow2.addView(padButton("Y", Y));
		faceRow2.addView(padButton("X", X));
		face.addView(faceRow2);
		FrameLayout.LayoutParams fl = new FrameLayout.LayoutParams(dp(LayoutParams.WRAP_CONTENT),
				dp(LayoutParams.WRAP_CONTENT));
		fl.gravity = Gravity.BOTTOM | Gravity.END;
		fl.setMargins(0, 0, dp(12), dp(12));
		layer.addView(face, fl);

		LinearLayout middle = new LinearLayout(this);
		middle.setOrientation(LinearLayout.HORIZONTAL);
		middle.addView(padButton("SELECT", SELECT));
		middle.addView(padButton("START", START));
		middle.addView(padButton("L", L));
		middle.addView(padButton("R", R));
		Button quit = new Button(this);
		quit.setText("✕");
		quit.setAlpha(0.5f);
		quit.setOnClickListener(v -> finish());
		middle.addView(quit);
		FrameLayout.LayoutParams ml = new FrameLayout.LayoutParams(dp(LayoutParams.WRAP_CONTENT),
				dp(LayoutParams.WRAP_CONTENT));
		ml.gravity = Gravity.BOTTOM | Gravity.CENTER_HORIZONTAL;
		ml.setMargins(0, 0, 0, dp(12));
		layer.addView(middle, ml);
		return layer;
	}

	private int dp(int value) {
		return (int) (value * getResources().getDisplayMetrics().density);
	}

	private Button padButton(String label, final int button) {
		Button b = new Button(this);
		b.setText(label);
		b.setAlpha(0.35f);
		b.setBackgroundColor(Color.TRANSPARENT);
		b.setTextColor(Color.WHITE);
		b.setOnTouchListener((v, event) -> {
			switch (event.getActionMasked()) {
				case MotionEvent.ACTION_DOWN:
					v.setAlpha(0.7f);
					SphynxPlayer.nativeSetButton(button, true);
					return true;
				case MotionEvent.ACTION_UP:
				case MotionEvent.ACTION_CANCEL:
					v.setAlpha(0.35f);
					SphynxPlayer.nativeSetButton(button, false);
					return true;
				default:
					return false;
			}
		});
		return b;
	}

	// --- renderer ----------------------------------------------------------
	private class Renderer implements GLSurfaceView.Renderer {
		@Override
		public void onSurfaceCreated(GL10 gl, EGLConfig config) {
			Log.i(TAG, "GL surface created");
		}

		@Override
		public void onSurfaceChanged(GL10 gl, int width, int height) {
			Log.i(TAG, "GL surface " + width + "x" + height);
			if (gl != null) gl.glViewport(0, 0, width, height);
		}

		@Override
		public void onDrawFrame(GL10 gl) {
			if (!running.get()) {
				if (gl != null) {
					gl.glClearColor(0f, 0f, 0f, 1f);
					gl.glClear(GL10.GL_COLOR_BUFFER_BIT);
				}
				return;
			}
			int status;
			try {
				status = SphynxPlayer.nativeFrame(surface.getWidth(), surface.getHeight());
			} catch (Throwable t) {
				Log.e(TAG, "frame failed", t);
				running.set(false);
				return;
			}
			if (status == 1) {
				running.set(false);
				runOnUiThread(() -> fail("The emulator stopped.\n"
						+ "This system may be too heavy for this device, or it needs a BIOS file.\n"
						+ "BIOS: " + String.valueOf(SphynxPlayer.nativeBiosStatus())
						+ "\n\n" + String.valueOf(SphynxPlayer.nativeStatus())));
				return;
			}
			if (overlay != null && (++frameCounter % 60) == 0) {
				overlay.setText(SphynxPlayer.nativeStatus());
			}
		}
	}

	// --- audio -------------------------------------------------------------
	private void startAudio() {
		if (audio != null) return;
		audio = new AudioThread(SphynxPlayer.nativeSampleRate());
		audio.start();
	}

	private void stopAudio() {
		if (audio == null) return;
		audio.shutdown();
		audio = null;
	}

	private static class AudioThread extends Thread {
		private final int rate;
		private volatile boolean alive = true;

		AudioThread(int rate) {
			this.rate = rate > 8000 ? rate : 44100;
			setPriority(Thread.MAX_PRIORITY);
		}

		@Override
		public void run() {
			int minBuf = AudioTrack.getMinBufferSize(rate, AudioFormat.CHANNEL_OUT_STEREO,
					AudioFormat.ENCODING_PCM_16BIT);
			if (minBuf <= 0) minBuf = rate / 10 * 4;
			AudioTrack track;
			try {
				track = new AudioTrack(AudioManager.STREAM_MUSIC, rate,
						AudioFormat.CHANNEL_OUT_STEREO, AudioFormat.ENCODING_PCM_16BIT,
						minBuf * 2, AudioTrack.MODE_STREAM);
			} catch (Throwable t) {
				Log.e(TAG, "AudioTrack could not be created", t);
				return;
			}
			if (track.getState() != AudioTrack.STATE_INITIALIZED) {
				Log.e(TAG, "AudioTrack failed to initialise");
				track.release();
				return;
			}
			track.play();
			short[] buffer = new short[2048];
			while (alive) {
				int n = 0;
				try {
					n = SphynxPlayer.nativeReadAudio(buffer);
				} catch (Throwable t) {
					Log.e(TAG, "audio read failed", t);
					break;
				}
				if (n > 0) {
					track.write(buffer, 0, n);
				} else {
					// Nothing produced yet: feed silence so the track keeps its
					// clock, but sleep a little instead of spinning.
					java.util.Arrays.fill(buffer, (short) 0);
					track.write(buffer, 0, buffer.length / 2);
					try {
						Thread.sleep(3);
					} catch (InterruptedException ignored) {
					}
				}
			}
			try {
				track.stop();
			} catch (IllegalStateException ignored) {
			}
			track.release();
		}

		void shutdown() {
			alive = false;
			try {
				join(250);
			} catch (InterruptedException ignored) {
			}
		}
	}

	// --- lifecycle ---------------------------------------------------------
	@Override
	protected void onPause() {
		super.onPause();
		running.set(false);
		stopAudio();
		if (surface != null) surface.onPause();
		if (sessionReady) SphynxPlayer.nativeReleaseAll();
	}

	@Override
	protected void onResume() {
		super.onResume();
		if (!sessionReady || surface == null) return;
		// Without this the GL thread stays paused forever and the picture
		// never comes back after the app went to the background.
		surface.onResume();
		startAudio();
		running.set(true);
	}

	@Override
	protected void onDestroy() {
		running.set(false);
		sessionReady = false;
		stopAudio();
		if (surface != null) surface.onPause();
		SphynxPlayer.nativeStop();
		super.onDestroy();
	}

	@Override
	public void onBackPressed() {
		finish();
	}
}
