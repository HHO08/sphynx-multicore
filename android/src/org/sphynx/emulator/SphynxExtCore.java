// ============================================================================
//  Sphynx — org.sphynx.emulator.SphynxExtCore
//
//  Java side of the external-core host. Inert by design: if the library is not
//  present every call degrades to a safe value, so the app behaves exactly like
//  upstream PPSSPP.
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

	/** Called once at startup with the APK's native library dir and data dir. */
	public static void setPaths(String nativeLibraryDir, String appDataDir) {
		if (isAvailable()) nativeSetPaths(nativeLibraryDir, appDataDir);
	}

	public static String buildId() { return isAvailable() ? nativeBuildId() : "unavailable"; }

	public static String selfCheck() { return isAvailable() ? nativeSelfCheck() : "unavailable"; }

	public static int systemCount() { return isAvailable() ? nativeSystemCount() : 0; }

	/** "psp", a system key such as "gb", or "" when the user must choose. */
	public static String detect(String path, byte[] head) {
		return isAvailable() ? nativeDetect(path, head) : "psp";
	}

	/** "key|Display name;..." for files that several systems could claim. */
	public static String candidates(String path) {
		return isAvailable() ? nativeCandidates(path) : "";
	}

	public static String corePath(String systemKey, String nativeLibraryDir) {
		return isAvailable() ? nativeCorePath(systemKey, nativeLibraryDir) : "";
	}

	public static int bundledCoreCount() { return isAvailable() ? nativeBundledCoreCount() : 0; }

	public static String bundledCores() { return isAvailable() ? nativeBundledCores() : ""; }

	public static String start(String corePath, String contentPath, String systemDir, String saveDir) {
		return isAvailable() ? nativeStart(corePath, contentPath, systemDir, saveDir) : "unavailable";
	}

	public static boolean runFrame() { return isAvailable() && nativeRunFrame(); }
	public static void stop() { if (isAvailable()) nativeStop(); }

	private static native void nativeSetPaths(String nativeLibraryDir, String appDataDir);
	private static native String nativeBuildId();
	private static native String nativeSelfCheck();
	private static native int nativeSystemCount();
	private static native String nativeDetect(String path, byte[] head);
	private static native String nativeCandidates(String path);
	private static native String nativeCorePath(String systemKey, String nativeLibraryDir);
	private static native int nativeBundledCoreCount();
	private static native String nativeBundledCores();
	private static native String nativeStart(String corePath, String contentPath, String systemDir, String saveDir);
	private static native boolean nativeRunFrame();
	private static native void nativeStop();
}
