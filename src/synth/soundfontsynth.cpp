//
// soundfontsynth.cpp
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

#include <fatfs/ff.h>
#include <circle/logger.h>
#include <circle/timer.h>

#include "config.h"
#include "lcd/ui.h"
#include "synth/gmsysex.h"
#include "synth/rolandsysex.h"
#include "synth/soundfontsynth.h"
#include "synth/yamahasysex.h"
#include "utility.h"
#include "zoneallocator.h"

LOGMODULE("soundfontsynth");
const char SoundFontPath[] = "soundfonts";

extern "C"
{
	// Replacements for fluid_sys.c functions
	void* fluid_alloc(size_t len)
	{
		return CZoneAllocator::Get()->Alloc(len, TZoneTag::FluidSynth);
	}

	void* fluid_realloc(void* ptr, size_t len)
	{
		return CZoneAllocator::Get()->Realloc(ptr, len, TZoneTag::FluidSynth);
	}

	void fluid_free(void* ptr)
	{
		CZoneAllocator::Get()->Free(ptr);
	}

	FILE* fluid_file_open(const char* path, const char** errMsg)
	{
		FILE* pFile = fopen(path, "rb");

		if (!pFile && errMsg)
			*errMsg = "Failed to open file";

		return pFile;
	}

	void fluid_msleep(unsigned int msecs) { CTimer::SimpleMsDelay(msecs); }
	double fluid_utime() { return static_cast<double>(CTimer::GetClockTicks()); }

	// Replacements for fluid_sfont.c functions
	// These were found to be much faster than FluidSynth's default approach of going through libc
	void* default_fopen(const char* path)
	{
		FIL* pFile = new FIL;
		if (f_open(pFile, path, FA_READ) != FR_OK)
		{
			delete pFile;
			pFile = nullptr;
		}

		return pFile;
	}

	int default_fclose(void* handle)
	{
		FIL* pFile = static_cast<FIL*>(handle);

		if (f_close(pFile) == FR_OK)
		{
			delete pFile;
			return FLUID_OK;
		}

		return FLUID_FAILED;
	}

	fluid_long_long_t default_ftell(void* handle)
	{
		FIL* pFile = static_cast<FIL*>(handle);
		return f_tell(pFile);
	}

	int safe_fread(void* buf, fluid_long_long_t count, void* fd)
	{
		FIL* pFile = static_cast<FIL*>(fd);
		UINT nRead;
		return f_read(pFile, buf, count, &nRead) == FR_OK ? FLUID_OK : FLUID_FAILED;
	}

	int safe_fseek(void* fd, fluid_long_long_t ofs, int whence)
	{
		FIL* pFile = static_cast<FIL*>(fd);

		switch (whence)
		{
		case SEEK_CUR:
			ofs += f_tell(pFile);
			break;

		case SEEK_END:
			ofs += f_size(pFile);
			break;

		default:
			break;
		}

		return f_lseek(pFile, ofs) == FR_OK ? FLUID_OK : FLUID_FAILED;
	}
}

// Historical temperament tuning tables (12 pitch values in cents, C through B)
static const double TuningPresets[][12] =
{
	// Equal temperament (12-TET)
	{0.0, 100.0, 200.0, 300.0, 400.0, 500.0, 600.0, 700.0, 800.0, 900.0, 1000.0, 1100.0},
	// Werckmeister III
	{0.0, 90.225, 192.180, 294.135, 390.225, 498.045, 588.270, 696.090, 792.180, 888.270, 996.090, 1092.180},
	// Kirnberger III
	{0.0, 90.225, 193.157, 294.135, 386.314, 498.045, 590.225, 696.578, 792.180, 889.735, 996.090, 1088.269},
	// Quarter-comma meantone
	{0.0, 76.049, 193.157, 310.265, 386.314, 503.422, 579.471, 696.578, 772.627, 889.735, 1006.843, 1082.892},
	// Pythagorean
	{0.0, 113.685, 203.910, 294.135, 407.820, 498.045, 611.730, 701.955, 815.640, 905.865, 996.090, 1109.775},
	// Just intonation (5-limit, C major)
	{0.0, 111.731, 203.910, 315.641, 386.314, 498.045, 590.224, 701.955, 813.686, 884.359, 1017.596, 1088.269},
	// Vallotti (Young)
	{0.0, 94.135, 196.090, 298.045, 392.180, 501.955, 592.180, 698.045, 796.090, 894.135, 1000.000, 1090.225},
};

