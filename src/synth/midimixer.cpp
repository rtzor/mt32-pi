//
// midimixer.cpp
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

#include "synth/midimixer.h"
#include "synth/synthbase.h"

CMIDIMixer::CMIDIMixer()
	: m_Lock(TASK_LEVEL),
	  m_bEnabled(false),
	  m_pDefaultSynth(nullptr)
{
	for (size_t i = 0; i < MIDIChannelCount; ++i)
		m_pChannelMap[i] = nullptr;
}

void CMIDIMixer::SetEnabled(bool bEnabled)
{
	m_Lock.Acquire();
	m_bEnabled = bEnabled;
	m_Lock.Release();
}

void CMIDIMixer::SetDefaultSynth(CSynthBase* pSynth)
{
	m_Lock.Acquire();
	m_pDefaultSynth = pSynth;
	m_Lock.Release();
}

void CMIDIMixer::SetChannelSynth(u8 nChannel, CSynthBase* pSynth)
{
	if (nChannel >= MIDIChannelCount)
		return;

	m_Lock.Acquire();
	m_pChannelMap[nChannel] = pSynth;
	m_Lock.Release();
}

CSynthBase* CMIDIMixer::GetChannelSynth(u8 nChannel) const
{
	if (nChannel >= MIDIChannelCount)
		return m_pDefaultSynth;

	CSynthBase* pSynth = m_pChannelMap[nChannel];
	return pSynth ? pSynth : m_pDefaultSynth;
}

void CMIDIMixer::HandleMIDIShortMessage(u32 nMessage)
{
	m_Lock.Acquire();

	CSynthBase* pTarget = nullptr;

	if (!m_bEnabled)
	{
		pTarget = m_pDefaultSynth;
	}
	else
	{
		const u8 nStatus = nMessage & 0xFF;
		if (nStatus < 0xF0)
		{
			// Channel message: route to the synth assigned to this channel.
			const u8 nChannel = nStatus & 0x0F;
			pTarget = m_pChannelMap[nChannel] ? m_pChannelMap[nChannel] : m_pDefaultSynth;
		}
		else
		{
			// System real-time / common: send to default synth.
			pTarget = m_pDefaultSynth;
		}
	}

	m_Lock.Release();

	if (pTarget)
		pTarget->HandleMIDIShortMessage(nMessage);
}

void CMIDIMixer::HandleMIDISysExMessage(const u8* pData, size_t nSize)
{
	m_Lock.Acquire();

	if (!m_bEnabled)
	{
		CSynthBase* pTarget = m_pDefaultSynth;
		m_Lock.Release();
		if (pTarget)
			pTarget->HandleMIDISysExMessage(pData, nSize);
		return;
	}

	// Collect all unique synthesizers (default + any channel assignments).
	// Broadcast SysEx to all of them.
	CSynthBase* pSynths[MIDIChannelCount + 1];
	size_t nUnique = 0;

	// Helper to add a synth pointer only if not already in the list.
	#define ADD_UNIQUE(p)                                         \
		do {                                                      \
			CSynthBase* _p = (p);                                 \
			if (_p) {                                             \
				bool _found = false;                              \
				for (size_t _i = 0; _i < nUnique; ++_i)          \
					if (pSynths[_i] == _p) { _found = true; break; } \
				if (!_found) pSynths[nUnique++] = _p;            \
			}                                                     \
		} while (0)

	ADD_UNIQUE(m_pDefaultSynth);
	for (size_t i = 0; i < MIDIChannelCount; ++i)
		ADD_UNIQUE(m_pChannelMap[i]);

	#undef ADD_UNIQUE

	m_Lock.Release();

	for (size_t i = 0; i < nUnique; ++i)
		pSynths[i]->HandleMIDISysExMessage(pData, nSize);
}
