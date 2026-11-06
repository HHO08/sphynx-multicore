// ============================================================================
//  Sphynx — extcores/src/android_paths.cpp
//
//  URI -> file resolution for Android. See android_paths.h for why.
//
//  This file deliberately contains no Android API calls except the optional JNI
//  fallback at the bottom, so the string mapping below is covered by the host
//  test suite (no device needed).
//
//  SPDX-License-Identifier: GPL-2.0-or-later
// ============================================================================
#include "android_paths.h"

#include <cstdlib>
#include <cstring>

#include <unistd.h>

#ifdef SPHYNX_ANDROID
#include <android/log.h>
#include <jni.h>
#define ALOG(...) __android_log_print(ANDROID_LOG_INFO, "SphynxPaths", __VA_ARGS__)
#else
#define ALOG(...) do { } while (0)
#endif

namespace sphynx {
namespace {

int HexVal(char c) {
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

std::string PercentDecode(const std::string &in) {
	std::string out;
	out.reserve(in.size());
	for (size_t i = 0; i < in.size(); ++i) {
		if (in[i] == '%' && i + 2 < in.size()) {
			const int hi = HexVal(in[i + 1]);
			const int lo = HexVal(in[i + 2]);
			if (hi >= 0 && lo >= 0) {
				out.push_back(char((hi << 4) | lo));
				i += 2;
				continue;
			}
		}
		out.push_back(in[i]);
	}
	return out;
}

bool Readable(const std::string &path) {
	return !path.empty() && ::access(path.c_str(), R_OK) == 0;
}

// "/storage" on a device; overridable so the host test can use a temp tree.
std::string StorageRoot() {
	const char *env = std::getenv("SPHYNX_STORAGE_ROOT");
	if (env && *env) return env;
	return "/storage";
}

bool EndsWith(const std::string &s, const char *suffix) {
	const size_t n = std::strlen(suffix);
	return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

// Some providers append "/children" to a document URI. The id itself contains
// slashes (primary:PSP/GAME/game.sfc), so only that exact suffix is removed —
// truncating at the first slash would throw the whole path away.
void StripChildrenSuffix(std::string *id) {
	if (EndsWith(*id, "/children")) id->resize(id->size() - std::strlen("/children"));
	while (!id->empty() && id->back() == '/') id->pop_back();
}

}  // namespace

bool IsContentUri(const std::string &path) {
	return path.compare(0, 10, "content://") == 0;
}

bool DocumentIdFromUri(const std::string &uri, std::string *docid) {
	if (!docid) return false;
	docid->clear();
	if (!IsContentUri(uri)) return false;

	// Prefer the document component: "<provider>/tree/<root>/document/<id>".
	size_t at = uri.rfind("/document/");
	if (at != std::string::npos) {
		*docid = PercentDecode(uri.substr(at + std::strlen("/document/")));
		StripChildrenSuffix(docid);
		return !docid->empty();
	}

	// Tree-only URI: the tree root itself (a folder). Not a game file, but map
	// it anyway so callers can tell "unresolvable" from "unknown".
	at = uri.rfind("/tree/");
	if (at != std::string::npos) {
		*docid = PercentDecode(uri.substr(at + std::strlen("/tree/")));
		StripChildrenSuffix(docid);
		return !docid->empty();
	}
	return false;
}

bool MapDocumentIdToPath(const std::string &rawdocid, std::string *out) {
	if (!out) return false;
	out->clear();
	if (rawdocid.empty()) return false;

	std::string docid = PercentDecode(rawdocid);

	// "raw:/storage/emulated/0/x.sfc" — some providers hand out full paths.
	if (docid.compare(0, 4, "raw:") == 0) {
		*out = docid.substr(4);
		return true;
	}
	if (docid[0] == '/') {   // already a path
		*out = docid;
		return true;
	}

	const size_t colon = docid.find(':');
	if (colon == std::string::npos) return false;   // "msf:1234", "12", ...

	std::string volume = docid.substr(0, colon);
	std::string rest = docid.substr(colon + 1);
	while (!rest.empty() && rest[0] == '/') rest.erase(0, 1);

	const std::string root = StorageRoot();
	if (volume == "primary") {
		*out = root + "/emulated/0/" + rest;
		return true;
	}
	// "home", "download", "msf", "media" and friends are not mountable paths.
	if (volume == "home" || volume == "download" || volume == "downloads" ||
	    volume == "msf" || volume == "media" || volume == "raw") {
		return false;
	}
	// Anything else is an SD-card UUID such as "12D4-3F2A".
	*out = root + "/" + volume + "/" + rest;
	return true;
}

std::string DisplayNameOf(const std::string &pathOrUri) {
	std::string text = pathOrUri;
	if (IsContentUri(text)) {
		const size_t slash = text.find_last_of('/');
		if (slash != std::string::npos) text = text.substr(slash + 1);
		text = PercentDecode(text);
	}
	// Drop any trailing slashes so "dir/" keeps only the directory name.
	while (text.size() > 1 && text.back() == '/') text.pop_back();
	const size_t slash = text.find_last_of('/');
	if (slash != std::string::npos && slash + 1 < text.size()) text = text.substr(slash + 1);
	return text;
}

std::string LocalPathFor(const std::string &pathOrUri, bool allow_copy) {
	if (pathOrUri.empty()) return std::string();

	if (IsContentUri(pathOrUri)) {
		std::string docid, mapped;
		if (DocumentIdFromUri(pathOrUri, &docid) && MapDocumentIdToPath(docid, &mapped) &&
		    Readable(mapped)) {
			ALOG("mapped %s -> %s", pathOrUri.c_str(), mapped.c_str());
			return mapped;
		}
		// The direct mapping failed: ask Java to hand us a copy (unless the
		// caller only wanted a cheap answer).
		if (!allow_copy) return std::string();
		std::string copied;
		if (CopyContentUriToCache(pathOrUri, &copied) && Readable(copied)) {
			ALOG("copied %s -> %s", pathOrUri.c_str(), copied.c_str());
			return copied;
		}
		ALOG("could not resolve %s", pathOrUri.c_str());
		return std::string();
	}

	// A bare document id can reach us when a caller stripped the scheme.
	if (pathOrUri.find("://") == std::string::npos && pathOrUri.find(':') != std::string::npos) {
		std::string mapped;
		if (MapDocumentIdToPath(pathOrUri, &mapped) && Readable(mapped)) return mapped;
	}
	return pathOrUri;
}

// ---------------------------------------------------------------------------
//  Java bridge (Android only)
// ---------------------------------------------------------------------------
#ifdef SPHYNX_ANDROID
namespace {
JavaVM *g_vm = nullptr;
jclass g_core_class = nullptr;   // global ref to org/sphynx/emulator/SphynxExtCore
}  // namespace

void CaptureJavaVM(void *jni_env) {
	JNIEnv *env = static_cast<JNIEnv *>(jni_env);
	if (!env) return;
	if (!g_vm) env->GetJavaVM(&g_vm);
	if (!g_core_class) {
		jclass local = env->FindClass("org/sphynx/emulator/SphynxExtCore");
		if (local) g_core_class = static_cast<jclass>(env->NewGlobalRef(local));
	}
}

bool CopyContentUriToCache(const std::string &uri, std::string *out) {
	if (!out) return false;
	out->clear();
	if (!g_vm) return false;   // nativeSetPaths() has not run yet

	JNIEnv *env = nullptr;
	bool attached = false;
	// The two JNI flavours disagree on AttachCurrentThread's first parameter
	// (JNIEnv** on Android, void** on the desktop JDK). Writing it through a
	// void* keeps one source honest for both.
	void *penv = nullptr;
	if (g_vm->GetEnv(&penv, JNI_VERSION_1_6) == JNI_OK) {
		env = static_cast<JNIEnv *>(penv);
	} else {
#if defined(__ANDROID__)
		if (g_vm->AttachCurrentThread(&env, nullptr) != JNI_OK) return false;
#else
		void *attached_env = nullptr;
		if (g_vm->AttachCurrentThread(&attached_env, nullptr) != JNI_OK) return false;
		env = static_cast<JNIEnv *>(attached_env);
#endif
		attached = true;
	}

	bool ok = false;
	if (g_core_class && env) {
		jmethodID mid = env->GetStaticMethodID(
			g_core_class, "copyContentToCache", "(Ljava/lang/String;)Ljava/lang/String;");
		if (mid) {
			jstring jpath = env->NewStringUTF(uri.c_str());
			jstring jres = static_cast<jstring>(
				env->CallStaticObjectMethod(g_core_class, mid, jpath));
			if (!env->ExceptionCheck() && jres) {
				const char *c = env->GetStringUTFChars(jres, nullptr);
				if (c) {
					*out = c;
					env->ReleaseStringUTFChars(jres, c);
					ok = !out->empty();
				}
			}
			if (env->ExceptionCheck()) env->ExceptionClear();
			if (jres) env->DeleteLocalRef(jres);
			if (jpath) env->DeleteLocalRef(jpath);
		}
	}
	if (attached) g_vm->DetachCurrentThread();
	return ok;
}
#else
void CaptureJavaVM(void *) {}
bool CopyContentUriToCache(const std::string &, std::string *) { return false; }
#endif

}  // namespace sphynx