static const char* const TuningPresetNames[] =
{
	"Equal",
	"Werckmeister III",
	"Kirnberger III",
	"Meantone 1/4",
	"Pythagorean",
	"Just Intonation",
	"Vallotti",
};

CSoundFontSynth::CSoundFontSynth(unsigned nSampleRate)
	: CSynthBase(nSampleRate),

	  m_pSettings(nullptr),
	  m_pSynth(nullptr),

	  m_nVolume(100),
	  m_nInitialGain(0.2f),

	  m_bReverbActive(true),
	  m_nReverbRoomSize(0.2f),
	  m_nReverbLevel(0.9f),
	  m_bChorusActive(true),
	  m_nChorusDepth(8.0f),

	  m_nPolyphony(0),
	  m_nPercussionMask(1 << 9),
	  m_nCurrentSoundFontIndex(0),
	  m_nTuningPreset(TuningEqual)
{
}

CSoundFontSynth::~CSoundFontSynth()
{
	if (m_pSynth)
		delete_fluid_synth(m_pSynth);

	if (m_pSettings)
		delete_fluid_settings(m_pSettings);
}

void CSoundFontSynth::FluidSynthLogCallback(int nLevel, const char* pMessage, void* pUser)
{
	CLogger::Get()->Write(From, static_cast<TLogSeverity>(nLevel), pMessage);
}

bool CSoundFontSynth::Initialize()
{
	const CConfig* const pConfig = CConfig::Get();

	if (!m_SoundFontManager.ScanSoundFonts())
		return false;

	// Try to get preferred SoundFont
	m_nCurrentSoundFontIndex = pConfig->FluidSynthSoundFont;
	const char* pSoundFontPath = m_SoundFontManager.GetSoundFontPath(m_nCurrentSoundFontIndex);

	// Fall back on first available SoundFont
	if (!pSoundFontPath)
	{
		pSoundFontPath = m_SoundFontManager.GetFirstValidSoundFontPath();
		m_nCurrentSoundFontIndex = 0;
	}

	// Give up
	if (!pSoundFontPath)
		return false;

	TFXProfile FXProfile = m_SoundFontManager.GetSoundFontFXProfile(m_nCurrentSoundFontIndex);

	// Install logging handlers
	fluid_set_log_function(FLUID_PANIC, FluidSynthLogCallback, this);
	fluid_set_log_function(FLUID_ERR, FluidSynthLogCallback, this);
	fluid_set_log_function(FLUID_WARN, FluidSynthLogCallback, this);
	fluid_set_log_function(FLUID_INFO, FluidSynthLogCallback, this);
	// fluid_set_log_function(FLUID_DBG, FluidSynthLogCallback, this);

	m_pSettings = new_fluid_settings();
	if (!m_pSettings)
	{
		LOGERR("Failed to create settings");
		return false;
	}

	// Set device ID to match the default Roland Sound Canvas ID so that it recognises some GS SysEx messages
	fluid_settings_setint(m_pSettings, "synth.device-id", static_cast<int>(TDeviceID::SoundCanvasDefault));
	fluid_settings_setnum(m_pSettings, "synth.sample-rate", static_cast<double>(m_nSampleRate));
	fluid_settings_setint(m_pSettings, "synth.threadsafe-api", false);
    // FluidSynth 2.6 defaults to the Dattorro reverb; keep the FreeVerb engine used up to 2.5 (lower CPU load, same sound)
    fluid_settings_setstr(m_pSettings, "synth.reverb.engine", "free");
	return Reinitialize(pSoundFontPath, &FXProfile);
}

