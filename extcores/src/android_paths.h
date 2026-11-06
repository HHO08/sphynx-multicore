// ============================================================================
//  Sphynx — extcores/src/android_paths.h
//
//  Turning an Android document URI into a file the emulator cores can open.
//
//  Why this exists
//  ---------------
//  On modern Android, PPSSPP browses the game folder through the Storage Access
//  Framework, so `Path::ToString()` hands us a URI like
//
//    content://com.android.externalstorage.documents/tree/primary%3APSP/document/primary%3APSP%2Fgame.sfc
//
//  A libretro core, on the other hand, does `fopen(path)` — it knows nothing
//  about URIs. Shipping the URI straight through is exactly the bug that made
//  every non-PSP game fail to start, so every path is passed through here first.
//
//  Resolution order:
//    1. plain filesystem path            -> used as is
//    2. document id that maps to a file  -> /storage/emulated/0/...   (no copy)
//    3. anything else                    -> copy into the app cache (Java side)
//
//  Step 3 is the safety net for providers that expose no file at all (Downloads,
//  Google Drive, …). It costs one copy of the ROM, once.
//
//  SPDX-License-Identifier: GPL-2.0-or-later
// ============================================================================
#pragma once

#include <string>

namespace sphynx {

// Maps a document id ("primary:PSP/game.sfc", "raw:/sdcard/x.sfc",
// "12D4-3F2A:roms/x.sfc") to a filesystem path. Pure string work: usable in
// host tests, and the storage root can be overridden with SPHYNX_STORAGE_ROOT.
bool MapDocumentIdToPath(const std::string &docid, std::string *out);

// Extracts the document id out of a content:// URI.
bool DocumentIdFromUri(const std::string &uri, std::string *docid);

// True when the string is an Android content:// URI.
bool IsContentUri(const std::string &path);

// The user-visible file name ("Ys III (USA).sfc") for a path or URI.
std::string DisplayNameOf(const std::string &pathOrUri);

// Best-effort local path. Empty when nothing can be opened.
// `allow_copy` = false keeps the call free of side effects: a URI that does not
// map straight onto a file simply fails instead of being copied into the cache.
// The game list uses that mode (thousands of entries must not copy anything);
// the player and the thumbnailer pass true.
std::string LocalPathFor(const std::string &pathOrUri, bool allow_copy = true);

// --- Android only ----------------------------------------------------------
// Remembers the process JavaVM so native threads can call back into Java.
void CaptureJavaVM(void *jni_env);
// Copies a content:// URI into the app cache via ContentResolver.
// Returns false when Java could not produce a local file.
bool CopyContentUriToCache(const std::string &uri, std::string *out);

}  // namespace sphynx
