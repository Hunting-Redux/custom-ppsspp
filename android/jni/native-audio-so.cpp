// Minimal audio streaming using OpenSL.
//
// Loosely based on the Android NDK sample code.

#include <assert.h>
#include <string.h>
#include <unistd.h>

// for native audio
#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>

#include "../base/logging.h"
#include "native-audio-so.h"

AudioContext::AudioContext(AndroidAudioCallback cb, int _FramesPerBuffer, int _SampleRate)
	  : audioCallback(cb), framesPerBuffer(_FramesPerBuffer), sampleRate(_SampleRate) {
	if (framesPerBuffer == 0)
		framesPerBuffer = 256;
	if (framesPerBuffer < 32)
		framesPerBuffer = 32;
	if (framesPerBuffer > 4096)
		framesPerBuffer = 4096;

	sampleRate = _SampleRate;
}

// This callback handler is called every time a buffer finishes playing.
// The documentation available is very unclear about how to best manage buffers.
// I've chosen to this approach: Instantly enqueue a buffer that was rendered to the last time,
// and then render the next. Hopefully it's okay to spend time in this callback after having enqueued. 
void OpenSLContext::bqPlayerCallbackWrap(SLAndroidSimpleBufferQueueItf bq, void *context) {
	OpenSLContext *ctx = (OpenSLContext *)context;
	ctx->BqPlayerCallback(bq);
}

void OpenSLContext::BqPlayerCallback(SLAndroidSimpleBufferQueueItf bq) {
	if (bq != bqPlayerBufferQueue) {
		ELOG("Wrong bq!");
		return;
	}

	int renderedFrames = audioCallback(buffer[curBuffer], framesPerBuffer);

	int sizeInBytes = framesPerBuffer * 2 * sizeof(short);
	int byteCount = (framesPerBuffer - renderedFrames) * 4;
	if (byteCount > 0) {
		memset(buffer[curBuffer] + renderedFrames * 2, 0, byteCount);
	}
	SLresult result = (*bqPlayerBufferQueue)->Enqueue(bqPlayerBufferQueue, buffer[curBuffer], sizeInBytes);

	// Comment from sample code:
	// the most likely other result is SL_RESULT_BUFFER_INSUFFICIENT,
	// which for this code example would indicate a programming error
	if (result != SL_RESULT_SUCCESS) {
		ELOG("OpenSL ES: Failed to enqueue! %i %i", renderedFrames, sizeInBytes);
	}

	curBuffer += 1; // Switch buffer
	if (curBuffer == NUM_BUFFERS)
		curBuffer = 0;
}

// create the engine and output mix objects
OpenSLContext::OpenSLContext(AndroidAudioCallback cb, int _FramesPerBuffer, int _SampleRate)
	: AudioContext(cb, _FramesPerBuffer, _SampleRate) {}

