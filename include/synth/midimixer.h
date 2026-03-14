//
// midimixer.h
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

#ifndef _midimixer_h
#define _midimixer_h

#include <circle/spinlock.h>
#include <circle/types.h>

class CSynthBase;

// CMIDIMixer routes MIDI channels (0–15) to individual synthesizer instances.
//
// When enabled every incoming short message is dispatched to the synthesizer
// that has been assigned to the message's MIDI channel.  If no explicit
// assignment exists for a channel the message is forwarded to the default
// synthesizer.  SysEx messages are broadcast to all currently registered
// synthesizers.
//
// When disabled all traffic is forwarded directly to the default synthesizer,
// preserving backward-compatible single-synth behaviour.
class CMIDIMixer
{
public:
	static constexpr size_t MIDIChannelCount = 16;

	CMIDIMixer();

	// Enable or disable channel-based routing.
	void SetEnabled(bool bEnabled);
	bool IsEnabled() const { return m_bEnabled; }

	// The default synthesizer receives all traffic when the mixer is disabled,
	// and also acts as the fallback for channels with no explicit assignment.
	void SetDefaultSynth(CSynthBase* pSynth);
	CSynthBase* GetDefaultSynth() const { return m_pDefaultSynth; }

	// Assign MIDI channel nChannel (0–15) to pSynth.  Pass nullptr to clear
	// the assignment and fall back to the default synthesizer.
	void SetChannelSynth(u8 nChannel, CSynthBase* pSynth);

	// Returns the synthesizer assigned to nChannel, or the default synth when
	// no explicit assignment has been made.
	CSynthBase* GetChannelSynth(u8 nChannel) const;

	// Route a short MIDI message to the appropriate synthesizer.
	void HandleMIDIShortMessage(u32 nMessage);

	// Broadcast a SysEx message to all registered synthesizers.
	void HandleMIDISysExMessage(const u8* pData, size_t nSize);

	CSpinLock m_Lock;

private:
	bool       m_bEnabled;
	CSynthBase* m_pChannelMap[MIDIChannelCount];
	CSynthBase* m_pDefaultSynth;
};

#endif
