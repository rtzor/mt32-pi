// ui_menu.cpp — Encoder rotary menu for CUserInterface
// Compiled as a separate translation unit; included in the build via Makefile.
//
// IMPORTANT: mt32synth.h must come BEFORE soundfontsynth.h to avoid a
// placement-new exception-specifier conflict.
// mt32synth.h → mt32emu.h → <fstream> → <new> (with noexcept, first)
// soundfontsynth.h → optional.h → circle/new.h (without noexcept, second)
// If circle/new.h comes first, the STL <new> conflicts when it adds noexcept.

#include <circle/logger.h>

#include "lcd/ui.h"
#include <circle/serial.h>
#include "mt32pi.h"
#include "synth/mt32synth.h"
#include "synth/soundfontsynth.h"
#include "synth/ymfmsynth.h"
#include "utility.h"

#include <stdio.h>


// ---------------------------------------------------------------------------
// Helpers (file-static)
// ---------------------------------------------------------------------------

static constexpr size_t MenuVisibleRows = 4;
static constexpr size_t MixerMenuItems  = 5;

// Number of menu items per synth (+ mixer items always appended)
static size_t GetMenuItemCount(const CSynthBase* pCurrent,
                               CSoundFontSynth* pSF,
                               CMT32Synth* pMT32,
                               CMT32Pi* pMT32Pi)
{
	size_t n = 0;
	if (pCurrent == pSF && pSF)    n = 12;
	else if (pCurrent == pMT32 && pMT32) n = 12;
	else if (pCurrent && pCurrent->GetType() == TSynth::Ymfm) n = 3;
	if (n > 0 && pMT32Pi) n += MixerMenuItems;
	return n;
}

// Label for item nItem
static const char* GetMenuItemLabel(const CSynthBase* pCurrent,
                                    CSoundFontSynth* pSF,
                                    CMT32Synth* pMT32,
                                    size_t nItem)
{
	if (pCurrent == pSF && pSF)
	{
		static const char* sfLabels[] =
			{ "SoundFont", "Gain",
			  "Reverb", "Rev.Room", "Rev.Level", "Rev.Damp", "Rev.Width",
			  "Chorus", "Cho.Depth", "Cho.Level", "Cho.Voices", "Cho.Speed" };
		return (nItem < 12) ? sfLabels[nItem] : nullptr;
	}
	if (pCurrent == pMT32 && pMT32)
	{
		static const char* mt32Labels[] = {
			"ROM Set", "Gain", "Rev.Gain",
			"Reverb", "NiceAmp", "NicePan",
			"NiceMix", "DAC", "MIDIDelay",
			"Analog", "Render", "Partials"
		};
		return (nItem < 12) ? mt32Labels[nItem] : nullptr;
	}
	if (pCurrent && pCurrent->GetType() == TSynth::Ymfm)
	{
		static const char* opl3Labels[] = { "Bank", "Chip", "Volume" };
		return (nItem < 3) ? opl3Labels[nItem] : nullptr;
	}
	return nullptr;
}

static const char* GetMixerMenuItemLabel(size_t nMixerIdx)
{
	static const char* mixerLabels[] =
		{ "Mixer", "Preset", "MT32 Vol", "Fluid Vol", "OPL3 Vol" };
	return (nMixerIdx < MixerMenuItems) ? mixerLabels[nMixerIdx] : nullptr;
}

