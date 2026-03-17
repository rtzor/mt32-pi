//
// soundfontsynth.h
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

#ifndef _soundfontsynth_h
#define _soundfontsynth_h

#include <circle/types.h>

#include <fluidsynth.h>

#include "soundfontmanager.h"
#include "synth/fxprofile.h"
#include "synth/synthbase.h"

enum TTuningPreset
{
	TuningEqual = 0,
	TuningWerckmeisterIII,
	TuningKirnbergerIII,
	TuningMeantone,
	TuningPythagorean,
	TuningJustIntonation,
	TuningVallotti,
	TuningCount
};

class CSoundFontSynth : public CSynthBase
{
public:
	CSoundFontSynth(unsigned nSampleRate);
	virtual ~CSoundFontSynth() override;

	// CSynthBase
	virtual bool Initialize() override;
	virtual void HandleMIDIShortMessage(u32 nMessage) override;
	virtual void HandleMIDISysExMessage(const u8* pData, size_t nSize) override;
	virtual bool IsActive() override;
	virtual void AllSoundOff() override;
	virtual void SetMasterVolume(u8 nVolume) override;
	virtual size_t Render(s16* pOutBuffer, size_t nFrames) override;
	virtual size_t Render(float* pOutBuffer, size_t nFrames) override;
	virtual void ReportStatus() const override;
	virtual void UpdateLCD(CLCD& LCD, unsigned int nTicks) override;
	virtual const char* GetName() const override { return "FluidSynth"; }
	virtual TSynth GetType() const override { return TSynth::SoundFont; }
	virtual const char* GetChannelInstrumentName(u8 nChannel) override;

	bool SwitchSoundFont(size_t nIndex);
	size_t GetSoundFontIndex() const { return m_nCurrentSoundFontIndex; }
	CSoundFontManager& GetSoundFontManager() { return m_SoundFontManager; }
	fluid_synth_t* GetFluidSynth() const { return m_pSynth; }

	// Real-time FX control
	void SetGain(float nGain);
	float GetGain() const { return m_nInitialGain; }
	void SetReverbActive(bool bActive);
	bool GetReverbActive() const { return m_bReverbActive; }
	void SetReverbDamping(float nDamping);
	float GetReverbDamping() const { return m_nReverbDamping; }
	void SetReverbRoomSize(float nRoomSize);
	float GetReverbRoomSize() const { return m_nReverbRoomSize; }
	void SetReverbLevel(float nLevel);
	float GetReverbLevel() const { return m_nReverbLevel; }
	void SetReverbWidth(float nWidth);
	float GetReverbWidth() const { return m_nReverbWidth; }
	void SetChorusActive(bool bActive);
	bool GetChorusActive() const { return m_bChorusActive; }
	void SetChorusDepth(float nDepth);
	float GetChorusDepth() const { return m_nChorusDepth; }
	void SetChorusLevel(float nLevel);
	float GetChorusLevel() const { return m_nChorusLevel; }
	void SetChorusVoices(int nVoices);
	int GetChorusVoices() const { return m_nChorusVoices; }
	void SetChorusSpeed(float nSpeed);
	float GetChorusSpeed() const { return m_nChorusSpeed; }

	void SetTuning(int nPreset);
	int GetTuning() const { return m_nTuningPreset; }
	static const char* GetTuningName(int nPreset);

	void SetPolyphony(int nPolyphony);
	int GetPolyphony() const { return m_nPolyphony; }

	void SetChannelType(int nChannel, int nType);
	u16 GetPercussionMask() const { return m_nPercussionMask; }

private:
	bool Reinitialize(const char* pSoundFontPath, const TFXProfile* pFXProfile);
	void ResetMIDIMonitor();
#ifndef NDEBUG
	void DumpFXSettings() const;
#endif
	bool ParseGMSysEx(const u8* pData, size_t nSize);
	bool ParseRolandSysEx(const u8* pData, size_t nSize);
	bool ParseYamahaSysEx(const u8* pData, size_t nSize);

	fluid_settings_t* m_pSettings;
	fluid_synth_t* m_pSynth;

	u8 m_nVolume;
	float m_nInitialGain;

	// Cached FX state (kept in sync with FluidSynth)
	bool  m_bReverbActive;
	float m_nReverbDamping;
	float m_nReverbRoomSize;
	float m_nReverbLevel;
	float m_nReverbWidth;
	bool  m_bChorusActive;
	float m_nChorusDepth;
	float m_nChorusLevel;
	int   m_nChorusVoices;
	float m_nChorusSpeed;

	int   m_nPolyphony;
	u16 m_nPercussionMask;
	size_t m_nCurrentSoundFontIndex;
	int   m_nTuningPreset;

	CSoundFontManager m_SoundFontManager;

	static void FluidSynthLogCallback(int nLevel, const char* pMessage, void* pUser);
};

#endif
