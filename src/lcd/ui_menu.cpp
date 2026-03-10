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
#include "synth/mt32synth.h"
#include "synth/soundfontsynth.h"
#include "utility.h"

#include <stdio.h>


// ---------------------------------------------------------------------------
// Helpers (file-static)
// ---------------------------------------------------------------------------

static constexpr size_t MenuVisibleRows = 4;

// Number of menu items per synth
static size_t GetMenuItemCount(const CSynthBase* pCurrent,
                               CSoundFontSynth* pSF,
                               CMT32Synth* pMT32)
{
	if (pCurrent == pSF && pSF)    return 6;
	if (pCurrent == pMT32 && pMT32) return 1;
	return 0;
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
			{ "SoundFont", "Reverb", "Rev.Room", "Rev.Level", "Chorus", "Cho.Depth" };
		return (nItem < 6) ? sfLabels[nItem] : nullptr;
	}
	if (pCurrent == pMT32 && pMT32)
	{
		static const char* mt32Labels[] = { "ROM Set" };
		return (nItem < 1) ? mt32Labels[nItem] : nullptr;
	}
	return nullptr;
}

// Formatted value string for item nItem
static void FormatMenuValue(char* pBuf, size_t nBufSize,
                             const CSynthBase* pCurrent,
                             CSoundFontSynth* pSF,
                             CMT32Synth* pMT32,
                             size_t nItem,
                             bool bReverbActive, float fReverbRoomSize,
                             float fReverbLevel,
                             bool bChorusActive, float fChorusDepth,
                             int nROMSet, int /*nSoundFont*/)
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
		case 1:  snprintf(pBuf, nBufSize, "%s",   bReverbActive   ? "ON" : "OFF"); break;
		case 2:  snprintf(pBuf, nBufSize, "%.1f", static_cast<double>(fReverbRoomSize)); break;
		case 3:  snprintf(pBuf, nBufSize, "%.1f", static_cast<double>(fReverbLevel));    break;
		case 4:  snprintf(pBuf, nBufSize, "%s",   bChorusActive   ? "ON" : "OFF"); break;
		case 5:  snprintf(pBuf, nBufSize, "%.0f", static_cast<double>(fChorusDepth));    break;
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
		default: break;
		}
	}
}

// ---------------------------------------------------------------------------
// CUserInterface menu methods
// ---------------------------------------------------------------------------

void CUserInterface::EnterMenu(CSoundFontSynth* pSF, CMT32Synth* pMT32,
                               CSynthBase* pCurrent)
{
	m_pMenuSF           = pSF;
	m_pMenuMT32         = pMT32;
	m_pMenuCurrentSynth = pCurrent;
	m_nMenuCursor       = 0;
	m_nMenuScroll       = 0;
	m_bMenuEditing      = false;

	// Snapshot current values from active synth
	if (pCurrent == pSF && pSF)
	{
		m_bMenuReverbActive   = pSF->GetReverbActive();
		m_fMenuReverbRoomSize = pSF->GetReverbRoomSize();
		m_fMenuReverbLevel    = pSF->GetReverbLevel();
		m_bMenuChorusActive   = pSF->GetChorusActive();
		m_fMenuChorusDepth    = pSF->GetChorusDepth();
		m_nMenuSoundFont      = static_cast<int>(pSF->GetSoundFontIndex());
		m_nMenuROMSet         = 0;
	}
	else if (pCurrent == pMT32 && pMT32)
	{
		m_bMenuReverbActive   = false;
		m_fMenuReverbRoomSize = 0.0f;
		m_fMenuReverbLevel    = 0.0f;
		m_bMenuChorusActive   = false;
		m_fMenuChorusDepth    = 0.0f;
		m_nMenuSoundFont      = 0;
		m_nMenuROMSet         = static_cast<int>(pMT32->GetROMSet());
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
		GetMenuItemCount(m_pMenuCurrentSynth, m_pMenuSF, m_pMenuMT32);
	if (nItems == 0)
		return true;

	if (m_bMenuEditing)
	{
		// Edit mode: adjust the selected item's value
		if (m_pMenuCurrentSynth == m_pMenuSF && m_pMenuSF)
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
			case 1: // Reverb ON/OFF
				m_bMenuReverbActive = !m_bMenuReverbActive;
				m_pMenuSF->SetReverbActive(m_bMenuReverbActive);
				break;
			case 2: // Reverb room size  [0.0 – 1.0]
				m_fMenuReverbRoomSize = Utility::Clamp(
					m_fMenuReverbRoomSize + nDelta * 0.1f, 0.0f, 1.0f);
				m_pMenuSF->SetReverbRoomSize(m_fMenuReverbRoomSize);
				break;
			case 3: // Reverb level      [0.0 – 1.0]
				m_fMenuReverbLevel = Utility::Clamp(
					m_fMenuReverbLevel + nDelta * 0.1f, 0.0f, 1.0f);
				m_pMenuSF->SetReverbLevel(m_fMenuReverbLevel);
				break;
			case 4: // Chorus ON/OFF
				m_bMenuChorusActive = !m_bMenuChorusActive;
				m_pMenuSF->SetChorusActive(m_bMenuChorusActive);
				break;
			case 5: // Chorus depth      [0.0 – 20.0]
				m_fMenuChorusDepth = Utility::Clamp(
					m_fMenuChorusDepth + nDelta * 1.0f, 0.0f, 20.0f);
				m_pMenuSF->SetChorusDepth(m_fMenuChorusDepth);
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
		GetMenuItemCount(m_pMenuCurrentSynth, m_pMenuSF, m_pMenuMT32);

	for (size_t i = 0; i < MenuVisibleRows; ++i)
	{
		const size_t nItemIdx = m_nMenuScroll + i;
		if (nItemIdx >= nItems)
			break;

		const char* pLabel =
			GetMenuItemLabel(m_pMenuCurrentSynth, m_pMenuSF, m_pMenuMT32, nItemIdx);
		if (!pLabel)
			break;

		char valBuf[7] = "";
		FormatMenuValue(valBuf, sizeof(valBuf),
		                m_pMenuCurrentSynth, m_pMenuSF, m_pMenuMT32,
		                nItemIdx,
		                m_bMenuReverbActive, m_fMenuReverbRoomSize, m_fMenuReverbLevel,
		                m_bMenuChorusActive, m_fMenuChorusDepth,
		                m_nMenuROMSet, m_nMenuSoundFont);

		// 20-char row: selector(1) + label(13) + value(6)
		char rowBuf[21];
		const bool bSelected = (nItemIdx == m_nMenuCursor);
		const char cSel = bSelected ? (m_bMenuEditing ? '*' : '>') : ' ';
		snprintf(rowBuf, sizeof(rowBuf), "%c%-13.13s%6.6s", cSel, pLabel, valBuf);

		LCD.Print(rowBuf, 0, static_cast<u8>(i), /*bClearLine=*/false, /*bImmediate=*/false);
	}
}