// Formatted value string for item nItem
static void FormatMenuValue(char* pBuf, size_t nBufSize,
                             const CSynthBase* pCurrent,
                             CSoundFontSynth* pSF,
                             CMT32Synth* pMT32,
                             size_t nItem,
							float fGain,
                             bool bReverbActive, float fReverbRoomSize,
							float fReverbLevel,
							float fReverbDamping,
							float fReverbWidth,
							bool bChorusActive, float fChorusDepth,
							float fChorusLevel, int nChorusVoices,
							float fChorusSpeed,
							int nROMSet, int /*nSoundFont*/,
							float fMT32Gain, float fMT32ReverbGain,
							bool bMT32ReverbEnabled,
							bool bMT32NiceAmp, bool bMT32NicePan, bool bMT32NiceMix,
							int nMT32DACMode, int nMT32MIDIDelay,
							int nMT32AnalogMode, int nMT32RendererType,
							int nMT32PartialCount)
{
	pBuf[0] = '\0';

	if (pCurrent == pSF && pSF)
	{
		switch (nItem)
		{
		case 0:
		{
			const size_t nIdx   = pSF->GetSoundFontIndex();
			const size_t nTotal = pSF->GetSoundFontManager().GetSoundFontCount();
			snprintf(pBuf, nBufSize, "%zu/%zu", nIdx + 1, nTotal);
			break;
		}
		case 1:  snprintf(pBuf, nBufSize, "%.2f", static_cast<double>(fGain));            break;
		case 2:  snprintf(pBuf, nBufSize, "%s",   bReverbActive   ? "ON" : "OFF");      break;
		case 3:  snprintf(pBuf, nBufSize, "%.1f", static_cast<double>(fReverbRoomSize));  break;
		case 4:  snprintf(pBuf, nBufSize, "%.1f", static_cast<double>(fReverbLevel));     break;
		case 5:  snprintf(pBuf, nBufSize, "%.1f", static_cast<double>(fReverbDamping));   break;
		case 6:  snprintf(pBuf, nBufSize, "%.1f", static_cast<double>(fReverbWidth));     break;
		case 7:  snprintf(pBuf, nBufSize, "%s",   bChorusActive   ? "ON" : "OFF");      break;
		case 8:  snprintf(pBuf, nBufSize, "%.1f", static_cast<double>(fChorusDepth));     break;
		case 9:  snprintf(pBuf, nBufSize, "%.1f", static_cast<double>(fChorusLevel));     break;
		case 10: snprintf(pBuf, nBufSize, "%d",   nChorusVoices);                         break;
		case 11: snprintf(pBuf, nBufSize, "%.2f", static_cast<double>(fChorusSpeed));     break;
		default: break;
		}
	}
	else if (pCurrent == pMT32 && pMT32)
	{
		switch (nItem)
		{
		case 0:
		{
			static const char* romNames[] = { "MT32old", "MT32new", "CM-32L" };
			const char* pName = (nROMSet >= 0 && nROMSet < 3) ? romNames[nROMSet] : "?";
			snprintf(pBuf, nBufSize, "%s", pName);
			break;
		}
		case 1:  snprintf(pBuf, nBufSize, "%.2f", static_cast<double>(fMT32Gain));       break;
		case 2:  snprintf(pBuf, nBufSize, "%.2f", static_cast<double>(fMT32ReverbGain)); break;
		case 3:  snprintf(pBuf, nBufSize, "%s",   bMT32ReverbEnabled ? "ON" : "OFF");   break;
		case 4:  snprintf(pBuf, nBufSize, "%s",   bMT32NiceAmp ? "ON" : "OFF");         break;
		case 5:  snprintf(pBuf, nBufSize, "%s",   bMT32NicePan ? "ON" : "OFF");         break;
		case 6:  snprintf(pBuf, nBufSize, "%s",   bMT32NiceMix ? "ON" : "OFF");         break;
		case 7:
		{
			static const char* dacNames[] = { "NICE", "PURE", "GEN1", "GEN2" };
			snprintf(pBuf, nBufSize, "%s", (nMT32DACMode >= 0 && nMT32DACMode < 4) ? dacNames[nMT32DACMode] : "?");
			break;
		}
		case 8:
		{
			static const char* delayNames[] = { "IMMD", "SHORT", "ALL" };
			snprintf(pBuf, nBufSize, "%s", (nMT32MIDIDelay >= 0 && nMT32MIDIDelay < 3) ? delayNames[nMT32MIDIDelay] : "?");
			break;
		}
		case 9:
		{
			static const char* analogNames[] = { "DIG", "COARSE", "ACCUR", "OVR" };
			snprintf(pBuf, nBufSize, "%s", (nMT32AnalogMode >= 0 && nMT32AnalogMode < 4) ? analogNames[nMT32AnalogMode] : "?");
			break;
		}
		case 10:
		{
			static const char* rendererNames[] = { "I16", "F32" };
			snprintf(pBuf, nBufSize, "%s", (nMT32RendererType >= 0 && nMT32RendererType < 2) ? rendererNames[nMT32RendererType] : "?");
			break;
		}
		case 11:
			snprintf(pBuf, nBufSize, "%d", nMT32PartialCount);
			break;
		default: break;
		}
	}
}

