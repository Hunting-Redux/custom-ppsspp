// Copyright (c) 2015- PPSSPP Project and Dolphin Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official git repository and contact information can be found at
// https://github.com/hrydgard/ppsspp and http://www.ppsspp.org/.

// Adapted from Dolphin.

// 16 bit Stereo

#define MAX_SAMPLES_DEFAULT (4096) // 2*64ms - had to double it for nVidia Shield which has huge buffers
#define MAX_SAMPLES_EXTRA   (8192)

#define LOW_WATERMARK_DEFAULT   1680 // 40 ms
#define LOW_WATERMARK_EXTRA 3360 // 80 ms

#define MAX_FREQ_SHIFT  200  // per 32000 Hz
#define CONTROL_FACTOR  0.2f // in freq_shift per fifo size offset
#define CONTROL_AVG     32

#include <cstring>

#include "base/logging.h"
#include "base/NativeApp.h"
#include "Common/ChunkFile.h"
#include "Common/MathUtil.h"
#include "Common/Atomics.h"
#include "Core/Config.h"
#include "Core/ConfigValues.h"
#include "Core/HW/StereoResampler.h"
#include "Core/HLE/__sceAudio.h"
#include "Core/Util/AudioFormat.h"  // for clamp_u8
#include "Core/System.h"

#ifdef _M_SSE
#include <emmintrin.h>
#endif
#if PPSSPP_ARCH(ARM_NEON)
#if defined(_MSC_VER) && PPSSPP_ARCH(ARM64)
#include <arm64_neon.h>
#else
#include <arm_neon.h>
#endif
#endif

StereoResampler::StereoResampler()
		: m_bufsize(MAX_SAMPLES_DEFAULT)
	  , m_lowwatermark(LOW_WATERMARK_DEFAULT)
		, m_input_sample_rate(44100)
		, m_indexW(0)
		, m_indexR(0)
		, m_numLeftI(0.0f)
		, m_frac(0)
		, underrunCount_(0)
		, overrunCount_(0)
		, sample_rate_(0.0f)
		, lastBufSize_(0) {
	// Need to have space for the worst case in case it changes.
	m_buffer = new int16_t[MAX_SAMPLES_EXTRA * 2]();

	// Some Android devices are v-synced to non-60Hz framerates. We simply timestretch audio to fit.
	// TODO: should only do this if auto frameskip is off?
	float refresh = System_GetPropertyFloat(SYSPROP_DISPLAY_REFRESH_RATE);

	// If framerate is "close"...
	if (refresh != 60.0f && refresh > 50.0f && refresh < 70.0f) {
		SetInputSampleRate((int)(44100 * (refresh / 60.0f)));
	}

	UpdateBufferSize();
}

StereoResampler::~StereoResampler() {
	delete[] m_buffer;
	m_buffer = nullptr;
}

void StereoResampler::UpdateBufferSize() {
	if (g_Config.bExtraAudioBuffering) {
		m_bufsize = MAX_SAMPLES_EXTRA;
		m_lowwatermark = LOW_WATERMARK_EXTRA;
	} else {
		m_bufsize = MAX_SAMPLES_DEFAULT;
		m_lowwatermark = LOW_WATERMARK_DEFAULT;
	}
}

template<bool useShift>
inline void ClampBufferToS16(s16 *out, const s32 *in, size_t size, s8 volShift) {
#ifdef _M_SSE
	// Size will always be 16-byte aligned as the hwBlockSize is.
	while (size >= 8) {
		__m128i in1 = _mm_loadu_si128((__m128i *)in);
		__m128i in2 = _mm_loadu_si128((__m128i *)(in + 4));
		__m128i packed = _mm_packs_epi32(in1, in2);
		if (useShift) {
			packed = _mm_srai_epi16(packed, volShift);
		}
		_mm_storeu_si128((__m128i *)out, packed);
		out += 8;
		in += 8;
		size -= 8;
	}
#elif PPSSPP_ARCH(ARM_NEON)
	int16x4_t signedVolShift = vdup_n_s16 (-volShift); // Can only dynamic-shift right, but by a signed integer
	while (size >= 8) {
		int32x4_t in1 = vld1q_s32(in);
		int32x4_t in2 = vld1q_s32(in + 4);
		int16x4_t packed1 = vqmovn_s32(in1);
		int16x4_t packed2 = vqmovn_s32(in2);
		if (useShift) {
			packed1 = vshl_s16(packed1, signedVolShift);
			packed2 = vshl_s16(packed2, signedVolShift);
		}
		vst1_s16(out, packed1);
		vst1_s16(out + 4, packed2);
		out += 8;
		in += 8;
		size -= 8;
	}
#endif
	// This does the remainder if SIMD was used, otherwise it does it all.
	for (size_t i = 0; i < size; i++) {
		out[i] = clamp_s16(useShift ? (in[i] >> volShift) : in[i]);
	}
}