void CSoundFontSynth::HandleMIDIShortMessage(u32 nMessage)
{
	const u8 nStatus  = nMessage & 0xFF;
	const u8 nChannel = nMessage & 0x0F;
	const u8 nData1   = (nMessage >> 8) & 0xFF;
	const u8 nData2   = (nMessage >> 16) & 0xFF;

	// Handle system real-time messages
	if (nStatus == 0xFF)
	{
		m_Lock.Acquire();
		fluid_synth_system_reset(m_pSynth);
		m_Lock.Release();
		return;
	}

	m_Lock.Acquire();

	// Handle channel messages
	switch (nStatus & 0xF0)
	{
		// Note off
		case 0x80:
			fluid_synth_noteoff(m_pSynth, nChannel, nData1);
			break;

		// Note on
		case 0x90:
			fluid_synth_noteon(m_pSynth, nChannel, nData1, nData2);
			break;

		// Polyphonic key pressure/aftertouch
		case 0xA0:
			fluid_synth_key_pressure(m_pSynth, nChannel, nData1, nData2);
			break;

		// Control change
		case 0xB0:
			fluid_synth_cc(m_pSynth, nChannel, nData1, nData2);
			break;

		// Program change
		case 0xC0:
			fluid_synth_program_change(m_pSynth, nChannel, nData1);
			break;

		// Channel pressure/aftertouch
		case 0xD0:
			fluid_synth_channel_pressure(m_pSynth, nChannel, nData1);
			break;

		// Pitch bend
		case 0xE0:
			fluid_synth_pitch_bend(m_pSynth, nChannel, (nData2 << 7) | nData1);
			break;
	}

	m_Lock.Release();

	// Update MIDI monitor
	CSynthBase::HandleMIDIShortMessage(nMessage);
}

void CSoundFontSynth::HandleMIDISysExMessage(const u8* pData, size_t nSize)
{
	// GM Mode On/Off: reset monitor but still forward to FluidSynth.
	const TGMSysExResult gm = ParseGMSysEx(pData, nSize);
	if (gm.bReset)
	{
		ResetMIDIMonitor();
		// Fall through to FluidSynth forwarding.
	}
	else
	{
		// Roland: display or control messages — apply side effects, then decide.
		bool bConsumed = false;

		const TRolandSysExResult roland = ParseRolandSysEx(pData, nSize);
		if (roland.bReset)
			ResetMIDIMonitor();
		if (roland.bPercChange)
			m_nPercussionMask ^= (-static_cast<int>(roland.nPercMode) ^ m_nPercussionMask) & (1 << roland.nPercChannel);
		if (roland.bDisplayText && m_pUI)
			m_pUI->ShowSysExText(CUserInterface::TSysExDisplayMessage::Roland, roland.pDisplayData, roland.nDisplaySize, roland.nAddressLo);
		if (roland.bDisplayDots && m_pUI)
			m_pUI->ShowSysExBitmap(CUserInterface::TSysExDisplayMessage::Roland, roland.pDisplayData, roland.nDisplaySize);
		bConsumed = roland.bConsume;

		if (!bConsumed)
		{
			// Yamaha: display or control messages.
			const TYamahaSysExResult yamaha = ParseYamahaSysEx(pData, nSize);
			if (yamaha.bReset)
				ResetMIDIMonitor();
			if (yamaha.bDisplayText && m_pUI)
				m_pUI->ShowSysExText(CUserInterface::TSysExDisplayMessage::Yamaha, yamaha.pDisplayData, yamaha.nDisplaySize, yamaha.nAddressLo);
			if (yamaha.bDisplayDots && m_pUI)
				m_pUI->ShowSysExBitmap(CUserInterface::TSysExDisplayMessage::Yamaha, yamaha.pDisplayData, yamaha.nDisplaySize);
			bConsumed = yamaha.bConsume;
		}

		if (bConsumed)
			return;
	}

	// No special handling; forward to FluidSynth SysEx parser, excluding leading 0xF0 and trailing 0xF7
	m_Lock.Acquire();
	fluid_synth_sysex(m_pSynth, reinterpret_cast<const char*>(pData + 1), nSize - 2, nullptr, nullptr, nullptr, false);
	m_Lock.Release();
}

