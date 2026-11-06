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
//  SPDX-License-Identifier: GPL-2.0-or-later
// ============================================================================
package org.sphynx.emulator;

import android.app.Activity;
import android.graphics.Color;
import android.media.AudioFormat;
import android.media.AudioManager;
import android.media.AudioTrack;
import android.opengl.GLSurfaceView;
import android.os.Bundle;
import android.util.Log;
import android.view.Gravity;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewGroup;
import android.view.ViewGroup.LayoutParams;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.TextView;

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

	private GLSurfaceView surface;
	private AudioThread audio;
	private TextView overlay;
	private final AtomicBoolean running = new AtomicBoolean(false);
	private int frameCounter;

	/** Set once the native session exists, so onResume knows to restart audio. */
	private boolean sessionReady;

	@Override
	protected void onCreate(Bundle savedInstanceState) {
		super.onCreate(savedInstanceState);
		getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
		SphynxExtCore.attachContext(this);
		if (!SphynxExtCore.isAvailable()) {
			fail("The emulator engine is missing from this build (libsphynx_extcore.so).");
			return;
		}

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

		String systemKey = detectSystem(path);
		if (systemKey == null || systemKey.isEmpty()) {
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
		startWithSystem(path, systemKey);
	}

	private void startWithSystem(String path, String systemKey) {
		String corePath = SphynxExtCore.corePath(systemKey, getApplicationInfo().nativeLibraryDir);
		if (corePath == null || corePath.isEmpty()) {
			fail("The emulator for this system is not in this build.\n(" + systemKey + ")");
			return;
		}
		File core = new File(corePath);
		if (!core.isFile()) {
			fail("The emulator file is missing:\n" + corePath);
			return;
		}
		launch(path, corePath, systemKey);
	}

	private void launch(String path, String corePath, String systemKey) {
		// Same folders the rest of the app uses: BIOS is looked up in PPSSPP's
		// own PSP/SYSTEM when it exists, saves stay app-private.
		File sysDir = new File(SphynxExtCore.systemDir(this));
		File saveDir = new File(SphynxExtCore.saveDir(this));

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
			StringBuilder message = new StringBuilder("This game could not be started:\n");
			message.append(err).append("\n\nSystem: ").append(systemKey);
			String bios = SphynxExtCore.biosHint(systemKey);
			if (bios != null && !bios.isEmpty()) {
				message.append("\nThis system needs a BIOS file: ").append(bios);
			}
			message.append("\n\nBIOS folder:\n").append(sysDir.getAbsolutePath());
			fail(message.toString());
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

		root.addView(buildControls(), new FrameLayout.LayoutParams(
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

		setContentView(root);
		startAudio();
		running.set(true);
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
		new android.app.AlertDialog.Builder(this)
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
						+ "This system may be too heavy for this device, or it needs a BIOS file.\n\n"
						+ String.valueOf(SphynxPlayer.nativeStatus())));
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
