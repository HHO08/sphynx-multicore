// ============================================================================
//  Sphynx — org.sphynx.emulator.SphynxPlayer
//
//  Native bridge for the playback screen. All the real work (core lifetime,
//  video, audio ring, input) is in extcores/src/player_android.cpp; this class
//  only names the entry points.
//
//  SPDX-License-Identifier: GPL-2.0-or-later
// ============================================================================
package org.sphynx.emulator;

public final class SphynxPlayer {
	private SphynxPlayer() {}

	/** @return null on success, or a human readable error. */
	public static native String nativeStart(String corePath, String contentPath,
	                                        String systemDir, String saveDir);
	public static native int nativeFrame(int width, int height);
	public static native void nativeStop();
	public static native int nativeSampleRate();
	public static native int nativeReadAudio(short[] buffer);
	public static native void nativeSetButton(int retropadId, boolean down);
	public static native void nativeReleaseAll();
	public static native String nativeStatus();
}
