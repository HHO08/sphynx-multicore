// ============================================================================
//  Sphynx — android/src/org/sphynx/emulator/SphynxExtCore.java
//
//  Java-side entry point for the external-core host (GB, NES, SNES, …).
//
//  It is intentionally inert: the library is loaded lazily and every call
//  degrades to a safe value when it is absent, so a build without extcores
//  behaves exactly like stock PPSSPP.
//
//  SPDX-License-Identifier: GPL-2.0-or-later
// ============================================================================
package org.sphynx.emulator;

import android.util.Log;

public final class SphynxExtCore {
	private static final String TAG = "SphynxExtCore";
	private static boolean sTried;
	private static boolean sLoaded;

	private SphynxExtCore() {}

	public static synchronized boolean isAvailable() {
		if (!sTried) {
			sTried = true;
			try {
				System.loadLibrary("sphynx_extcore");
				sLoaded = true;
			} catch (UnsatisfiedLinkError e) {
				Log.i(TAG, "external-core host not present in this build");
				sLoaded = false;
			}
		}
		return sLoaded;
	}

	public static String buildId() { return isAvailable() ? nativeBuildId() : "unavailable"; }

	/** "" when the registry is consistent, otherwise the failure report. */
	public static String selfCheck() { return isAvailable() ? nativeSelfCheck() : "unavailable"; }

	public static int systemCount() { return isAvailable() ? nativeSystemCount() : 0; }

	/**
	 * Detects the system for a file.
	 * @return "psp" (PPSSPP's own core), a system key such as "gb", or "" when
	 *         the user must be asked.
	 */
	public static String detect(String path, byte[] head) {
		return isAvailable() ? nativeDetect(path, head) : "psp";
	}

	/** Starts a core; returns "" on success or a human-readable error. */
	public static String start(String corePath, String contentPath, String systemDir, String saveDir) {
		return isAvailable() ? nativeStart(corePath, contentPath, systemDir, saveDir) : "unavailable";
	}

	public static boolean runFrame() { return isAvailable() && nativeRunFrame(); }
	public static void stop() { if (isAvailable()) nativeStop(); }

	private static native String nativeBuildId();
	private static native String nativeSelfCheck();
	private static native int nativeSystemCount();
	private static native String nativeDetect(String path, byte[] head);
	private static native String nativeStart(String corePath, String contentPath, String systemDir, String saveDir);
	private static native boolean nativeRunFrame();
	private static native void nativeStop();
}
