// ============================================================================
//  Sphynx — extcores/tests/android-stubs/SLES/OpenSLES.h
//
//  A minimal stand-in for the NDK's OpenSL ES headers, used only by the host
//  type-check in CI (the real APK build uses the NDK's own headers).
//
//  Why: extcores/src/player_audio.cpp must be type-checked on a runner without
//  the NDK. Without these declarations the file would silently compile to its
//  silent stub on the host and a typo in the OpenSL calls would only surface
//  after a twelve-minute APK build.
//
//  Only what player_audio.cpp uses is declared, with the real signatures.
//
//  SPDX-License-Identifier: GPL-2.0-or-later
// ============================================================================
#pragma once

#include <stdint.h>

typedef uint32_t SLuint32;
typedef int32_t SLint32;
typedef uint32_t SLboolean;
typedef int32_t SLresult;

#define SL_RESULT_SUCCESS               ((SLresult)0)
#define SL_BOOLEAN_FALSE                ((SLboolean)0)
#define SL_BOOLEAN_TRUE                 ((SLboolean)1)

#define SL_PLAYSTATE_STOPPED            ((SLuint32)1)
#define SL_PLAYSTATE_PAUSED             ((SLuint32)2)
#define SL_PLAYSTATE_PLAYING            ((SLuint32)3)

#define SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE ((SLuint32)0x800007BD)
#define SL_DATALOCATOR_OUTPUTMIX        ((SLuint32)0x80000004)
#define SL_DATAFORMAT_PCM               ((SLuint32)0x80000002)
#define SL_PCMSAMPLEFORMAT_FIXED_16     ((SLuint32)16)
#define SL_SPEAKER_FRONT_LEFT           ((SLuint32)0x1)
#define SL_SPEAKER_FRONT_RIGHT          ((SLuint32)0x2)
#define SL_BYTEORDER_LITTLEENDIAN       ((SLuint32)0x1)

struct SLInterfaceID_ { int unused; };
typedef const struct SLInterfaceID_ *SLInterfaceID;

struct SLObjectItf_;
typedef const struct SLObjectItf_ *const *SLObjectItf;

struct SLEngineItf_;
typedef const struct SLEngineItf_ *const *SLEngineItf;

struct SLPlayItf_;
typedef const struct SLPlayItf_ *const *SLPlayItf;

struct SLAndroidSimpleBufferQueueItf_;
typedef const struct SLAndroidSimpleBufferQueueItf_ *const *SLAndroidSimpleBufferQueueItf;

struct SLObjectItf_ {
	SLresult (*Realize)(SLObjectItf self, SLboolean async);
	SLresult (*Resume)(SLObjectItf self, SLboolean async);
	SLresult (*GetState)(SLObjectItf self, SLuint32 *state);
	SLresult (*GetInterface)(SLObjectItf self, const SLInterfaceID iid, void *pInterface);
	SLresult (*Destroy)(SLObjectItf self);
};

struct SLDataLocator_AndroidSimpleBufferQueue {
	SLuint32 locatorType;
	SLuint32 numBuffers;
};

struct SLDataFormat_PCM {
	SLuint32 formatType;
	SLuint32 numChannels;
	SLuint32 samplesPerSec;
	SLuint32 bitsPerSample;
	SLuint32 containerSize;
	SLuint32 channelMask;
	SLuint32 endianness;
};

struct SLDataLocator_OutputMix {
	SLuint32 locatorType;
	SLObjectItf outputMix;
};

struct SLDataSource {
	void *pLocator;
	void *pFormat;
};

struct SLDataSink {
	void *pLocator;
	void *pFormat;
};

struct SLEngineItf_ {
	SLresult (*CreateAudioPlayer)(SLEngineItf self, SLObjectItf *pPlayer,
	                              SLDataSource *pAudioSrc, SLDataSink *pAudioSnk,
	                              SLuint32 numInterfaces, const SLInterfaceID *pInterfaceIds,
	                              const SLboolean *pInterfaceRequired);
	SLresult (*CreateOutputMix)(SLEngineItf self, SLObjectItf *pMix, SLuint32 numInterfaces,
	                            const SLInterfaceID *pInterfaceIds,
	                            const SLboolean *pInterfaceRequired);
};

struct SLPlayItf_ {
	SLresult (*SetPlayState)(SLPlayItf self, SLuint32 state);
};

struct SLAndroidSimpleBufferQueueItf_ {
	SLresult (*Enqueue)(SLAndroidSimpleBufferQueueItf self, const void *pBuffer, SLuint32 size);
	SLresult (*Clear)(SLAndroidSimpleBufferQueueItf self);
	SLresult (*GetState)(SLAndroidSimpleBufferQueueItf self, SLuint32 *pState);
	SLresult (*RegisterCallback)(SLAndroidSimpleBufferQueueItf self,
	                             void (*callback)(SLAndroidSimpleBufferQueueItf, void *),
	                             void *pContext);
};

SLresult slCreateEngine(SLObjectItf *pEngine, SLuint32 numOptions, const void *pEngineOptions,
                        SLuint32 numInterfaces, const SLInterfaceID *pInterfaceIds,
                        const SLboolean *pInterfaceRequired);

extern const SLInterfaceID SL_IID_ENGINE;
extern const SLInterfaceID SL_IID_PLAY;
extern const SLInterfaceID SL_IID_ANDROIDSIMPLEBUFFERQUEUE;