// ---------------------------------------------------------------------------
// CUserInterface menu methods
// ---------------------------------------------------------------------------

void CUserInterface::EnterMenu(CSoundFontSynth* pSF, CMT32Synth* pMT32,
                               CSynthBase* pCurrent, CMT32Pi* pMT32Pi,
                               CYmfmSynth* pYmfm)
{
	m_pMenuSF           = pSF;
	m_pMenuMT32         = pMT32;
	m_pMenuYmfm         = pYmfm;
	m_pMenuCurrentSynth = pCurrent;
	m_pMenuMT32Pi       = pMT32Pi;
	m_nMenuCursor       = 0;
	m_nMenuScroll       = 0;
	m_bMenuEditing      = false;

	// Snapshot current values from active synth
	if (pCurrent == pSF && pSF)
	{
		m_fMenuGain           = pSF->GetGain();
		m_bMenuReverbActive   = pSF->GetReverbActive();
		m_fMenuReverbDamping  = pSF->GetReverbDamping();
		m_fMenuReverbRoomSize = pSF->GetReverbRoomSize();
		m_fMenuReverbLevel    = pSF->GetReverbLevel();
		m_fMenuReverbWidth    = pSF->GetReverbWidth();
		m_bMenuChorusActive   = pSF->GetChorusActive();
		m_fMenuChorusDepth    = pSF->GetChorusDepth();
		m_fMenuChorusLevel    = pSF->GetChorusLevel();
		m_nMenuChorusVoices   = pSF->GetChorusVoices();
		m_fMenuChorusSpeed    = pSF->GetChorusSpeed();
		m_nMenuSoundFont      = static_cast<int>(pSF->GetSoundFontIndex());
		m_nMenuROMSet         = 0;
	}
	else if (pCurrent == pMT32 && pMT32)
	{
		m_fMenuGain           = 0.0f;
		m_bMenuReverbActive   = false;
		m_fMenuReverbDamping  = 0.0f;
		m_fMenuReverbRoomSize = 0.0f;
		m_fMenuReverbLevel    = 0.0f;
		m_fMenuReverbWidth    = 0.0f;
		m_bMenuChorusActive   = false;
		m_fMenuChorusDepth    = 0.0f;
		m_fMenuChorusLevel    = 0.0f;
		m_nMenuChorusVoices   = 0;
		m_fMenuChorusSpeed    = 0.0f;
		m_nMenuSoundFont      = 0;
		m_nMenuROMSet         = static_cast<int>(pMT32->GetROMSet());
		m_fMenuMT32Gain          = pMT32->GetOutputGain();
		m_fMenuMT32ReverbGain    = pMT32->GetReverbOutputGain();
		m_bMenuMT32ReverbEnabled = pMT32->GetReverbEnabled();
		m_bMenuMT32NiceAmpRamp   = pMT32->GetNiceAmpRamp();
		m_bMenuMT32NicePanning   = pMT32->GetNicePanning();
		m_bMenuMT32NicePartMix   = pMT32->GetNicePartialMixing();
		m_nMenuMT32DACMode       = static_cast<int>(pMT32->GetDACInputMode());
		m_nMenuMT32MIDIDelay     = static_cast<int>(pMT32->GetMIDIDelayMode());
		m_nMenuMT32AnalogMode    = static_cast<int>(pMT32->GetAnalogOutputMode());
		m_nMenuMT32RendererType  = static_cast<int>(pMT32->GetRendererType());
		m_nMenuMT32PartialCount  = static_cast<int>(pMT32->GetPartialCount());
	}
	else if (pCurrent == pYmfm && pYmfm)
	{
		m_nMenuYmfmVol = pMT32Pi ? pMT32Pi->GetMasterVolume() : 100;
	}

	// Initialize mixer values
	if (pMT32Pi)
	{
		const auto ms = pMT32Pi->GetMixerStatus();
		m_bMenuMixerEnabled  = ms.bEnabled;
		m_nMenuMixerPreset   = ms.nPreset;
		m_nMenuMixerMT32Vol  = static_cast<int>(ms.fMT32Volume * 100.0f + 0.5f);
		m_nMenuMixerFluidVol = static_cast<int>(ms.fFluidVolume * 100.0f + 0.5f);
		m_nMenuMixerYmfmVol  = static_cast<int>(ms.fYmfmVolume * 100.0f + 0.5f);
	}

	m_State = TState::InMenu;
}

