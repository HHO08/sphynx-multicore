// ============================================================================
//  Sphynx — extcores/jni/sphynx_extcore_jni.cpp
//
//  JNI surface of the external-core host. Deliberately tiny: the Java side of
//  PPSSPP (NativeActivity + the Java GL surface) keeps doing what it already
//  does, and only the parts that need to reach an external core come through
//  here.
//
//  Package: org.sphynx.emulator  ->  Java_org_sphynx_emulator_SphynxExtCore_*
//
//  Compiled only for Android (see extcores/CMakeLists.txt).
//
//  SPDX-License-Identifier: GPL-2.0-or-later
// ============================================================================
#include "sphynx_extcore.h"
#include "sphynx_ui_api.h"

#include <sys/stat.h>

#include <cstring>
#include <string>
#include <vector>

#ifdef SPHYNX_ANDROID
#include <android/log.h>
#include <jni.h>

#define SXLOG(...) __android_log_print(ANDROID_LOG_INFO, "SphynxExtCore", __VA_ARGS__)
#else
#define SXLOG(...) ((void)0)
#endif

namespace {

// One session per process: the design puts every external core in its own
// Android process, so a global is both sufficient and the safest choice.
sphynx::Session *g_session = nullptr;

jstring ToJavaString(JNIEnv *env, const std::string &s) {
#ifdef SPHYNX_ANDROID
	return env->NewStringUTF(s.c_str());
#else
	(void)env;
	(void)s;
	return nullptr;
#endif
}

}  // namespace

#ifdef SPHYNX_ANDROID