inline void ClampBufferToS16WithVolume(s16 *out, const s32 *in, size_t size) {
	int volume = g_Config.iGlobalVolume;
	if (PSP_CoreParameter().fpsLimit != FPSLimit::NORMAL || PSP_CoreParameter().unthrottle) {
		if (g_Config.iAltSpeedVolume != -1) {
			volume = g_Config.iAltSpeedVolume;
		}
	}

	if (volume >= VOLUME_MAX) {
		ClampBufferToS16<false>(out, in, size, 0);
	} else if (volume <= VOLUME_OFF) {
		memset(out, 0, size * sizeof(s16));
	} else {
		ClampBufferToS16<true>(out, in, size, VOLUME_MAX - (s8)volume);
	}
}

void StereoResampler::Clear() {
	memset(m_buffer, 0, m_bufsize * 2 * sizeof(int16_t));
}

// Executed from sound stream thread
unsigned int StereoResampler::Mix(short* samples, unsigned int numSamples, bool consider_framelimit, int sample_rate) {
	if (!samples)
		return 0;

	unsigned int currentSample = 0;

	// Cache access in non-volatile variable
	// This is the only function changing the read value, so it's safe to
	// cache it locally although it's written here.
	// The writing pointer will be modified outside, but it will only increase,
	// so we will just ignore new written data while interpolating.
	// Without this cache, the compiler wouldn't be allowed to optimize the
	// interpolation loop.
	u32 indexR = Common::AtomicLoad(m_indexR);
	u32 indexW = Common::AtomicLoad(m_indexW);

	const int INDEX_MASK = (m_bufsize * 2 - 1);

	// We force on the audio resampler if the output sample rate doesn't match the input.
	if (!g_Config.bAudioResampler && sample_rate == (int)m_input_sample_rate) {
		for (; currentSample < numSamples * 2 && ((indexW - indexR) & INDEX_MASK) > 2; currentSample += 2) {
			s16 l1 = m_buffer[indexR & INDEX_MASK]; //current
			s16 r1 = m_buffer[(indexR + 1) & INDEX_MASK]; //current
			samples[currentSample] = l1;
			samples[currentSample + 1] = r1;
			indexR += 2;
		}
		sample_rate_ = (float)sample_rate;
	} else {
		// Drift prevention mechanism
		float numLeft = (float)(((indexW - indexR) & INDEX_MASK) / 2);
		m_numLeftI = (numLeft + m_numLeftI*(CONTROL_AVG - 1)) / CONTROL_AVG;
		float offset = (m_numLeftI - m_lowwatermark) * CONTROL_FACTOR;
		if (offset > MAX_FREQ_SHIFT) offset = MAX_FREQ_SHIFT;
		if (offset < -MAX_FREQ_SHIFT) offset = -MAX_FREQ_SHIFT;

		sample_rate_ = (float)(m_input_sample_rate + offset);
		const u32 ratio = (u32)(65536.0 * sample_rate_ / (double)sample_rate);

		// TODO: consider a higher-quality resampling algorithm.
		// TODO: Add a fast path for 1:1.
		for (; currentSample < numSamples * 2 && ((indexW - indexR) & INDEX_MASK) > 2; currentSample += 2) {
			u32 indexR2 = indexR + 2; //next sample
			s16 l1 = m_buffer[indexR & INDEX_MASK]; //current
			s16 r1 = m_buffer[(indexR + 1) & INDEX_MASK]; //current
			s16 l2 = m_buffer[indexR2 & INDEX_MASK]; //next
			s16 r2 = m_buffer[(indexR2 + 1) & INDEX_MASK]; //next
			int sampleL = ((l1 << 16) + (l2 - l1) * (u16)m_frac) >> 16;
			int sampleR = ((r1 << 16) + (r2 - r1) * (u16)m_frac) >> 16;
			samples[currentSample] = sampleL;
			samples[currentSample + 1] = sampleR;
			m_frac += ratio;
			indexR += 2 * (u16)(m_frac >> 16);
			m_frac &= 0xffff;
		}
	}

	int realSamples = currentSample;
	if (currentSample < numSamples * 2)
		underrunCount_++;

	// Padding with the last value to reduce clicking
	short s[2];
	s[0] = clamp_s16(m_buffer[(indexR - 1) & INDEX_MASK]);
	s[1] = clamp_s16(m_buffer[(indexR - 2) & INDEX_MASK]);
	for (; currentSample < numSamples * 2; currentSample += 2) {
		samples[currentSample] = s[0];
		samples[currentSample + 1] = s[1];
	}

	// Flush cached variable
	Common::AtomicStore(m_indexR, indexR);

	//if (realSamples != numSamples * 2) {
	//	ILOG("Underrun! %i / %i", realSamples / 2, numSamples);
	//}
	lastBufSize_ = (m_indexW - m_indexR) & INDEX_MASK;

	return realSamples / 2;
}