void CUserInterface::ExitMenu()
{
	m_State = TState::None;
}

bool CUserInterface::MenuEncoderEvent(s8 nDelta)
{
	if (m_State != TState::InMenu)
		return false;

	const size_t nItems =
		GetMenuItemCount(m_pMenuCurrentSynth, m_pMenuSF, m_pMenuMT32, m_pMenuMT32Pi);
	if (nItems == 0)
		return true;

	if (m_bMenuEditing)
	{
		// Mixer items (appended after synth items)
		const size_t nSynthItems = m_pMenuMT32Pi ? (nItems - MixerMenuItems) : nItems;
		if (m_nMenuCursor >= nSynthItems && m_pMenuMT32Pi)
		{
			switch (m_nMenuCursor - nSynthItems)
			{
			case 0: // Mixer ON/OFF
				m_bMenuMixerEnabled = !m_bMenuMixerEnabled;
				m_pMenuMT32Pi->SetMixerEnabled(m_bMenuMixerEnabled);
				break;
			case 1: // Preset (0-3)
				m_nMenuMixerPreset = (m_nMenuMixerPreset + nDelta + 4) % 4;
				m_pMenuMT32Pi->SetMixerPreset(m_nMenuMixerPreset);
				break;
			case 2: // MT32 Vol [0-100]
				m_nMenuMixerMT32Vol = Utility::Clamp(m_nMenuMixerMT32Vol + nDelta, 0, 100);
				m_pMenuMT32Pi->SetMixerEngineVolume("mt32", m_nMenuMixerMT32Vol);
				break;
			case 3: // Fluid Vol [0-100]
				m_nMenuMixerFluidVol = Utility::Clamp(m_nMenuMixerFluidVol + nDelta, 0, 100);
				m_pMenuMT32Pi->SetMixerEngineVolume("fluidsynth", m_nMenuMixerFluidVol);
				break;
			case 4: // OPL3 Vol [0-100]
				m_nMenuMixerYmfmVol = Utility::Clamp(m_nMenuMixerYmfmVol + nDelta, 0, 100);
				m_pMenuMT32Pi->SetMixerEngineVolume("ymfm", m_nMenuMixerYmfmVol);
				break;
			default: break;
			}
		}
		else if (m_pMenuYmfm && m_pMenuCurrentSynth == m_pMenuYmfm && m_pMenuMT32Pi)
		{
			switch (m_nMenuCursor)
			{
			case 0: // Bank — cycle through scanned WOPL banks
			{
				const size_t nBanks = m_pMenuYmfm->GetBankManager().GetBankCount();
				if (nBanks > 0)
				{
					const size_t nNext = static_cast<size_t>(
						(static_cast<int>(m_pMenuYmfm->GetCurrentBankIndex()) + nDelta
						 + static_cast<int>(nBanks)) % static_cast<int>(nBanks));
					m_pMenuYmfm->SwitchBank(nNext);
				}
				break;
			}
			case 1: // Chip — toggle OPL2/OPL3
				if (nDelta != 0)
				{
					const TOplChipMode eCurrent = m_pMenuYmfm->GetChipMode();
					const TOplChipMode eNew = (eCurrent == TOplChipMode::OPL3)
						? TOplChipMode::OPL2 : TOplChipMode::OPL3;
					m_pMenuYmfm->SetChipMode(eNew);
				}
				break;
			case 2: // Volume 0-100
				m_nMenuYmfmVol = Utility::Clamp(m_nMenuYmfmVol + nDelta, 0, 100);
				m_pMenuMT32Pi->SetMasterVolumePercent(m_nMenuYmfmVol);
				break;
			default: break;
			}
		}
		else if (m_pMenuCurrentSynth == m_pMenuSF && m_pMenuSF)
		{
			switch (m_nMenuCursor)
			{
			case 0: // SoundFont index
			{
				const size_t nCount = m_pMenuSF->GetSoundFontManager().GetSoundFontCount();
				if (nCount > 0)
				{
					m_nMenuSoundFont = static_cast<int>(
						(m_nMenuSoundFont + nDelta + static_cast<int>(nCount))
						% static_cast<int>(nCount));
					m_pMenuSF->SwitchSoundFont(static_cast<size_t>(m_nMenuSoundFont));
				}
				break;
			}
			case 1: // Gain [0.0 – 5.0]
				m_fMenuGain = Utility::Clamp(m_fMenuGain + nDelta * 0.05f, 0.0f, 5.0f);
				m_pMenuSF->SetGain(m_fMenuGain);
				break;
			case 2: // Reverb ON/OFF
				m_bMenuReverbActive = !m_bMenuReverbActive;
				m_pMenuSF->SetReverbActive(m_bMenuReverbActive);
				break;
			case 3: // Reverb room size  [0.0 – 1.0]
				m_fMenuReverbRoomSize = Utility::Clamp(
					m_fMenuReverbRoomSize + nDelta * 0.1f, 0.0f, 1.0f);
				m_pMenuSF->SetReverbRoomSize(m_fMenuReverbRoomSize);
				break;
			case 4: // Reverb level      [0.0 – 1.0]
				m_fMenuReverbLevel = Utility::Clamp(
					m_fMenuReverbLevel + nDelta * 0.1f, 0.0f, 1.0f);
				m_pMenuSF->SetReverbLevel(m_fMenuReverbLevel);
				break;
			case 5: // Reverb damping    [0.0 – 1.0]
				m_fMenuReverbDamping = Utility::Clamp(
					m_fMenuReverbDamping + nDelta * 0.1f, 0.0f, 1.0f);
				m_pMenuSF->SetReverbDamping(m_fMenuReverbDamping);
				break;
			case 6: // Reverb width      [0.0 – 100.0]
				m_fMenuReverbWidth = Utility::Clamp(
					m_fMenuReverbWidth + nDelta * 1.0f, 0.0f, 100.0f);
				m_pMenuSF->SetReverbWidth(m_fMenuReverbWidth);
				break;
			case 7: // Chorus ON/OFF
				m_bMenuChorusActive = !m_bMenuChorusActive;
				m_pMenuSF->SetChorusActive(m_bMenuChorusActive);
				break;
			case 8: // Chorus depth      [0.0 – 20.0]
				m_fMenuChorusDepth = Utility::Clamp(
					m_fMenuChorusDepth + nDelta * 0.5f, 0.0f, 20.0f);
				m_pMenuSF->SetChorusDepth(m_fMenuChorusDepth);
				break;
			case 9: // Chorus level      [0.0 – 10.0]
				m_fMenuChorusLevel = Utility::Clamp(
					m_fMenuChorusLevel + nDelta * 0.1f, 0.0f, 10.0f);
				m_pMenuSF->SetChorusLevel(m_fMenuChorusLevel);
				break;
			case 10: // Chorus voices     [0 – 99]
				m_nMenuChorusVoices = Utility::Clamp(m_nMenuChorusVoices + nDelta, 0, 99);
				m_pMenuSF->SetChorusVoices(m_nMenuChorusVoices);
				break;
			case 11: // Chorus speed      [0.01 – 5.0]
				m_fMenuChorusSpeed = Utility::Clamp(
					m_fMenuChorusSpeed + nDelta * 0.05f, 0.01f, 5.0f);
				m_pMenuSF->SetChorusSpeed(m_fMenuChorusSpeed);
				break;
			default:
				break;
			}
		}
		else if (m_pMenuCurrentSynth == m_pMenuMT32 && m_pMenuMT32)
		{
			switch (m_nMenuCursor)
			{
			case 0: // ROM set (3 options)
				m_nMenuROMSet = (m_nMenuROMSet + nDelta + 3) % 3;
				m_pMenuMT32->SwitchROMSet(static_cast<TMT32ROMSet>(m_nMenuROMSet));
				break;
			case 1: // Gain [0.0 – 5.0]
				m_fMenuMT32Gain = Utility::Clamp(m_fMenuMT32Gain + nDelta * 0.1f, 0.0f, 5.0f);
				m_pMenuMT32->SetOutputGain(m_fMenuMT32Gain);
				break;
			case 2: // Reverb gain [0.0 – 5.0]
				m_fMenuMT32ReverbGain = Utility::Clamp(m_fMenuMT32ReverbGain + nDelta * 0.1f, 0.0f, 5.0f);
				m_pMenuMT32->SetReverbOutputGain(m_fMenuMT32ReverbGain);
				break;
			case 3: // Reverb ON/OFF
				m_bMenuMT32ReverbEnabled = !m_bMenuMT32ReverbEnabled;
				m_pMenuMT32->SetReverbEnabled(m_bMenuMT32ReverbEnabled);
				break;
			case 4: // NiceAmp ON/OFF
				m_bMenuMT32NiceAmpRamp = !m_bMenuMT32NiceAmpRamp;
				m_pMenuMT32->SetNiceAmpRamp(m_bMenuMT32NiceAmpRamp);
				break;
			case 5: // NicePan ON/OFF
				m_bMenuMT32NicePanning = !m_bMenuMT32NicePanning;
				m_pMenuMT32->SetNicePanning(m_bMenuMT32NicePanning);
				break;
			case 6: // NiceMix ON/OFF
				m_bMenuMT32NicePartMix = !m_bMenuMT32NicePartMix;
				m_pMenuMT32->SetNicePartialMixing(m_bMenuMT32NicePartMix);
				break;
			case 7: // DAC mode (4 options)
				m_nMenuMT32DACMode = (m_nMenuMT32DACMode + nDelta + 4) % 4;
				m_pMenuMT32->SetDACInputMode(static_cast<MT32Emu::DACInputMode>(m_nMenuMT32DACMode));
				break;
			case 8: // MIDI delay (3 options)
				m_nMenuMT32MIDIDelay = (m_nMenuMT32MIDIDelay + nDelta + 3) % 3;
				m_pMenuMT32->SetMIDIDelayMode(static_cast<MT32Emu::MIDIDelayMode>(m_nMenuMT32MIDIDelay));
				break;
			case 9: // Analog mode (4 options)
				m_nMenuMT32AnalogMode = (m_nMenuMT32AnalogMode + nDelta + 4) % 4;
				m_pMenuMT32->SetAnalogOutputMode(static_cast<MT32Emu::AnalogOutputMode>(m_nMenuMT32AnalogMode));
				break;
			case 10: // Renderer type (2 options)
				m_nMenuMT32RendererType = (m_nMenuMT32RendererType + nDelta + 2) % 2;
				m_pMenuMT32->SetRendererType(static_cast<MT32Emu::RendererType>(m_nMenuMT32RendererType));
				break;
			case 11: // Partial count [8 - 256]
				m_nMenuMT32PartialCount = Utility::Clamp(m_nMenuMT32PartialCount + nDelta, 8, 256);
				m_pMenuMT32->SetPartialCount(static_cast<u32>(m_nMenuMT32PartialCount));
				break;
			default:
				break;
			}
		}
	}
	else
	{
		// Navigation mode: move cursor
		if (nDelta > 0)
		{
			if (m_nMenuCursor + 1 < nItems)
			{
				++m_nMenuCursor;
				if (m_nMenuCursor >= m_nMenuScroll + MenuVisibleRows)
					m_nMenuScroll = m_nMenuCursor - MenuVisibleRows + 1;
			}
		}
		else if (nDelta < 0)
		{
			if (m_nMenuCursor > 0)
			{
				--m_nMenuCursor;
				if (m_nMenuCursor < m_nMenuScroll)
					m_nMenuScroll = m_nMenuCursor;
			}
		}
	}

	return true;
}

