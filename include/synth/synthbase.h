//
// synthbase.h
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

#ifndef _synthbase_h
#define _synthbase_h

#include <circle/spinlock.h>
#include <circle/types.h>

#include "lcd/lcd.h"
#include "lcd/ui.h"
#include "midimonitor.h"

// Audio output hardware target that a synthesizer should be routed to.
// The actual hardware device is owned by the kernel (CMT32Pi); this value
// is used to communicate the desired routing so the kernel can create and
// connect the appropriate CSoundBaseDevice instance.
enum class TSynthAudioOutput
{
	PWM,   // GPIO PWM (headphone jack)
	HDMI,  // HDMI digital audio
	I2S,   // I²S external DAC
};

class CSynthBase
{
public:
	CSynthBase(unsigned int nSampleRate)
		: m_Lock(TASK_LEVEL),
		  m_nSampleRate(nSampleRate),
		  m_pUI(nullptr),
		  m_AudioOutput(TSynthAudioOutput::PWM)
	{
	}

	virtual ~CSynthBase() = default;

	virtual bool Initialize() = 0;
	virtual void HandleMIDIShortMessage(u32 nMessage) { m_MIDIMonitor.OnShortMessage(nMessage); };
	virtual void HandleMIDISysExMessage(const u8* pData, size_t nSize) = 0;
	virtual bool IsActive() = 0;
	virtual void AllSoundOff() { m_MIDIMonitor.AllNotesOff(); };
	virtual void SetMasterVolume(u8 nVolume) = 0;
	virtual size_t Render(s16* pOutBuffer, size_t nFrames) = 0;
	virtual size_t Render(float* pOutBuffer, size_t nFrames) = 0;
	virtual void ReportStatus() const = 0;
	virtual void UpdateLCD(CLCD& LCD, unsigned int nTicks) = 0;
	void SetUserInterface(CUserInterface* pUI) { m_pUI = pUI; }

	// Desired audio output for this synthesizer instance.
	TSynthAudioOutput GetAudioOutput() const { return m_AudioOutput; }
	void SetAudioOutput(TSynthAudioOutput eOutput) { m_AudioOutput = eOutput; }

	CSpinLock m_Lock;
	unsigned int m_nSampleRate;
	CMIDIMonitor m_MIDIMonitor;
	CUserInterface* m_pUI;

private:
	// Stored last so that the explicitly-initialised public members above are
	// initialised in declaration order before this one.
	TSynthAudioOutput m_AudioOutput;
};

#endif