// Called once at app start: tells the host where the bundled cores live and
// where BIOS/save files belong.
extern "C" JNIEXPORT void JNICALL
Java_org_sphynx_emulator_SphynxExtCore_nativeSetPaths(JNIEnv *env, jclass, jstring jlibdir,
                                                     jstring jdatadir) {
	auto str = [&](jstring s) -> std::string {
		if (!s) return std::string();
		const char *c = env->GetStringUTFChars(s, nullptr);
		std::string out = c ? c : "";
		env->ReleaseStringUTFChars(s, c);
		return out;
	};
	const std::string lib = str(jlibdir);
	const std::string data = str(jdatadir);
	const std::string sys = data + "/sphynx/system";
	const std::string save = data + "/sphynx/save";
	::mkdir(sys.c_str(), 0777);
	::mkdir(save.c_str(), 0777);
	sphynx_ui_set_paths(lib.c_str(), sys.c_str(), save.c_str());
	SXLOG("paths: lib=%s sys=%s save=%s", lib.c_str(), sys.c_str(), save.c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_org_sphynx_emulator_SphynxExtCore_nativeCandidates(JNIEnv *env, jclass, jstring jpath) {
	const char *p = jpath ? env->GetStringUTFChars(jpath, nullptr) : nullptr;
	std::string out = sphynx_ui_candidates(p ? p : "");
	if (p) env->ReleaseStringUTFChars(jpath, p);
	return ToJavaString(env, out);
}

extern "C" JNIEXPORT jstring JNICALL
Java_org_sphynx_emulator_SphynxExtCore_nativeBuildId(JNIEnv *env, jclass) {
	return ToJavaString(env, sphynx::BuildId());
}

// Returns "" when the registry is consistent, otherwise the failure report.
extern "C" JNIEXPORT jstring JNICALL
Java_org_sphynx_emulator_SphynxExtCore_nativeSelfCheck(JNIEnv *env, jclass) {
	std::string report;
	const bool ok = sphynx::SelfCheck(&report);
	return ToJavaString(env, ok ? std::string() : report);
}

extern "C" JNIEXPORT jint JNICALL
Java_org_sphynx_emulator_SphynxExtCore_nativeSystemCount(JNIEnv *, jclass) {
	return sphynx::SystemCount();
}

// Absolute path of the bundled core for a system key, or "" when absent.
extern "C" JNIEXPORT jstring JNICALL
Java_org_sphynx_emulator_SphynxExtCore_nativeCorePath(JNIEnv *env, jclass, jstring jkey,
                                                     jstring jlibdir) {
	auto str = [&](jstring s) -> std::string {
		if (!s) return std::string();
		const char *c = env->GetStringUTFChars(s, nullptr);
		std::string out = c ? c : "";
		env->ReleaseStringUTFChars(s, c);
		return out;
	};
	const std::string key = str(jkey);
	const std::string dir = str(jlibdir);
	std::string path;
	if (key == "psp") return ToJavaString(env, "");   // no external core for PSP
	path = sphynx::CoreLibraryPathForSystem(dir, key.c_str());
	return ToJavaString(env, path);
}

extern "C" JNIEXPORT jint JNICALL
Java_org_sphynx_emulator_SphynxExtCore_nativeBundledCoreCount(JNIEnv *, jclass) {
	return sphynx::BundledCoreCount();
}

// "name:file;name:file;..." — used by the about screen and by CI checks.
extern "C" JNIEXPORT jstring JNICALL
Java_org_sphynx_emulator_SphynxExtCore_nativeBundledCores(JNIEnv *env, jclass) {
	std::string out;
	const int n = sphynx::BundledCoreCount();
	const char *const *libs = sphynx::BundledCoreLibs();
	for (int i = 0; i < n; ++i) {
		out += libs[i];
		out += ':';
		const char *file = sphynx::CoreLibraryFile(libs[i]);
		out += file ? file : "?";
		if (i + 1 < n) out += ';';
	}
	return ToJavaString(env, out);
}

// Detects the system for `path`. `head` is an optional 4 KiB prefix.
// Returns the best system key, or "" when the user must be asked.
extern "C" JNIEXPORT jstring JNICALL
Java_org_sphynx_emulator_SphynxExtCore_nativeDetect(JNIEnv *env, jclass, jstring jpath,
                                                   jbyteArray jhead) {
	const char *path = jpath ? env->GetStringUTFChars(jpath, nullptr) : nullptr;
	std::vector<uint8_t> head;
	if (jhead) {
		const jsize n = env->GetArrayLength(jhead);
		head.resize(static_cast<size_t>(n));
		env->GetByteArrayRegion(jhead, 0, n, reinterpret_cast<jbyte *>(head.data()));
	}
	const sphynx::DetectResult r = sphynx::Detect(path ? path : "", head.empty() ? nullptr : head.data(), head.size());
	if (path) env->ReleaseStringUTFChars(jpath, path);

	if (r.is_psp_native) return ToJavaString(env, "psp");   // PPSSPP's own core
	if (r.candidates.empty()) return ToJavaString(env, "");
	if (r.needs_user_choice) return ToJavaString(env, "");  // ask the user once
	const sphynx::SystemInfo *info = sphynx::FindSystem(r.candidates.front().id);
	return ToJavaString(env, info ? info->key : "");
}

// Starts a core. Returns "" on success or a human-readable error.
extern "C" JNIEXPORT jstring JNICALL
Java_org_sphynx_emulator_SphynxExtCore_nativeStart(JNIEnv *env, jclass, jstring jcore,
                                                  jstring jcontent, jstring jsys,
                                                  jstring jsave) {
	auto str = [&](jstring s) -> std::string {
		if (!s) return std::string();
		const char *c = env->GetStringUTFChars(s, nullptr);
		std::string out = c ? c : "";
		env->ReleaseStringUTFChars(s, c);
		return out;
	};
	if (g_session) {
		delete g_session;
		g_session = nullptr;
	}
	g_session = new sphynx::Session();
	std::string err;
	const bool ok = g_session->Start(str(jcore), str(jcontent), str(jsys), str(jsave), &err);
	if (!ok) {
		delete g_session;
		g_session = nullptr;
		SXLOG("start failed: %s", err.c_str());
		return ToJavaString(env, err.empty() ? "unknown error" : err);
	}
	SXLOG("session started");
	return ToJavaString(env, "");
}

extern "C" JNIEXPORT jboolean JNICALL
Java_org_sphynx_emulator_SphynxExtCore_nativeRunFrame(JNIEnv *, jclass) {
	if (!g_session) return JNI_FALSE;
	return g_session->RunFrame() ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_org_sphynx_emulator_SphynxExtCore_nativeStop(JNIEnv *, jclass) {
	if (g_session) {
		g_session->Stop();
		delete g_session;
		g_session = nullptr;
	}
}

#endif  // SPHYNX_ANDROID
