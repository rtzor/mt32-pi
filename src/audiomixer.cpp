//
// audiomixer.cpp
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

#include "audiomixer.h"

#include <circle/timer.h>

#ifdef __aarch64__
#include <arm_neon.h>
#endif

#ifndef UNIT_TEST
#include "synth/synthbase.h"
#else
#include "synthbase_stub.h"
#endif

#include <cstring>

CAudioMixer::CAudioMixer()
	: m_nEngineCount(0),
	  m_fMasterVolume(1.0f),
	  m_pSoloEngine(nullptr)
{
	for (unsigned i = 0; i < MaxEngines; ++i)
	{
		m_Engines[i].pEngine = nullptr;
		m_Engines[i].fVolume = 0.8f;
		m_Engines[i].fPan    = 0.0f;
	}
}

bool CAudioMixer::AddEngine(CSynthBase* pEngine)
{
	if (!pEngine || m_nEngineCount >= MaxEngines)
		return false;

	// Don't add duplicates
	if (FindEngine(pEngine) >= 0)
		return false;

	m_Engines[m_nEngineCount].pEngine = pEngine;
	m_Engines[m_nEngineCount].fVolume = 0.8f;
	m_Engines[m_nEngineCount].fPan    = 0.0f;
	++m_nEngineCount;
	return true;
}

void CAudioMixer::SetEngineVolume(CSynthBase* pEngine, float fVolume)
{
	int idx = FindEngine(pEngine);
	if (idx >= 0)
		m_Engines[idx].fVolume = Clamp(fVolume, 0.0f, 1.0f);
}

float CAudioMixer::GetEngineVolume(CSynthBase* pEngine) const
{
	int idx = FindEngine(pEngine);
	return (idx >= 0) ? m_Engines[idx].fVolume : 0.0f;
}

void CAudioMixer::SetEnginePan(CSynthBase* pEngine, float fPan)
{
	int idx = FindEngine(pEngine);
	if (idx >= 0)
		m_Engines[idx].fPan = Clamp(fPan, -1.0f, 1.0f);
}

float CAudioMixer::GetEnginePan(CSynthBase* pEngine) const
{
	int idx = FindEngine(pEngine);
	return (idx >= 0) ? m_Engines[idx].fPan : 0.0f;
}

void CAudioMixer::SetMasterVolume(float fVolume)
{
	m_fMasterVolume = Clamp(fVolume, 0.0f, 1.0f);
}

void CAudioMixer::SetSoloEngine(CSynthBase* pEngine)
{
	// Only allow engines that are registered
	if (FindEngine(pEngine) >= 0)
		m_pSoloEngine = pEngine;
}