bool CSoundFontSynth::IsActive()
{
	m_Lock.Acquire();
	int nVoices = fluid_synth_get_active_voice_count(m_pSynth);
	m_Lock.Release();

	return nVoices > 0;
}

void CSoundFontSynth::AllSoundOff()
{
	m_Lock.Acquire();
	fluid_synth_all_sounds_off(m_pSynth, -1);
	m_Lock.Release();

	// Reset MIDI monitor
	CSynthBase::AllSoundOff();
}

void CSoundFontSynth::SetMasterVolume(u8 nVolume)
{
	m_nVolume = nVolume;
	m_Lock.Acquire();
	fluid_synth_set_gain(m_pSynth, m_nVolume / 100.0f * m_nInitialGain);
	m_Lock.Release();
}

size_t CSoundFontSynth::Render(float* pOutBuffer, size_t nFrames)
{
	m_Lock.Acquire();
	assert(fluid_synth_write_float(m_pSynth, nFrames, pOutBuffer, 0, 2, pOutBuffer, 1, 2) == FLUID_OK);
	m_Lock.Release();
	return nFrames;
}

size_t CSoundFontSynth::Render(s16* pOutBuffer, size_t nFrames)
{
	m_Lock.Acquire();
	assert(fluid_synth_write_s16(m_pSynth, nFrames, pOutBuffer, 0, 2, pOutBuffer, 1, 2) == FLUID_OK);
	m_Lock.Release();
	return nFrames;
}

void CSoundFontSynth::ReportStatus() const
{
	if (m_pUI)
		m_pUI->ShowSystemMessage(m_SoundFontManager.GetSoundFontName(m_nCurrentSoundFontIndex));
}

void CSoundFontSynth::UpdateLCD(CLCD& LCD, unsigned int nTicks)
{
	const u8 nBarHeight = LCD.Height();
	float ChannelLevels[16], PeakLevels[16];
	m_MIDIMonitor.GetChannelLevels(nTicks, ChannelLevels, PeakLevels, m_nPercussionMask);
	CUserInterface::DrawChannelLevels(LCD, nBarHeight, ChannelLevels, PeakLevels, 16, true);
}

bool CSoundFontSynth::SwitchSoundFont(size_t nIndex)
{
	// Is this SoundFont already active?
	if (m_nCurrentSoundFontIndex == nIndex)
	{
		if (m_pUI)
			m_pUI->ShowSystemMessage("Already selected!");
		return false;
	}

	// Get SoundFont if available
	const char* pSoundFontPath = m_SoundFontManager.GetSoundFontPath(nIndex);
	if (!pSoundFontPath)
	{
		if (m_pUI)
			m_pUI->ShowSystemMessage("SoundFont not avail!");
		return false;
	}

	if (m_pUI)
		m_pUI->ShowSystemMessage("Loading SoundFont", true);

	TFXProfile FXProfile = m_SoundFontManager.GetSoundFontFXProfile(nIndex);

	// We can't use fluid_synth_sfunload() as we don't support the lazy SoundFont unload timer, so trash the entire synth and create a new one
	if (!Reinitialize(pSoundFontPath, &FXProfile))
	{
		if (m_pUI)
			m_pUI->ShowSystemMessage("SF switch failed!");

		return false;
	}

	m_nCurrentSoundFontIndex = nIndex;

	LOGNOTE("Loaded \"%s\"", m_SoundFontManager.GetSoundFontName(nIndex));
	if (m_pUI)
		m_pUI->ClearSpinnerMessage();

	return true;
}

