// ============================================================================
//  Sphynx — extcores/jni/sphynx_launch_jni.cpp
//
//  Opens SphynxPlayerActivity straight from native code.
//
//  Why not only use PPSSPP's command queue?
//  ----------------------------------------
//  The queue (Sphynx_SendCommandToJava -> NativeActivity.processCommand) is
//  drained once per rendered frame. It works, but it is one more thing that has
//  to be alive at the exact moment a game is tapped. If the queue is not
//  drained at that moment, the tap does nothing and the user sees the PSP
//  loader fail again. This file removes that dependency: with the app Context
//  cached at start-up, the Intent is built and started here, synchronously.
//
//  Android only (extcores/CMakeLists.txt adds it for the Android build).
//
//  SPDX-License-Identifier: GPL-2.0-or-later
// ============================================================================
#include "sphynx_ui_api.h"
#include "diag_log.h"

#ifdef SPHYNX_ANDROID

#include <android/log.h>
#include <jni.h>

#define LJLOG(...) __android_log_print(ANDROID_LOG_INFO, "SphynxLaunch", __VA_ARGS__)

namespace {

JavaVM *g_vm = nullptr;
jobject g_context = nullptr;   // global ref: the app Context
jclass g_intent_class = nullptr;
jclass g_activity_class = nullptr;

// FLAG_ACTIVITY_NEW_TASK: needed because the Context we hold is the
// application Context, not a running Activity.
constexpr jint kFlagNewTask = 0x10000000;

void DetachIfNeeded(JNIEnv *env, bool attached) {
	if (attached && g_vm) g_vm->DetachCurrentThread();
	(void)env;
}

}  // namespace

extern "C" void sphynx_ui_set_context(void *env_ptr, void *context) {
	JNIEnv *env = static_cast<JNIEnv *>(env_ptr);
	if (!env) return;
	if (env->GetJavaVM(&g_vm) != JNI_OK) g_vm = nullptr;
	if (!context) return;
	if (g_context) {
		env->DeleteGlobalRef(g_context);
		g_context = nullptr;
	}
	g_context = env->NewGlobalRef(static_cast<jobject>(context));
	sphynx::Logf("context attached (%s)", g_context ? "ok" : "NewGlobalRef failed");
}

extern "C" int sphynx_ui_launch_player(const char *path) {
	if (!path || !*path) return 0;
	if (!g_context || !g_vm) {
		sphynx::LogLine("player launch: no Context yet, falling back to the command queue");
		return 0;
	}

	JNIEnv *env = nullptr;
	bool attached = false;
	const jint got = g_vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6);
	if (got == JNI_EDETACHED) {
		// The NDK declares AttachCurrentThread(JNIEnv**, void*) while the desktop
		// JDK declares (void**, void*): one cast per toolchain, same call.
#if defined(__ANDROID__)
		const jint attached_rc = g_vm->AttachCurrentThread(&env, nullptr);
#else
		const jint attached_rc = g_vm->AttachCurrentThread(reinterpret_cast<void **>(&env), nullptr);
#endif
		if (attached_rc != JNI_OK) {
			sphynx::LogLine("player launch: AttachCurrentThread failed");
			return -1;
		}
		attached = true;
	} else if (got != JNI_OK) {
		sphynx::LogLine("player launch: no JNIEnv");
		return -1;
	}

	int result = -1;
	if (!g_intent_class) g_intent_class = env->FindClass("android/content/Intent");
	if (!g_activity_class) g_activity_class = env->FindClass("org/sphynx/emulator/SphynxPlayerActivity");
	if (!g_intent_class || !g_activity_class) {
		if (env->ExceptionCheck()) env->ExceptionClear();
		sphynx::LogLine("player launch: cannot find Intent/SphynxPlayerActivity class");
		DetachIfNeeded(env, attached);
		return -1;
	}

	// new Intent(context, SphynxPlayerActivity.class).addFlags(NEW_TASK)
	//     .putExtra("sphynx.path", path)
	jmethodID intent_ctor = env->GetMethodID(g_intent_class, "<init>",
	                                        "(Landroid/content/Context;Ljava/lang/Class;)V");
	jmethodID add_flags = env->GetMethodID(g_intent_class, "addFlags",
	                                       "(I)Landroid/content/Intent;");
	jmethodID put_extra = env->GetMethodID(g_intent_class, "putExtra",
	                                       "(Ljava/lang/String;Ljava/lang/String;)Landroid/content/Intent;");
	if (!intent_ctor || !add_flags || !put_extra) {
		if (env->ExceptionCheck()) env->ExceptionClear();
		sphynx::LogLine("player launch: Intent methods missing");
		DetachIfNeeded(env, attached);
		return -1;
	}

	jobject intent = env->NewObject(g_intent_class, intent_ctor, g_context, g_activity_class);
	if (!intent) {
		if (env->ExceptionCheck()) env->ExceptionClear();
		sphynx::LogLine("player launch: NewObject(Intent) failed");
		DetachIfNeeded(env, attached);
		return -1;
	}
	env->CallObjectMethod(intent, add_flags, kFlagNewTask);

	jstring key = env->NewStringUTF("sphynx.path");
	jstring value = env->NewStringUTF(path);
	env->CallObjectMethod(intent, put_extra, key, value);
	env->DeleteLocalRef(key);
	env->DeleteLocalRef(value);

	if (env->ExceptionCheck()) {
		env->ExceptionDescribe();
		env->ExceptionClear();
		env->DeleteLocalRef(intent);
		sphynx::Logf("player launch: could not build the Intent for %s", path);
		DetachIfNeeded(env, attached);
		return -1;
	}

	jclass context_class = env->GetObjectClass(g_context);
	jmethodID start_activity = context_class
	                                   ? env->GetMethodID(context_class, "startActivity", "(Landroid/content/Intent;)V")
	                                   : nullptr;
	if (!start_activity) {
		if (env->ExceptionCheck()) env->ExceptionClear();
		sphynx::LogLine("player launch: Context.startActivity missing");
	} else {
		env->CallVoidMethod(g_context, start_activity, intent);
		if (env->ExceptionCheck()) {
			env->ExceptionDescribe();
			env->ExceptionClear();
			sphynx::Logf("player launch: startActivity threw for %s", path);
		} else {
			result = 1;
			sphynx::Logf("player launch: opened the player for %s", path);
		}
	}
	if (context_class) env->DeleteLocalRef(context_class);
	env->DeleteLocalRef(intent);
	DetachIfNeeded(env, attached);
	return result;
}

#endif  // SPHYNX_ANDROID