bool CUserInterface::MenuSelectEvent()
{
	if (m_State != TState::InMenu)
		return false;

	// Toggle edit mode for the selected item
	m_bMenuEditing = !m_bMenuEditing;
	return true;
}

bool CUserInterface::MenuBackEvent()
{
	if (m_State != TState::InMenu)
		return false;

	if (m_bMenuEditing)
		m_bMenuEditing = false;
	else
		ExitMenu();

	return true;
}

void CUserInterface::DrawMenu(CLCD& LCD) const
{
	if (LCD.GetType() != CLCD::TType::Graphical)
	{
		LCD.Print("[MENU]", 0, 0, true);
		return;
	}

	const size_t nItems =
		GetMenuItemCount(m_pMenuCurrentSynth, m_pMenuSF, m_pMenuMT32, m_pMenuMT32Pi);
	const size_t nSynthItems = m_pMenuMT32Pi ? (nItems > MixerMenuItems ? nItems - MixerMenuItems : 0) : nItems;

	for (size_t i = 0; i < MenuVisibleRows; ++i)
	{
		const size_t nItemIdx = m_nMenuScroll + i;
		if (nItemIdx >= nItems)
			break;

		const char* pLabel = nullptr;
		char valBuf[7] = "";

		if (nItemIdx >= nSynthItems)
		{
			// Mixer item
			const size_t nMixerIdx = nItemIdx - nSynthItems;
			pLabel = GetMixerMenuItemLabel(nMixerIdx);
			switch (nMixerIdx)
			{
			case 0: snprintf(valBuf, sizeof(valBuf), "%s", m_bMenuMixerEnabled ? "ON" : "OFF"); break;
			case 1:
			{
				static const char* presets[] = { "MT32", "Fluid", "Split", "Cust" };
				snprintf(valBuf, sizeof(valBuf), "%s",
					(m_nMenuMixerPreset >= 0 && m_nMenuMixerPreset < 4) ? presets[m_nMenuMixerPreset] : "?");
				break;
			}
			case 2: snprintf(valBuf, sizeof(valBuf), "%d%%", m_nMenuMixerMT32Vol); break;
			case 3: snprintf(valBuf, sizeof(valBuf), "%d%%", m_nMenuMixerFluidVol); break;
			case 4: snprintf(valBuf, sizeof(valBuf), "%d%%", m_nMenuMixerYmfmVol); break;
			default: break;
			}
		}
		else if (m_pMenuYmfm && m_pMenuCurrentSynth == m_pMenuYmfm)
		{
			// OPL3 items — label + value inline
			static const char* opl3Labels[] = { "Bank", "Chip", "Volume" };
			pLabel = (nItemIdx < 3) ? opl3Labels[nItemIdx] : nullptr;
			if (nItemIdx == 0)
				snprintf(valBuf, sizeof(valBuf), "%.6s", m_pMenuYmfm->GetBankName());
			else if (nItemIdx == 1)
				snprintf(valBuf, sizeof(valBuf), "%s",
					m_pMenuYmfm->GetChipMode() == TOplChipMode::OPL3 ? "OPL3" : "OPL2");
			else if (nItemIdx == 2)
				snprintf(valBuf, sizeof(valBuf), "%d%%", m_nMenuYmfmVol);
		}
		else
		{
			pLabel = GetMenuItemLabel(m_pMenuCurrentSynth, m_pMenuSF, m_pMenuMT32, nItemIdx);
			FormatMenuValue(valBuf, sizeof(valBuf),
			                m_pMenuCurrentSynth, m_pMenuSF, m_pMenuMT32,
			                nItemIdx,
			                m_fMenuGain,
			                m_bMenuReverbActive, m_fMenuReverbRoomSize, m_fMenuReverbLevel,
			                m_fMenuReverbDamping, m_fMenuReverbWidth,
			                m_bMenuChorusActive, m_fMenuChorusDepth,
			                m_fMenuChorusLevel, m_nMenuChorusVoices, m_fMenuChorusSpeed,
			                m_nMenuROMSet, m_nMenuSoundFont,
			                m_fMenuMT32Gain, m_fMenuMT32ReverbGain,
			                m_bMenuMT32ReverbEnabled,
			                m_bMenuMT32NiceAmpRamp, m_bMenuMT32NicePanning, m_bMenuMT32NicePartMix,
			                m_nMenuMT32DACMode, m_nMenuMT32MIDIDelay,
			                m_nMenuMT32AnalogMode, m_nMenuMT32RendererType,
			                m_nMenuMT32PartialCount);
		}

		if (!pLabel)
			break;

		// 20-char row: selector(1) + label(13) + value(6)
		char rowBuf[21];
		const bool bSelected = (nItemIdx == m_nMenuCursor);
		const char cSel = bSelected ? (m_bMenuEditing ? '*' : '>') : ' ';
		snprintf(rowBuf, sizeof(rowBuf), "%c%-13.13s%6.6s", cSel, pLabel, valBuf);

		LCD.Print(rowBuf, 0, static_cast<u8>(i), /*bClearLine=*/false, /*bImmediate=*/false);
	}
}