bool CSoundFontSynth::Reinitialize(const char* pSoundFontPath, const TFXProfile* pFXProfile)
{
	const CConfig* const pConfig = CConfig::Get();

	m_Lock.Acquire();

	if (m_pSynth)
		delete_fluid_synth(m_pSynth);

	m_pSynth = new_fluid_synth(m_pSettings);

	if (!m_pSynth)
	{
		m_Lock.Release();
		LOGERR("Failed to create synth");
		return false;
	}

	fluid_synth_set_polyphony(m_pSynth, pConfig->FluidSynthPolyphony);
	m_nPolyphony = pConfig->FluidSynthPolyphony;

	m_nInitialGain = pFXProfile->nGain.ValueOr(pConfig->FluidSynthDefaultGain);
	fluid_synth_set_gain(m_pSynth, m_nVolume / 100.0f * m_nInitialGain);

	// Use values from effects profile if set, otherwise use defaults
	m_bReverbActive   = pFXProfile->bReverbActive.ValueOr(pConfig->FluidSynthDefaultReverbActive);
	m_nReverbDamping  = pFXProfile->nReverbDamping.ValueOr(pConfig->FluidSynthDefaultReverbDamping);
	m_nReverbRoomSize = pFXProfile->nReverbRoomSize.ValueOr(pConfig->FluidSynthDefaultReverbRoomSize);
	m_nReverbLevel    = pFXProfile->nReverbLevel.ValueOr(pConfig->FluidSynthDefaultReverbLevel);
	m_nReverbWidth    = pFXProfile->nReverbWidth.ValueOr(pConfig->FluidSynthDefaultReverbWidth);
	m_bChorusActive   = pFXProfile->bChorusActive.ValueOr(pConfig->FluidSynthDefaultChorusActive);
	m_nChorusDepth    = pFXProfile->nChorusDepth.ValueOr(pConfig->FluidSynthDefaultChorusDepth);
	m_nChorusLevel    = pFXProfile->nChorusLevel.ValueOr(pConfig->FluidSynthDefaultChorusLevel);
	m_nChorusVoices   = pFXProfile->nChorusVoices.ValueOr(pConfig->FluidSynthDefaultChorusVoices);
	m_nChorusSpeed    = pFXProfile->nChorusSpeed.ValueOr(pConfig->FluidSynthDefaultChorusSpeed);

	fluid_synth_reverb_on(m_pSynth, -1, m_bReverbActive);
	fluid_synth_set_reverb_group_damp(m_pSynth, -1, m_nReverbDamping);
	fluid_synth_set_reverb_group_level(m_pSynth, -1, m_nReverbLevel);
	fluid_synth_set_reverb_group_roomsize(m_pSynth, -1, m_nReverbRoomSize);
	fluid_synth_set_reverb_group_width(m_pSynth, -1, m_nReverbWidth);

	fluid_synth_chorus_on(m_pSynth, -1, m_bChorusActive);
	fluid_synth_set_chorus_group_depth(m_pSynth, -1, m_nChorusDepth);
	fluid_synth_set_chorus_group_level(m_pSynth, -1, m_nChorusLevel);
	fluid_synth_set_chorus_group_nr(m_pSynth, -1, m_nChorusVoices);
	fluid_synth_set_chorus_group_speed(m_pSynth, -1, m_nChorusSpeed);

	// Reapply tuning if non-equal
	if (m_nTuningPreset != TuningEqual)
	{
		fluid_synth_activate_octave_tuning(m_pSynth, 0, m_nTuningPreset,
			TuningPresetNames[m_nTuningPreset], TuningPresets[m_nTuningPreset], false);
		for (int ch = 0; ch < 16; ++ch)
			fluid_synth_activate_tuning(m_pSynth, ch, 0, m_nTuningPreset, true);
	}

#ifndef NDEBUG
	DumpFXSettings();
#endif

	ResetMIDIMonitor();

	m_Lock.Release();

	const unsigned int nLoadStart = CTimer::GetClockTicks();

	if (fluid_synth_sfload(m_pSynth, pSoundFontPath, true) == FLUID_FAILED)
	{
		LOGERR("Failed to load SoundFont");
		return false;
	}

	const float nLoadTime = (CTimer::GetClockTicks() - nLoadStart) / 1000000.0f;
	LOGNOTE("\"%s\" loaded in %0.2f seconds", pSoundFontPath, nLoadTime);

	return true;
}

void CSoundFontSynth::ResetMIDIMonitor()
{
	m_MIDIMonitor.AllNotesOff();
	m_MIDIMonitor.ResetControllers(false);
	m_nPercussionMask = 1 << 9;
}

