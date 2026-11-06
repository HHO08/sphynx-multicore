// ============================================================================
//  Sphynx — playback screen for every non-PSP system.
//
//  Own GLSurfaceView + own EGL context + own AudioTrack: nothing here can
//  disturb PPSSPP's PSP renderer, which keeps running (paused) underneath in
//  its own process state.
//
//  Touch layout: a D-pad on the left, A/B/X/Y on the right, START/SELECT in the
//  middle, and a back button — the same physical arrangement as PPSSPP's own
//  on-screen pad so muscle memory carries over.
//
//  SPDX-License-Identifier: GPL-2.0-or-later
// ============================================================================
package org.sphynx.emulator;

import android.app.Activity;
import android.content.Intent;
import android.graphics.Color;
import android.media.AudioFormat;
import android.media.AudioManager;
import android.media.AudioTrack;
import android.opengl.GLSurfaceView;
import android.os.Build;
import android.os.Bundle;
import android.util.Log;
import android.view.Gravity;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowManager;
import android.widget.Button;
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

	private GLSurfaceView surface;
	private AudioThread audio;
	private TextView overlay;
	private final AtomicBoolean running = new AtomicBoolean(false);

	static {
		System.loadLibrary("sphynx_extcore");
	}

	@Override
	protected void onCreate(Bundle savedInstanceState) {
		super.onCreate(savedInstanceState);
		getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);

		String path = getIntent() != null ? getIntent().getStringExtra(EXTRA_PATH) : null;
		if (path == null) {
			finish();
			return;
		}

		String systemKey = detectSystem(path);
		if (systemKey == null || systemKey.isEmpty()) {
			// Shared extensions (.cue / .chd / .iso) can belong to several
			// systems. Ask exactly once, then play.
			final String[] choices = candidateSystems(path);
			if (choices.length > 0) {
				askSystem(choices, path);
				return;
			}
			Toast.makeText(this, getString(org.sphynx.emulator.R.string.sphynxUnknownContent),
					Toast.LENGTH_LONG).show();
			finish();
			return;
		}
		String nativeDir = getApplicationInfo().nativeLibraryDir;
		String corePath = SphynxExtCore.corePath(systemKey, nativeDir);
		if (corePath == null || corePath.isEmpty()) {
			Toast.makeText(this, getString(org.sphynx.emulator.R.string.sphynxCoreMissing),
					Toast.LENGTH_LONG).show();
			finish();
			return;
		}
		launch(path, corePath);
	}

	private void launch(String path, String corePath) {
		File sysDir = new File(getFilesDir(), "sphynx/system");
		File saveDir = new File(getFilesDir(), "sphynx/save");
		//noinspection ResultOfMethodCallIgnored
		sysDir.mkdirs();
		//noinspection ResultOfMethodCallIgnored
		saveDir.mkdirs();

		String err = SphynxPlayer.nativeStart(corePath, path, sysDir.getAbsolutePath(),
				saveDir.getAbsolutePath());
		if (err != null && !err.isEmpty()) {
			Log.e(TAG, "start failed: " + err);
			Toast.makeText(this, getString(org.sphynx.emulator.R.string.sphynxStartFailed) + " " + err,
					Toast.LENGTH_LONG).show();
			finish();
			return;
		}
		running.set(true);

		FrameLayout root = new FrameLayout(this);
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
		overlay.setTextSize(11f);
		overlay.setAlpha(0.55f);
		FrameLayout.LayoutParams op = new FrameLayout.LayoutParams(
				ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT);
		op.gravity = Gravity.TOP | Gravity.START;
		op.setMargins(12, 10, 0, 0);
		root.addView(overlay, op);

		setContentView(root);
		audio = new AudioThread(SphynxPlayer.nativeSampleRate());
		audio.start();
	}

	/** "key|Display name;..." -> {"key\u0000Display name", ...} */
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
				.setTitle(getString(org.sphynx.emulator.R.string.sphynxChooseSystem))
				.setItems(labels, (dialog, which) -> {
					String key = choices[which];
					int bar = key.indexOf('|');
					if (bar >= 0) key = key.substring(0, bar);
					startWithSystem(path, key);
				})
				.setOnCancelListener(dialog -> finish())
				.show();
	}

	private void startWithSystem(String path, String systemKey) {
		String corePath = SphynxExtCore.corePath(systemKey, getApplicationInfo().nativeLibraryDir);
		if (corePath == null || corePath.isEmpty()) {
			Toast.makeText(this, getString(org.sphynx.emulator.R.string.sphynxCoreMissing),
					Toast.LENGTH_LONG).show();
			finish();
			return;
		}
		launch(path, corePath);
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
		dl.setMargins(dp(16), 0, 0, dp(16));
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
		fl.setMargins(0, 0, dp(16), dp(16));
		layer.addView(face, fl);

		LinearLayout middle = new LinearLayout(this);
		middle.setOrientation(LinearLayout.HORIZONTAL);
		middle.addView(padButton("SELECT", SELECT));
		middle.addView(padButton("START", START));
		Button quit = new Button(this);
		quit.setText("✕");
		quit.setOnClickListener(v -> finish());
		middle.addView(quit);
		FrameLayout.LayoutParams ml = new FrameLayout.LayoutParams(dp(LayoutParams.WRAP_CONTENT),
				dp(LayoutParams.WRAP_CONTENT));
		ml.gravity = Gravity.BOTTOM | Gravity.CENTER_HORIZONTAL;
		ml.setMargins(0, 0, 0, dp(16));
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
		}

		@Override
		public void onDrawFrame(GL10 gl) {
			if (!running.get()) return;
			int status = SphynxPlayer.nativeFrame(surface.getWidth(), surface.getHeight());
			if (status == 1) {
				// Core stopped (watchdog, crash of the core code, or user reset).
				running.set(false);
				runOnUiThread(() -> {
					Toast.makeText(SphynxPlayerActivity.this,
							getString(org.sphynx.emulator.R.string.sphynxCoreStopped),
							Toast.LENGTH_LONG).show();
					finish();
				});
			}
			if (overlay != null && (System.nanoTime() % 60) == 0) {
				overlay.setText(SphynxPlayer.nativeStatus());
			}
		}
	}

	// --- audio -------------------------------------------------------------
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
			if (minBuf <= 0) minBuf = rate / 10;
			AudioTrack track = new AudioTrack(AudioManager.STREAM_MUSIC, rate,
					AudioFormat.CHANNEL_OUT_STEREO, AudioFormat.ENCODING_PCM_16BIT,
					minBuf * 2, AudioTrack.MODE_STREAM);
			if (track.getState() != AudioTrack.STATE_INITIALIZED) {
				Log.e(TAG, "AudioTrack failed to initialise");
				return;
			}
			track.play();
			short[] buffer = new short[1024];
			long silence = 0;
			while (alive) {
				int n = SphynxPlayer.nativeReadAudio(buffer);
				if (n > 0) {
					track.write(buffer, 0, n);
					silence = 0;
				} else {
					// Keep the pipeline fed so the first sound is not delayed.
					java.util.Arrays.fill(buffer, (short) 0);
					track.write(buffer, 0, buffer.length);
					if (++silence > 400) {
						try {
							Thread.sleep(4);
						} catch (InterruptedException ignored) {
						}
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
		}
	}

	@Override
	protected void onPause() {
		super.onPause();
		running.set(false);
		if (surface != null) surface.onPause();
		SphynxPlayer.nativeReleaseAll();
	}

	@Override
	protected void onDestroy() {
		running.set(false);
		if (audio != null) {
			audio.shutdown();
			audio = null;
		}
		if (surface != null) surface.onPause();
		SphynxPlayer.nativeStop();
		super.onDestroy();
	}

	@Override
	public void onBackPressed() {
		finish();
	}
}
