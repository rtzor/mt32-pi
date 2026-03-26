//
// midirouter.h
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

#ifndef _midirouter_h
#define _midirouter_h

#ifndef UNIT_TEST
#include <circle/types.h>
#else
#include <cstdint>
#include <cstddef>
using u8  = uint8_t;
using u32 = uint32_t;
#endif

class CSynthBase;

enum class TRouterPreset
{
	SingleMT32,       // All 16 channels → MT-32
	SingleFluid,      // All 16 channels → FluidSynth
	SingleYmfm,       // All 16 channels → OPL3
	SplitGM,          // Channels 1-9 → MT-32, 10-16 → FluidSynth
	Custom            // User-defined per-channel mapping
};

class CMIDIRouter
{
public:
	static constexpr unsigned NumChannels = 16;
	static constexpr unsigned MaxEngines  = 2;
	static constexpr unsigned NumCCs      = 128;

	// Engine indices for CC filter table
	static constexpr unsigned EngMT32  = 0;
	static constexpr unsigned EngFluid = 1;

	CMIDIRouter();

	// Register synth engines (call during init, before routing)
	void SetMT32Engine(CSynthBase* pEngine)       { m_pMT32 = pEngine; }
	void SetFluidSynthEngine(CSynthBase* pEngine)  { m_pFluidSynth = pEngine; }
	void SetYmfmEngine(CSynthBase* pEngine)        { m_pYmfm = pEngine; }

	// Channel mapping — which engine receives each channel
	void SetChannelEngine(u8 nChannel, CSynthBase* pEngine);
	void SetAllChannels(CSynthBase* pEngine);
	CSynthBase* GetChannelEngine(u8 nChannel) const;

	// Channel remapping — translate source channel to a different target channel
	// e.g. SetChannelRemap(4, 0) means MIDI ch5 arrives at the engine as ch1
	void SetChannelRemap(u8 nSrcChannel, u8 nDstChannel);
	u8   GetChannelRemap(u8 nSrcChannel) const;
	void ResetChannelRemap();  // identity mapping (each channel maps to itself)

	// Presets
	void ApplyPreset(TRouterPreset Preset);
	TRouterPreset GetPreset() const { return m_Preset; }

	// Routing
	void RouteShortMessage(u32 nMessage);
	void RouteSysEx(const u8* pData, size_t nSize);

	// State queries
	bool IsDualMode() const;
	bool IsEnabled() const { return m_bEnabled; }
	void SetEnabled(bool bEnabled) { m_bEnabled = bEnabled; }

	// Get the engine that has the most channels assigned (for UI/LCD)
	CSynthBase* GetPrimaryEngine() const;

	// Get engine name string for a channel (for web UI)
	const char* GetChannelEngineName(u8 nChannel) const;

	// CC Filters — per-engine allow/block for each CC number
	// By default all CCs are allowed (true)
	void SetCCFilter(unsigned nEngine, u8 nCC, bool bAllow);
	bool GetCCFilter(unsigned nEngine, u8 nCC) const;
	void ResetCCFilters();  // allow all

	// Layering — NoteOn/Off sent to both engines on layered channels
	void SetLayering(u8 nChannel, bool bLayered);
	bool GetLayering(u8 nChannel) const;
	void SetAllLayering(bool bLayered);
	bool HasAnyLayering() const;

	// Per-channel volume (scales CC7 messages before routing, 0.0–1.0)
	void  SetChannelVolume(u8 nChannel, float fVolume);
	float GetChannelVolume(u8 nChannel) const;
	void  ResetChannelVolumes();  // reset all to 1.0

private:
	bool         m_bEnabled;
	TRouterPreset m_Preset;
	CSynthBase*  m_pChannelMap[NumChannels];
	u8           m_nChannelRemap[NumChannels];  // target channel for each source
	bool         m_bCCFilter[MaxEngines][NumCCs]; // true = allow, false = block
	bool         m_bLayered[NumChannels];         // true = duplicate notes to both engines
	float        m_fChannelVolume[NumChannels];   // CC7 scaling factor per channel (1.0 = full)
	CSynthBase*  m_pMT32;
	CSynthBase*  m_pFluidSynth;
	CSynthBase*  m_pYmfm;
};

#endif