#ifndef NDEBUG
void CSoundFontSynth::DumpFXSettings() const
{
	double nGain, nReverbDamping, nReverbLevel, nReverbRoomSize, nReverbWidth, nChorusDepth, nChorusLevel, nChorusSpeed;
	int nChorusVoices;

	nGain = fluid_synth_get_gain(m_pSynth);

	assert(fluid_synth_get_reverb_group_damp(m_pSynth, -1, &nReverbDamping) == FLUID_OK);
	assert(fluid_synth_get_reverb_group_level(m_pSynth, -1, &nReverbLevel) == FLUID_OK);
	assert(fluid_synth_get_reverb_group_roomsize(m_pSynth, -1, &nReverbRoomSize) == FLUID_OK);
	assert(fluid_synth_get_reverb_group_width(m_pSynth, -1, &nReverbWidth) == FLUID_OK);

	assert(fluid_synth_get_chorus_group_depth(m_pSynth, -1, &nChorusDepth) == FLUID_OK);
	assert(fluid_synth_get_chorus_group_level(m_pSynth, -1, &nChorusLevel) == FLUID_OK);
	assert(fluid_synth_get_chorus_group_nr(m_pSynth, -1, &nChorusVoices) == FLUID_OK);
	assert(fluid_synth_get_chorus_group_speed(m_pSynth, -1, &nChorusSpeed) == FLUID_OK);

	LOGNOTE("Gain: %.2f", nGain);

	LOGNOTE("Reverb: %.2f, %.2f, %.2f, %.2f",
		nReverbDamping,
		nReverbLevel,
		nReverbRoomSize,
		nReverbWidth
	);

	LOGNOTE("Chorus: %.2f, %.2f, %d, %.2f",
		nChorusDepth,
		nChorusLevel,
		nChorusVoices,
		nChorusSpeed
	);
}
#endif

// ---------------------------------------------------------------------------
// SysEx parsers — free functions; no side effects, no class state.
// ---------------------------------------------------------------------------

TGMSysExResult ParseGMSysEx(const u8* pData, size_t nSize)
{
	TGMSysExResult result;

	// Must be at least size of header plus Start/End of Exclusive bytes
	if (nSize < sizeof(TGMSysExHeader) + 2)
		return result;

	const auto& Header = reinterpret_cast<const TGMSysExHeader&>(pData[1]);

	if (Header.ManufacturerID == TManufacturerID::UniversalNonRealTime &&
	    Header.DeviceID == TDeviceID::AllCall &&
	    Header.SubID1 == TUniversalSubID::GeneralMIDI)
	{
		if (Header.SubID2 == TGMSubID::GeneralMIDIOn || Header.SubID2 == TGMSubID::GeneralMIDIOff)
			result.bReset = true;
	}

	return result;
}

TRolandSysExResult ParseRolandSysEx(const u8* pData, size_t nSize)
{
	TRolandSysExResult result;

	// Must be at least size of header plus a data byte, a checksum byte, and Start/End of Exclusive bytes
	if (nSize < sizeof(TRolandSysExHeader) + 4)
		return result;

	const auto& Header = reinterpret_cast<const TRolandSysExHeader&>(pData[1]);

	if (Header.ManufacturerID != TManufacturerID::Roland)
		return result;

	const u32 nAddressHiMed = Header.Address[0] << 16 | Header.Address[1] << 8;
	const u8 nAddressLo = Header.Address[2];
	const u8* pRolandData = pData + sizeof(TRolandSysExHeader) + 1;
	const size_t nRolandDataSize = nSize - sizeof(TRolandSysExHeader) - 3;
	const u8 nChecksum = pData[nSize - 2];

	if (Utility::RolandChecksum(Header.Address, sizeof(Header.Address) + nRolandDataSize) != nChecksum)
		return result;

	result.bValid = true;

	// Single byte GS messages
	if (Header.ModelID == TRolandModelID::GS && nRolandDataSize == 1)
	{
		if ((nAddressHiMed == TRolandAddress::GSReset || nAddressHiMed == TRolandAddress::SystemModeSet) && *pRolandData == 0)
		{
			result.bReset = true;
			// Don't consume; forward to FluidSynth
		}
		else if ((nAddressHiMed & TRolandAddressMask::PatchPart) == TRolandAddress::UseForRhythmPart)
		{
			result.bPercChange  = true;
			result.nPercChannel = Header.Address[1] & 0x0F;
			result.nPercMode    = *pRolandData ? 1 : 0;
			// Don't consume; forward to FluidSynth
		}
	}
	else if (Header.ModelID == TRolandModelID::SC55)
	{
		if (nAddressHiMed == TRolandAddress::SC55DisplayText)
		{
			result.bDisplayText = true;
			result.pDisplayData = pRolandData;
			result.nDisplaySize = nRolandDataSize;
			result.nAddressLo   = nAddressLo;
			result.bConsume     = true;
		}
		else if (nAddressHiMed == TRolandAddress::SC55DisplayDots)
		{
			result.bDisplayDots = true;
			result.pDisplayData = pRolandData;
			result.nDisplaySize = nRolandDataSize;
			result.bConsume     = true;
		}
	}

	return result;
}

