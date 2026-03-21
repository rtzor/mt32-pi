//
// audioeffects.h
//
// mt32-pi - A baremetal MIDI synthesizer for Raspberry Pi
// Copyright (C) 2020-2023 Dale Whinham <daleyo@gmail.com>
//
// This file is part of mt32-pi.
//
// mt32-pi is free software: you can redistribute it and/or modify it under the
// terms of the GNU General Public License as published by the Free Software
// Foundation, either version 3 of the License, or (at your option) any later
// version.
//
// mt32-pi is distributed in the hope that it will be useful, but WITHOUT ANY
// WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
// FOR A PARTICULAR PURPOSE. See the GNU General Public License for more
// details.
//
// You should have received a copy of the GNU General Public License along with
// mt32-pi. If not, see <http://www.gnu.org/licenses/>.
//

#ifndef _audioeffects_h
#define _audioeffects_h

#include <cstddef>

// ---------------------------------------------------------------------------
// Biquad filter — transposed direct form II
// Coefficients computed externally (ComputeLowShelf / ComputeHighShelf).
// ---------------------------------------------------------------------------
struct TBiquad
{
	float b0 = 0.0f, b1 = 0.0f, b2 = 0.0f;
	float              a1 = 0.0f, a2 = 0.0f;
	float z1 = 0.0f, z2 = 0.0f;

	float Process(float x)
	{
		float y = b0 * x + z1;
		z1 = b1 * x - a1 * y + z2;
		z2 = b2 * x - a2 * y;
		return y;
	}
};

// ---------------------------------------------------------------------------
// CAudioEffects — post-mix signal processing chain:
//   1. EQ      — two biquad shelving filters (bass low-shelf + treble high-shelf)
//   2. Reverb  — Freeverb (Schroeder-Moorer comb + allpass network)
//   3. Limiter — soft-knee saturation (cubic Hermite polynomial), always-on
//                safety hard-clamp, optional soft-saturation in the knee
// ---------------------------------------------------------------------------
class CAudioEffects
{
public:
	// Shelving EQ corner frequencies
	static constexpr float BassFrequency   = 200.0f;   // Hz, low-shelf
	static constexpr float TrebleFrequency = 6000.0f;  // Hz, high-shelf

	// Freeverb topology constants
	static constexpr int   NumCombs     = 8;
	static constexpr int   NumAllpass   = 4;

	struct TConfig
	{
		// EQ: two shelving stages; each gain 0 dB = bypass
		bool  bEQEnabled  = false;
		int   nBassGain   = 0;    // dB, -12..+12
		int   nTrebleGain = 0;    // dB, -12..+12

		// Soft-knee limiter (replaces hard clamp when enabled)
		bool  bLimiterEnabled = false;

		// Reverb (Freeverb)
		bool  bReverbEnabled  = false;
		float fReverbRoomSize = 0.5f;   // 0..1
		float fReverbDamping  = 0.5f;   // 0..1
		float fReverbWet      = 0.33f;  // 0..1 (proportion of reverb in output)
	};

	CAudioEffects();
	~CAudioEffects();

	// Call on startup or whenever config / sample-rate changes.
	// Allocates/reallocates reverb delay lines if needed.
	void Configure(const TConfig& Config, float fSampleRate);

	// Apply effects chain in-place to stereo interleaved float buffer.
	// pBuffer holds nFrames * 2 floats; values expected in [-1, +1].
	void Process(float* pBuffer, size_t nFrames);

	const TConfig& GetConfig() const { return m_Config; }

private:
	// Audio EQ Cookbook (R. Bristow-Johnson) — S = 1 shelf slope
	static void ComputeLowShelf (TBiquad& f, float fFreqHz, float fGainDb, float fSr);
	static void ComputeHighShelf(TBiquad& f, float fFreqHz, float fGainDb, float fSr);

	// Freeverb LBCF comb filter (lowpass-feedback comb)
	struct TComb
	{
		float* pBuffer      = nullptr;
		int    nSize        = 0;
		int    nPos         = 0;
		float  fFeedback    = 0.0f;
		float  fDamp1       = 0.0f;
		float  fDamp2       = 1.0f;
		float  fFilterStore = 0.0f;

		void  Init(int size);
		void  Free();
		float Process(float x);
		void  SetDamp(float d)     { fDamp1 = d;         fDamp2 = 1.0f - d; }
		void  SetFeedback(float f) { fFeedback = f; }
	};

	// Freeverb allpass filter
	struct TAllpass
	{
		float* pBuffer = nullptr;
		int    nSize   = 0;
		int    nPos    = 0;

		void  Init(int size);
		void  Free();
		float Process(float x);
	};

	// Freeverb tuning
	static constexpr int   StereoSpread = 23;     // R-channel delay offset (samples)
	static constexpr float ScaleDamp    = 0.4f;
	static constexpr float ScaleRoom    = 0.28f;
	static constexpr float OffsetRoom   = 0.7f;
	static constexpr float FixedGain    = 0.015f; // input pre-gain into reverb network

	// EQ biquads (one per stereo channel)
	TBiquad m_BassL,   m_BassR;
	TBiquad m_TrebleL, m_TrebleR;

	// Reverb delay structures (stereo)
	TComb    m_CombL[NumCombs];
	TComb    m_CombR[NumCombs];
	TAllpass m_AllpassL[NumAllpass];
	TAllpass m_AllpassR[NumAllpass];

	float   m_fSampleRate = 48000.0f;
	TConfig m_Config;
};

#endif
