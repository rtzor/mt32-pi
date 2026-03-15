//
// audiomixer.h
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

#ifndef _audiomixer_h
#define _audiomixer_h

#ifndef UNIT_TEST
#include <circle/types.h>
#else
#include <cstdint>
#include <cstddef>
#endif

class CSynthBase;

class CAudioMixer
{
public:
	static constexpr unsigned MaxEngines = 4;
	static constexpr unsigned NumChannels = 2;  // stereo

	CAudioMixer();

	// Register engines (call during init)
	bool AddEngine(CSynthBase* pEngine);

	// Per-engine controls
	void SetEngineVolume(CSynthBase* pEngine, float fVolume);
	float GetEngineVolume(CSynthBase* pEngine) const;
	void SetEnginePan(CSynthBase* pEngine, float fPan);
	float GetEnginePan(CSynthBase* pEngine) const;

	// Master controls
	void SetMasterVolume(float fVolume);
	float GetMasterVolume() const { return m_fMasterVolume; }

	// Render mixed audio (stereo interleaved, nFrames = number of sample frames)
	// pOutput must have space for nFrames * NumChannels floats
	void Render(float* pOutput, size_t nFrames);

	// If only one engine should render (single mode optimization),
	// set this to bypass mixing overhead
	void SetSoloEngine(CSynthBase* pEngine);
	void ClearSoloEngine() { m_pSoloEngine = nullptr; }
	CSynthBase* GetSoloEngine() const { return m_pSoloEngine; }

	unsigned GetEngineCount() const { return m_nEngineCount; }

private:
	struct TEngineSlot
	{
		CSynthBase* pEngine;
		float       fVolume;   // 0.0 – 1.0
		float       fPan;      // -1.0 (left) to +1.0 (right)
	};

	int FindEngine(CSynthBase* pEngine) const;

	static float Clamp(float val, float lo, float hi);

	TEngineSlot   m_Engines[MaxEngines];
	unsigned      m_nEngineCount;
	float         m_fMasterVolume;
	CSynthBase*   m_pSoloEngine;
};

#endif