TYamahaSysExResult ParseYamahaSysEx(const u8* pData, size_t nSize)
{
	TYamahaSysExResult result;

	// Must be at least size of header plus a data byte and Start/End of Exclusive bytes
	if (nSize < sizeof(TYamahaSysExHeader) + 3)
		return result;

	const auto& Header = reinterpret_cast<const TYamahaSysExHeader&>(pData[1]);

	if (Header.ManufacturerID != TManufacturerID::Yamaha)
		return result;

	const u32 nAddressHiMed = Header.Address[0] << 16 | Header.Address[1] << 8;
	const u8 nAddressLo = Header.Address[2];
	const u8* pYamahaData = pData + sizeof(TYamahaSysExHeader) + 1;
	const size_t nYamahaDataSize = nSize - sizeof(TYamahaSysExHeader) - 2;

	if (Header.ModelID != TYamahaModelID::XG)
		return result;

	result.bValid = true;

	if (nAddressHiMed == TYamahaAddress::XGSystemOn && *pYamahaData == 0)
	{
		result.bReset = true;
		// Don't consume; forward to FluidSynth
	}
	else if (nAddressHiMed == TYamahaAddress::DisplayLetter)
	{
		result.bDisplayText = true;
		result.pDisplayData = pYamahaData;
		result.nDisplaySize = nYamahaDataSize;
		result.nAddressLo   = nAddressLo;
		result.bConsume     = true;
	}
	else if (nAddressHiMed == TYamahaAddress::DisplayBitmap)
	{
		result.bDisplayDots = true;
		result.pDisplayData = pYamahaData;
		result.nDisplaySize = nYamahaDataSize;
		result.bConsume     = true;
	}

	return result;
}

void CSoundFontSynth::SetGain(float nGain)
{
	m_nInitialGain = nGain;
	m_Lock.Acquire();
	if (m_pSynth)
		fluid_synth_set_gain(m_pSynth, m_nVolume / 100.0f * m_nInitialGain);
	m_Lock.Release();
}

void CSoundFontSynth::SetReverbActive(bool bActive)
{
	ApplyFluidParam(m_bReverbActive, bActive,
		[](fluid_synth_t* s, bool v) { fluid_synth_reverb_on(s, -1, v); });
}

void CSoundFontSynth::SetReverbRoomSize(float nRoomSize)
{
	ApplyFluidParam(m_nReverbRoomSize, nRoomSize,
		[](fluid_synth_t* s, float v) { fluid_synth_set_reverb_group_roomsize(s, -1, v); });
}

void CSoundFontSynth::SetReverbLevel(float nLevel)
{
	ApplyFluidParam(m_nReverbLevel, nLevel,
		[](fluid_synth_t* s, float v) { fluid_synth_set_reverb_group_level(s, -1, v); });
}

void CSoundFontSynth::SetReverbDamping(float nDamping)
{
	ApplyFluidParam(m_nReverbDamping, nDamping,
		[](fluid_synth_t* s, float v) { fluid_synth_set_reverb_group_damp(s, -1, v); });
}