void StereoResampler::PushSamples(const s32 *samples, unsigned int num_samples) {
	UpdateBufferSize();
	const int INDEX_MASK = (m_bufsize * 2 - 1);
	// Cache access in non-volatile variable
	// indexR isn't allowed to cache in the audio throttling loop as it
	// needs to get updates to not deadlock.
	u32 indexW = Common::AtomicLoad(m_indexW);

	u32 cap = m_bufsize * 2;
	// If unthottling, no need to fill up the entire buffer, just screws up timing after releasing unthrottle.
	if (PSP_CoreParameter().unthrottle)
		cap = m_lowwatermark * 2;

	// Check if we have enough free space
	// indexW == m_indexR results in empty buffer, so indexR must always be smaller than indexW
	if (num_samples * 2 + ((indexW - Common::AtomicLoad(m_indexR)) & INDEX_MASK) >= cap) {
		if (!PSP_CoreParameter().unthrottle)
			overrunCount_++;
		// TODO: "Timestretch" by doing a windowed overlap with existing buffer content?
		return;
	}

	int over_bytes = num_samples * 4 - (m_bufsize * 2 - (indexW & INDEX_MASK)) * sizeof(short);
	if (over_bytes > 0) {
		ClampBufferToS16WithVolume(&m_buffer[indexW & INDEX_MASK], samples, (num_samples * 4 - over_bytes) / 2);
		ClampBufferToS16WithVolume(&m_buffer[0], samples + (num_samples * 4 - over_bytes) / sizeof(short), over_bytes / 2);
	} else {
		ClampBufferToS16WithVolume(&m_buffer[indexW & INDEX_MASK], samples, num_samples * 2);
	}

	Common::AtomicAdd(m_indexW, num_samples * 2);
	lastPushSize_ = num_samples;
}

void StereoResampler::GetAudioDebugStats(AudioDebugStats *stats) {
	stats->buffered = lastBufSize_;
	stats->underrunCount += underrunCount_;
	underrunCount_ = 0;
	stats->overrunCount += overrunCount_;
	overrunCount_ = 0;
	stats->watermark = m_lowwatermark;
	stats->bufsize = m_bufsize * 2;
	stats->instantSampleRate = (int)sample_rate_;
	stats->lastPushSize = lastPushSize_;
}

void StereoResampler::SetInputSampleRate(unsigned int rate) {
	m_input_sample_rate = rate;
}

void StereoResampler::DoState(PointerWrap &p) {
	auto s = p.Section("resampler", 1);
	if (!s)
		return;
	if (p.mode == p.MODE_READ)
		Clear();
}
