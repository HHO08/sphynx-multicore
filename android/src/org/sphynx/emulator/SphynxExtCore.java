// ============================================================================
//  Sphynx — org.sphynx.emulator.SphynxExtCore
//
//  Java side of the external-core host.
//
//  Two jobs, both defensive:
//    1. Native bridge. If the library is missing, every call degrades to a safe
//       value so the app behaves exactly like upstream PPSSPP.
//    2. Resolving Android document URIs. On modern Android PPSSPP browses the
//       game folder through the Storage Access Framework, so what the native
//       side receives is a content:// URI, not a file path. Emulator cores use
//       fopen(), so a URI copy is made into the app cache the first time it is
//       needed. This is the fix for "every game fails to start".
//
//  SPDX-License-Identifier: GPL-2.0-or-later
// ============================================================================
package org.sphynx.emulator;

import android.content.ContentResolver;
import android.content.Context;
import android.database.Cursor;
import android.net.Uri;
import android.provider.OpenableColumns;
import android.util.Log;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;

public final class SphynxExtCore {
	private static final String TAG = "SphynxExtCore";
	private static boolean sTried;
	private static boolean sLoaded;

	// Set once from the app's context; needed by the URI resolver below.
	private static Context sContext;

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

	/** Called once at startup: where the cores live, where BIOS/saves belong. */
	public static void setPaths(String nativeLibraryDir, String systemDir, String saveDir) {
		if (isAvailable()) nativeSetPaths(nativeLibraryDir, systemDir, saveDir);
	}

	/**
	 * Where the cores look for BIOS files. PPSSPP's own PSP/SYSTEM folder wins
	 * when it exists, so a user who already keeps BIOS files for PPSSPP finds
	 * them here too instead of being told to copy them again.
	 */
	public static String systemDir(Context context) {
		File psp = new File(android.os.Environment.getExternalStorageDirectory(), "PSP/SYSTEM");
		if (psp.isDirectory()) return psp.getAbsolutePath();
		File fallback = new File(context.getFilesDir(), "sphynx/system");
		//noinspection ResultOfMethodCallIgnored
		fallback.mkdirs();
		return fallback.getAbsolutePath();
	}

	/** Saves and battery RAM. App-private: always writable, never needs a grant. */
	public static String saveDir(Context context) {
		File dir = new File(context.getFilesDir(), "sphynx/save");
		//noinspection ResultOfMethodCallIgnored
		dir.mkdirs();
		return dir.getAbsolutePath();
	}

	/** "" when the system needs no BIOS, otherwise the file name to look for. */
	public static String biosHint(String systemKey) {
		return isAvailable() ? nativeBiosHint(systemKey) : "";
	}

	/** The app context is only used to copy content:// files into the cache. */
	public static void attachContext(Context context) {
		sContext = context != null ? context.getApplicationContext() : null;
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

	// ------------------------------------------------------------------------
	//  Android storage
	// ------------------------------------------------------------------------

	public static boolean isContentUri(String path) {
		return path != null && path.startsWith("content://");
	}

	/**
	 * Returns a filesystem path an emulator core can open, or null.
	 * A plain path is returned unchanged; a content:// URI is copied once into
	 * the app cache (the file name is kept, the folder is derived from the URI,
	 * so repeated calls reuse the same copy).
	 */
	public static String resolvePath(String pathOrUri) {
		if (pathOrUri == null || pathOrUri.isEmpty()) return null;
		if (!isContentUri(pathOrUri)) return pathOrUri;

		// Native knows the storage layout best (primary:, raw:, SD-card ids).
		final String nativeResult = isAvailable() ? nativeResolvePath(pathOrUri) : "";
		if (nativeResult != null && !nativeResult.isEmpty()) return nativeResult;

		// Fall back to copying through the ContentResolver.
		return copyContentToCache(pathOrUri);
	}

	/**
	 * Copies a content:// URI into the app cache and returns the new path.
	 * Called from Java and from the native thumbnail worker (JNI); therefore it
	 * takes no Context argument and never throws.
	 */
	public static String copyContentToCache(String uriString) {
		Context context = sContext;
		if (context == null || uriString == null || uriString.isEmpty()) return "";
		try {
			final Uri uri = Uri.parse(uriString);
			final ContentResolver resolver = context.getContentResolver();

			String name = queryDisplayName(resolver, uri);
			if (name == null || name.isEmpty()) name = "content.bin";
			// Keep the extension: system detection and the cores both need it.
			name = name.replaceAll("[/\\\\:*?\"<>|]", "_");

			File dir = new File(context.getCacheDir(), "sphynx_roms");
			//noinspection ResultOfMethodCallIgnored
			dir.mkdirs();
			File out = new File(dir, shortHash(uriString) + "_" + name);
			if (out.isFile() && out.length() > 0) return out.getAbsolutePath();

			try (InputStream in = resolver.openInputStream(uri)) {
				if (in == null) return "";
				File tmp = new File(dir, out.getName() + ".part");
				try (FileOutputStream fos = new FileOutputStream(tmp)) {
					byte[] buffer = new byte[64 * 1024];
					int read;
					while ((read = in.read(buffer)) > 0) fos.write(buffer, 0, read);
					fos.flush();
				}
				if (!tmp.renameTo(out)) {
					//noinspection ResultOfMethodCallIgnored
					tmp.delete();
					return "";
				}
			}
			Log.i(TAG, "copied " + uriString + " -> " + out.getAbsolutePath());
			return out.getAbsolutePath();
		} catch (IOException | SecurityException | IllegalArgumentException e) {
			Log.w(TAG, "cannot copy " + uriString, e);
			return "";
		}
	}

	private static String queryDisplayName(ContentResolver resolver, Uri uri) {
		try (Cursor c = resolver.query(uri, null, null, null, null)) {
			if (c != null && c.moveToFirst()) {
				int idx = c.getColumnIndex(OpenableColumns.DISPLAY_NAME);
				if (idx >= 0 && !c.isNull(idx)) return c.getString(idx);
			}
		} catch (Exception ignored) {
			// Provider without a display name: fall back to the last path part.
		}
		String last = uri.getLastPathSegment();
		if (last == null) return null;
		int slash = last.lastIndexOf('%');
		return slash > 0 ? last.substring(0, slash) : last;
	}

	private static String shortHash(String text) {
		try {
			MessageDigest md = MessageDigest.getInstance("SHA-256");
			byte[] digest = md.digest(text.getBytes("UTF-8"));
			StringBuilder sb = new StringBuilder();
			for (int i = 0; i < 6; ++i) sb.append(String.format("%02x", digest[i]));
			return sb.toString();
		} catch (NoSuchAlgorithmException | IOException e) {
			return Integer.toHexString(text.hashCode());
		}
	}

	private static native void nativeSetPaths(String nativeLibraryDir, String systemDir, String saveDir);
	private static native String nativeBiosHint(String systemKey);
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
	private static native String nativeResolvePath(String pathOrUri);
}