void CSoundFontSynth::SetReverbWidth(float nWidth)
{
	ApplyFluidParam(m_nReverbWidth, nWidth,
		[](fluid_synth_t* s, float v) { fluid_synth_set_reverb_group_width(s, -1, v); });
}

void CSoundFontSynth::SetChorusActive(bool bActive)
{
	ApplyFluidParam(m_bChorusActive, bActive,
		[](fluid_synth_t* s, bool v) { fluid_synth_chorus_on(s, -1, v); });
}

void CSoundFontSynth::SetChorusDepth(float nDepth)
{
	ApplyFluidParam(m_nChorusDepth, nDepth,
		[](fluid_synth_t* s, float v) { fluid_synth_set_chorus_group_depth(s, -1, v); });
}

void CSoundFontSynth::SetChorusLevel(float nLevel)
{
	ApplyFluidParam(m_nChorusLevel, nLevel,
		[](fluid_synth_t* s, float v) { fluid_synth_set_chorus_group_level(s, -1, v); });
}

void CSoundFontSynth::SetChorusVoices(int nVoices)
{
	ApplyFluidParam(m_nChorusVoices, nVoices,
		[](fluid_synth_t* s, int v) { fluid_synth_set_chorus_group_nr(s, -1, v); });
}

void CSoundFontSynth::SetChorusSpeed(float nSpeed)
{
	ApplyFluidParam(m_nChorusSpeed, nSpeed,
		[](fluid_synth_t* s, float v) { fluid_synth_set_chorus_group_speed(s, -1, v); });
}

const char* CSoundFontSynth::GetTuningName(int nPreset)
{
	if (nPreset < 0 || nPreset >= TuningCount)
		return "Equal";
	return TuningPresetNames[nPreset];
}

void CSoundFontSynth::SetTuning(int nPreset)
{
	if (nPreset < 0 || nPreset >= TuningCount)
		nPreset = TuningEqual;

	m_nTuningPreset = nPreset;
	m_Lock.Acquire();
	if (m_pSynth)
	{
		if (nPreset == TuningEqual)
		{
			// Deactivate tuning on all channels (return to equal temperament)
			for (int ch = 0; ch < 16; ++ch)
				fluid_synth_deactivate_tuning(m_pSynth, ch, true);
		}
		else
		{
			// Create the octave tuning table (bank 0, prog = preset index)
			fluid_synth_activate_octave_tuning(m_pSynth, 0, nPreset,
				TuningPresetNames[nPreset], TuningPresets[nPreset], false);
			// Activate on all channels
			for (int ch = 0; ch < 16; ++ch)
				fluid_synth_activate_tuning(m_pSynth, ch, 0, nPreset, true);
		}
	}
	m_Lock.Release();
}

void CSoundFontSynth::SetPolyphony(int nPolyphony)
{
	if (nPolyphony < 1 || nPolyphony > 65535)
		return;

	m_nPolyphony = nPolyphony;
	m_Lock.Acquire();
	if (m_pSynth)
		fluid_synth_set_polyphony(m_pSynth, nPolyphony);
	m_Lock.Release();
}

void CSoundFontSynth::SetChannelType(int nChannel, int nType)
{
	if (nChannel < 0 || nChannel >= 16)
		return;
	if (nType != 0 && nType != 1)
		return;

	// Update percussion mask
	m_nPercussionMask ^= (-nType ^ m_nPercussionMask) & (1 << nChannel);

	m_Lock.Acquire();
	if (m_pSynth)
		fluid_synth_set_channel_type(m_pSynth, nChannel, nType);
	m_Lock.Release();
}

const char* CSoundFontSynth::GetChannelInstrumentName(u8 nChannel)
{
	if (!m_pSynth || nChannel >= 16)
		return nullptr;

	fluid_preset_t* pPreset = fluid_synth_get_channel_preset(m_pSynth, nChannel);
	if (!pPreset)
		return nullptr;

	return fluid_preset_get_name(pPreset);
}
