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

void CAudioMixer::Render(float* pOutput, size_t nFrames)
{
	const size_t nSamples = nFrames * NumChannels;

	// Solo mode: render directly into the output buffer (no mixing overhead)
	if (m_pSoloEngine)
	{
		m_pSoloEngine->Render(pOutput, nFrames);

		// Apply per-engine volume and master volume
		int idx = FindEngine(m_pSoloEngine);
		float fVol = (idx >= 0) ? m_Engines[idx].fVolume : 1.0f;
		float fGain = fVol * m_fMasterVolume;
		if (fGain < 1.0f)
		{
			for (size_t i = 0; i < nSamples; ++i)
				pOutput[i] *= fGain;
		}
		return;
	}

	// Clear output
	memset(pOutput, 0, nSamples * sizeof(float));

	if (m_nEngineCount == 0)
		return;

	// Temp buffer for each engine's output (stack allocation, stereo interleaved)
	float tempBuf[nSamples];

	for (unsigned e = 0; e < m_nEngineCount; ++e)
	{
		TEngineSlot& slot = m_Engines[e];
		if (!slot.pEngine)
			continue;

		// Render this engine
		slot.pEngine->Render(tempBuf, nFrames);

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
		for (size_t i = 0; i < nSamples; i += NumChannels)
		{
			pOutput[i]     += tempBuf[i]     * fGainL;
			pOutput[i + 1] += tempBuf[i + 1] * fGainR;
		}
	}

	// Apply master volume and clamp
	for (size_t i = 0; i < nSamples; ++i)
	{
		pOutput[i] *= m_fMasterVolume;
		pOutput[i] = Clamp(pOutput[i], -1.0f, 1.0f);
	}
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