bool OpenSLContext::Init() {
	SLresult result;
	// create engine
	result = slCreateEngine(&engineObject, 0, NULL, 0, NULL, NULL);
	if (result != SL_RESULT_SUCCESS) {
		ELOG("OpenSL ES: Failed to create the engine: %d", (int)result);
		engineObject = nullptr;
		return false;
	}
	result = (*engineObject)->Realize(engineObject, SL_BOOLEAN_FALSE);
	assert(SL_RESULT_SUCCESS == result);
	result = (*engineObject)->GetInterface(engineObject, SL_IID_ENGINE, &engineEngine);
	assert(SL_RESULT_SUCCESS == result);
	result = (*engineEngine)->CreateOutputMix(engineEngine, &outputMixObject, 0, 0, 0);
	if (result != SL_RESULT_SUCCESS) {
		ELOG("OpenSL ES: Failed to create output mix: %d", (int)result);
		(*engineObject)->Destroy(engineObject);
		engineEngine = nullptr;
		engineObject = nullptr;
		return false;
	}
	result = (*outputMixObject)->Realize(outputMixObject, SL_BOOLEAN_FALSE);
	assert(SL_RESULT_SUCCESS == result);

	// The constants, such as SL_SAMPLINGRATE_44_1, are just 44100000.
	SLuint32 sr = (SLuint32)sampleRate * 1000;

	SLDataLocator_AndroidSimpleBufferQueue loc_bufq = {SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE, NUM_BUFFERS};
	SLDataFormat_PCM format_pcm = {
		SL_DATAFORMAT_PCM,
		2,
		sr,
		SL_PCMSAMPLEFORMAT_FIXED_16,
		SL_PCMSAMPLEFORMAT_FIXED_16,
		SL_SPEAKER_FRONT_LEFT | SL_SPEAKER_FRONT_RIGHT,
		SL_BYTEORDER_LITTLEENDIAN
	};

	SLDataSource audioSrc = {&loc_bufq, &format_pcm};

	// configure audio sink
	SLDataLocator_OutputMix loc_outmix = {SL_DATALOCATOR_OUTPUTMIX, outputMixObject};
	SLDataSink audioSnk = {&loc_outmix, NULL};

	// create audio player
	const SLInterfaceID ids[2] = {SL_IID_BUFFERQUEUE, SL_IID_VOLUME};
	const SLboolean req[2] = {SL_BOOLEAN_TRUE, SL_BOOLEAN_TRUE};
	result = (*engineEngine)->CreateAudioPlayer(engineEngine, &bqPlayerObject, &audioSrc, &audioSnk, 2, ids, req);
	if (result != SL_RESULT_SUCCESS) {
		ELOG("OpenSL ES: CreateAudioPlayer failed: %d", (int)result);
		(*outputMixObject)->Destroy(outputMixObject);
		outputMixObject = nullptr;

		// Should really tear everything down here. Sigh.
		(*engineObject)->Destroy(engineObject);
		engineEngine = nullptr;
		engineObject = nullptr;
		return false;
	}

	result = (*bqPlayerObject)->Realize(bqPlayerObject, SL_BOOLEAN_FALSE);
	assert(SL_RESULT_SUCCESS == result);
	result = (*bqPlayerObject)->GetInterface(bqPlayerObject, SL_IID_PLAY, &bqPlayerPlay);
	assert(SL_RESULT_SUCCESS == result);
	result = (*bqPlayerObject)->GetInterface(bqPlayerObject, SL_IID_BUFFERQUEUE,
		&bqPlayerBufferQueue);
	assert(SL_RESULT_SUCCESS == result);
	result = (*bqPlayerBufferQueue)->RegisterCallback(bqPlayerBufferQueue, &bqPlayerCallbackWrap, this);
	assert(SL_RESULT_SUCCESS == result);
	result = (*bqPlayerObject)->GetInterface(bqPlayerObject, SL_IID_VOLUME, &bqPlayerVolume);
	assert(SL_RESULT_SUCCESS == result);
	result = (*bqPlayerPlay)->SetPlayState(bqPlayerPlay, SL_PLAYSTATE_PLAYING);
	assert(SL_RESULT_SUCCESS == result);

	// Allocate and enqueue N empty buffers.
	for (int i = 0; i < NUM_BUFFERS; i++) {
		buffer[i] = new short[framesPerBuffer * 2]{};
	}

	int sizeInBytes = framesPerBuffer * 2 * sizeof(short);
	for (int i = 0; i < NUM_BUFFERS; i++) {
		result = (*bqPlayerBufferQueue)->Enqueue(bqPlayerBufferQueue, buffer[i], sizeInBytes);
		if (SL_RESULT_SUCCESS != result) {
			return false;
		}
	}

	curBuffer = 0;
	return true;
}

// shut down the native audio system
OpenSLContext::~OpenSLContext() {
	if (bqPlayerPlay) {
		ILOG("OpenSLWrap_Shutdown - stopping playback");
		SLresult result;
		result = (*bqPlayerPlay)->SetPlayState(bqPlayerPlay, SL_PLAYSTATE_STOPPED);
		if (SL_RESULT_SUCCESS != result) {
			ELOG("SetPlayState failed");
		}
	}

	ILOG("OpenSLWrap_Shutdown - deleting player object");

	if (bqPlayerObject) {
		(*bqPlayerObject)->Destroy(bqPlayerObject);
		bqPlayerObject = nullptr;
		bqPlayerPlay = nullptr;
		bqPlayerBufferQueue = nullptr;
		bqPlayerVolume = nullptr;
	}

	ILOG("OpenSLWrap_Shutdown - deleting mix object");

	if (outputMixObject) {
		(*outputMixObject)->Destroy(outputMixObject);
		outputMixObject = nullptr;
	}

	ILOG("OpenSLWrap_Shutdown - deleting engine object");

	if (engineObject) {
		(*engineObject)->Destroy(engineObject);
		engineObject = nullptr;
		engineEngine = nullptr;
	}

	for (int i = 0; i < NUM_BUFFERS; i++) {
		delete[] buffer[i];
		buffer[i] = nullptr;
	}
	ILOG("OpenSLWrap_Shutdown - finished");
}	