void CAudioMixer::Render(float* pOutput, size_t nFrames, TRenderProfile* pProfile)
{
	const size_t nSamples = nFrames * NumChannels;
	unsigned nMixUs = 0;

	if (pProfile)
	{
		for (unsigned i = 0; i < MaxEngines; ++i)
			pProfile->nEngineRenderUs[i] = 0;
		pProfile->nMixUs = 0;
	}

	// Solo mode: render directly into the output buffer (no mixing overhead)
	if (m_pSoloEngine)
	{
		const unsigned nRenderStart = CTimer::GetClockTicks();
		m_pSoloEngine->Render(pOutput, nFrames);
		const unsigned nRenderElapsed = CTimer::GetClockTicks() - nRenderStart;

		// Apply per-engine volume and master volume
		int idx = FindEngine(m_pSoloEngine);
		if (pProfile && idx >= 0)
			pProfile->nEngineRenderUs[idx] = nRenderElapsed;

		const unsigned nMixStart = CTimer::GetClockTicks();
		float fVol = (idx >= 0) ? m_Engines[idx].fVolume : 1.0f;
		float fGain = fVol * m_fMasterVolume;
		if (fGain < 1.0f)
		{
#ifdef __aarch64__
			float32x4_t vGain = vdupq_n_f32(fGain);
			size_t i = 0;
			for (; i + 4 <= nSamples; i += 4)
			{
				float32x4_t v = vld1q_f32(pOutput + i);
				vst1q_f32(pOutput + i, vmulq_f32(v, vGain));
			}
			for (; i < nSamples; ++i)
				pOutput[i] *= fGain;
#else
			for (size_t i = 0; i < nSamples; ++i)
				pOutput[i] *= fGain;
#endif
		}
		nMixUs = CTimer::GetClockTicks() - nMixStart;
		if (pProfile)
			pProfile->nMixUs = nMixUs;
		return;
	}

	// Clear output
	const unsigned nClearStart = CTimer::GetClockTicks();
	memset(pOutput, 0, nSamples * sizeof(float));
	nMixUs += CTimer::GetClockTicks() - nClearStart;

	if (m_nEngineCount == 0)
	{
		if (pProfile)
			pProfile->nMixUs = nMixUs;
		return;
	}

	// Temp buffer for each engine's output (stack allocation, stereo interleaved)
	float tempBuf[nSamples];

	for (unsigned e = 0; e < m_nEngineCount; ++e)
	{
		TEngineSlot& slot = m_Engines[e];
		if (!slot.pEngine)
			continue;

		// Render this engine
		const unsigned nRenderStart = CTimer::GetClockTicks();
		slot.pEngine->Render(tempBuf, nFrames);
		const unsigned nRenderElapsed = CTimer::GetClockTicks() - nRenderStart;
		if (pProfile)
			pProfile->nEngineRenderUs[e] = nRenderElapsed;

		// Compute per-channel gain from volume + pan
		// Simple constant-power-ish pan law:
		//   Left  gain = volume * (1.0 - pan) / 2   when pan in [-1, +1]
		//   Right gain = volume * (1.0 + pan) / 2
		// At center (pan=0): both = volume * 0.5... that halves the signal.
		// Instead use linear pan law that preserves level at center:
		//   Left  gain = volume * min(1.0, 1.0 - pan)
		//   Right gain = volume * min(1.0, 1.0 + pan)
		const float fVol = slot.fVolume;
		const float fPan = slot.fPan;
		const float fGainL = fVol * (fPan < 0.0f ? 1.0f : 1.0f - fPan);
		const float fGainR = fVol * (fPan > 0.0f ? 1.0f : 1.0f + fPan);

		// Mix into output
		const unsigned nMixStart = CTimer::GetClockTicks();
#ifdef __aarch64__
		{
			// Pack {gainL, gainR, gainL, gainR} for 4-float NEON load (2 stereo frames)
			const float gainPairs[4] = { fGainL, fGainR, fGainL, fGainR };
			float32x4_t vGains = vld1q_f32(gainPairs);
			size_t i = 0;
			for (; i + 4 <= nSamples; i += 4)
			{
				float32x4_t vOut  = vld1q_f32(pOutput + i);
				float32x4_t vTemp = vld1q_f32(tempBuf  + i);
				vst1q_f32(pOutput + i, vmlaq_f32(vOut, vTemp, vGains));
			}
			for (; i < nSamples; i += NumChannels)
			{
				pOutput[i]     += tempBuf[i]     * fGainL;
				pOutput[i + 1] += tempBuf[i + 1] * fGainR;
			}
		}
#else
		for (size_t i = 0; i < nSamples; i += NumChannels)
		{
			pOutput[i]     += tempBuf[i]     * fGainL;
			pOutput[i + 1] += tempBuf[i + 1] * fGainR;
		}
#endif
		nMixUs += CTimer::GetClockTicks() - nMixStart;
	}

	// Apply master volume and clamp
	const unsigned nPostStart = CTimer::GetClockTicks();
#ifdef __aarch64__
        {
                float32x4_t vMaster = vdupq_n_f32(m_fMasterVolume);
                float32x4_t vMin    = vdupq_n_f32(-1.0f);
                float32x4_t vMax    = vdupq_n_f32( 1.0f);
                size_t i = 0;
                for (; i + 4 <= nSamples; i += 4)
                {
                        float32x4_t v = vld1q_f32(pOutput + i);
                        v = vmulq_f32(v, vMaster);
                        v = vmaxq_f32(v, vMin);
                        v = vminq_f32(v, vMax);
                        vst1q_f32(pOutput + i, v);
                }
                for (; i < nSamples; ++i)
                {
                        pOutput[i] *= m_fMasterVolume;
                        pOutput[i] = Clamp(pOutput[i], -1.0f, 1.0f);
                }
        }
#else
        for (size_t i = 0; i < nSamples; ++i)
        {
                pOutput[i] *= m_fMasterVolume;
                pOutput[i] = Clamp(pOutput[i], -1.0f, 1.0f);
        }
#endif
	nMixUs += CTimer::GetClockTicks() - nPostStart;
	if (pProfile)
		pProfile->nMixUs = nMixUs;
}

int CAudioMixer::FindEngine(CSynthBase* pEngine) const
{
	for (unsigned i = 0; i < m_nEngineCount; ++i)
	{
		if (m_Engines[i].pEngine == pEngine)
			return static_cast<int>(i);
	}
	return -1;
}

float CAudioMixer::Clamp(float val, float lo, float hi)
{
	if (val < lo) return lo;
	if (val > hi) return hi;
	return val;
}
