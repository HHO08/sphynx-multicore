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
