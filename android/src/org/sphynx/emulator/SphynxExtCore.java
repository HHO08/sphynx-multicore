// ============================================================================
//  Sphynx — org.sphynx.emulator.SphynxExtCore
//
//  Java side of the external-core host.
//
//  Three jobs, all defensive:
//    1. Native bridge. If the library is missing, every call degrades to a safe
//       value so the app behaves exactly like upstream PPSSPP.
//    2. Resolving Android document URIs. On modern Android PPSSPP browses the
//       game folder through the Storage Access Framework, so what the native
//       side receives is a content:// URI, not a file path. Emulator cores use
//       fopen(), so a URI is turned into a real file first.
//    3. The BIOS tree: PSP/SYSTEMS_BIOS, one folder per system that uses BIOS.
//       Every core is handed its own folder as the libretro system directory,
//       so the user can drop files in, swap them or delete them at any time and
//       the next launch reads whatever is there.
//
//  SPDX-License-Identifier: GPL-2.0-or-later
// ============================================================================
package org.sphynx.emulator;

import android.content.ContentResolver;
import android.content.Context;
import android.database.Cursor;
import android.net.Uri;
import android.os.Environment;
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

	// ------------------------------------------------------------------------
	//  Folders
	// ------------------------------------------------------------------------

	/**
	 * PPSSPP's own memory-stick folder (".../PSP"). The native layer learns the
	 * real one from PPSSPP at startup, so this always matches what PPSSPP uses
	 * — including a memstick folder the user moved to the SD card.
	 */
	public static String memstickDir(Context context) {
		// 1. Authoritative: the native layer learned it from PPSSPP itself.
		String root = isAvailable() ? nativeBiosRoot() : "";
		if (root != null && !root.isEmpty()) {
			File parent = new File(root).getParentFile();
			if (parent != null) return parent.getAbsolutePath();
		}
		// 2. Before the native layer knows: look for the folder PPSSPP already
		//    uses. Both layouts exist in the wild ("PSP" next to the storage
		//    root, and "Android/media/PSP" on newer setups).
		File external = Environment.getExternalStorageDirectory();
		File[] candidates = {
				new File(external, "PSP"),
				new File(external, "Android/media/PSP"),
				new File(context.getExternalFilesDir(null), "PSP"),
		};
		for (File candidate : candidates) {
			if (candidate == null) continue;
			if (new File(candidate, "GAME").isDirectory() || new File(candidate, "SYSTEM").isDirectory()) {
				return candidate.getAbsolutePath();
			}
		}
		// 3. Nothing found: use the standard location and create it.
		File psp = new File(external, "PSP");
		if (!psp.isDirectory()) {
			//noinspection ResultOfMethodCallIgnored
			psp.mkdirs();
		}
		if (psp.isDirectory()) return psp.getAbsolutePath();
		File fallback = new File(context.getFilesDir(), "PSP");
		//noinspection ResultOfMethodCallIgnored
		fallback.mkdirs();
		return fallback.getAbsolutePath();
	}

	/** Kept for compatibility: PPSSPP's traditional BIOS folder. */
	public static String systemDir(Context context) {
		File dir = new File(memstickDir(context), "SYSTEM");
		//noinspection ResultOfMethodCallIgnored
		dir.mkdirs();
		return dir.getAbsolutePath();
	}

	/** Saves and battery RAM. App-private: always writable, never needs a grant. */
	public static String saveDir(Context context) {
		File dir = new File(context.getFilesDir(), "sphynx/save");
		//noinspection ResultOfMethodCallIgnored
		dir.mkdirs();
		return dir.getAbsolutePath();
	}

	// ------------------------------------------------------------------------
	//  BIOS tree: PSP/SYSTEMS_BIOS
	// ------------------------------------------------------------------------

	/**
	 * Creates PSP/SYSTEMS_BIOS with one folder per system that needs a BIOS
	 * (the 12 mandatory ones and the 4 recommended ones), writes a README in
	 * each, and copies any BIOS the user already keeps in PSP/SYSTEM into the
	 * matching folder. Safe to call on every start: it never overwrites a file.
	 */
	public static void ensureBiosTree(Context context) {
		if (!isAvailable()) return;
		String memstick = memstickDir(context);
		nativeSetMemstick(memstick);
		int made = nativeEnsureBiosTree(memstick);
		Log.i(TAG, "BIOS tree at " + nativeBiosRoot() + " (" + made + " new folders)");
	}

	public static String biosRoot() {
		return isAvailable() ? nativeBiosRoot() : "";
	}

	/** Folder of one system, or "" when that system needs no BIOS at all. */
	public static String biosDir(String systemKey) {
		return isAvailable() ? nativeBiosDir(systemKey) : "";
	}

	/**
	 * The libretro "system directory" for a system: its own BIOS folder when it
	 * has one, PPSSPP's PSP/SYSTEM otherwise. Every core looks for its BIOS
	 * exactly here.
	 */
	public static String systemDirForSystem(Context context, String systemKey) {
		String dir = biosDir(systemKey);
		if (dir != null && !dir.isEmpty()) return dir;
		return systemDir(context);
	}

	/** Expected BIOS files that are missing ("" = complete or none needed). */
	public static String missingBios(String systemKey) {
		return isAvailable() ? nativeBiosMissing(systemKey) : "";
	}

	/** "complete" / "missing x, y" / "no BIOS needed". */
	public static String biosStatus(String systemKey) {
		return isAvailable() ? nativeBiosStatus(systemKey) : "";
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

	/** "key|Display name;...", for files that several systems could claim. */
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
	private static native void nativeSetMemstick(String memstickDir);
	private static native int nativeEnsureBiosTree(String memstickDir);
	private static native String nativeBiosRoot();
	private static native String nativeBiosDir(String systemKey);
	private static native String nativeBiosMissing(String systemKey);
	private static native String nativeBiosStatus(String systemKey);
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
