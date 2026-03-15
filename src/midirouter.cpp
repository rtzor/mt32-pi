//
// midirouter.cpp
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

#include "midirouter.h"

#ifndef UNIT_TEST
#include "synth/synthbase.h"
#else
#include "synthbase_stub.h"
#endif

CMIDIRouter::CMIDIRouter()
	: m_bEnabled(false),
	  m_Preset(TRouterPreset::SingleMT32),
	  m_pMT32(nullptr),
	  m_pFluidSynth(nullptr)
{
	for (unsigned i = 0; i < NumChannels; ++i)
	{
		m_pChannelMap[i] = nullptr;
		m_nChannelRemap[i] = static_cast<u8>(i);  // identity mapping
		m_bLayered[i] = false;
	}
	ResetCCFilters();
}

void CMIDIRouter::SetChannelEngine(u8 nChannel, CSynthBase* pEngine)
{
	if (nChannel < NumChannels)
	{
		m_pChannelMap[nChannel] = pEngine;
		m_Preset = TRouterPreset::Custom;
	}
}

void CMIDIRouter::SetAllChannels(CSynthBase* pEngine)
{
	for (unsigned i = 0; i < NumChannels; ++i)
		m_pChannelMap[i] = pEngine;
}

CSynthBase* CMIDIRouter::GetChannelEngine(u8 nChannel) const
{
	if (nChannel < NumChannels)
		return m_pChannelMap[nChannel];
	return nullptr;
}

void CMIDIRouter::ApplyPreset(TRouterPreset Preset)
{
	m_Preset = Preset;

	switch (Preset)
	{
		case TRouterPreset::SingleMT32:
			SetAllChannels(m_pMT32);
			break;

		case TRouterPreset::SingleFluid:
			SetAllChannels(m_pFluidSynth);
			break;

		case TRouterPreset::SplitGM:
			// Channels 1-9 (indices 0-8) → MT-32
			for (unsigned i = 0; i < 9; ++i)
				m_pChannelMap[i] = m_pMT32;
			// Channels 10-16 (indices 9-15) → FluidSynth
			for (unsigned i = 9; i < NumChannels; ++i)
				m_pChannelMap[i] = m_pFluidSynth;
			break;

		case TRouterPreset::Custom:
			// Don't change the table — user manages it
			break;
	}
}

void CMIDIRouter::RouteShortMessage(u32 nMessage)
{
	const u8 status = nMessage & 0xFF;

	// System real-time and system common messages (>= 0xF0)
	// go to all engines that are in the channel map
	if (status >= 0xF0)
	{
		if (m_pMT32)
			m_pMT32->HandleMIDIShortMessage(nMessage);
		if (m_pFluidSynth && m_pFluidSynth != m_pMT32)
			m_pFluidSynth->HandleMIDIShortMessage(nMessage);
		return;
	}

	// Channel messages: extract channel, remap, and route
	const u8 nChannel = status & 0x0F;
	const u8 nMsgType = status & 0xF0;
	CSynthBase* pTarget = m_pChannelMap[nChannel];

	// Build the (possibly remapped) message once
	const u8 nRemapped = m_nChannelRemap[nChannel];
	const u32 nRouted = (nRemapped != nChannel)
		? ((nMessage & 0xFFFFFF00u) | ((nMsgType) | nRemapped))
		: nMessage;

	// CC filtering: check if this CC# is allowed for the target engine
	if (nMsgType == 0xB0)
	{
		const u8 nCC = static_cast<u8>((nMessage >> 8) & 0x7F);

		auto SendIfAllowed = [&](CSynthBase* pEng, unsigned nEngIdx, u32 nMsg)
		{
			if (pEng && m_bCCFilter[nEngIdx][nCC])
				pEng->HandleMIDIShortMessage(nMsg);
		};

		if (pTarget == m_pMT32)
			SendIfAllowed(m_pMT32, EngMT32, nRouted);
		else if (pTarget == m_pFluidSynth)
			SendIfAllowed(m_pFluidSynth, EngFluid, nRouted);

		// Layering: send CC to the other engine too (if allowed)
		if (m_bLayered[nChannel])
		{
			if (pTarget != m_pMT32 && m_pMT32)
				SendIfAllowed(m_pMT32, EngMT32, nRouted);
			if (pTarget != m_pFluidSynth && m_pFluidSynth)
				SendIfAllowed(m_pFluidSynth, EngFluid, nRouted);
		}
		return;
	}

	// Layering for NoteOn/Off: duplicate to both engines
	if (m_bLayered[nChannel] && (nMsgType == 0x90 || nMsgType == 0x80))
	{
		if (m_pMT32)
			m_pMT32->HandleMIDIShortMessage(nRouted);
		if (m_pFluidSynth && m_pFluidSynth != m_pMT32)
			m_pFluidSynth->HandleMIDIShortMessage(nRouted);
		return;
	}

	// Normal routing
	if (pTarget)
		pTarget->HandleMIDIShortMessage(nRouted);
}

void CMIDIRouter::RouteSysEx(const u8* pData, size_t nSize)
{
	// Minimum valid SysEx: F0 <id> ... F7  (at least 3 bytes)
	if (!pData || nSize < 3)
		return;

	// Check manufacturer ID (byte after F0)
	const u8 nManufacturer = pData[1];

	// Roland SysEx (manufacturer 0x41) → MT-32
	if (nManufacturer == 0x41)
	{
		if (m_pMT32)
			m_pMT32->HandleMIDISysExMessage(pData, nSize);
		return;
	}

	// Non-realtime / realtime universal SysEx (0x7E, 0x7F) → both engines
	if (nManufacturer == 0x7E || nManufacturer == 0x7F)
	{
		if (m_pMT32)
			m_pMT32->HandleMIDISysExMessage(pData, nSize);
		if (m_pFluidSynth)
			m_pFluidSynth->HandleMIDISysExMessage(pData, nSize);
		return;
	}

	// All other SysEx → FluidSynth (GM/GS/XG)
	if (m_pFluidSynth)
		m_pFluidSynth->HandleMIDISysExMessage(pData, nSize);
}

bool CMIDIRouter::IsDualMode() const
{
	if (!m_pMT32 || !m_pFluidSynth)
		return false;

	bool bHasMT32 = false;
	bool bHasFluid = false;
	for (unsigned i = 0; i < NumChannels; ++i)
	{
		if (m_pChannelMap[i] == m_pMT32)
			bHasMT32 = true;
		else if (m_pChannelMap[i] == m_pFluidSynth)
			bHasFluid = true;

		if (bHasMT32 && bHasFluid)
			return true;
	}
	return false;
}

CSynthBase* CMIDIRouter::GetPrimaryEngine() const
{
	unsigned nMT32Count = 0;
	unsigned nFluidCount = 0;

	for (unsigned i = 0; i < NumChannels; ++i)
	{
		if (m_pChannelMap[i] == m_pMT32)
			++nMT32Count;
		else if (m_pChannelMap[i] == m_pFluidSynth)
			++nFluidCount;
	}

	if (nMT32Count >= nFluidCount)
		return m_pMT32;
	return m_pFluidSynth;
}

void CMIDIRouter::SetChannelRemap(u8 nSrcChannel, u8 nDstChannel)
{
	if (nSrcChannel < NumChannels && nDstChannel < NumChannels)
	{
		m_nChannelRemap[nSrcChannel] = nDstChannel;
		m_Preset = TRouterPreset::Custom;
	}
}

u8 CMIDIRouter::GetChannelRemap(u8 nSrcChannel) const
{
	if (nSrcChannel < NumChannels)
		return m_nChannelRemap[nSrcChannel];
	return nSrcChannel;
}

void CMIDIRouter::ResetChannelRemap()
{
	for (unsigned i = 0; i < NumChannels; ++i)
		m_nChannelRemap[i] = static_cast<u8>(i);
}

const char* CMIDIRouter::GetChannelEngineName(u8 nChannel) const
{
	if (nChannel >= NumChannels)
		return "none";

	CSynthBase* pEngine = m_pChannelMap[nChannel];
	if (!pEngine)
		return "none";

	return pEngine->GetName();
}

// ---------------------------------------------------------------------------
// CC Filters
// ---------------------------------------------------------------------------

void CMIDIRouter::SetCCFilter(unsigned nEngine, u8 nCC, bool bAllow)
{
	if (nEngine < MaxEngines && nCC < NumCCs)
		m_bCCFilter[nEngine][nCC] = bAllow;
}

bool CMIDIRouter::GetCCFilter(unsigned nEngine, u8 nCC) const
{
	if (nEngine < MaxEngines && nCC < NumCCs)
		return m_bCCFilter[nEngine][nCC];
	return true;
}

void CMIDIRouter::ResetCCFilters()
{
	for (unsigned e = 0; e < MaxEngines; ++e)
		for (unsigned cc = 0; cc < NumCCs; ++cc)
			m_bCCFilter[e][cc] = true;
}

// ---------------------------------------------------------------------------
// Layering
// ---------------------------------------------------------------------------

void CMIDIRouter::SetLayering(u8 nChannel, bool bLayered)
{
	if (nChannel < NumChannels)
		m_bLayered[nChannel] = bLayered;
}

bool CMIDIRouter::GetLayering(u8 nChannel) const
{
	if (nChannel < NumChannels)
		return m_bLayered[nChannel];
	return false;
}

void CMIDIRouter::SetAllLayering(bool bLayered)
{
	for (unsigned i = 0; i < NumChannels; ++i)
		m_bLayered[i] = bLayered;
}

bool CMIDIRouter::HasAnyLayering() const
{
	for (unsigned i = 0; i < NumChannels; ++i)
		if (m_bLayered[i])
			return true;
	return false;
}
