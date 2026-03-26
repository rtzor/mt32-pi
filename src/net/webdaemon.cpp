//
// webdaemon.cpp
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

#include <circle/logger.h>
#include <circle/serial.h>
#include <circle/startup.h>
#include <circle/string.h>

#include <cctype>
#include <cstdlib>
#include <cstring>

#include <fatfs/ff.h>

#include "mt32pi.h"
#include "net/webdaemon.h"

LOGMODULE("httpd");

namespace
{
	constexpr u16 DefaultPort = 80;
	constexpr unsigned MaxContentSize = 32768;

	// Zero-allocation HTML builder — writes directly into the pre-allocated HTTP
	// response buffer provided by Circle's HTTP daemon.  Every Append() is a
	// single memcpy with no heap involvement, replacing the CString pattern that
	// called new[]+delete[] on every operator+= (O(n²) copies per page).
	struct HtmlWriter
	{
		char* const pStart;
		char*       pCur;
		char* const pEnd;
		bool        bOk;

		HtmlWriter(u8* pBuf, unsigned nCap)
			: pStart(reinterpret_cast<char*>(pBuf))
			, pCur(pStart)
			, pEnd(pStart + nCap)
			, bOk(true) {}

		void Append(const char* pStr)
		{
			if (!bOk || !pStr) return;
			const unsigned n = static_cast<unsigned>(std::strlen(pStr));
			if (pCur + n > pEnd) { bOk = false; return; }
			memcpy(pCur, pStr, n);
			pCur += n;
		}

		void AppendChar(char c)
		{
			if (!bOk) return;
			if (pCur >= pEnd) { bOk = false; return; }
			*pCur++ = c;
		}

		unsigned Length() const { return static_cast<unsigned>(pCur - pStart); }
	};

	constexpr const char* ConfigPath       = "SD:mt32-pi.cfg";
	constexpr const char* ConfigTempPath   = "SD:mt32-pi.cfg.new";
	constexpr const char* ConfigBackupPath = "SD:mt32-pi.cfg.bak";
	constexpr const char* WPAConfigPath    = "SD:wpa_supplicant.conf";

	enum class TConfigSection
	{
		None,
		System,
		Audio,
		MIDI,
		Control,
		MT32Emu,
		FluidSynth,
		Ymfm,
		LCD,
		Network,
	};

	struct TReplacement
	{
		TConfigSection Section;
		const char* pKey;
		CString Value;
		bool bApplied;
	};

	const char* BoolText(bool bValue)
	{
		return bValue ? "On" : "Off";
	}

	const char* NetworkModeText(CConfig::TNetworkMode Mode)
	{
		switch (Mode)
		{
			case CConfig::TNetworkMode::Ethernet:
				return "Ethernet";
			case CConfig::TNetworkMode::WiFi:
				return "Wi-Fi";
			case CConfig::TNetworkMode::Off:
			default:
				return "Off";
		}
	}

	const char* AudioOutputText(CConfig::TAudioOutputDevice Device)
	{
		switch (Device)
		{
			case CConfig::TAudioOutputDevice::PWM:
				return "PWM";
			case CConfig::TAudioOutputDevice::HDMI:
				return "HDMI";
			case CConfig::TAudioOutputDevice::I2S:
				return "I2S";
			default:
				return "Unknown";
		}
	}

	const char* ControlSchemeText(CConfig::TControlScheme Scheme)
	{
		switch (Scheme)
		{
			case CConfig::TControlScheme::SimpleButtons:
				return "Simple buttons";
			case CConfig::TControlScheme::SimpleEncoder:
				return "Simple encoder";
			case CConfig::TControlScheme::None:
			default:
				return "None";
		}
	}

	const char* LCDTypeText(CConfig::TLCDType Type)
	{
		switch (Type)
		{
			case CConfig::TLCDType::HD44780FourBit:
				return "HD44780 4-bit";
			case CConfig::TLCDType::HD44780I2C:
				return "HD44780 I2C";
			case CConfig::TLCDType::SH1106I2C:
				return "SH1106 I2C";
			case CConfig::TLCDType::SSD1306I2C:
				return "SSD1306 I2C";
			case CConfig::TLCDType::None:
			default:
				return "None";
		}
	}

	const char* DefaultSynthText(CConfig::TSystemDefaultSynth Synth)
	{
		switch (Synth)
		{
			case CConfig::TSystemDefaultSynth::MT32:
				return "MT-32";
			case CConfig::TSystemDefaultSynth::Ymfm:
				return "OPL3";
			case CConfig::TSystemDefaultSynth::SoundFont:
			default:
				return "SoundFont";
		}
	}

	const char* EncoderTypeText(CConfig::TEncoderType Type)
	{
		switch (Type)
		{
			case CConfig::TEncoderType::Quarter:
				return "Quarter";
			case CConfig::TEncoderType::Half:
				return "Half";
			case CConfig::TEncoderType::Full:
			default:
				return "Full";
		}
	}

	const char* MT32ROMSetText(TMT32ROMSet ROMSet)
	{
		switch (ROMSet)
		{
			case TMT32ROMSet::MT32Old:
				return "MT-32 old";
			case TMT32ROMSet::MT32New:
				return "MT-32 new";
			case TMT32ROMSet::CM32L:
				return "CM-32L";
			case TMT32ROMSet::Any:
			default:
				return "Any";
		}
	}

	const char* RotationText(CConfig::TLCDRotation Rotation)
	{
		switch (Rotation)
		{
			case CConfig::TLCDRotation::Inverted:
				return "Inverted";
			case CConfig::TLCDRotation::Normal:
			default:
				return "Normal";
		}
	}

	const char* MirrorText(CConfig::TLCDMirror Mirror)
	{
		switch (Mirror)
		{
			case CConfig::TLCDMirror::Mirrored:
				return "Mirrored";
			case CConfig::TLCDMirror::Normal:
			default:
				return "Normal";
		}
	}

	const char* SelectedAttr(bool bSelected)
	{
		return bSelected ? " selected" : "";
	}

	void AppendEscaped(HtmlWriter& Out, const char* pText)
	{
		if (!pText)
		{
			Out.Append("-");
			return;
		}

		for (const char* pCurrent = pText; *pCurrent; ++pCurrent)
		{
			switch (*pCurrent)
			{
				case '&':
					Out.Append("&amp;");
					break;
				case '<':
					Out.Append("&lt;");
					break;
				case '>':
					Out.Append("&gt;");
					break;
				case '"':
					Out.Append("&quot;");
					break;
				default:
					Out.AppendChar(*pCurrent);
					break;
			}
		}
	}

	void AppendRow(HtmlWriter& Out, const char* pLabel, const char* pValue)
	{
		Out.Append("<tr><th>");
		AppendEscaped(Out, pLabel);
		Out.Append("</th><td>");
		AppendEscaped(Out, pValue);
		Out.Append("</td></tr>");
	}

	void AppendIntRow(HtmlWriter& Out, const char* pLabel, int nValue)
	{
		CString Value;
		Value.Format("%d", nValue);
		AppendRow(Out, pLabel, Value);
	}

	void AppendFloatRow(HtmlWriter& Out, const char* pLabel, float nValue)
	{
		CString Value;
		Value.Format("%.2f", nValue);
		AppendRow(Out, pLabel, Value);
	}

	void AppendSectionStart(HtmlWriter& Out, const char* pTitle)
	{
		Out.Append("<section><h2>");
		AppendEscaped(Out, pTitle);
		Out.Append("</h2><table>");
	}

	void AppendSectionEnd(HtmlWriter& Out)
	{
		Out.Append("</table></section>");
	}

	void AppendJSONEscaped(CString& Out, const char* pText)
	{
		if (!pText)
		{
			Out += "-";
			return;
		}

		for (const char* pCurrent = pText; *pCurrent; ++pCurrent)
		{
			switch (*pCurrent)
			{
				case '\\':
					Out += "\\\\";
					break;
				case '"':
					Out += "\\\"";
					break;
				case '\n':
					Out += "\\n";
					break;
				case '\r':
					Out += "\\r";
					break;
				case '\t':
					Out += "\\t";
					break;
				default:
					Out += *pCurrent;
					break;
			}
		}
	}

	void AppendJSONPair(CString& Out, const char* pKey, const char* pValue, bool bComma = true)
	{
		Out += "\"";
		Out += pKey;
		Out += "\":\"";
		AppendJSONEscaped(Out, pValue);
		Out += "\"";
		if (bComma)
			Out += ",";
	}

	void AppendJSONPairBool(CString& Out, const char* pKey, bool bValue, bool bComma = true)
	{
		Out += "\"";
		Out += pKey;
		Out += "\":";
		Out += bValue ? "true" : "false";
		if (bComma)
			Out += ",";
	}

	void AppendJSONPairInt(CString& Out, const char* pKey, int nValue, bool bComma = true)
	{
		CString Value;
		Value.Format("%d", nValue);

		Out += "\"";
		Out += pKey;
		Out += "\":";
		Out += Value;
		if (bComma)
			Out += ",";
	}

	void AppendJSONPairFloat(CString& Out, const char* pKey, float nValue, bool bComma = true)
	{
		CString Value;
		Value.Format("%.4f", nValue);

		Out += "\"";
		Out += pKey;
		Out += "\":";
		Out += Value;
		if (bComma)
			Out += ",";
	}

	bool ParseIntStrict(const char* pText, int& nOut)
	{
		if (!pText || !*pText)
			return false;

		char* pEnd = nullptr;
		const long nValue = std::strtol(pText, &pEnd, 10);
		if (pEnd == pText)
			return false;

		while (pEnd && *pEnd)
		{
			if (!std::isspace(static_cast<unsigned char>(*pEnd)))
				return false;
			++pEnd;
		}

		nOut = static_cast<int>(nValue);
		return true;
	}

	bool ParseFloatStrict(const char* pText, float& nOut)
	{
		if (!pText || !*pText)
			return false;

		char* pEnd = nullptr;
		const float nValue = std::strtof(pText, &pEnd);
		if (pEnd == pText)
			return false;

		while (pEnd && *pEnd)
		{
			if (!std::isspace(static_cast<unsigned char>(*pEnd)))
				return false;
			++pEnd;
		}

		nOut = nValue;
		return true;
	}

	const char* SkipSpaces(const char* pText)
	{
		while (pText && *pText && std::isspace(static_cast<unsigned char>(*pText)))
			++pText;
		return pText;
	}

	char FromHex(char c)
	{
		if (c >= '0' && c <= '9')
			return c - '0';
		if (c >= 'a' && c <= 'f')
			return 10 + c - 'a';
		if (c >= 'A' && c <= 'F')
			return 10 + c - 'A';
		return 0;
	}

	void URLDecode(const char* pInput, char* pOutput, size_t nOutputSize)
	{
		if (!pOutput || nOutputSize == 0)
			return;

		size_t nOut = 0;
		for (size_t i = 0; pInput && pInput[i] && nOut + 1 < nOutputSize; ++i)
		{
			const char c = pInput[i];
			if (c == '+')
			{
				pOutput[nOut++] = ' ';
			}
			else if (c == '%' && pInput[i + 1] && pInput[i + 2])
			{
				const char hi = FromHex(pInput[i + 1]);
				const char lo = FromHex(pInput[i + 2]);
				pOutput[nOut++] = static_cast<char>((hi << 4) | lo);
				i += 2;
			}
			else
			{
				pOutput[nOut++] = c;
			}
		}

		pOutput[nOut] = '\0';
	}

	bool GetFormValue(const char* pFormData, const char* pKey, char* pOutValue, size_t nOutSize)
	{
		if (!pFormData || !pKey || !pOutValue || nOutSize == 0)
			return false;

		const size_t nKeyLen = std::strlen(pKey);
		const char* pCurrent = pFormData;

		while (*pCurrent)
		{
			const char* pPairEnd = std::strchr(pCurrent, '&');
			if (!pPairEnd)
				pPairEnd = pCurrent + std::strlen(pCurrent);

			const char* pEqual = std::strchr(pCurrent, '=');
			if (pEqual && pEqual < pPairEnd)
			{
				const size_t nParamKeyLen = static_cast<size_t>(pEqual - pCurrent);
				if (nParamKeyLen == nKeyLen && std::strncmp(pCurrent, pKey, nKeyLen) == 0)
				{
					char EncodedValue[256];
					const size_t nRawLen = static_cast<size_t>(pPairEnd - (pEqual + 1));
					const size_t nCopyLen = nRawLen < sizeof(EncodedValue) - 1 ? nRawLen : sizeof(EncodedValue) - 1;
					std::memcpy(EncodedValue, pEqual + 1, nCopyLen);
					EncodedValue[nCopyLen] = '\0';

					URLDecode(EncodedValue, pOutValue, nOutSize);
					return true;
				}
			}

			pCurrent = *pPairEnd ? pPairEnd + 1 : pPairEnd;
		}

		return false;
	}

	bool ReadLine(FIL* pFile, char* pBuffer, size_t nBufferSize, bool& bEOF)
	{
		if (!pFile || !pBuffer || nBufferSize < 2)
			return false;

		size_t nOut = 0;
		bEOF = false;

		while (nOut + 1 < nBufferSize)
		{
			char c = 0;
			UINT nRead = 0;
			if (f_read(pFile, &c, 1, &nRead) != FR_OK)
				return false;
			if (nRead == 0)
			{
				bEOF = true;
				break;
			}

			if (c == '\n')
				break;
			if (c == '\r')
				continue;

			pBuffer[nOut++] = c;
		}

		pBuffer[nOut] = '\0';
		return nOut > 0 || !bEOF;
	}

	bool WriteLine(FIL* pFile, const char* pLine)
	{
		if (!pFile || !pLine)
			return false;

		UINT nWritten = 0;
		const UINT nLen = static_cast<UINT>(std::strlen(pLine));
		if (f_write(pFile, pLine, nLen, &nWritten) != FR_OK || nWritten != nLen)
			return false;

		if (f_write(pFile, "\n", 1, &nWritten) != FR_OK || nWritten != 1)
			return false;

		return true;
	}

	TConfigSection ParseSection(const char* pLine)
	{
		const char* pTrimmed = SkipSpaces(pLine);
		if (std::strcmp(pTrimmed, "[system]") == 0)
			return TConfigSection::System;
		if (std::strcmp(pTrimmed, "[audio]") == 0)
			return TConfigSection::Audio;
		if (std::strcmp(pTrimmed, "[midi]") == 0)
			return TConfigSection::MIDI;
		if (std::strcmp(pTrimmed, "[control]") == 0)
			return TConfigSection::Control;
		if (std::strcmp(pTrimmed, "[mt32emu]") == 0)
			return TConfigSection::MT32Emu;
		if (std::strcmp(pTrimmed, "[fluidsynth]") == 0)
			return TConfigSection::FluidSynth;
		if (std::strcmp(pTrimmed, "[ymfm]") == 0)
			return TConfigSection::Ymfm;
		if (std::strcmp(pTrimmed, "[lcd]") == 0)
			return TConfigSection::LCD;
		if (std::strcmp(pTrimmed, "[network]") == 0)
			return TConfigSection::Network;
		return TConfigSection::None;
	}

	bool IsAssignmentForKey(const char* pLine, const char* pKey)
	{
		const char* pTrimmed = SkipSpaces(pLine);
		const size_t nKeyLen = std::strlen(pKey);

		if (std::strncmp(pTrimmed, pKey, nKeyLen) != 0)
			return false;

		pTrimmed += nKeyLen;
		while (*pTrimmed && std::isspace(static_cast<unsigned char>(*pTrimmed)))
			++pTrimmed;

		return *pTrimmed == '=';
	}

	bool SaveConfigWithBackup(const TReplacement* pReplacements, size_t nReplacements, CString& Error)
	{
		FIL Src;
		FIL Dst;

		if (f_open(&Src, ConfigPath, FA_READ) != FR_OK)
		{
			Error = "Unable to open config file";
			return false;
		}

		if (f_open(&Dst, ConfigTempPath, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK)
		{
			f_close(&Src);
			Error = "Unable to open temp config file";
			return false;
		}

		char Line[512];
		bool bEOF = false;
		TConfigSection CurrentSection = TConfigSection::None;

		while (ReadLine(&Src, Line, sizeof(Line), bEOF))
		{
			CurrentSection = ParseSection(Line) != TConfigSection::None ? ParseSection(Line) : CurrentSection;

			bool bReplaced = false;
			for (size_t i = 0; i < nReplacements; ++i)
			{
				if (pReplacements[i].Section == CurrentSection && IsAssignmentForKey(Line, pReplacements[i].pKey))
				{
					CString NewLine;
					NewLine += pReplacements[i].pKey;
					NewLine += " = ";
					NewLine += pReplacements[i].Value;

					if (!WriteLine(&Dst, NewLine))
					{
						f_close(&Src);
						f_close(&Dst);
						Error = "Failed writing updated config line";
						return false;
					}

					const_cast<TReplacement*>(pReplacements)[i].bApplied = true;
					bReplaced = true;
					break;
				}
			}

			if (!bReplaced)
			{
				if (!WriteLine(&Dst, Line))
				{
					f_close(&Src);
					f_close(&Dst);
					Error = "Failed copying config line";
					return false;
				}
			}

			if (bEOF)
				break;
		}

		f_close(&Src);
		f_close(&Dst);

		f_unlink(ConfigBackupPath);
		if (f_rename(ConfigPath, ConfigBackupPath) != FR_OK)
		{
			f_unlink(ConfigTempPath);
			Error = "Unable to create config backup";
			return false;
		}

		if (f_rename(ConfigTempPath, ConfigPath) != FR_OK)
		{
			f_rename(ConfigBackupPath, ConfigPath);
			Error = "Unable to activate new config";
			return false;
		}

		return true;
	}

	// Append all synth/mixer runtime fields to a JSON object (no leading '{', no trailing '}').
	// Used by both /api/runtime/status and /api/runtime/set to produce identical payloads.
	void AppendSynthStatusJSON(CString& JSON, const CMT32Pi::TSystemState& st)
	{
		AppendJSONPair(JSON, "active_synth",    st.pActiveSynthName);
		AppendJSONPair(JSON, "mt32_rom_name",   st.pMT32ROMName);
		AppendJSONPair(JSON, "soundfont_name",  st.pSoundFontName);
		AppendJSONPairInt(JSON, "mt32_rom_set",     st.nMT32ROMSetIndex);
		AppendJSONPairInt(JSON, "soundfont_index",  static_cast<int>(st.nSoundFontIndex));
		AppendJSONPairInt(JSON, "soundfont_count",  static_cast<int>(st.nSoundFontCount));
		AppendJSONPairInt(JSON, "master_volume",    st.nMasterVolume);
		AppendJSONPairBool(JSON, "sf_available",    st.bSFFXAvailable);
		AppendJSONPairFloat(JSON, "sf_gain",         st.bSFFXAvailable ? st.fSFGain          : 0.0f);
		AppendJSONPairBool(JSON, "sf_reverb_active", st.bSFFXAvailable ? st.bSFReverbActive  : false);
		AppendJSONPairFloat(JSON, "sf_reverb_room",  st.bSFFXAvailable ? st.fSFReverbRoom    : 0.0f);
		AppendJSONPairFloat(JSON, "sf_reverb_level", st.bSFFXAvailable ? st.fSFReverbLevel   : 0.0f);
		AppendJSONPairFloat(JSON, "sf_reverb_damping", st.bSFFXAvailable ? st.fSFReverbDamping : 0.0f);
		AppendJSONPairFloat(JSON, "sf_reverb_width", st.bSFFXAvailable ? st.fSFReverbWidth   : 0.0f);
		AppendJSONPairBool(JSON, "sf_chorus_active", st.bSFFXAvailable ? st.bSFChorusActive  : false);
		AppendJSONPairFloat(JSON, "sf_chorus_depth", st.bSFFXAvailable ? st.fSFChorusDepth   : 0.0f);
		AppendJSONPairFloat(JSON, "sf_chorus_level", st.bSFFXAvailable ? st.fSFChorusLevel   : 0.0f);
		AppendJSONPairInt(JSON, "sf_chorus_voices",  st.bSFFXAvailable ? st.nSFChorusVoices  : 1);
		AppendJSONPairFloat(JSON, "sf_chorus_speed", st.bSFFXAvailable ? st.fSFChorusSpeed   : 0.0f);
		AppendJSONPairInt(JSON, "sf_tuning",         st.nSFTuning);
		AppendJSONPair(JSON, "sf_tuning_name",       st.pSFTuningName);
		AppendJSONPairInt(JSON, "sf_polyphony",      st.nSFPolyphony);
		AppendJSONPairInt(JSON, "sf_percussion_mask", static_cast<int>(st.nSFPercussionMask));
		AppendJSONPairFloat(JSON, "mt32_reverb_gain",  st.fMT32ReverbGain);
		AppendJSONPairBool(JSON, "mt32_reverb_active", st.bMT32ReverbActive);
		AppendJSONPairBool(JSON, "mt32_nice_amp",      st.bMT32NiceAmpRamp);
		AppendJSONPairBool(JSON, "mt32_nice_pan",      st.bMT32NicePanning);
		AppendJSONPairBool(JSON, "mt32_nice_mix",      st.bMT32NicePartialMixing);
		AppendJSONPairInt(JSON, "mt32_dac_mode",       st.nMT32DACMode);
		AppendJSONPairInt(JSON, "mt32_midi_delay",     st.nMT32MIDIDelayMode);
		AppendJSONPairInt(JSON, "mt32_analog_mode",    st.nMT32AnalogMode);
		AppendJSONPairInt(JSON, "mt32_renderer_type",  st.nMT32RendererType);
		AppendJSONPairInt(JSON, "mt32_partial_count",  st.nMT32PartialCount);
		AppendJSONPairBool(JSON, "mixer_enabled",  st.Mixer.bEnabled);
		AppendJSONPairInt(JSON, "mixer_preset",    st.Mixer.nPreset);
		AppendJSONPairBool(JSON, "mixer_dual_mode", st.Mixer.bDualMode);
		AppendJSONPairFloat(JSON, "ymfm_volume",     st.Mixer.fYmfmVolume);
		AppendJSONPairBool(JSON, "ymfm_available",  st.bYmfmAvailable);
		AppendJSONPair(JSON,     "ymfm_bank_name",  st.pYmfmBankName ? st.pYmfmBankName : "");
		AppendJSONPairBool(JSON, "ymfm_chip_is_opl3", st.bYmfmChipIsOPL3, false);
	}
}

CWebDaemon::CWebDaemon(CNetSubSystem* pNetSubSystem, CMT32Pi* pMT32Pi, u16 nPort)
	: CHTTPDaemon(pNetSubSystem, nullptr, MaxContentSize, nPort == 0 ? DefaultPort : nPort),
	  m_pMT32Pi(pMT32Pi),
	  m_nPort(nPort == 0 ? DefaultPort : nPort)
{
}

CWebDaemon::CWebDaemon(CNetSubSystem* pNetSubSystem, CMT32Pi* pMT32Pi, CSocket* pSocket, u16 nPort)
	: CHTTPDaemon(pNetSubSystem, pSocket, MaxContentSize, nPort == 0 ? DefaultPort : nPort),
	  m_pMT32Pi(pMT32Pi),
	  m_nPort(nPort == 0 ? DefaultPort : nPort)
{
}

CWebDaemon::~CWebDaemon()
{
}

CHTTPDaemon* CWebDaemon::CreateWorker(CNetSubSystem* pNetSubSystem, CSocket* pSocket)
{
	return new CWebDaemon(pNetSubSystem, m_pMT32Pi, pSocket, m_nPort);
}

THTTPStatus CWebDaemon::GetContent(const char* pPath,
	const char* pParams,
	const char* pFormData,
	u8* pBuffer,
	unsigned* pLength,
	const char** ppContentType)
{
	(void) pParams;

	if (!pPath || !pBuffer || !pLength || !ppContentType)
		return HTTPInternalServerError;

	if (!strcmp(pPath, "/health"))
	{
		const char* pBody = "OK\n";
		const unsigned nBodyLength = 3;

		if (*pLength < nBodyLength)
			return HTTPInternalServerError;

		memcpy(pBuffer, pBody, nBodyLength);
		*pLength = nBodyLength;
		*ppContentType = "text/plain";
		return HTTPOK;
	}

	if (!m_pMT32Pi)
		return HTTPInternalServerError;

	// Static assets
	if (!strcmp(pPath, "/app.css"))
		return BuildStylesheet(pBuffer, pLength, ppContentType);
	if (!strcmp(pPath, "/app.js"))
		return BuildScript(pBuffer, pLength, ppContentType);

	// HTML pages
	if (!strcmp(pPath, "/") || !strcmp(pPath, "/index.html"))
		return BuildStatusPage(pBuffer, pLength, ppContentType);
	if (!strcmp(pPath, "/sound"))
		return BuildSoundPage(pBuffer, pLength, ppContentType);
	if (!strcmp(pPath, "/sequencer"))
		return BuildSequencerPage(pBuffer, pLength, ppContentType);
	if (!strcmp(pPath, "/mixer"))
		return BuildMixerPage(pBuffer, pLength, ppContentType);
	if (!strcmp(pPath, "/monitor"))
		return BuildMonitorPage(pBuffer, pLength, ppContentType);
	if (!strcmp(pPath, "/config"))
		return BuildConfigPage(pBuffer, pLength, ppContentType);

	// API endpoints
	if (strncmp(pPath, "/api/", 5) == 0)
		return HandleAPIRequest(pPath, pParams, pFormData, pBuffer, pLength, ppContentType);

	return HTTPNotFound;
}

THTTPStatus CWebDaemon::HandleAPIRequest(const char* pPath,
	const char* pParams,
	const char* pFormData,
	u8* pBuffer,
	unsigned* pLength,
	const char** ppContentType)
{
	const bool bIsStatusAPIPath = strcmp(pPath, "/api/status") == 0;
	const bool bIsMIDIAPIPath = strcmp(pPath, "/api/midi") == 0;
 	const bool bIsConfigSavePath = strcmp(pPath, "/api/config/save") == 0;
	const bool bIsRuntimeStatusPath = strcmp(pPath, "/api/runtime/status") == 0;
	const bool bIsRuntimeSetPath = strcmp(pPath, "/api/runtime/set") == 0;
	const bool bIsSystemRebootPath = strcmp(pPath, "/api/system/reboot") == 0;
	const bool bIsSeqStatusPath    = strcmp(pPath, "/api/sequencer/status") == 0;
	const bool bIsSeqPlayPath      = strcmp(pPath, "/api/sequencer/play") == 0;
	const bool bIsSeqStopPath      = strcmp(pPath, "/api/sequencer/stop") == 0;
	const bool bIsSeqPausePath     = strcmp(pPath, "/api/sequencer/pause") == 0;
	const bool bIsSeqResumePath    = strcmp(pPath, "/api/sequencer/resume") == 0;
	const bool bIsSeqNextPath      = strcmp(pPath, "/api/sequencer/next") == 0;
	const bool bIsSeqPrevPath      = strcmp(pPath, "/api/sequencer/prev") == 0;
	const bool bIsSeqAutoNextPath  = strcmp(pPath, "/api/sequencer/autonext") == 0;
	const bool bIsSeqFilesPath      = strcmp(pPath, "/api/sequencer/files") == 0;
	const bool bIsSeqLoopPath       = strcmp(pPath, "/api/sequencer/loop")  == 0;
	const bool bIsSeqSeekPath       = strcmp(pPath, "/api/sequencer/seek")  == 0;
	const bool bIsSeqTempoPath      = strcmp(pPath, "/api/sequencer/tempo") == 0;
	const bool bIsMidiNotePath      = strcmp(pPath, "/api/midi/note")        == 0;
	const bool bIsMidiRawPath       = strcmp(pPath, "/api/midi/raw")         == 0;
	const bool bIsMidiLogPath       = strcmp(pPath, "/api/midi/log")         == 0;
	const bool bIsMidiLogClearPath  = strcmp(pPath, "/api/midi/log/clear")   == 0;
	const bool bIsWifiReadPath      = strcmp(pPath, "/api/wifi/read") == 0;
	const bool bIsWifiSavePath      = strcmp(pPath, "/api/wifi/save") == 0;
	const bool bIsMixerStatusPath   = strcmp(pPath, "/api/mixer/status") == 0;
	const bool bIsMixerSetPath      = strcmp(pPath, "/api/mixer/set") == 0;
	const bool bIsMixerPresetPath   = strcmp(pPath, "/api/mixer/preset") == 0;
	const bool bIsRouterSavePath    = strcmp(pPath, "/api/router/save") == 0;
	const bool bIsRouterLoadPath    = strcmp(pPath, "/api/router/load") == 0;
	const bool bIsSFInfoPath        = strcmp(pPath, "/api/soundfont/info") == 0;
	const bool bIsRecStartPath      = strcmp(pPath, "/api/recorder/start") == 0;
	const bool bIsRecStopPath       = strcmp(pPath, "/api/recorder/stop")  == 0;
	const bool bIsPlListPath    = strcmp(pPath, "/api/playlist")         == 0;
	const bool bIsPlAddPath     = strcmp(pPath, "/api/playlist/add")     == 0;
	const bool bIsPlRemovePath  = strcmp(pPath, "/api/playlist/remove")  == 0;
	const bool bIsPlClearPath   = strcmp(pPath, "/api/playlist/clear")   == 0;
	const bool bIsPlUpPath      = strcmp(pPath, "/api/playlist/up")      == 0;
	const bool bIsPlDownPath    = strcmp(pPath, "/api/playlist/down")    == 0;
	const bool bIsPlShufflePath = strcmp(pPath, "/api/playlist/shuffle") == 0;
	const bool bIsPlRepeatPath  = strcmp(pPath, "/api/playlist/repeat")  == 0;
	const bool bIsPlPlayPath    = strcmp(pPath, "/api/playlist/play")    == 0;
	const bool bIsPlAddAllPath  = strcmp(pPath, "/api/playlist/add-all") == 0;

	// ---- GET /api/sequencer/status ----
	if (bIsSeqStatusPath)
	{
		const CMT32Pi::TSequencerStatus s = m_pMT32Pi->GetSequencerStatus();
		CString JSON;
		JSON += "{";
		AppendJSONPairBool(JSON, "playing", s.bPlaying);
		AppendJSONPairBool(JSON, "loading", s.bLoading);
		AppendJSONPairBool(JSON, "paused", s.bPaused);
		AppendJSONPairBool(JSON, "loop_enabled", s.bLoopEnabled);
		AppendJSONPairBool(JSON, "auto_next", s.bAutoNext);
		AppendJSONPairBool(JSON, "finished", s.bFinished);
		AppendJSONPair(JSON, "file", s.pFile ? s.pFile : "");
		AppendJSONPairInt(JSON, "event_count", static_cast<int>(s.nEventCount));
		AppendJSONPairInt(JSON, "duration_ms", static_cast<int>(s.nDurationMs));
		AppendJSONPairInt(JSON, "elapsed_ms", static_cast<int>(s.nElapsedMs));
		AppendJSONPairInt(JSON, "current_tick", s.nCurrentTick);
		AppendJSONPairInt(JSON, "total_ticks", s.nTotalTicks);
		AppendJSONPairInt(JSON, "bpm", s.nBPM);
		AppendJSONPairInt(JSON, "division", s.nDivision);
		AppendJSONPairFloat(JSON, "tempo_multiplier", static_cast<float>(s.nTempoMultiplier));
		AppendJSONPairInt(JSON, "file_size_kb", static_cast<int>(s.nFileSizeKB), false);
		if (s.pDiag && s.pDiag[0])
		{
			JSON += ",";
			AppendJSONPair(JSON, "diag", s.pDiag, false);
		}
		JSON += "}";
		const unsigned nLen = JSON.GetLength();
		if (*pLength < nLen) return HTTPInternalServerError;
		memcpy(pBuffer, static_cast<const char*>(JSON), nLen);
		*pLength = nLen;
		*ppContentType = "application/json; charset=utf-8";
		return HTTPOK;
	}

	// ---- POST /api/sequencer/loop  (body: enabled=on|off) ----
	if (bIsSeqLoopPath)
	{
		char EnabledVal[8] = {};
		bool bEnabled = false;
		if (pFormData && *pFormData && GetFormValue(pFormData, "enabled", EnabledVal, sizeof(EnabledVal)))
			CConfig::ParseOption(EnabledVal, &bEnabled);
		m_pMT32Pi->SetSequencerLoop(bEnabled);
		const char* pBody = "{\"ok\":true}";
		const unsigned nLen = static_cast<unsigned>(std::strlen(pBody));
		if (*pLength < nLen) return HTTPInternalServerError;
		memcpy(pBuffer, pBody, nLen);
		*pLength = nLen;
		*ppContentType = "application/json; charset=utf-8";
		return HTTPOK;
	}

	// ---- POST /api/sequencer/play  (body: file=SD%3Atest.mid) ----
	if (bIsSeqPlayPath)
	{
		char FilePath[260] = {};
		if (pFormData && *pFormData)
			GetFormValue(pFormData, "file", FilePath, sizeof(FilePath));

		if (!FilePath[0])
		{
			const char* pBody = "{\"ok\":false,\"message\":\"missing file param\"}";
			const unsigned nLen = static_cast<unsigned>(std::strlen(pBody));
			if (*pLength < nLen) return HTTPInternalServerError;
			memcpy(pBuffer, pBody, nLen);
			*pLength = nLen;
			*ppContentType = "application/json; charset=utf-8";
			return HTTPBadRequest;
		}

		m_pMT32Pi->SequencerPlayFile(FilePath);
		const char* pBody = "{\"ok\":true}";
		const unsigned nLen = static_cast<unsigned>(std::strlen(pBody));
		if (*pLength < nLen) return HTTPInternalServerError;
		memcpy(pBuffer, pBody, nLen);
		*pLength = nLen;
		*ppContentType = "application/json; charset=utf-8";
		return HTTPOK;
	}

	// ---- POST /api/sequencer/stop ----
	if (bIsSeqStopPath)
	{
		m_pMT32Pi->SequencerStop();
		const char* pBody = "{\"ok\":true}";
		const unsigned nLen = static_cast<unsigned>(std::strlen(pBody));
		if (*pLength < nLen) return HTTPInternalServerError;
		memcpy(pBuffer, pBody, nLen);
		*pLength = nLen;
		*ppContentType = "application/json; charset=utf-8";
		return HTTPOK;
	}

	// ---- POST /api/sequencer/pause ----
	if (bIsSeqPausePath)
	{
		const bool bOk = m_pMT32Pi->SequencerPause();
		CString JSON;
		JSON.Format("{\"ok\":%s}", bOk ? "true" : "false");
		const unsigned nLen = JSON.GetLength();
		if (*pLength < nLen) return HTTPInternalServerError;
		memcpy(pBuffer, static_cast<const char*>(JSON), nLen);
		*pLength = nLen;
		*ppContentType = "application/json; charset=utf-8";
		return bOk ? HTTPOK : HTTPBadRequest;
	}

	// ---- POST /api/sequencer/resume ----
	if (bIsSeqResumePath)
	{
		const bool bOk = m_pMT32Pi->SequencerResume();
		CString JSON;
		JSON.Format("{\"ok\":%s}", bOk ? "true" : "false");
		const unsigned nLen = JSON.GetLength();
		if (*pLength < nLen) return HTTPInternalServerError;
		memcpy(pBuffer, static_cast<const char*>(JSON), nLen);
		*pLength = nLen;
		*ppContentType = "application/json; charset=utf-8";
		return bOk ? HTTPOK : HTTPBadRequest;
	}

	// ---- POST /api/sequencer/next ----
	if (bIsSeqNextPath)
	{
		m_pMT32Pi->SequencerNext();
		const char* pBody = "{\"ok\":true}";
		const unsigned nLen = static_cast<unsigned>(std::strlen(pBody));
		if (*pLength < nLen) return HTTPInternalServerError;
		memcpy(pBuffer, pBody, nLen);
		*pLength = nLen;
		*ppContentType = "application/json; charset=utf-8";
		return HTTPOK;
	}

	// ---- POST /api/sequencer/prev ----
	if (bIsSeqPrevPath)
	{
		m_pMT32Pi->SequencerPrev();
		const char* pBody = "{\"ok\":true}";
		const unsigned nLen = static_cast<unsigned>(std::strlen(pBody));
		if (*pLength < nLen) return HTTPInternalServerError;
		memcpy(pBuffer, pBody, nLen);
		*pLength = nLen;
		*ppContentType = "application/json; charset=utf-8";
		return HTTPOK;
	}

	// ---- POST /api/sequencer/autonext  (body: enabled=on|off) ----
	if (bIsSeqAutoNextPath)
	{
		char EnabledVal[8] = {};
		bool bEnabled = false;
		if (pFormData && *pFormData && GetFormValue(pFormData, "enabled", EnabledVal, sizeof(EnabledVal)))
			CConfig::ParseOption(EnabledVal, &bEnabled);
		m_pMT32Pi->SetSequencerAutoNext(bEnabled);
		const char* pBody = "{\"ok\":true}";
		const unsigned nLen = static_cast<unsigned>(std::strlen(pBody));
		if (*pLength < nLen) return HTTPInternalServerError;
		memcpy(pBuffer, pBody, nLen);
		*pLength = nLen;
		*ppContentType = "application/json; charset=utf-8";
		return HTTPOK;
	}

	// ---- GET /api/sequencer/files ----
	if (bIsSeqFilesPath)
	{
		CString JSON;
		m_pMT32Pi->GetMIDIFileListJSON(JSON);
		const unsigned nLen = JSON.GetLength();
		if (*pLength < nLen) return HTTPInternalServerError;
		memcpy(pBuffer, static_cast<const char*>(JSON), nLen);
		*pLength = nLen;
		*ppContentType = "application/json; charset=utf-8";
		return HTTPOK;
	}

	// ---- POST /api/sequencer/seek  (body: ticks=12345) ----
	if (bIsSeqSeekPath)
	{
		char TicksVal[16] = {};
		int nTicks = 0;
		if (pFormData && *pFormData && GetFormValue(pFormData, "ticks", TicksVal, sizeof(TicksVal)))
			nTicks = atoi(TicksVal);
		const bool bOk = m_pMT32Pi->SequencerSeek(nTicks);
		CString JSON;
		JSON.Format("{\"ok\":%s}", bOk ? "true" : "false");
		const unsigned nLen = JSON.GetLength();
		if (*pLength < nLen) return HTTPInternalServerError;
		memcpy(pBuffer, static_cast<const char*>(JSON), nLen);
		*pLength = nLen;
		*ppContentType = "application/json; charset=utf-8";
		return bOk ? HTTPOK : HTTPBadRequest;
	}

	// ---- POST /api/sequencer/tempo  (body: multiplier=1.5 OR bpm=140) ----
	if (bIsSeqTempoPath)
	{
		char MultVal[16] = {};
		char BPMVal[16] = {};
		bool bOk = false;
		if (pFormData && *pFormData)
		{
			if (GetFormValue(pFormData, "multiplier", MultVal, sizeof(MultVal)))
			{
				const double nMult = strtod(MultVal, nullptr);
				if (nMult > 0.0)
					bOk = m_pMT32Pi->SequencerSetTempoMultiplier(nMult);
			}
			else if (GetFormValue(pFormData, "bpm", BPMVal, sizeof(BPMVal)))
			{
				const double nBPM = strtod(BPMVal, nullptr);
				if (nBPM > 0.0)
					bOk = m_pMT32Pi->SequencerSetTempoBPM(nBPM);
			}
		}
		CString JSON;
		JSON.Format("{\"ok\":%s}", bOk ? "true" : "false");
		const unsigned nLen = JSON.GetLength();
		if (*pLength < nLen) return HTTPInternalServerError;
		memcpy(pBuffer, static_cast<const char*>(JSON), nLen);
		*pLength = nLen;
		*ppContentType = "application/json; charset=utf-8";
		return bOk ? HTTPOK : HTTPBadRequest;
	}

	// ---- POST /api/midi/raw  (body: bytes=144,60,100,128,60,0,...) ----
	if (bIsMidiRawPath)
	{
		char BytesStr[256] = {};
		if (pFormData && *pFormData)
			GetFormValue(pFormData, "bytes", BytesStr, sizeof(BytesStr));
		u8 midi[64];
		size_t nMidi = 0;
		char* p = BytesStr;
		while (*p && nMidi < sizeof(midi))
		{
			char* pEnd;
			long v = std::strtol(p, &pEnd, 10);
			if (pEnd == p) break;
			midi[nMidi++] = static_cast<u8>(v & 0xFFu);
			p = pEnd;
			while (*p == ',') ++p;
		}
		if (nMidi > 0)
			m_pMT32Pi->SendRawMIDI(midi, nMidi);
		const char* pBody = "{\"ok\":true}";
		const unsigned nLen = static_cast<unsigned>(std::strlen(pBody));
		if (*pLength < nLen) return HTTPInternalServerError;
		memcpy(pBuffer, pBody, nLen);
		*pLength = nLen;
		*ppContentType = "application/json; charset=utf-8";
		return HTTPOK;
	}

	// ---- GET /api/midi/log ----
	if (bIsMidiLogPath)
	{
		static constexpr unsigned MaxLog   = CMIDIMonitor::EventLogSize;
		static constexpr unsigned MaxSxLog = CMIDIMonitor::SysExLogSize;
		CMIDIMonitor::TEventEntry  Events[MaxLog];
		CMIDIMonitor::TSysExEntry  SysExEvents[MaxSxLog];
		const unsigned nCount   = m_pMT32Pi->GetMIDIEventLog(Events, MaxLog);
		const unsigned nSxCount = m_pMT32Pi->GetSysExLog(SysExEvents, MaxSxLog);

		CString JSON;
		JSON += "{\"events\":[";
		bool bFirst = true;

		for (unsigned i = 0; i < nCount; ++i)
		{
			if (!bFirst) JSON += ",";
			bFirst = false;
			const u32 msg     = Events[i].nRawMessage;
			const u8 nStatus  = msg & 0xF0;
			const u8 nChannel = msg & 0x0F;
			const u8 nData1   = (msg >> 8)  & 0x7F;
			const u8 nData2   = (msg >> 16) & 0x7F;

			const char* pType = "Unknown";
			switch (nStatus)
			{
				case 0x80: pType = "Note Off";    break;
				case 0x90: pType = (nData2 > 0) ? "Note On" : "Note Off"; break;
				case 0xA0: pType = "Aftertouch";  break;
				case 0xB0: pType = "CC";          break;
				case 0xC0: pType = "Prog Change"; break;
				case 0xD0: pType = "Chan Press";  break;
				case 0xE0: pType = "Pitch Bend";  break;
				default:   pType = "RT";          break;
			}

			JSON += "{";
			CString Tmp;
			Tmp.Format("%u", Events[i].nTimestampMs);
			AppendJSONPair(JSON, "ts", static_cast<const char*>(Tmp));
			Tmp.Format("%u", static_cast<unsigned>(nChannel) + 1);
			AppendJSONPair(JSON, "ch", static_cast<const char*>(Tmp));
			AppendJSONPair(JSON, "type", pType);
			Tmp.Format("%u", static_cast<unsigned>(nData1));
			AppendJSONPair(JSON, "d1", static_cast<const char*>(Tmp));
			Tmp.Format("%u", static_cast<unsigned>(nData2));
			AppendJSONPair(JSON, "d2", static_cast<const char*>(Tmp), false);
			JSON += "}";
		}

		for (unsigned i = 0; i < nSxCount; ++i)
		{
			if (!bFirst) JSON += ",";
			bFirst = false;
			const CMIDIMonitor::TSysExEntry& Sx = SysExEvents[i];
			const u8* p = Sx.nData;
			const u8  n = Sx.nStoredBytes;

			// Decode known SysEx patterns
			const char* pDecoded = "Unknown";
			if (n >= 4 && p[0] == 0xF0)
			{
				if (p[1] == 0x7E && n >= 6 && p[3] == 0x09)
				{
					if      (p[4] == 0x01) pDecoded = "GM System On";
					else if (p[4] == 0x02) pDecoded = "GM System Off";
					else if (p[4] == 0x03) pDecoded = "GM2 System On";
				}
				else if (p[1] == 0x7F && n >= 8 && p[3] == 0x04 && p[4] == 0x01)
					pDecoded = "Master Volume";
				else if (p[1] == 0x41 && n >= 6 && p[3] == 0x42)
					pDecoded = (p[4] == 0x12) ? "Roland GS" : "Roland GS Req";
				else if (p[1] == 0x43)
					pDecoded = "Yamaha XG";
				else if (p[1] == 0x7D)
					pDecoded = "mt32-pi";
			}

			// Hex preview (first 8 bytes)
			CString Hex;
			const u8 nShow = n < 8 ? n : 8;
			for (u8 b = 0; b < nShow; ++b)
			{
				CString Byte;
				Byte.Format(b > 0 ? " %02X" : "%02X", static_cast<unsigned>(p[b]));
				Hex += static_cast<const char*>(Byte);
			}
			if (Sx.nFullSize > 8) Hex += "..";

			CString Tmp;
			JSON += "{";
			Tmp.Format("%u", Sx.nTimestampMs);
			AppendJSONPair(JSON, "ts", static_cast<const char*>(Tmp));
			AppendJSONPair(JSON, "ch", "-");
			AppendJSONPair(JSON, "type", "SysEx");
			AppendJSONPair(JSON, "d1", static_cast<const char*>(Hex));
			AppendJSONPair(JSON, "d2", pDecoded, false);
			JSON += "}";
		}

		JSON += "]}";

		const unsigned nLen = JSON.GetLength();
		if (*pLength < nLen) return HTTPInternalServerError;
		memcpy(pBuffer, static_cast<const char*>(JSON), nLen);
		*pLength = nLen;
		*ppContentType = "application/json; charset=utf-8";
		return HTTPOK;
	}

	// ---- POST /api/midi/log/clear ----
	if (bIsMidiLogClearPath)
	{
		m_pMT32Pi->ClearMIDIEventLog();
		m_pMT32Pi->ClearSysExLog();
		const char* pResp = "{\"ok\":true}";
		const unsigned nLen = strlen(pResp);
		if (*pLength < nLen) return HTTPInternalServerError;
		memcpy(pBuffer, pResp, nLen);
		*pLength = nLen;
		*ppContentType = "application/json; charset=utf-8";
		return HTTPOK;
	}

	// ---- POST /api/midi/note  (body: type=on|off&ch=0-15&note=0-127&vel=0-127) ----
	if (bIsMidiNotePath)
	{
		char TypeVal[8] = {};
		char ChVal[8]   = {};
		char NoteVal[8] = {};
		char VelVal[8]  = {};
		if (pFormData && *pFormData)
		{
			GetFormValue(pFormData, "type", TypeVal, sizeof(TypeVal));
			GetFormValue(pFormData, "ch",   ChVal,   sizeof(ChVal));
			GetFormValue(pFormData, "note", NoteVal, sizeof(NoteVal));
			GetFormValue(pFormData, "vel",  VelVal,  sizeof(VelVal));
		}
		const u8 nCh   = static_cast<u8>(atoi(ChVal))   & 0x0Fu;
		const u8 nNote = static_cast<u8>(atoi(NoteVal))  & 0x7Fu;
		const u8 nVel  = static_cast<u8>(atoi(VelVal))   & 0x7Fu;
		const bool bNoteOn = (strcmp(TypeVal, "on") == 0);
		u8 midi[3] = { static_cast<u8>((bNoteOn ? 0x90u : 0x80u) | nCh), nNote, nVel };
		m_pMT32Pi->SendRawMIDI(midi, 3);
		const char* pBody = "{\"ok\":true}";
		const unsigned nLen = static_cast<unsigned>(std::strlen(pBody));
		if (*pLength < nLen) return HTTPInternalServerError;
		memcpy(pBuffer, pBody, nLen);
		*pLength = nLen;
		*ppContentType = "application/json; charset=utf-8";
		return HTTPOK;
	}

	if (bIsSystemRebootPath)
	{
		const char* pBody = "{\"ok\":true,\"message\":\"Reboot requested\"}\n";
		const unsigned nBodyLength = static_cast<unsigned>(std::strlen(pBody));
		if (*pLength < nBodyLength)
			return HTTPInternalServerError;

		memcpy(pBuffer, pBody, nBodyLength);
		*pLength = nBodyLength;
		*ppContentType = "application/json; charset=utf-8";

		m_pMT32Pi->RequestReboot();
		return HTTPOK;
	}

	if (bIsWifiReadPath)
	{
		char ssid[128] = {};
		char country[8] = {};

		FIL fr;
		if (f_open(&fr, WPAConfigPath, FA_READ) == FR_OK)
		{
			char Line[256];
			bool bEOF = false;
			while (ReadLine(&fr, Line, sizeof(Line), bEOF))
			{
				const char* p = Line;
				while (*p == ' ' || *p == '\t') ++p;
				if (std::strncmp(p, "country=", 8) == 0)
					std::strncpy(country, p + 8, sizeof(country) - 1);
				else if (std::strncmp(p, "ssid=\"", 6) == 0)
				{
					std::strncpy(ssid, p + 6, sizeof(ssid) - 1);
					for (char* q = ssid; *q; ++q)
						if (*q == '"') { *q = '\0'; break; }
				}
				if (bEOF) break;
			}
			f_close(&fr);
		}

		CString JSON;
		JSON += "{";
		AppendJSONPair(JSON, "ssid", ssid);
		AppendJSONPair(JSON, "country", country, false);
		JSON += "}";
		const unsigned nWRLen = JSON.GetLength();
		if (*pLength < nWRLen) return HTTPInternalServerError;
		memcpy(pBuffer, static_cast<const char*>(JSON), nWRLen);
		*pLength = nWRLen;
		*ppContentType = "application/json; charset=utf-8";
		return HTTPOK;
	}

	if (bIsWifiSavePath)
	{
		if (!pFormData || !*pFormData)
			return HTTPBadRequest;

		char WiFiSSID[128] = {};
		char WiFiPSK[128] = {};
		char WiFiCountry[8] = {};

		if (!GetFormValue(pFormData, "wifi_ssid", WiFiSSID, sizeof(WiFiSSID))
		 || !GetFormValue(pFormData, "wifi_country", WiFiCountry, sizeof(WiFiCountry)))
			return HTTPBadRequest;
		GetFormValue(pFormData, "wifi_psk", WiFiPSK, sizeof(WiFiPSK));

		// If no new PSK provided, preserve existing one from the file
		if (WiFiPSK[0] == '\0')
		{
			FIL fp;
			if (f_open(&fp, WPAConfigPath, FA_READ) == FR_OK)
			{
				char Line[256];
				bool bEOF = false;
				while (ReadLine(&fp, Line, sizeof(Line), bEOF))
				{
					const char* p = Line;
					while (*p == ' ' || *p == '\t') ++p;
					if (std::strncmp(p, "psk=\"", 5) == 0)
					{
						std::strncpy(WiFiPSK, p + 5, sizeof(WiFiPSK) - 1);
						for (char* q = WiFiPSK; *q; ++q)
							if (*q == '"') { *q = '\0'; break; }
						break;
					}
					if (bEOF) break;
				}
				f_close(&fp);
			}
		}

		// Sanitize: no quotes or backslashes that could break the file format
		for (char* p = WiFiSSID;    *p; ++p) if (*p == '"' || *p == '\\') *p = '_';
		for (char* p = WiFiPSK;     *p; ++p) if (*p == '"' || *p == '\\') *p = '_';
		for (char* p = WiFiCountry; *p; ++p)
			if ((*p < 'A' || *p > 'Z') && (*p < 'a' || *p > 'z')) *p = 'X';

		FIL fw;
		if (f_open(&fw, WPAConfigPath, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK)
		{
			const char* pErr = "{\"ok\":false,\"message\":\"Could not open wpa_supplicant.conf\"}";
			const unsigned nEL = static_cast<unsigned>(std::strlen(pErr));
			if (*pLength < nEL) return HTTPInternalServerError;
			memcpy(pBuffer, pErr, nEL);
			*pLength = nEL;
			*ppContentType = "application/json; charset=utf-8";
			return HTTPOK;
		}

		CString L;
		bool bOK = true;
		L.Format("country=%s", WiFiCountry);
		bOK = bOK && WriteLine(&fw, static_cast<const char*>(L));
		bOK = bOK && WriteLine(&fw, "");
		bOK = bOK && WriteLine(&fw, "network={");
		L.Format("\tssid=\"%s\"", WiFiSSID);
		bOK = bOK && WriteLine(&fw, static_cast<const char*>(L));
		L.Format("\tpsk=\"%s\"", WiFiPSK);
		bOK = bOK && WriteLine(&fw, static_cast<const char*>(L));
		bOK = bOK && WriteLine(&fw, "\tproto=WPA2");
		bOK = bOK && WriteLine(&fw, "\tkey_mgmt=WPA-PSK");
		bOK = bOK && WriteLine(&fw, "}");
		f_close(&fw);

		const char* pBody = bOK ? "{\"ok\":true}" : "{\"ok\":false,\"message\":\"Error writing wpa_supplicant.conf\"}";
		const unsigned nWSLen = static_cast<unsigned>(std::strlen(pBody));
		if (*pLength < nWSLen) return HTTPInternalServerError;
		memcpy(pBuffer, pBody, nWSLen);
		*pLength = nWSLen;
		*ppContentType = "application/json; charset=utf-8";
		return HTTPOK;
	}

	// ---- GET /api/mixer/status ----
	if (bIsMixerStatusPath)
	{
		const CMT32Pi::TMixerStatus ms = m_pMT32Pi->GetMixerStatus();
		CString JSON;
		JSON += "{";
		AppendJSONPairBool(JSON, "enabled", ms.bEnabled);
		AppendJSONPairInt(JSON, "preset", ms.nPreset);
		AppendJSONPairBool(JSON, "dual_mode", ms.bDualMode);
		AppendJSONPairFloat(JSON, "master_volume", ms.fMasterVolume);
		AppendJSONPairFloat(JSON, "mt32_volume", ms.fMT32Volume);
		AppendJSONPairFloat(JSON, "fluid_volume", ms.fFluidVolume);
		AppendJSONPairFloat(JSON, "ymfm_volume", ms.fYmfmVolume);
		AppendJSONPairFloat(JSON, "mt32_pan", ms.fMT32Pan);
		AppendJSONPairFloat(JSON, "fluid_pan", ms.fFluidPan);

		// Audio render performance
		AppendJSONPairInt(JSON, "render_us", static_cast<int>(ms.nRenderUs));
		AppendJSONPairInt(JSON, "render_avg_us", static_cast<int>(ms.nRenderAvgUs));
		AppendJSONPairInt(JSON, "render_peak_us", static_cast<int>(ms.nRenderPeakUs));
		AppendJSONPairInt(JSON, "deadline_us", static_cast<int>(ms.nDeadlineUs));
		AppendJSONPairInt(JSON, "cpu_load", static_cast<int>(ms.nCpuLoadPercent));

		// Post-mix audio effects
		AppendJSONPairBool(JSON, "fx_eq_enabled",      ms.bEffectsEQEnabled);
		AppendJSONPairBool(JSON, "fx_limiter_enabled", ms.bEffectsLimiterEnabled);
		AppendJSONPairBool(JSON, "fx_reverb_enabled",  ms.bEffectsReverbEnabled);
		AppendJSONPairInt(JSON,  "fx_eq_bass",         ms.nEffectsEQBass);
		AppendJSONPairInt(JSON,  "fx_eq_treble",       ms.nEffectsEQTreble);
		AppendJSONPairInt(JSON,  "fx_reverb_room",     ms.nEffectsReverbRoom);
		AppendJSONPairInt(JSON,  "fx_reverb_damp",     ms.nEffectsReverbDamp);
		AppendJSONPairInt(JSON,  "fx_reverb_wet",      ms.nEffectsReverbWet);

		// MIDI Thru
		AppendJSONPairBool(JSON, "midi_thru_enabled",  ms.bMIDIThruEnabled);

		// Channel array
		JSON += "\"channels\":[";
		for (int i = 0; i < 16; ++i)
		{
			if (i > 0) JSON += ",";
			JSON += "{";
			CString ChNum;
			ChNum.Format("%d", i + 1);
			AppendJSONPair(JSON, "ch", static_cast<const char*>(ChNum));
			AppendJSONPair(JSON, "engine", ms.pChannelEngine[i]);
			AppendJSONPairInt(JSON, "remap", static_cast<int>(ms.nChannelRemap[i]) + 1);
			AppendJSONPairBool(JSON, "layered", ms.bLayered[i]);
			AppendJSONPairInt(JSON, "vol", ms.nChannelVolume[i]);
			AppendJSONPair(JSON, "instrument", ms.pChannelInstrument[i] ? ms.pChannelInstrument[i] : "", false);
			JSON += "}";
		}
		JSON += "]}";

		const unsigned nLen = JSON.GetLength();
		if (*pLength < nLen) return HTTPInternalServerError;
		memcpy(pBuffer, static_cast<const char*>(JSON), nLen);
		*pLength = nLen;
		*ppContentType = "application/json; charset=utf-8";
		return HTTPOK;
	}

	// ---- POST /api/mixer/set ----
	if (bIsMixerSetPath)
	{
		if (!pFormData || !*pFormData)
			return HTTPBadRequest;

		char Param[64] = {};
		char Value[128] = {};
		if (!GetFormValue(pFormData, "param", Param, sizeof(Param))
		 || !GetFormValue(pFormData, "value", Value, sizeof(Value)))
			return HTTPBadRequest;

		bool bApplied = false;
		int nVal = 0;

		if (std::strcmp(Param, "enabled") == 0)
		{
			bool bEnabled = false;
			if (CConfig::ParseOption(Value, &bEnabled))
				bApplied = m_pMT32Pi->SetMixerEnabled(bEnabled);
		}
		else if (std::strcmp(Param, "mt32_volume") == 0)
		{
			if (ParseIntStrict(Value, nVal))
				bApplied = m_pMT32Pi->SetMixerEngineVolume("mt32", nVal);
		}
		else if (std::strcmp(Param, "fluid_volume") == 0)
		{
			if (ParseIntStrict(Value, nVal))
				bApplied = m_pMT32Pi->SetMixerEngineVolume("fluidsynth", nVal);
		}
		else if (std::strcmp(Param, "opl3_volume") == 0)
		{
			if (ParseIntStrict(Value, nVal))
				bApplied = m_pMT32Pi->SetMixerEngineVolume("ymfm", nVal);
		}
		else if (std::strcmp(Param, "opl3_pan") == 0)
		{
			if (ParseIntStrict(Value, nVal))
				bApplied = m_pMT32Pi->SetMixerEnginePan("ymfm", nVal);
		}
		else if (std::strcmp(Param, "mt32_pan") == 0)
		{
			if (ParseIntStrict(Value, nVal))
				bApplied = m_pMT32Pi->SetMixerEnginePan("mt32", nVal);
		}
		else if (std::strcmp(Param, "fluid_pan") == 0)
		{
			if (ParseIntStrict(Value, nVal))
				bApplied = m_pMT32Pi->SetMixerEnginePan("fluidsynth", nVal);
		}
		else if (std::strcmp(Param, "channel_engine") == 0)
		{
			// Value format: "<ch>,<engine>" e.g. "5,mt32"
			char* pComma = std::strchr(Value, ',');
			if (pComma)
			{
				*pComma = '\0';
				int nCh = 0;
				if (ParseIntStrict(Value, nCh) && nCh >= 1 && nCh <= 16)
					bApplied = m_pMT32Pi->SetMixerChannelEngine(static_cast<u8>(nCh - 1), pComma + 1);
			}
		}
		else if (std::strcmp(Param, "channel_remap") == 0)
		{
			// Value format: "<src_ch>,<dst_ch>" e.g. "5,1"
			char* pComma = std::strchr(Value, ',');
			if (pComma)
			{
				*pComma = '\0';
				int nSrc = 0, nDst = 0;
				if (ParseIntStrict(Value, nSrc) && ParseIntStrict(pComma + 1, nDst)
				 && nSrc >= 1 && nSrc <= 16 && nDst >= 1 && nDst <= 16)
					bApplied = m_pMT32Pi->SetMixerChannelRemap(
						static_cast<u8>(nSrc - 1), static_cast<u8>(nDst - 1));
			}
		}
		else if (std::strcmp(Param, "channel_layer") == 0)
		{
			// Value format: "<ch>,<on|off>" e.g. "5,on"
			char* pComma = std::strchr(Value, ',');
			if (pComma)
			{
				*pComma = '\0';
				int nCh = 0;
				bool bLayer = false;
				if (ParseIntStrict(Value, nCh) && nCh >= 1 && nCh <= 16
				 && CConfig::ParseOption(pComma + 1, &bLayer))
					bApplied = m_pMT32Pi->SetMixerLayering(static_cast<u8>(nCh - 1), bLayer);
			}
		}
		else if (std::strcmp(Param, "all_layer") == 0)
		{
			bool bLayer = false;
			if (CConfig::ParseOption(Value, &bLayer))
				bApplied = m_pMT32Pi->SetMixerAllLayering(bLayer);
		}
		else if (std::strcmp(Param, "cc_filter") == 0)
		{
			// Value format: "<engine_idx>,<cc>,<on|off>" e.g. "0,7,off"
			// engine_idx: 0=MT32, 1=FluidSynth
			char* p1 = std::strchr(Value, ',');
			if (p1)
			{
				*p1 = '\0';
				char* p2 = std::strchr(p1 + 1, ',');
				if (p2)
				{
					*p2 = '\0';
					int nEng = 0, nCC = 0;
					bool bAllow = false;
					if (ParseIntStrict(Value, nEng) && ParseIntStrict(p1 + 1, nCC)
					 && CConfig::ParseOption(p2 + 1, &bAllow)
					 && nEng >= 0 && nEng <= 1 && nCC >= 0 && nCC <= 127)
						bApplied = m_pMT32Pi->SetMixerCCFilter(
							static_cast<unsigned>(nEng), static_cast<u8>(nCC), bAllow);
				}
			}
		}
		else if (std::strcmp(Param, "cc_filter_reset") == 0)
		{
			m_pMT32Pi->ResetMixerCCFilters();
			bApplied = true;
		}
		else if (std::strcmp(Param, "channel_volume") == 0)
		{
			// value=CH,VOL  (CH 1-16, VOL 0-100)
			const char* pComma = std::strchr(Value, ',');
			if (pComma)
			{
				char ChStr[8] = {};
				const size_t nChLen = static_cast<size_t>(pComma - Value);
				if (nChLen < sizeof(ChStr))
				{
					memcpy(ChStr, Value, nChLen);
					const int nCh  = std::atoi(ChStr);
					const int nVol = std::atoi(pComma + 1);
					if (nCh >= 1 && nCh <= 16 && nVol >= 0 && nVol <= 100)
						bApplied = m_pMT32Pi->SetMixerChannelVolume(static_cast<u8>(nCh - 1), nVol);
				}
			}
		}
		else if (std::strcmp(Param, "channel_volume_reset") == 0)
		{
			m_pMT32Pi->ResetMixerChannelVolumes();
			bApplied = true;
		}
		else if (std::strcmp(Param, "fx_eq_enabled") == 0)
		{
			bool b = false;
			if (CConfig::ParseOption(Value, &b))
				bApplied = m_pMT32Pi->SetEffectEQEnabled(b);
		}
		else if (std::strcmp(Param, "fx_eq_bass") == 0)
		{
			if (ParseIntStrict(Value, nVal))
				bApplied = m_pMT32Pi->SetEffectEQBass(nVal);
		}
		else if (std::strcmp(Param, "fx_eq_treble") == 0)
		{
			if (ParseIntStrict(Value, nVal))
				bApplied = m_pMT32Pi->SetEffectEQTreble(nVal);
		}
		else if (std::strcmp(Param, "fx_limiter_enabled") == 0)
		{
			bool b = false;
			if (CConfig::ParseOption(Value, &b))
				bApplied = m_pMT32Pi->SetEffectLimiterEnabled(b);
		}
		else if (std::strcmp(Param, "fx_reverb_enabled") == 0)
		{
			bool b = false;
			if (CConfig::ParseOption(Value, &b))
				bApplied = m_pMT32Pi->SetEffectReverbEnabled(b);
		}
		else if (std::strcmp(Param, "fx_reverb_room") == 0)
		{
			if (ParseIntStrict(Value, nVal))
				bApplied = m_pMT32Pi->SetEffectReverbRoom(nVal);
		}
		else if (std::strcmp(Param, "fx_reverb_damp") == 0)
		{
			if (ParseIntStrict(Value, nVal))
				bApplied = m_pMT32Pi->SetEffectReverbDamp(nVal);
		}
		else if (std::strcmp(Param, "fx_reverb_wet") == 0)
		{
			if (ParseIntStrict(Value, nVal))
				bApplied = m_pMT32Pi->SetEffectReverbWet(nVal);
		}
		else if (std::strcmp(Param, "midi_thru") == 0)
		{
			bool b = false;
			if (CConfig::ParseOption(Value, &b))
				bApplied = m_pMT32Pi->SetMIDIThruEnabled(b);
		}

		const char* pOK = bApplied ? "{\"ok\":true}" : "{\"ok\":false}";
		const unsigned nLen = static_cast<unsigned>(std::strlen(pOK));
		if (*pLength < nLen) return HTTPInternalServerError;
		memcpy(pBuffer, pOK, nLen);
		*pLength = nLen;
		*ppContentType = "application/json; charset=utf-8";
		return bApplied ? HTTPOK : HTTPBadRequest;
	}

	// ---- POST /api/mixer/preset ----
	if (bIsMixerPresetPath)
	{
		if (!pFormData || !*pFormData)
			return HTTPBadRequest;

		char PresetVal[32] = {};
		if (!GetFormValue(pFormData, "preset", PresetVal, sizeof(PresetVal)))
			return HTTPBadRequest;

		int nPreset = -1;
		if (std::strcmp(PresetVal, "single_mt32") == 0)    nPreset = 0;
		else if (std::strcmp(PresetVal, "single_fluid") == 0) nPreset = 1;
		else if (std::strcmp(PresetVal, "split_gm") == 0)     nPreset = 2;
		else ParseIntStrict(PresetVal, nPreset);

		const bool bApplied = m_pMT32Pi->SetMixerPreset(nPreset);
		const char* pOK = bApplied ? "{\"ok\":true}" : "{\"ok\":false}";
		const unsigned nLen = static_cast<unsigned>(std::strlen(pOK));
		if (*pLength < nLen) return HTTPInternalServerError;
		memcpy(pBuffer, pOK, nLen);
		*pLength = nLen;
		*ppContentType = "application/json; charset=utf-8";
		return bApplied ? HTTPOK : HTTPBadRequest;
	}

	const CConfig* pConfig = m_pMT32Pi->GetConfig();
	if (!pConfig)
		return HTTPInternalServerError;

	// ---- POST /api/router/save ----
	if (bIsRouterSavePath)
	{
		const bool bOK = m_pMT32Pi->SaveRouterPreset();
		const char* pResp = bOK ? "{\"ok\":true}" : "{\"ok\":false}";
		const unsigned nLen = static_cast<unsigned>(std::strlen(pResp));
		if (*pLength < nLen) return HTTPInternalServerError;
		memcpy(pBuffer, pResp, nLen);
		*pLength = nLen;
		*ppContentType = "application/json; charset=utf-8";
		return bOK ? HTTPOK : HTTPInternalServerError;
	}

	// ---- POST /api/router/load ----
	if (bIsRouterLoadPath)
	{
		const bool bOK = m_pMT32Pi->LoadRouterPreset();
		const char* pResp = bOK ? "{\"ok\":true}" : "{\"ok\":false}";
		const unsigned nLen = static_cast<unsigned>(std::strlen(pResp));
		if (*pLength < nLen) return HTTPInternalServerError;
		memcpy(pBuffer, pResp, nLen);
		*pLength = nLen;
		*ppContentType = "application/json; charset=utf-8";
		return bOK ? HTTPOK : HTTPBadRequest;
	}

	if (bIsConfigSavePath)
	{
		if (!pFormData || !*pFormData)
			return HTTPBadRequest;

		char DefaultSynth[32];
		char SystemVerbose[16];
		char SystemUSB[16];
		char SystemI2CBaudRate[16];
		char SystemPowerSaveTimeout[16];
		char AudioOutputDevice[16];
		char AudioSampleRate[16];
		char AudioChunkSize[16];
		char AudioReversedStereo[16];
		char MIDIGPIOBaudRate[16];
		char MIDIGPIOThru[16];
		char MIDIUSBSerialBaudRate[16];
		char ControlScheme[32];
		char EncoderType[32];
		char EncoderReversed[16];
		char ControlMister[16];
		char ControlSwitchTimeout[16];
		char MT32EmuGain[16];
		char MT32EmuReverbGain[16];
		char MT32EmuResamplerQuality[16];
		char MT32EmuMIDIChannels[16];
		char MT32EmuROMSet[16];
		char MT32EmuReversedStereo[16];
		char FluidSynthSoundFont[16];
		char FluidSynthPolyphony[16];
		char FluidSynthGain[16];
		char FluidSynthReverb[16];
		char FluidSynthReverbDamping[16];
		char FluidSynthReverbLevel[16];
		char FluidSynthReverbRoomSize[16];
		char FluidSynthReverbWidth[16];
		char FluidSynthChorus[16];
		char FluidSynthChorusDepth[16];
		char FluidSynthChorusLevel[16];
		char FluidSynthChorusVoices[16];
		char FluidSynthChorusSpeed[16];
		char YmfmBankFile[64];
		char YmfmChip[16];
		char LCDType[32];
		char LCDWidth[16];
		char LCDHeight[16];
		char LCDAddress[16];
		char LCDRotation[16];
		char LCDMirror[16];
		char NetworkMode[32];
		char NetworkDHCP[16];
		char IPAddress[32];
		char SubnetMask[32];
		char DefaultGateway[32];
		char DNSServer[32];
		char Hostname[64];
		char NetworkRTPMIDI[16];
		char NetworkUDPMIDI[16];
		char NetworkFTP[16];
		char NetworkFTPUsername[64];
		char NetworkFTPPassword[64];
		char NetworkWeb[16];
		char NetworkWebPort[16];
		char NetworkOSC[16];
		char NetworkOSCPort[16];

		if (!GetFormValue(pFormData, "default_synth", DefaultSynth, sizeof(DefaultSynth))
		 || !GetFormValue(pFormData, "system_verbose", SystemVerbose, sizeof(SystemVerbose))
		 || !GetFormValue(pFormData, "system_usb", SystemUSB, sizeof(SystemUSB))
		 || !GetFormValue(pFormData, "system_i2c_baud_rate", SystemI2CBaudRate, sizeof(SystemI2CBaudRate))
		 || !GetFormValue(pFormData, "system_power_save_timeout", SystemPowerSaveTimeout, sizeof(SystemPowerSaveTimeout))
		 || !GetFormValue(pFormData, "audio_output_device", AudioOutputDevice, sizeof(AudioOutputDevice))
		 || !GetFormValue(pFormData, "audio_sample_rate", AudioSampleRate, sizeof(AudioSampleRate))
		 || !GetFormValue(pFormData, "audio_chunk_size", AudioChunkSize, sizeof(AudioChunkSize))
		 || !GetFormValue(pFormData, "audio_reversed_stereo", AudioReversedStereo, sizeof(AudioReversedStereo))
		 || !GetFormValue(pFormData, "midi_gpio_baud_rate", MIDIGPIOBaudRate, sizeof(MIDIGPIOBaudRate))
		 || !GetFormValue(pFormData, "midi_gpio_thru", MIDIGPIOThru, sizeof(MIDIGPIOThru))
		 || !GetFormValue(pFormData, "midi_usb_serial_baud_rate", MIDIUSBSerialBaudRate, sizeof(MIDIUSBSerialBaudRate))
		 || !GetFormValue(pFormData, "control_scheme", ControlScheme, sizeof(ControlScheme))
		 || !GetFormValue(pFormData, "encoder_type", EncoderType, sizeof(EncoderType))
		 || !GetFormValue(pFormData, "encoder_reversed", EncoderReversed, sizeof(EncoderReversed))
		 || !GetFormValue(pFormData, "control_mister", ControlMister, sizeof(ControlMister))
		 || !GetFormValue(pFormData, "control_switch_timeout", ControlSwitchTimeout, sizeof(ControlSwitchTimeout))
		 || !GetFormValue(pFormData, "mt32emu_gain", MT32EmuGain, sizeof(MT32EmuGain))
		 || !GetFormValue(pFormData, "mt32emu_reverb_gain", MT32EmuReverbGain, sizeof(MT32EmuReverbGain))
		 || !GetFormValue(pFormData, "mt32emu_resampler_quality", MT32EmuResamplerQuality, sizeof(MT32EmuResamplerQuality))
		 || !GetFormValue(pFormData, "mt32emu_midi_channels", MT32EmuMIDIChannels, sizeof(MT32EmuMIDIChannels))
		 || !GetFormValue(pFormData, "mt32emu_rom_set", MT32EmuROMSet, sizeof(MT32EmuROMSet))
		 || !GetFormValue(pFormData, "mt32emu_reversed_stereo", MT32EmuReversedStereo, sizeof(MT32EmuReversedStereo))
		 || !GetFormValue(pFormData, "fs_soundfont", FluidSynthSoundFont, sizeof(FluidSynthSoundFont))
		 || !GetFormValue(pFormData, "fs_polyphony", FluidSynthPolyphony, sizeof(FluidSynthPolyphony))
		 || !GetFormValue(pFormData, "fs_gain", FluidSynthGain, sizeof(FluidSynthGain))
		 || !GetFormValue(pFormData, "fs_reverb", FluidSynthReverb, sizeof(FluidSynthReverb))
		 || !GetFormValue(pFormData, "fs_reverb_damping", FluidSynthReverbDamping, sizeof(FluidSynthReverbDamping))
		 || !GetFormValue(pFormData, "fs_reverb_level", FluidSynthReverbLevel, sizeof(FluidSynthReverbLevel))
		 || !GetFormValue(pFormData, "fs_reverb_room_size", FluidSynthReverbRoomSize, sizeof(FluidSynthReverbRoomSize))
		 || !GetFormValue(pFormData, "fs_reverb_width", FluidSynthReverbWidth, sizeof(FluidSynthReverbWidth))
		 || !GetFormValue(pFormData, "fs_chorus", FluidSynthChorus, sizeof(FluidSynthChorus))
		 || !GetFormValue(pFormData, "fs_chorus_depth", FluidSynthChorusDepth, sizeof(FluidSynthChorusDepth))
		 || !GetFormValue(pFormData, "fs_chorus_level", FluidSynthChorusLevel, sizeof(FluidSynthChorusLevel))
		 || !GetFormValue(pFormData, "fs_chorus_voices", FluidSynthChorusVoices, sizeof(FluidSynthChorusVoices))
		 || !GetFormValue(pFormData, "fs_chorus_speed", FluidSynthChorusSpeed, sizeof(FluidSynthChorusSpeed))
		 || !GetFormValue(pFormData, "ymfm_bank_file", YmfmBankFile, sizeof(YmfmBankFile))
		 || !GetFormValue(pFormData, "ymfm_chip", YmfmChip, sizeof(YmfmChip))
		 || !GetFormValue(pFormData, "lcd_type", LCDType, sizeof(LCDType))
		 || !GetFormValue(pFormData, "lcd_width", LCDWidth, sizeof(LCDWidth))
		 || !GetFormValue(pFormData, "lcd_height", LCDHeight, sizeof(LCDHeight))
		 || !GetFormValue(pFormData, "lcd_i2c_address", LCDAddress, sizeof(LCDAddress))
		 || !GetFormValue(pFormData, "lcd_rotation", LCDRotation, sizeof(LCDRotation))
		 || !GetFormValue(pFormData, "lcd_mirror", LCDMirror, sizeof(LCDMirror))
		 || !GetFormValue(pFormData, "network_mode", NetworkMode, sizeof(NetworkMode))
		 || !GetFormValue(pFormData, "network_dhcp", NetworkDHCP, sizeof(NetworkDHCP))
		 || !GetFormValue(pFormData, "network_ip", IPAddress, sizeof(IPAddress))
		 || !GetFormValue(pFormData, "network_subnet", SubnetMask, sizeof(SubnetMask))
		 || !GetFormValue(pFormData, "network_gateway", DefaultGateway, sizeof(DefaultGateway))
		 || !GetFormValue(pFormData, "network_dns", DNSServer, sizeof(DNSServer))
		 || !GetFormValue(pFormData, "network_hostname", Hostname, sizeof(Hostname))
		 || !GetFormValue(pFormData, "network_rtp_midi", NetworkRTPMIDI, sizeof(NetworkRTPMIDI))
		 || !GetFormValue(pFormData, "network_udp_midi", NetworkUDPMIDI, sizeof(NetworkUDPMIDI))
		 || !GetFormValue(pFormData, "network_ftp", NetworkFTP, sizeof(NetworkFTP))
		 || !GetFormValue(pFormData, "network_ftp_username", NetworkFTPUsername, sizeof(NetworkFTPUsername))
		 || !GetFormValue(pFormData, "network_ftp_password", NetworkFTPPassword, sizeof(NetworkFTPPassword))
		 || !GetFormValue(pFormData, "network_web", NetworkWeb, sizeof(NetworkWeb))
		 || !GetFormValue(pFormData, "network_web_port", NetworkWebPort, sizeof(NetworkWebPort))
		 || !GetFormValue(pFormData, "network_osc", NetworkOSC, sizeof(NetworkOSC))
		 || !GetFormValue(pFormData, "network_osc_port", NetworkOSCPort, sizeof(NetworkOSCPort)))
		{
			return HTTPBadRequest;
		}

		CConfig::TSystemDefaultSynth ParsedDefaultSynth;
		bool ParsedSystemVerbose = false;
		bool ParsedSystemUSB = false;
		int ParsedSystemI2CBaudRate = 0;
		int ParsedSystemPowerSaveTimeout = 0;
		CConfig::TAudioOutputDevice ParsedAudioOutputDevice;
		int ParsedAudioSampleRate = 0;
		int ParsedAudioChunkSize = 0;
		bool ParsedAudioReversedStereo = false;
		int ParsedMIDIGPIOBaudRate = 0;
		bool ParsedMIDIGPIOThru = false;
		int ParsedMIDIUSBSerialBaudRate = 0;
		CConfig::TControlScheme ParsedControlScheme;
		CConfig::TEncoderType ParsedEncoderType;
		bool ParsedEncoderReversed = false;
		bool ParsedControlMister = false;
		int ParsedControlSwitchTimeout = 0;
		float ParsedMT32EmuGain = 0.0f;
		float ParsedMT32EmuReverbGain = 0.0f;
		CConfig::TMT32EmuResamplerQuality ParsedMT32EmuResamplerQuality;
		CConfig::TMT32EmuMIDIChannels ParsedMT32EmuMIDIChannels;
		CConfig::TMT32EmuROMSet ParsedMT32EmuROMSet;
		bool ParsedMT32EmuReversedStereo = false;
		int ParsedFluidSynthSoundFont = 0;
		int ParsedFluidSynthPolyphony = 0;
		float ParsedFluidSynthGain = 0.0f;
		bool ParsedFluidSynthReverb = false;
		float ParsedFluidSynthReverbDamping = 0.0f;
		float ParsedFluidSynthReverbLevel = 0.0f;
		float ParsedFluidSynthReverbRoomSize = 0.0f;
		float ParsedFluidSynthReverbWidth = 0.0f;
		bool ParsedFluidSynthChorus = false;
		float ParsedFluidSynthChorusDepth = 0.0f;
		float ParsedFluidSynthChorusLevel = 0.0f;
		int ParsedFluidSynthChorusVoices = 0;
		float ParsedFluidSynthChorusSpeed = 0.0f;
		CConfig::TYmfmChip ParsedYmfmChip;
		CConfig::TLCDType ParsedLCDType;
		int ParsedLCDWidth = 0;
		int ParsedLCDHeight = 0;
		int ParsedLCDAddress = 0;
		CConfig::TLCDRotation ParsedLCDRotation;
		CConfig::TLCDMirror ParsedLCDMirror;
		CConfig::TNetworkMode ParsedNetworkMode;
		bool ParsedNetworkDHCP = false;
		CIPAddress ParsedIPAddress;
		CIPAddress ParsedSubnetMask;
		CIPAddress ParsedDefaultGateway;
		CIPAddress ParsedDNSServer;
		CString ParsedHostname;
		bool ParsedRTPMIDI = false;
		bool ParsedUDPMIDI = false;
		bool ParsedFTP = false;
		CString ParsedFTPUsername;
		CString ParsedFTPPassword;
		bool ParsedWeb = false;
		int ParsedWebPort = 0;
		bool ParsedOSC = false;
		int ParsedOSCPort = 0;

		if (!CConfig::ParseOption(DefaultSynth, &ParsedDefaultSynth)
		 || !CConfig::ParseOption(SystemVerbose, &ParsedSystemVerbose)
		 || !CConfig::ParseOption(SystemUSB, &ParsedSystemUSB)
		 || !CConfig::ParseOption(SystemI2CBaudRate, &ParsedSystemI2CBaudRate)
		 || !CConfig::ParseOption(SystemPowerSaveTimeout, &ParsedSystemPowerSaveTimeout)
		 || !CConfig::ParseOption(AudioOutputDevice, &ParsedAudioOutputDevice)
		 || !CConfig::ParseOption(AudioSampleRate, &ParsedAudioSampleRate)
		 || !CConfig::ParseOption(AudioChunkSize, &ParsedAudioChunkSize)
		 || !CConfig::ParseOption(AudioReversedStereo, &ParsedAudioReversedStereo)
		 || !CConfig::ParseOption(MIDIGPIOBaudRate, &ParsedMIDIGPIOBaudRate)
		 || !CConfig::ParseOption(MIDIGPIOThru, &ParsedMIDIGPIOThru)
		 || !CConfig::ParseOption(MIDIUSBSerialBaudRate, &ParsedMIDIUSBSerialBaudRate)
		 || !CConfig::ParseOption(ControlScheme, &ParsedControlScheme)
		 || !CConfig::ParseOption(EncoderType, &ParsedEncoderType)
		 || !CConfig::ParseOption(EncoderReversed, &ParsedEncoderReversed)
		 || !CConfig::ParseOption(ControlMister, &ParsedControlMister)
		 || !CConfig::ParseOption(ControlSwitchTimeout, &ParsedControlSwitchTimeout)
		 || !CConfig::ParseOption(MT32EmuGain, &ParsedMT32EmuGain)
		 || !CConfig::ParseOption(MT32EmuReverbGain, &ParsedMT32EmuReverbGain)
		 || !CConfig::ParseOption(MT32EmuResamplerQuality, &ParsedMT32EmuResamplerQuality)
		 || !CConfig::ParseOption(MT32EmuMIDIChannels, &ParsedMT32EmuMIDIChannels)
		 || !CConfig::ParseOption(MT32EmuROMSet, &ParsedMT32EmuROMSet)
		 || !CConfig::ParseOption(MT32EmuReversedStereo, &ParsedMT32EmuReversedStereo)
		 || !CConfig::ParseOption(FluidSynthSoundFont, &ParsedFluidSynthSoundFont)
		 || !CConfig::ParseOption(FluidSynthPolyphony, &ParsedFluidSynthPolyphony)
		 || !CConfig::ParseOption(FluidSynthGain, &ParsedFluidSynthGain)
		 || !CConfig::ParseOption(FluidSynthReverb, &ParsedFluidSynthReverb)
		 || !CConfig::ParseOption(FluidSynthReverbDamping, &ParsedFluidSynthReverbDamping)
		 || !CConfig::ParseOption(FluidSynthReverbLevel, &ParsedFluidSynthReverbLevel)
		 || !CConfig::ParseOption(FluidSynthReverbRoomSize, &ParsedFluidSynthReverbRoomSize)
		 || !CConfig::ParseOption(FluidSynthReverbWidth, &ParsedFluidSynthReverbWidth)
		 || !CConfig::ParseOption(FluidSynthChorus, &ParsedFluidSynthChorus)
		 || !CConfig::ParseOption(FluidSynthChorusDepth, &ParsedFluidSynthChorusDepth)
		 || !CConfig::ParseOption(FluidSynthChorusLevel, &ParsedFluidSynthChorusLevel)
		 || !CConfig::ParseOption(FluidSynthChorusVoices, &ParsedFluidSynthChorusVoices)
		 || !CConfig::ParseOption(FluidSynthChorusSpeed, &ParsedFluidSynthChorusSpeed)
		 || !CConfig::ParseOption(YmfmChip, &ParsedYmfmChip)
		 || !CConfig::ParseOption(LCDType, &ParsedLCDType)
		 || !CConfig::ParseOption(LCDWidth, &ParsedLCDWidth)
		 || !CConfig::ParseOption(LCDHeight, &ParsedLCDHeight)
		 || !CConfig::ParseOption(LCDAddress, &ParsedLCDAddress, true)
		 || !CConfig::ParseOption(LCDRotation, &ParsedLCDRotation)
		 || !CConfig::ParseOption(LCDMirror, &ParsedLCDMirror)
		 || !CConfig::ParseOption(NetworkMode, &ParsedNetworkMode)
		 || !CConfig::ParseOption(NetworkDHCP, &ParsedNetworkDHCP)
		 || !CConfig::ParseOption(IPAddress, &ParsedIPAddress)
		 || !CConfig::ParseOption(SubnetMask, &ParsedSubnetMask)
		 || !CConfig::ParseOption(DefaultGateway, &ParsedDefaultGateway)
		 || !CConfig::ParseOption(DNSServer, &ParsedDNSServer)
		 || !CConfig::ParseOption(Hostname, &ParsedHostname)
		 || !CConfig::ParseOption(NetworkRTPMIDI, &ParsedRTPMIDI)
		 || !CConfig::ParseOption(NetworkUDPMIDI, &ParsedUDPMIDI)
		 || !CConfig::ParseOption(NetworkFTP, &ParsedFTP)
		 || !CConfig::ParseOption(NetworkFTPUsername, &ParsedFTPUsername)
		 || !CConfig::ParseOption(NetworkFTPPassword, &ParsedFTPPassword)
		 || !CConfig::ParseOption(NetworkWeb, &ParsedWeb)
		 || !CConfig::ParseOption(NetworkWebPort, &ParsedWebPort)
		 || !CConfig::ParseOption(NetworkOSC, &ParsedOSC)
		 || !CConfig::ParseOption(NetworkOSCPort, &ParsedOSCPort))
		{
			return HTTPBadRequest;
		}

		TReplacement Replacements[] = {
			{TConfigSection::System,     "default_synth",    DefaultSynth,              false},
			{TConfigSection::System,     "verbose",          SystemVerbose,             false},
			{TConfigSection::System,     "usb",              SystemUSB,                 false},
			{TConfigSection::System,     "i2c_baud_rate",    SystemI2CBaudRate,         false},
			{TConfigSection::System,     "power_save_timeout", SystemPowerSaveTimeout,  false},
			{TConfigSection::Audio,      "output_device",    AudioOutputDevice,         false},
			{TConfigSection::Audio,      "sample_rate",      AudioSampleRate,           false},
			{TConfigSection::Audio,      "chunk_size",       AudioChunkSize,            false},
			{TConfigSection::Audio,      "reversed_stereo",  AudioReversedStereo,       false},
			{TConfigSection::MIDI,       "gpio_baud_rate",   MIDIGPIOBaudRate,          false},
			{TConfigSection::MIDI,       "gpio_thru",        MIDIGPIOThru,              false},
			{TConfigSection::MIDI,       "usb_serial_baud_rate", MIDIUSBSerialBaudRate, false},
			{TConfigSection::Control,    "scheme",           ControlScheme,             false},
			{TConfigSection::Control,    "encoder_type",     EncoderType,               false},
			{TConfigSection::Control,    "encoder_reversed", EncoderReversed,           false},
			{TConfigSection::Control,    "mister",           ControlMister,             false},
			{TConfigSection::Control,    "switch_timeout",   ControlSwitchTimeout,      false},
			{TConfigSection::MT32Emu,    "gain",             MT32EmuGain,               false},
			{TConfigSection::MT32Emu,    "reverb_gain",      MT32EmuReverbGain,         false},
			{TConfigSection::MT32Emu,    "resampler_quality", MT32EmuResamplerQuality,  false},
			{TConfigSection::MT32Emu,    "midi_channels",    MT32EmuMIDIChannels,       false},
			{TConfigSection::MT32Emu,    "rom_set",          MT32EmuROMSet,             false},
			{TConfigSection::MT32Emu,    "reversed_stereo",  MT32EmuReversedStereo,     false},
			{TConfigSection::FluidSynth, "soundfont",        FluidSynthSoundFont,       false},
			{TConfigSection::FluidSynth, "polyphony",        FluidSynthPolyphony,       false},
			{TConfigSection::FluidSynth, "gain",             FluidSynthGain,            false},
			{TConfigSection::FluidSynth, "reverb",           FluidSynthReverb,          false},
			{TConfigSection::FluidSynth, "reverb_damping",   FluidSynthReverbDamping,   false},
			{TConfigSection::FluidSynth, "reverb_level",     FluidSynthReverbLevel,     false},
			{TConfigSection::FluidSynth, "reverb_room_size", FluidSynthReverbRoomSize,  false},
			{TConfigSection::FluidSynth, "reverb_width",     FluidSynthReverbWidth,     false},
			{TConfigSection::FluidSynth, "chorus",           FluidSynthChorus,          false},
			{TConfigSection::FluidSynth, "chorus_depth",     FluidSynthChorusDepth,     false},
			{TConfigSection::FluidSynth, "chorus_level",     FluidSynthChorusLevel,     false},
			{TConfigSection::FluidSynth, "chorus_voices",    FluidSynthChorusVoices,    false},
			{TConfigSection::FluidSynth, "chorus_speed",     FluidSynthChorusSpeed,     false},
			{TConfigSection::Ymfm,       "bank_file",        YmfmBankFile,              false},
			{TConfigSection::Ymfm,       "chip",             YmfmChip,                  false},
			{TConfigSection::LCD,        "type",             LCDType,                   false},
			{TConfigSection::LCD,        "width",            LCDWidth,                  false},
			{TConfigSection::LCD,        "height",           LCDHeight,                 false},
			{TConfigSection::LCD,        "i2c_lcd_address",  LCDAddress,                false},
			{TConfigSection::LCD,        "rotation",         LCDRotation,               false},
			{TConfigSection::LCD,        "mirror",           LCDMirror,                 false},
			{TConfigSection::Network,    "mode",             NetworkMode,               false},
			{TConfigSection::Network,    "dhcp",             NetworkDHCP,               false},
			{TConfigSection::Network,    "ip_address",       IPAddress,                 false},
			{TConfigSection::Network,    "subnet_mask",      SubnetMask,                false},
			{TConfigSection::Network,    "default_gateway",  DefaultGateway,            false},
			{TConfigSection::Network,    "dns_server",       DNSServer,                 false},
			{TConfigSection::Network,    "hostname",         Hostname,                  false},
			{TConfigSection::Network,    "rtp_midi",         NetworkRTPMIDI,            false},
			{TConfigSection::Network,    "udp_midi",         NetworkUDPMIDI,            false},
			{TConfigSection::Network,    "ftp",              NetworkFTP,                false},
			{TConfigSection::Network,    "ftp_username",     NetworkFTPUsername,        false},
			{TConfigSection::Network,    "ftp_password",     NetworkFTPPassword,        false},
			{TConfigSection::Network,    "web",              NetworkWeb,                false},
			{TConfigSection::Network,    "web_port",         NetworkWebPort,            false},
			{TConfigSection::Network,    "osc",              NetworkOSC,                false},
			{TConfigSection::Network,    "osc_port",         NetworkOSCPort,            false},
		};

		CString SaveError;
		if (!SaveConfigWithBackup(Replacements, sizeof(Replacements) / sizeof(Replacements[0]), SaveError))
		{
			CString JSON;
			JSON += "{\"ok\":false,\"message\":\"";
			AppendJSONEscaped(JSON, SaveError);
			JSON += "\"}";

			const unsigned nBodyLength = JSON.GetLength();
			if (*pLength < nBodyLength)
				return HTTPInternalServerError;

			memcpy(pBuffer, static_cast<const char*>(JSON), nBodyLength);
			*pLength = nBodyLength;
			*ppContentType = "application/json; charset=utf-8";
			return HTTPInternalServerError;
		}

		const char* pBody = "{\"ok\":true,\"message\":\"Config saved. Backup: mt32-pi.cfg.bak\",\"reboot_required\":true}";
		const unsigned nBodyLength = static_cast<unsigned>(std::strlen(pBody));
		if (*pLength < nBodyLength)
			return HTTPInternalServerError;

		memcpy(pBuffer, pBody, nBodyLength);
		*pLength = nBodyLength;
		*ppContentType = "application/json; charset=utf-8";
		return HTTPOK;
	}

	if (bIsRuntimeSetPath)
	{
		if (!pFormData || !*pFormData)
			return HTTPBadRequest;

		char Param[64];
		char Value[128];
		if (!GetFormValue(pFormData, "param", Param, sizeof(Param))
		 || !GetFormValue(pFormData, "value", Value, sizeof(Value)))
		{
			return HTTPBadRequest;
		}

		bool bApplied = false;
		if (std::strcmp(Param, "active_synth") == 0)
		{
			if (std::strcmp(Value, "mt32") == 0)
				bApplied = m_pMT32Pi->SetActiveSynth(TSynth::MT32);
			else if (std::strcmp(Value, "soundfont") == 0)
				bApplied = m_pMT32Pi->SetActiveSynth(TSynth::SoundFont);
			else if (std::strcmp(Value, "opl3") == 0)
				bApplied = m_pMT32Pi->SetActiveSynth(TSynth::Ymfm);
		}
		else if (std::strcmp(Param, "mt32_rom_set") == 0)
		{
			if (std::strcmp(Value, "mt32_old") == 0)
				bApplied = m_pMT32Pi->SetMT32ROMSet(TMT32ROMSet::MT32Old);
			else if (std::strcmp(Value, "mt32_new") == 0)
				bApplied = m_pMT32Pi->SetMT32ROMSet(TMT32ROMSet::MT32New);
			else if (std::strcmp(Value, "cm32l") == 0)
				bApplied = m_pMT32Pi->SetMT32ROMSet(TMT32ROMSet::CM32L);
		}
		else if (std::strcmp(Param, "soundfont_index") == 0)
		{
			int nIndex = 0;
			if (ParseIntStrict(Value, nIndex) && nIndex >= 0)
				bApplied = m_pMT32Pi->SetSoundFontIndex(static_cast<size_t>(nIndex));
		}
		else if (std::strcmp(Param, "master_volume") == 0)
		{
			int nVolume = 0;
			if (ParseIntStrict(Value, nVolume))
				bApplied = m_pMT32Pi->SetMasterVolumePercent(nVolume);
		}
		else if (std::strcmp(Param, "sf_reverb_active") == 0)
		{
			bool bEnabled = false;
			if (CConfig::ParseOption(Value, &bEnabled))
				bApplied = m_pMT32Pi->SetSoundFontReverbActive(bEnabled);
		}
		else if (std::strcmp(Param, "sf_reverb_room") == 0)
		{
			float nRoom = 0.0f;
			if (ParseFloatStrict(Value, nRoom))
				bApplied = m_pMT32Pi->SetSoundFontReverbRoomSize(nRoom);
		}
		else if (std::strcmp(Param, "sf_reverb_level") == 0)
		{
			float nLevel = 0.0f;
			if (ParseFloatStrict(Value, nLevel))
				bApplied = m_pMT32Pi->SetSoundFontReverbLevel(nLevel);
		}
		else if (std::strcmp(Param, "sf_chorus_active") == 0)
		{
			bool bEnabled = false;
			if (CConfig::ParseOption(Value, &bEnabled))
				bApplied = m_pMT32Pi->SetSoundFontChorusActive(bEnabled);
		}
		else if (std::strcmp(Param, "sf_chorus_depth") == 0)
		{
			float nDepth = 0.0f;
			if (ParseFloatStrict(Value, nDepth))
				bApplied = m_pMT32Pi->SetSoundFontChorusDepth(nDepth);
		}
		else if (std::strcmp(Param, "sf_chorus_level") == 0)
		{
			float nLevel = 0.0f;
			if (ParseFloatStrict(Value, nLevel))
				bApplied = m_pMT32Pi->SetSoundFontChorusLevel(nLevel);
		}
		else if (std::strcmp(Param, "sf_chorus_voices") == 0)
		{
			int nVoices = 1;
			if (ParseIntStrict(Value, nVoices))
				bApplied = m_pMT32Pi->SetSoundFontChorusVoices(nVoices);
		}
		else if (std::strcmp(Param, "sf_chorus_speed") == 0)
		{
			float nSpeed = 0.0f;
			if (ParseFloatStrict(Value, nSpeed))
				bApplied = m_pMT32Pi->SetSoundFontChorusSpeed(nSpeed);
		}
		else if (std::strcmp(Param, "sf_reverb_damping") == 0)
		{
			float nDamping = 0.0f;
			if (ParseFloatStrict(Value, nDamping))
				bApplied = m_pMT32Pi->SetSoundFontReverbDamping(nDamping);
		}
		else if (std::strcmp(Param, "sf_reverb_width") == 0)
		{
			float nWidth = 0.0f;
			if (ParseFloatStrict(Value, nWidth))
				bApplied = m_pMT32Pi->SetSoundFontReverbWidth(nWidth);
		}
		else if (std::strcmp(Param, "sf_gain") == 0)
		{
			float nGain = 0.0f;
			if (ParseFloatStrict(Value, nGain))
				bApplied = m_pMT32Pi->SetSoundFontGain(nGain);
		}
		else if (std::strcmp(Param, "sf_tuning") == 0)
		{
			int nPreset = 0;
			if (ParseIntStrict(Value, nPreset))
				bApplied = m_pMT32Pi->SetSoundFontTuning(nPreset);
		}
		else if (std::strcmp(Param, "sf_polyphony") == 0)
		{
			int nPoly = 0;
			if (ParseIntStrict(Value, nPoly))
				bApplied = m_pMT32Pi->SetSoundFontPolyphony(nPoly);
		}
		else if (std::strcmp(Param, "sf_channel_type") == 0)
		{
			int nChan = 0, nType = 0;
			const char* pComma = std::strchr(Value, ',');
			if (pComma)
			{
				char ChanBuf[8];
				size_t nLen = pComma - Value;
				if (nLen < sizeof(ChanBuf))
				{
					std::memcpy(ChanBuf, Value, nLen);
					ChanBuf[nLen] = '\0';
					if (ParseIntStrict(ChanBuf, nChan) && ParseIntStrict(pComma + 1, nType))
						bApplied = m_pMT32Pi->SetSoundFontChannelType(nChan, nType);
				}
			}
		}
		// MT-32 Parameters
		else if (std::strcmp(Param, "mt32_reverb_gain") == 0)
		{
			float nGain = 0.0f;
			if (ParseFloatStrict(Value, nGain))
				bApplied = m_pMT32Pi->SetMT32ReverbOutputGain(nGain);
		}
		else if (std::strcmp(Param, "mt32_reverb_active") == 0)
		{
			bool bEnabled = false;
			if (CConfig::ParseOption(Value, &bEnabled))
				bApplied = m_pMT32Pi->SetMT32ReverbActive(bEnabled);
		}
		else if (std::strcmp(Param, "mt32_nice_amp") == 0)
		{
			bool bEnabled = false;
			if (CConfig::ParseOption(Value, &bEnabled))
				bApplied = m_pMT32Pi->SetMT32NiceAmpRamp(bEnabled);
		}
		else if (std::strcmp(Param, "mt32_nice_pan") == 0)
		{
			bool bEnabled = false;
			if (CConfig::ParseOption(Value, &bEnabled))
				bApplied = m_pMT32Pi->SetMT32NicePanning(bEnabled);
		}
		else if (std::strcmp(Param, "mt32_nice_mix") == 0)
		{
			bool bEnabled = false;
			if (CConfig::ParseOption(Value, &bEnabled))
				bApplied = m_pMT32Pi->SetMT32NicePartialMixing(bEnabled);
		}
		else if (std::strcmp(Param, "mt32_dac_mode") == 0)
		{
			int nMode = 0;
			if (ParseIntStrict(Value, nMode))
				bApplied = m_pMT32Pi->SetMT32DACMode(nMode);
		}
		else if (std::strcmp(Param, "mt32_midi_delay") == 0)
		{
			int nMode = 0;
			if (ParseIntStrict(Value, nMode))
				bApplied = m_pMT32Pi->SetMT32MIDIDelayMode(nMode);
		}
		else if (std::strcmp(Param, "mt32_analog_mode") == 0)
		{
			int nMode = 0;
			if (ParseIntStrict(Value, nMode))
				bApplied = m_pMT32Pi->SetMT32AnalogMode(nMode);
		}
		else if (std::strcmp(Param, "mt32_renderer_type") == 0)
		{
			int nType = 0;
			if (ParseIntStrict(Value, nType))
				bApplied = m_pMT32Pi->SetMT32RendererType(nType);
		}
		else if (std::strcmp(Param, "mt32_partial_count") == 0)
		{
			int nCount = 0;
			if (ParseIntStrict(Value, nCount))
				bApplied = m_pMT32Pi->SetMT32PartialCount(nCount);
		}

		const CMT32Pi::TSystemState st = m_pMT32Pi->GetSystemState();
		CString JSON;
		JSON += "{";
		AppendJSONPairBool(JSON, "ok", bApplied);
		AppendSynthStatusJSON(JSON, st);
		JSON += "}";

		const unsigned nBodyLength = JSON.GetLength();
		if (*pLength < nBodyLength)
			return HTTPInternalServerError;

		memcpy(pBuffer, static_cast<const char*>(JSON), nBodyLength);
		*pLength = nBodyLength;
		*ppContentType = "application/json; charset=utf-8";
		return bApplied ? HTTPOK : HTTPBadRequest;
	}

	if (bIsRuntimeStatusPath)
	{
		const CMT32Pi::TSystemState st = m_pMT32Pi->GetSystemState();
		CString JSON;
		JSON += "{";
		AppendSynthStatusJSON(JSON, st);
		JSON += "}";

		const unsigned nBodyLength = JSON.GetLength();
		if (*pLength < nBodyLength)
			return HTTPInternalServerError;

		memcpy(pBuffer, static_cast<const char*>(JSON), nBodyLength);
		*pLength = nBodyLength;
		*ppContentType = "application/json; charset=utf-8";
		return HTTPOK;
	}

	if (bIsSFInfoPath)
	{
		char IndexStr[16] = {};
		int nIndex = -1;
		if (pFormData && GetFormValue(pFormData, "index", IndexStr, sizeof(IndexStr)))
			nIndex = atoi(IndexStr);

		const size_t nCount = m_pMT32Pi->GetSoundFontCount();
		if (nIndex < 0 || static_cast<size_t>(nIndex) >= nCount)
			return HTTPBadRequest;

		const char* pName = m_pMT32Pi->GetSoundFontName(static_cast<size_t>(nIndex));
		const char* pPath = m_pMT32Pi->GetSoundFontPath(static_cast<size_t>(nIndex));

		unsigned int nSizeKB = 0;
		if (pPath)
		{
			FILINFO finfo;
			if (f_stat(pPath, &finfo) == FR_OK)
				nSizeKB = static_cast<unsigned int>(finfo.fsize / 1024);
		}

		CString JSON;
		JSON += "{";
		AppendJSONPair(JSON, "name", pName ? pName : "");
		AppendJSONPair(JSON, "path", pPath ? pPath : "");
		AppendJSONPairInt(JSON, "size_kb", static_cast<int>(nSizeKB), false);
		JSON += "}";

		const unsigned nBodyLength = JSON.GetLength();
		if (*pLength < nBodyLength)
			return HTTPInternalServerError;
		memcpy(pBuffer, static_cast<const char*>(JSON), nBodyLength);
		*pLength = nBodyLength;
		*ppContentType = "application/json; charset=utf-8";
		return HTTPOK;
	}

	if (bIsStatusAPIPath)
	{
		const CMT32Pi::TSystemState st = m_pMT32Pi->GetSystemState();

		CString JSON;
		JSON += "{";
		AppendJSONPair(JSON, "active_synth",      st.pActiveSynthName);
		AppendJSONPair(JSON, "network_interface", st.pNetworkInterfaceName);
		AppendJSONPairBool(JSON, "network_ready", st.bNetworkReady);
		AppendJSONPair(JSON, "ip",                st.IPAddress);
		AppendJSONPair(JSON, "hostname",          pConfig->NetworkHostname);
		AppendJSONPairInt(JSON, "web_port",       pConfig->NetworkWebServerPort);
		AppendJSONPairBool(JSON, "osc",           pConfig->NetworkOSC);
		AppendJSONPairInt(JSON, "osc_port",       pConfig->NetworkOSCPort);
		AppendJSONPair(JSON, "mt32_rom_name",     st.pMT32ROMName);
		AppendJSONPair(JSON, "soundfont_name",    st.pSoundFontName);
		AppendJSONPair(JSON, "soundfont_path",    st.pSoundFontPath);
		AppendJSONPairBool(JSON, "recording",     st.bMidiRecording);
		AppendJSONPairInt(JSON, "soundfont_index", static_cast<int>(st.nSoundFontIndex));
		AppendJSONPairInt(JSON, "soundfont_count", static_cast<int>(st.nSoundFontCount), false);
		JSON += "}";

		const unsigned nBodyLength = JSON.GetLength();
		if (*pLength < nBodyLength)
			return HTTPInternalServerError;

		memcpy(pBuffer, static_cast<const char*>(JSON), nBodyLength);
		*pLength = nBodyLength;
		*ppContentType = "application/json; charset=utf-8";
		return HTTPOK;
	}

	if (bIsMIDIAPIPath)
	{
		float Levels[16];
		float Peaks[16];
		m_pMT32Pi->GetMIDIChannelLevels(Levels, Peaks);

		CString JSON;
		JSON += "{\"active_synth\":\"";
		AppendJSONEscaped(JSON, m_pMT32Pi->GetActiveSynthName());
		JSON += "\",\"channels\":[";

		for (int nChannel = 0; nChannel < 16; ++nChannel)
		{
			if (nChannel > 0)
				JSON += ",";

			JSON += "{";
			AppendJSONPairInt(JSON, "channel", nChannel + 1);
			AppendJSONPairFloat(JSON, "level", Levels[nChannel]);
			AppendJSONPairFloat(JSON, "peak", Peaks[nChannel], false);
			JSON += "}";
		}

		JSON += "]}";

		const unsigned nBodyLength = JSON.GetLength();
		if (*pLength < nBodyLength)
			return HTTPInternalServerError;

		memcpy(pBuffer, static_cast<const char*>(JSON), nBodyLength);
		*pLength = nBodyLength;
		*ppContentType = "application/json; charset=utf-8";
		return HTTPOK;
	}

	// ---- POST /api/recorder/start ----
	if (bIsRecStartPath)
	{
		const bool bOK = m_pMT32Pi->StartMidiRecording();
		const char* pBody = bOK ? "{\"recording\":true}" : "{\"error\":\"already_recording\"}";
		const unsigned nLen = static_cast<unsigned>(__builtin_strlen(pBody));
		if (*pLength < nLen) return HTTPInternalServerError;
		memcpy(pBuffer, pBody, nLen);
		*pLength = nLen;
		*ppContentType = "application/json; charset=utf-8";
		return bOK ? HTTPOK : HTTPBadRequest;
	}

	// ---- POST /api/recorder/stop ----
	if (bIsRecStopPath)
	{
		m_pMT32Pi->StopMidiRecording();
		constexpr const char* pBody = "{\"recording\":false}";
		const unsigned nLen = static_cast<unsigned>(__builtin_strlen(pBody));
		if (*pLength < nLen) return HTTPInternalServerError;
		memcpy(pBuffer, pBody, nLen);
		*pLength = nLen;
		*ppContentType = "application/json; charset=utf-8";
		return HTTPOK;
	}

	// ---- GET /api/playlist ----
	if (bIsPlListPath)
	{
		CString JSON;
		m_pMT32Pi->GetPlaylistJSON(JSON);
		const unsigned nLen = JSON.GetLength();
		if (*pLength < nLen) return HTTPInternalServerError;
		memcpy(pBuffer, static_cast<const char*>(JSON), nLen);
		*pLength = nLen;
		*ppContentType = "application/json; charset=utf-8";
		return HTTPOK;
	}

	// ---- POST /api/playlist/add ----
	if (bIsPlAddPath)
	{
		char FileVal[256] = {};
		if (pFormData && *pFormData)
			GetFormValue(pFormData, "file", FileVal, sizeof(FileVal));
		m_pMT32Pi->PlaylistAdd(FileVal);
		constexpr const char* pBody = "{\"ok\":true}";
		const unsigned nLen = static_cast<unsigned>(__builtin_strlen(pBody));
		if (*pLength < nLen) return HTTPInternalServerError;
		memcpy(pBuffer, pBody, nLen);
		*pLength = nLen;
		*ppContentType = "application/json; charset=utf-8";
		return HTTPOK;
	}

	// ---- POST /api/playlist/add-all ----
	if (bIsPlAddAllPath)
	{
		m_pMT32Pi->PlaylistAddAll();
		constexpr const char* pBody = "{\"ok\":true}";
		const unsigned nLen = static_cast<unsigned>(__builtin_strlen(pBody));
		if (*pLength < nLen) return HTTPInternalServerError;
		memcpy(pBuffer, pBody, nLen);
		*pLength = nLen;
		*ppContentType = "application/json; charset=utf-8";
		return HTTPOK;
	}

	// ---- POST /api/playlist/remove ----
	if (bIsPlRemovePath)
	{
		char IdxVal[8] = {};
		unsigned nIndex = 0;
		if (pFormData && *pFormData && GetFormValue(pFormData, "index", IdxVal, sizeof(IdxVal)))
			nIndex = static_cast<unsigned>(atoi(IdxVal));
		m_pMT32Pi->PlaylistRemove(nIndex);
		constexpr const char* pBody = "{\"ok\":true}";
		const unsigned nLen = static_cast<unsigned>(__builtin_strlen(pBody));
		if (*pLength < nLen) return HTTPInternalServerError;
		memcpy(pBuffer, pBody, nLen);
		*pLength = nLen;
		*ppContentType = "application/json; charset=utf-8";
		return HTTPOK;
	}

	// ---- POST /api/playlist/clear ----
	if (bIsPlClearPath)
	{
		m_pMT32Pi->PlaylistClear();
		constexpr const char* pBody = "{\"ok\":true}";
		const unsigned nLen = static_cast<unsigned>(__builtin_strlen(pBody));
		if (*pLength < nLen) return HTTPInternalServerError;
		memcpy(pBuffer, pBody, nLen);
		*pLength = nLen;
		*ppContentType = "application/json; charset=utf-8";
		return HTTPOK;
	}

	// ---- POST /api/playlist/up ----
	if (bIsPlUpPath)
	{
		char IdxVal[8] = {};
		unsigned nIndex = 0;
		if (pFormData && *pFormData && GetFormValue(pFormData, "index", IdxVal, sizeof(IdxVal)))
			nIndex = static_cast<unsigned>(atoi(IdxVal));
		const bool bOk = m_pMT32Pi->PlaylistMoveUp(nIndex);
		CString JSON;
		JSON.Format("{\"ok\":%s}", bOk ? "true" : "false");
		const unsigned nLen = JSON.GetLength();
		if (*pLength < nLen) return HTTPInternalServerError;
		memcpy(pBuffer, static_cast<const char*>(JSON), nLen);
		*pLength = nLen;
		*ppContentType = "application/json; charset=utf-8";
		return HTTPOK;
	}

	// ---- POST /api/playlist/down ----
	if (bIsPlDownPath)
	{
		char IdxVal[8] = {};
		unsigned nIndex = 0;
		if (pFormData && *pFormData && GetFormValue(pFormData, "index", IdxVal, sizeof(IdxVal)))
			nIndex = static_cast<unsigned>(atoi(IdxVal));
		const bool bOk = m_pMT32Pi->PlaylistMoveDown(nIndex);
		CString JSON;
		JSON.Format("{\"ok\":%s}", bOk ? "true" : "false");
		const unsigned nLen = JSON.GetLength();
		if (*pLength < nLen) return HTTPInternalServerError;
		memcpy(pBuffer, static_cast<const char*>(JSON), nLen);
		*pLength = nLen;
		*ppContentType = "application/json; charset=utf-8";
		return HTTPOK;
	}

	// ---- POST /api/playlist/shuffle ----
	if (bIsPlShufflePath)
	{
		char OnVal[8] = {};
		if (pFormData && *pFormData)
			GetFormValue(pFormData, "enabled", OnVal, sizeof(OnVal));
		m_pMT32Pi->PlaylistSetShuffle(strcmp(OnVal, "on") == 0);
		constexpr const char* pBody = "{\"ok\":true}";
		const unsigned nLen = static_cast<unsigned>(__builtin_strlen(pBody));
		if (*pLength < nLen) return HTTPInternalServerError;
		memcpy(pBuffer, pBody, nLen);
		*pLength = nLen;
		*ppContentType = "application/json; charset=utf-8";
		return HTTPOK;
	}

	// ---- POST /api/playlist/repeat ----
	if (bIsPlRepeatPath)
	{
		char OnVal[8] = {};
		if (pFormData && *pFormData)
			GetFormValue(pFormData, "enabled", OnVal, sizeof(OnVal));
		m_pMT32Pi->PlaylistSetRepeat(strcmp(OnVal, "on") == 0);
		constexpr const char* pBody = "{\"ok\":true}";
		const unsigned nLen = static_cast<unsigned>(__builtin_strlen(pBody));
		if (*pLength < nLen) return HTTPInternalServerError;
		memcpy(pBuffer, pBody, nLen);
		*pLength = nLen;
		*ppContentType = "application/json; charset=utf-8";
		return HTTPOK;
	}

	// ---- POST /api/playlist/play ----
	if (bIsPlPlayPath)
	{
		char IdxVal[8] = {};
		unsigned nIndex = 0;
		if (pFormData && *pFormData && GetFormValue(pFormData, "index", IdxVal, sizeof(IdxVal)))
			nIndex = static_cast<unsigned>(atoi(IdxVal));
		m_pMT32Pi->PlaylistPlay(nIndex);
		constexpr const char* pBody = "{\"ok\":true}";
		const unsigned nLen = static_cast<unsigned>(__builtin_strlen(pBody));
		if (*pLength < nLen) return HTTPInternalServerError;
		memcpy(pBuffer, pBody, nLen);
		*pLength = nLen;
		*ppContentType = "application/json; charset=utf-8";
		return HTTPOK;
	}

	return HTTPNotFound;
}

THTTPStatus CWebDaemon::BuildStylesheet(u8* pBuffer, unsigned* pLength, const char** ppContentType)
{
		static const char pCSS[] =
			"body{font:14px/1.45 system-ui,sans-serif;margin:0;background:#0f172a;color:#e2e8f0;}"
			"main{max-width:1040px;margin:0 auto;padding:24px;}"
			"h1{margin:0 0 8px;font-size:28px;}h2{font-size:18px;margin:0 0 12px;}"
			"p{margin:0 0 16px;color:#94a3b8;}"
			"section{background:#111827;border:1px solid #334155;border-radius:14px;padding:16px;margin-bottom:14px;}"
			"table{width:100%;border-collapse:collapse;}th,td{padding:6px 0;vertical-align:top;border-bottom:1px solid #1e293b;}"
			"th{width:44%;text-align:left;color:#93c5fd;font-weight:600;padding-right:12px;}td{color:#e5e7eb;}"
			".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(220px,1fr));gap:10px;}"
			"label{display:flex;flex-direction:column;gap:4px;color:#cbd5e1;}"
			"input,select,button{background:#0b1220;color:#e2e8f0;border:1px solid #334155;border-radius:8px;padding:8px;}"
			"input:disabled,select:disabled{opacity:.45;cursor:not-allowed;}"
			"button{cursor:pointer;}button.primary{background:#1d4ed8;border-color:#1d4ed8;}button.warn{background:#7f1d1d;border-color:#7f1d1d;}"
			"button.danger{background:#7f1d1d;border-color:#7f1d1d;}"
			"a{color:#93c5fd;text-decoration:none;}"
			"nav{display:flex;gap:16px;margin-bottom:18px;flex-wrap:wrap;padding:10px 14px;"
			"background:#111827;border:1px solid #334155;border-radius:12px;}"
			"nav a{color:#94a3b8;padding:4px 10px;border-radius:8px;}"
			"nav a:hover{color:#e2e8f0;background:#1e293b;}"
			"nav a.active{color:#93c5fd;background:#1e3a5f;font-weight:600;}"
			".hero{display:flex;flex-wrap:wrap;gap:12px;margin:14px 0 18px;}"
			".pill{background:#1e293b;border:1px solid #475569;border-radius:999px;padding:8px 14px;color:#f8fafc;}"
			".statusbar{display:grid;grid-template-columns:repeat(auto-fit,minmax(180px,1fr));gap:10px;margin-bottom:14px;}"
			".statusbar .pill{background:#0b1220;border-color:#334155;color:#cbd5e1;border-radius:999px;}"
			".badge{display:inline-block;padding:3px 10px;border-radius:999px;font-size:12px;background:#1e293b;border:1px solid #475569;}"
			".badge.playing{background:#14532d;border-color:#16a34a;}.badge.paused{background:#432d03;border-color:#d97706;}.badge.finished{background:#44403c;border-color:#a8a29e;}.badge.loading{background:#1e3a5f;border-color:#3b82f6;}.badge.recording{background:#4c0519;border-color:#e11d48;}"
			".tabbar{display:flex;gap:10px;flex-wrap:wrap;margin:0 0 14px;}"
			".tabbtn{background:#0b1220;color:#cbd5e1;border:1px solid #334155;border-radius:999px;padding:10px 14px;cursor:pointer;}"
			".tabbtn.active{background:#1d4ed8;border-color:#1d4ed8;color:#fff;}"
			".section-hidden{display:none;}"
			"button.loop-on{background:#1e3a5f;border-color:#3b82f6;color:#93c5fd;}"
			".prog-bg,.seq-prog-bg{background:#0b1220;border:1px solid #334155;border-radius:999px;height:10px;overflow:hidden;margin:8px 0;}"
			".prog-fill,.seq-prog-fill{height:100%;width:0%;background:linear-gradient(90deg,#1d4ed8,#22d3ee);transition:width .4s;}"
			".meter-grid{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:10px;max-width:100%;}"
			".meter{display:flex;align-items:center;gap:8px;min-width:0;}"
			".meter-label{width:32px;flex:0 0 32px;color:#93c5fd;font-size:12px;}"
			".meter-bar{position:relative;flex:1;min-width:0;height:10px;background:#0b1220;border:1px solid #334155;border-radius:999px;overflow:hidden;}"
			".meter-fill{position:absolute;left:0;top:0;bottom:0;background:linear-gradient(90deg,#22d3ee,#4ade80,#facc15,#f97316);width:0%;}"
			".meter-peak{position:absolute;top:-1px;bottom:-1px;width:2px;background:#f8fafc;left:0%;}"
			"@media(max-width:860px){.meter-grid{grid-template-columns:1fr;}}"
			".subgroup-title{grid-column:1/-1;color:#7dd3fc;font-size:11px;font-weight:700;margin:10px 0 2px;"
			"padding-bottom:4px;border-bottom:1px solid #1e293b;text-transform:uppercase;letter-spacing:.06em;}"
			"canvas{display:block;width:100%;border-radius:10px;background:#0a1020;}"
			"code{font:12px ui-monospace,monospace;color:#bfdbfe;}"
			"#status,#rtStatus{margin-top:10px;color:#93c5fd;}"
		"label small{display:block;font-size:.72em;color:#64748b;line-height:1.3;margin-bottom:2px;}"
		"#_tray{position:fixed;top:16px;right:16px;z-index:9999;display:flex;flex-direction:column;gap:8px;pointer-events:none;}"
		".tst{padding:10px 16px;border-radius:10px;font-size:13px;opacity:0;transform:translateX(40px);transition:opacity .25s,transform .25s;background:#1e293b;border:1px solid #475569;color:#e2e8f0;}"
		".tst.show{opacity:1;transform:translateX(0);}.tst.ok{background:#14532d;border-color:#16a34a;color:#4ade80;}.tst.err{background:#7f1d1d;border-color:#ef4444;color:#fca5a5;}"
		"#_dm{position:fixed;bottom:16px;right:16px;padding:6px 14px;border-radius:999px;cursor:pointer;z-index:9999;font-size:12px;background:#1e293b;border:1px solid #475569;color:#cbd5e1;}"
		"body.light{background:#f1f5f9;color:#0f172a;}"
		"body.light section{background:#fff;border-color:#e2e8f0;}"
		"body.light th{color:#1d4ed8;}body.light td{color:#1e293b;}"
		"body.light nav{background:#fff;border-color:#e2e8f0;}"
		"body.light nav a{color:#475569;}body.light nav a:hover{background:#f1f5f9;color:#0f172a;}"
		"body.light nav a.active{color:#1d4ed8;background:#dbeafe;}"
		"body.light .pill{background:#e2e8f0;border-color:#cbd5e1;color:#0f172a;}"
		"body.light input,body.light select,body.light button{background:#f8fafc;color:#0f172a;border-color:#cbd5e1;}"
		"body.light p{color:#475569;}body.light h1,body.light h2{color:#0f172a;}"
		"body.light #_dm{background:#e2e8f0;border-color:#cbd5e1;color:#475569;}"
		"body.light .tst{background:#f8fafc;border-color:#cbd5e1;color:#0f172a;}"
		"body.light button.primary{background:#1d4ed8;border-color:#1d4ed8;color:#fff;}"
		"body.light button.danger,body.light button.warn{background:#7f1d1d;border-color:#7f1d1d;color:#fff;}"
		"body.light button.loop-on{background:#dbeafe;border-color:#3b82f6;color:#1d4ed8;}"
		"body.light .tabbtn{background:#e2e8f0;color:#1e293b;border-color:#cbd5e1;}"
		"body.light .tabbtn.active{background:#1d4ed8;border-color:#1d4ed8;color:#fff;}"
		"body.light .badge{background:#e2e8f0;border-color:#cbd5e1;color:#374151;}"
		"body.light .badge.playing{background:#dcfce7;border-color:#16a34a;color:#15803d;}"
		"body.light .badge.paused{background:#fef9c3;border-color:#d97706;color:#92400e;}"
		"body.light .badge.recording{background:#fee2e2;border-color:#e11d48;color:#be123c;}"
		"body.light .badge.loading{background:#dbeafe;border-color:#3b82f6;color:#1d4ed8;}"
		"body.light .badge.finished{background:#f3f4f6;border-color:#9ca3af;color:#374151;}"
		"body.light .prog-bg,body.light .seq-prog-bg{background:#e2e8f0;border-color:#cbd5e1;}"
		"body.light .meter-bar{background:#e2e8f0;border-color:#cbd5e1;}"
		"body.light canvas{background:#f1f5f9;}"
		"@media(max-width:600px){main{padding:12px;}h1{font-size:22px;}nav{gap:8px;padding:8px 10px;}nav a{padding:3px 8px;font-size:13px;}.grid{grid-template-columns:1fr;}table{font-size:12px;}#_dm{bottom:10px;right:10px;}}";
		const unsigned nLen = static_cast<unsigned>(std::strlen(pCSS));
		if (*pLength < nLen) return HTTPInternalServerError;
		memcpy(pBuffer, pCSS, nLen);
		*pLength = nLen;
		*ppContentType = "text/css; charset=utf-8";
		return HTTPOK;
}

THTTPStatus CWebDaemon::BuildScript(u8* pBuffer, unsigned* pLength, const char** ppContentType)
{
		// Shared JS: serial queue helper _qs(), fmt(), WS connect with reconnect
		static const char pJS[] =
			"var _q=[],_qb=false;"
			"function _qd(){if(!_q.length){_qb=false;return;}_qb=true;var e=_q.shift();"
			"var ac=new AbortController(),_ti=setTimeout(function(){ac.abort();},500);"
			"fetch(e[0],{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:e[1],signal:ac.signal})"
			".then(function(r){clearTimeout(_ti);return r.json();}).then(function(j){if(e[2])e[2](j);})"
			".catch(function(){clearTimeout(_ti);if(e[2])e[2](null);}).finally(_qd);}"
			"function _qs(u,b,cb){_q.push([u,b||'',cb||null]);if(!_qb)_qd();}"
			"function fmt(ms){var s=Math.floor(ms/1000);return Math.floor(s/60)+':'+(s%60<10?'0':'')+(s%60);}"
			// Active-nav: mark current page in navbar
			"(function(){"
			"var p=location.pathname;document.querySelectorAll('nav a').forEach(function(a){"
			"var h=a.getAttribute('href');if(h==='/'?p===h:p.startsWith(h))a.classList.add('active');"
		"});})();"
		"(function(){var t=document.createElement('div');t.id='_tray';document.body.appendChild(t);})();"
		"function showToast(msg,ok){var tr=document.getElementById('_tray');if(!tr)return;"
		"var t=document.createElement('div');t.className='tst'+(ok===false?' err':' ok');"
		"t.textContent=msg;tr.appendChild(t);"
		"setTimeout(function(){t.classList.add('show');},10);"
		"setTimeout(function(){t.classList.remove('show');setTimeout(function(){if(t.parentNode)t.parentNode.removeChild(t);},300);},2500);}"
		"(function(){var d=localStorage.getItem('dm')==='1';if(d)document.body.classList.add('light');"
		"var b=document.createElement('button');b.id='_dm';b.textContent=d?'Light mode':'Dark mode';"
		"b.onclick=function(){var on=document.body.classList.toggle('light');"
		"localStorage.setItem('dm',on?'1':'0');b.textContent=on?'Light mode':'Dark mode';};"
		"document.body.appendChild(b);})();";
		const unsigned nLen = static_cast<unsigned>(std::strlen(pJS));
		if (*pLength < nLen) return HTTPInternalServerError;
		memcpy(pBuffer, pJS, nLen);
		*pLength = nLen;
		*ppContentType = "application/javascript; charset=utf-8";
		return HTTPOK;
}

THTTPStatus CWebDaemon::BuildSequencerPage(u8* pBuffer, unsigned* pLength, const char** ppContentType)
{
		HtmlWriter html(pBuffer, *pLength);
		html.Append("<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width, initial-scale=1'>");
		html.Append("<title>mt32-pi sequencer</title><link rel='stylesheet' href='/app.css'>");
		html.Append("<style>");
		// Player card
		html.Append("#sq-card{background:#111827;border:1px solid #1e293b;border-radius:16px;padding:0;margin-bottom:14px;overflow:hidden;}");
		html.Append("#sq-hdr{display:flex;align-items:center;justify-content:space-between;gap:10px;padding:14px 16px 10px;border-bottom:1px solid #1e293b;}");
		html.Append("#sq-hdr h1{margin:0;font-size:17px;font-weight:700;color:#f8fafc;white-space:nowrap;}");
		html.Append("#sq-file-wrap{display:flex;align-items:center;gap:6px;min-width:0;}");
		html.Append("#seq-file{max-width:220px;font-size:12px;padding:5px 8px;border-radius:8px;}");
		html.Append("#sq-refresh{padding:4px 8px;font-size:12px;border-radius:8px;background:transparent;border:1px solid #334155;color:#64748b;cursor:pointer;}");
		html.Append("#sq-refresh:hover{border-color:#475569;color:#94a3b8;}");
		html.Append("#sq-body{padding:14px 16px 16px;}");
		// Status row
		html.Append("#sq-status-row{display:flex;align-items:center;justify-content:space-between;margin-bottom:8px;}");
		html.Append("#sq-time{font-size:13px;color:#64748b;font-variant-numeric:tabular-nums;}");
		html.Append("#sq-time strong{color:#94a3b8;}");
		// Progress bar
		html.Append("#pb{cursor:pointer;height:8px;border-radius:999px;background:#1e293b;position:relative;margin-bottom:14px;touch-action:none;}");
		html.Append("#pb:hover{background:#1e3a5f;}");
		html.Append("#prog{height:100%;border-radius:999px;background:linear-gradient(90deg,#1d4ed8,#22d3ee);width:0%;pointer-events:none;transition:width .15s;}");
		// Transport
		html.Append("#sq-transport{display:flex;align-items:center;justify-content:center;gap:6px;margin-bottom:12px;}");
		html.Append(".trn-btn{background:transparent;border:1px solid #334155;border-radius:10px;color:#94a3b8;padding:8px 14px;font-size:15px;cursor:pointer;transition:background .1s,color .1s,border-color .1s;}");
		html.Append(".trn-btn:hover{background:#1e293b;color:#e2e8f0;border-color:#475569;}");
		html.Append("#trn-play{background:#1d4ed8;border-color:#1d4ed8;color:#fff;padding:10px 28px;font-size:16px;font-weight:600;border-radius:12px;}");
		html.Append("#trn-play:hover{background:#2563eb;border-color:#2563eb;}");
		// Toggle pills (Loop / Auto-next)
		html.Append("#sq-toggles{display:flex;align-items:center;justify-content:center;gap:8px;margin-bottom:14px;}");
		html.Append(".tgl-pill{display:flex;align-items:center;gap:5px;font-size:12px;color:#64748b;cursor:pointer;padding:4px 10px;border-radius:999px;border:1px solid #334155;user-select:none;transition:background .1s,color .1s,border-color .1s;}");
		html.Append(".tgl-pill.on{background:#1e3a5f;border-color:#3b82f6;color:#93c5fd;}");
		html.Append(".tgl-pill .dot{width:8px;height:8px;border-radius:50%;background:#334155;transition:background .15s;}");
		html.Append(".tgl-pill.on .dot{background:#3b82f6;}");
		// Tempo row
		html.Append("#sq-tempo-row{display:flex;align-items:center;justify-content:space-between;border-top:1px solid #1e293b;padding-top:12px;flex-wrap:wrap;gap:8px;}");
		html.Append(".tmeta{font-size:12px;color:#64748b;}");
		html.Append(".tmeta strong{color:#94a3b8;}");
		html.Append("#sq-speed-ctrl{display:flex;align-items:center;gap:4px;}");
		html.Append(".spd-btn{width:26px;height:26px;border-radius:6px;font-size:14px;display:flex;align-items:center;justify-content:center;padding:0;background:transparent;border:1px solid #334155;color:#64748b;cursor:pointer;}");
		html.Append(".spd-btn:hover{background:#1e293b;color:#e2e8f0;}");
		html.Append("#seq-tmv{font-size:13px;font-weight:700;color:#93c5fd;min-width:46px;text-align:center;}");
		html.Append("#spd-reset{font-size:10px;padding:2px 6px;border-radius:5px;border:1px solid #1e3a5f;background:transparent;color:#475569;cursor:pointer;}");
		html.Append("#spd-reset:hover{color:#94a3b8;border-color:#334155;}");
		// Playlist
		html.Append("#sq-pl{background:#111827;border:1px solid #1e293b;border-radius:16px;overflow:hidden;}");
		html.Append("#sq-pl-hdr{display:flex;align-items:center;justify-content:space-between;padding:12px 16px;border-bottom:1px solid #1e293b;}");
		html.Append("#sq-pl-hdr h2{margin:0;font-size:15px;color:#f8fafc;}");
		html.Append("#sq-pl-acts{display:flex;gap:6px;}");
		html.Append(".pl-act{font-size:11px;padding:3px 9px;border-radius:6px;border:1px solid #334155;background:transparent;color:#64748b;cursor:pointer;}");
		html.Append(".pl-act:hover{background:#1e293b;color:#94a3b8;}");
		html.Append(".pl-act.primary{background:#1d4ed8;border-color:#1d4ed8;color:#fff;}");
		html.Append(".pl-act.primary:hover{background:#2563eb;}");
		html.Append(".pl-act.danger{background:#7f1d1d;border-color:#7f1d1d;color:#fff;}");
		html.Append("#sq-pl-opts{display:flex;gap:6px;padding:8px 16px;border-bottom:1px solid #1e293b;}");
		html.Append("#pl-body{padding:4px 0;}");
		html.Append(".pl-empty{display:flex;flex-direction:column;align-items:center;justify-content:center;gap:8px;padding:28px 16px;color:#475569;font-size:13px;}");
		html.Append(".pl-empty-btn{font-size:12px;padding:5px 14px;border-radius:8px;border:1px dashed #334155;background:transparent;color:#64748b;cursor:pointer;}");
		html.Append(".pl-empty-btn:hover{border-color:#475569;color:#94a3b8;}");
		html.Append(".pl-row{display:flex;align-items:center;gap:6px;padding:5px 12px;border-left:3px solid transparent;transition:background .1s;}");
		html.Append(".pl-row:hover{background:#0b1220;}");
		html.Append(".pl-row.active{background:#0f2744;border-left-color:#1d4ed8;}");
		html.Append(".pl-name{flex:1;font-size:13px;color:#cbd5e1;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;cursor:pointer;}");
		html.Append(".pl-row.active .pl-name{color:#93c5fd;font-weight:600;}");
		html.Append(".pl-btns{display:flex;gap:2px;flex-shrink:0;}");
		html.Append(".pl-btn{font-size:11px;padding:1px 5px;border-radius:4px;border:1px solid #1e293b;background:transparent;color:#475569;cursor:pointer;}");
		html.Append(".pl-btn:hover{background:#1e293b;color:#94a3b8;}");
		html.Append(".pl-btn.rm{color:#7f1d1d;border-color:#7f1d1d;}");
		html.Append(".pl-btn.rm:hover{background:#7f1d1d;color:#fff;}");
		// Light mode
		html.Append("body.light #sq-card,body.light #sq-pl{background:#fff;border-color:#e2e8f0;}");
		html.Append("body.light #sq-hdr,body.light #sq-pl-hdr,body.light #sq-pl-opts,body.light #sq-tempo-row{border-color:#e2e8f0;}");
		html.Append("body.light #sq-hdr h1,body.light #sq-pl-hdr h2{color:#0f172a;}");
		html.Append("body.light #pb{background:#e2e8f0;}body.light #pb:hover{background:#dbeafe;}");
		html.Append("body.light #sq-time{color:#475569;}body.light #sq-time strong{color:#374151;}");
		html.Append("body.light .trn-btn{border-color:#cbd5e1;color:#475569;}body.light .trn-btn:hover{background:#f1f5f9;color:#0f172a;}");
		html.Append("body.light #trn-play{background:#1d4ed8;border-color:#1d4ed8;color:#fff;}");
		html.Append("body.light .tgl-pill{border-color:#cbd5e1;color:#64748b;}body.light .tgl-pill.on{background:#dbeafe;border-color:#3b82f6;color:#1d4ed8;}");
		html.Append("body.light .tgl-pill .dot{background:#cbd5e1;}body.light .tgl-pill.on .dot{background:#3b82f6;}");
		html.Append("body.light .tmeta{color:#64748b;}body.light .tmeta strong{color:#374151;}");
		html.Append("body.light #seq-tmv{color:#1d4ed8;}");
		html.Append("body.light .spd-btn{border-color:#cbd5e1;color:#475569;}body.light .spd-btn:hover{background:#f1f5f9;}");
		html.Append("body.light #spd-reset{border-color:#cbd5e1;color:#64748b;}");
		html.Append("body.light .pl-act{border-color:#cbd5e1;color:#475569;}body.light .pl-act:hover{background:#f1f5f9;color:#0f172a;}");
		html.Append("body.light .pl-row:hover{background:#f8fafc;}body.light .pl-row.active{background:#dbeafe;border-left-color:#1d4ed8;}");
		html.Append("body.light .pl-name{color:#374151;}body.light .pl-row.active .pl-name{color:#1d4ed8;}");
		html.Append("body.light .pl-btn{border-color:#e2e8f0;color:#64748b;}body.light .pl-btn:hover{background:#f1f5f9;color:#374151;}");
		html.Append("body.light .pl-empty{color:#94a3b8;}body.light .pl-empty-btn{border-color:#cbd5e1;color:#64748b;}");
		html.Append("#seq-file{max-width:200px;}");
		html.Append("</style></head><body><main>");
		html.Append("<script src='/app.js'></script>");
		html.Append("<nav><a href='/'>Status</a><a href='/sound'>Sound</a><a href='/config'>Config</a><a href='/sequencer' class='active'>Sequencer</a><a href='/mixer'>Mixer</a><a href='/monitor'>Monitor</a></nav>");

		// ── Player card ───────────────────────────────────────────────────────
		html.Append("<div id='sq-card'>");
		// Header: title + file selector
		html.Append("<div id='sq-hdr'>");
		html.Append("<h1>&#127926; Sequencer</h1>");
		html.Append("<div id='sq-file-wrap'>");
		html.Append("<select id='seq-file' onchange='doPlaySelected()'><option value=''>Loading...</option></select>");
		html.Append("<button id='sq-refresh' onclick='loadFiles()' title='Refresh file list'>&#8635;</button>");
		html.Append("</div></div>");
		// Body
		html.Append("<div id='sq-body'>");
		// Status + time row
		html.Append("<div id='sq-status-row'>");
		html.Append("<span id='seq-badge' class='badge'>Stopped</span>");
		html.Append("<div id='sq-time'><strong id='seq-elapsed'>0:00</strong> / <span id='seq-duration'>0:00</span></div>");
		html.Append("</div>");
		// Progress bar (seekable)
		html.Append("<div id='pb' onclick='seekClick(event,this)' title='Click to seek'>");
		html.Append("<div id='prog'></div></div>");
		// Transport buttons
		html.Append("<div id='sq-transport'>");
		html.Append("<button class='trn-btn' onclick='doPrev()' title='Previous'>&#9664;&#9664;</button>");
		html.Append("<button class='trn-btn' id='trn-play' onclick='_trPlay()'>&#9654;</button>");
		html.Append("<button class='trn-btn' onclick='doStop()' title='Stop'>&#9646;</button>");
		html.Append("<button class='trn-btn' onclick='doNext()' title='Next'>&#9654;&#9654;</button>");
		html.Append("</div>");
		// Toggle pills: Loop + Auto-next
		html.Append("<div id='sq-toggles'>");
		html.Append("<div class='tgl-pill' id='tgl-loop' onclick='toggleLoop()' title='Loop'><span class='dot'></span>Loop</div>");
		html.Append("<div class='tgl-pill' id='tgl-autonext' onclick='toggleAutoNext()' title='Auto-next'><span class='dot'></span>Auto-next</div>");
		html.Append("</div>");
		// Tempo row
		html.Append("<div id='sq-tempo-row'>");
		html.Append("<div class='tmeta'>&#9835; <strong id='seq-bpm'>&#8212;</strong> BPM</div>");
		html.Append("<div id='sq-speed-ctrl'>");
		html.Append("<button class='spd-btn' onclick='adjTempo(-0.1)'>&#8722;</button>");
		html.Append("<span id='seq-tmv'>1.00&#215;</span>");
		html.Append("<button class='spd-btn' onclick='adjTempo(0.1)'>+</button>");
		html.Append("<button id='spd-reset' onclick='adjTempo(0)'>1&#215;</button>");
		html.Append("</div></div>");
		html.Append("</div></div>"); // close sq-body + sq-card

		// ── Playlist ──────────────────────────────────────────────────────────
		html.Append("<div id='sq-pl' style='margin-top:14px;'>");
		html.Append("<div id='sq-pl-hdr'>");
		html.Append("<h2>Playlist <span id='pl-count' style='font-size:12px;font-weight:400;color:#64748b;'></span></h2>");
		html.Append("<div id='sq-pl-acts'>");
		html.Append("<button class='pl-act primary' onclick='plAdd()'>+ Add</button>");
		html.Append("<button class='pl-act' onclick='plAddAll()'>+ All</button>");
		html.Append("<button class='pl-act danger' onclick='plClear()'>Clear</button>");
		html.Append("</div></div>");
		html.Append("<div id='sq-pl-opts'>");
		html.Append("<div class='tgl-pill' id='tgl-shuffle' onclick='togglePlShuffle()' title='Shuffle'><span class='dot'></span>Shuffle</div>");
		html.Append("<div class='tgl-pill' id='tgl-repeat' onclick='togglePlRepeat()' title='Repeat'><span class='dot'></span>Repeat</div>");
		html.Append("</div>");
		html.Append("<div id='pl-body'></div>");
		html.Append("</div>"); // close sq-pl

		html.Append("<script>");
		// State
		html.Append("var _wsOk=false,_ls={},_ct=1.0;");
		html.Append("var _lElp=0,_lDur=1,_lTime=0,_interp=null;");
		html.Append("var _plCnt=-1,_plShuffle=false,_plRepeat=false,_plIdx=-1;");
		// Progress bar
		html.Append("function _updBar(elp,dur){");
		html.Append("document.getElementById('prog').style.width=Math.min(100,elp/dur*100).toFixed(1)+'%';");
		html.Append("document.getElementById('seq-elapsed').textContent=fmt(elp);}");
		html.Append("function _startInterp(){_interp=setInterval(function(){");
		html.Append("var est=Math.min(_lDur,_lElp+(Date.now()-_lTime)*_ct);");
		html.Append("_updBar(est,_lDur);},150);}");
		// Toggle pill helpers
		html.Append("function _setPill(id,on){var el=document.getElementById(id);if(el){if(on)el.classList.add('on');else el.classList.remove('on');}}");
		// Transport: unified play action
		html.Append("function _trPlay(){");
		html.Append("var st=_ls.loading?'loading':(_ls.paused?'paused':(_ls.playing?'playing':(_ls.finished?'finished':'stopped')));");
		html.Append("if(st==='playing')_qs('/api/sequencer/pause','',function(){});");
		html.Append("else if(st==='paused')_qs('/api/sequencer/resume','',function(){});");
		html.Append("else doPlaySelected();}");
		// Play selected file
		html.Append("function doPlaySelected(){var f=document.getElementById('seq-file').value;");
		html.Append("if(!f)return;");
		html.Append("_qs('/api/sequencer/play','file='+encodeURIComponent(f),function(j){if(!j||!j.ok)showToast('Play failed',false);});}");
		// applyStatus
		html.Append("function applyStatus(d){if(!d)return;_ls=d;");
		html.Append("var st=d.loading?'loading':(d.paused?'paused':(d.playing?'playing':(d.finished?'finished':'stopped')));");
		// Badge
		html.Append("var b=document.getElementById('seq-badge');");
		html.Append("b.textContent=st==='loading'?'Loading\u2026':(st==='playing'?'Playing':(st==='paused'?'Paused':(st==='finished'?'Finished':'Stopped')));");
		html.Append("b.className='badge'+(st==='playing'?' playing':(st==='finished'?' finished':(st==='loading'?' loading':(st==='paused'?' paused':''))));");
		// Play button icon
		html.Append("var pb2=document.getElementById('trn-play');");
		html.Append("if(pb2)pb2.textContent=st==='playing'?'\\u23F8':(st==='paused'?'\\u25B6':'\\u25B6');");
		// Sync file selector if possible
		html.Append("var fn=(d.file||'');");
		html.Append("var sel=document.getElementById('seq-file');");
		html.Append("if(sel&&fn&&sel.value!==fn){for(var oi=0;oi<sel.options.length;oi++){if(sel.options[oi].value===fn){sel.value=fn;break;}}}");
		// Duration / progress
		html.Append("var dur=d.duration_ms>0?d.duration_ms:1;");
		html.Append("var elp=st==='finished'?dur:(d.elapsed_ms||0);");
		html.Append("document.getElementById('seq-duration').textContent=fmt(d.duration_ms||0);");
		html.Append("if(d.tempo_multiplier!==undefined)_ct=d.tempo_multiplier;");
		html.Append("_lElp=elp;_lDur=dur;_lTime=Date.now();");
		html.Append("if(_interp){clearInterval(_interp);_interp=null;}");
		html.Append("if(st==='playing'){_updBar(elp,dur);_startInterp();}else{_updBar(elp,dur);}");
		// BPM + speed
		html.Append("var bpm=d.bpm||0;document.getElementById('seq-bpm').textContent=bpm>0?bpm:'\\u2014';");
		html.Append("var tm=(_ct||1).toFixed(2)+'\\u00d7';document.getElementById('seq-tmv').textContent=tm;");
		// Loop / auto-next pills
		html.Append("_setPill('tgl-loop',!!d.loop_enabled);");
		html.Append("_setPill('tgl-autonext',!!d.auto_next);");
		// Playlist state
		html.Append("if(typeof d.pl_count!=='undefined'){");
		html.Append("var pc=document.getElementById('pl-count');if(pc)pc.textContent=d.pl_count>0?'('+d.pl_count+' tracks)':'';");
		html.Append("_setPill('tgl-shuffle',!!d.pl_shuffle);_setPill('tgl-repeat',!!d.pl_repeat);");
		html.Append("_plShuffle=!!d.pl_shuffle;_plRepeat=!!d.pl_repeat;");
		html.Append("if(d.pl_count!==_plCnt||d.pl_idx!==_plIdx){_plCnt=d.pl_count;_plIdx=d.pl_idx;loadPlaylist();}");
		html.Append("else{document.querySelectorAll('.pl-row').forEach(function(r,i){if(i===d.pl_idx)r.classList.add('active');else r.classList.remove('active');});}}");
		html.Append("}"); // close applyStatus
		// Poll fallback + WebSocket
		html.Append("function schedPoll(){if(_wsOk)return;setTimeout(function(){_qs('/api/sequencer/status','',function(d){applyStatus(d);if(!_wsOk)schedPoll();});},1000);}");
		html.Append("(function(){var ws=null,_rt=0;");
		html.Append("function wsConnect(){ws=new WebSocket('ws://'+location.hostname+':8765/');");
		html.Append("ws.onopen=function(){_wsOk=true;};");
		html.Append("ws.onmessage=function(e){try{applyStatus(JSON.parse(e.data));}catch(x){}};");
		html.Append("ws.onclose=function(){_wsOk=false;schedPoll();_rt=Math.min((_rt||500)*2,8000);setTimeout(wsConnect,_rt);};");
		html.Append("ws.onerror=function(){ws.close();};}wsConnect();})();");
		// Seek
		html.Append("function seekClick(ev,el){var r=el.getBoundingClientRect();");
		html.Append("var f=Math.max(0,Math.min(1,(ev.clientX-r.left)/r.width));");
		html.Append("var tt=(_ls&&_ls.total_ticks)||0;if(tt<=0)return;");
		html.Append("_lElp=Math.round(f*_lDur);_lTime=Date.now();");
		html.Append("_qs('/api/sequencer/seek','ticks='+Math.round(f*tt),function(){});}");
		// Tempo
		html.Append("function adjTempo(delta){var t=delta===0?1.0:Math.round(Math.max(0.1,Math.min(4.0,_ct+delta))*10)/10;");
		html.Append("_qs('/api/sequencer/tempo','multiplier='+t,function(){_ct=t;document.getElementById('seq-tmv').textContent=t.toFixed(2)+'\\u00d7';});}");
		// File list
		html.Append("function loadFiles(){var sel=document.getElementById('seq-file');");
		html.Append("_qs('/api/sequencer/files','',function(files){sel.innerHTML='';");
		html.Append("if(!files||!files.length){sel.innerHTML='<option value=\"\">No MIDI files</option>';return;}");
		html.Append("for(var i=0;i<files.length;i++){var o=document.createElement('option');o.value=files[i];");
		html.Append("o.textContent=files[i].replace(/^(SD:|USB:)/,'');sel.appendChild(o);}");
		html.Append("if(_ls&&_ls.file){for(var oi=0;oi<sel.options.length;oi++){if(sel.options[oi].value===_ls.file){sel.value=_ls.file;break;}}}});}");
		// Controls
		html.Append("function doStop(){_qs('/api/sequencer/stop','',function(){});}");
		html.Append("function doNext(){_qs('/api/sequencer/next','',function(){});}");
		html.Append("function doPrev(){_qs('/api/sequencer/prev','',function(){});}");
		html.Append("function toggleLoop(){var on=!document.getElementById('tgl-loop').classList.contains('on');");
		html.Append("_qs('/api/sequencer/loop','enabled='+(on?'on':'off'),function(){_setPill('tgl-loop',on);});}");
		html.Append("function toggleAutoNext(){var on=!document.getElementById('tgl-autonext').classList.contains('on');");
		html.Append("_qs('/api/sequencer/autonext','enabled='+(on?'on':'off'),function(){_setPill('tgl-autonext',on);});}");
		// Playlist
		html.Append("function loadPlaylist(){_qs('/api/playlist','',function(d){");
		html.Append("if(!d)return;_plCnt=d.count;_plShuffle=d.shuffle;_plRepeat=d.repeat;");
		html.Append("var pc=document.getElementById('pl-count');if(pc)pc.textContent=d.count>0?'('+d.count+' tracks)':'';");
		html.Append("_setPill('tgl-shuffle',!!d.shuffle);_setPill('tgl-repeat',!!d.repeat);");
		html.Append("var el=document.getElementById('pl-body');if(!el)return;");
		html.Append("if(!d.entries||!d.entries.length){");
		html.Append("el.innerHTML='<div class=\"pl-empty\"><div>No tracks loaded</div><button class=\"pl-empty-btn\" onclick=\"plAdd()\">+ Add MIDI</button></div>';return;}");
		html.Append("el.innerHTML='';");
		html.Append("for(var i=0;i<d.entries.length;i++){");
		html.Append("var cur=i===d.index;");
		html.Append("var nm=d.entries[i].replace(/^(SD:|USB:)/,'');");
		html.Append("var row=document.createElement('div');row.className='pl-row'+(cur?' active':'');");
		html.Append("row.innerHTML='<div class=\"pl-name\" onclick=\"plPlay('+i+')\">'+nm+'</div>'");
		html.Append("+'<div class=\"pl-btns\">'");
		html.Append("+'<button class=\"pl-btn\" onclick=\"plUp('+i+')\">\\u2191</button>'");
		html.Append("+'<button class=\"pl-btn\" onclick=\"plDown('+i+')\">\\u2193</button>'");
		html.Append("+'<button class=\"pl-btn rm\" onclick=\"plRemove('+i+')\">\\u00d7</button>'");
		html.Append("+'</div>';");
		html.Append("el.appendChild(row);}});}");
		html.Append("function plAdd(){var f=document.getElementById('seq-file').value;");
		html.Append("if(!f){showToast('Select a file first.',false);return;}");
		html.Append("_qs('/api/playlist/add','file='+encodeURIComponent(f),function(){loadPlaylist();});}");
		html.Append("function plAddAll(){_qs('/api/playlist/add-all','',function(){loadPlaylist();});}");
		html.Append("function plClear(){_qs('/api/playlist/clear','',function(){loadPlaylist();});}");
		html.Append("function plRemove(i){_qs('/api/playlist/remove','index='+i,function(){loadPlaylist();});}");
		html.Append("function plUp(i){_qs('/api/playlist/up','index='+i,function(){loadPlaylist();});}");
		html.Append("function plDown(i){_qs('/api/playlist/down','index='+i,function(){loadPlaylist();});}");
		html.Append("function plPlay(i){_qs('/api/playlist/play','index='+i,function(){});}");
		html.Append("function togglePlShuffle(){var on=!document.getElementById('tgl-shuffle').classList.contains('on');");
		html.Append("_qs('/api/playlist/shuffle','enabled='+(on?'on':'off'),function(){loadPlaylist();});}");
		html.Append("function togglePlRepeat(){var on=!document.getElementById('tgl-repeat').classList.contains('on');");
		html.Append("_qs('/api/playlist/repeat','enabled='+(on?'on':'off'),function(){loadPlaylist();});}");
		// Keyboard shortcut: Space = play/pause
		html.Append("document.addEventListener('keydown',function(e){");
		html.Append("if(e.code==='Space'&&e.target.tagName!=='INPUT'&&e.target.tagName!=='SELECT'){e.preventDefault();_trPlay();}");
		html.Append("});");
		html.Append("loadPlaylist();loadFiles();schedPoll();");
		html.Append("</script></main></body></html>");

		if (!html.bOk)
			return HTTPInternalServerError;
		*pLength = html.Length();
		*ppContentType = "text/html; charset=utf-8";
		return HTTPOK;
}

THTTPStatus CWebDaemon::BuildMixerPage(u8* pBuffer, unsigned* pLength, const char** ppContentType)
{
		HtmlWriter html(pBuffer, *pLength);
		html.Append("<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width, initial-scale=1'>");
		html.Append("<title>mt32-pi mixer</title><link rel='stylesheet' href='/app.css'></head><body><main>");
		html.Append("<script src='/app.js'></script>");
		html.Append("<h1>MIDI Mixer / Router</h1>");
		html.Append("<p>Route MIDI channels to engines and remap channel numbers.</p>");
		html.Append("<nav><a href='/'>Status</a><a href='/sound'>Sound</a><a href='/config'>Config</a><a href='/sequencer'>Sequencer</a><a href='/mixer'>Mixer</a><a href='/monitor'>Monitor</a></nav>");

		// Preset + Enable section
		html.Append("<section><h2>Mode</h2><div class='grid'>");
		html.Append("<label>Preset<select id='mx-preset' onchange='setPreset(this.value)'>");
		html.Append("<option value='0'>Single MT-32</option><option value='1'>Single FluidSynth</option>");
		html.Append("<option value='2'>Single OPL3</option><option value='3'>Split GM</option><option value='4'>Custom</option></select></label>");
		html.Append("<label>Mixer<select id='mx-enabled' onchange='setEnabled(this.value)'>");
		html.Append("<option value='1'>Enabled</option><option value='0'>Disabled</option></select></label>");
		html.Append("</div></section>");

		// Volume / Pan
		html.Append("<section><h2>Engine Levels</h2><div class='grid'>");
		html.Append("<label>MT-32 vol<input id='mx-mt32v' type='range' min='0' max='100' oninput='clearTimeout(this._t);this._t=setTimeout(()=>setParam(\"mt32_volume\",this.value),40)'></label>");
		html.Append("<label>FluidSynth vol<input id='mx-fluidv' type='range' min='0' max='100' oninput='clearTimeout(this._t);this._t=setTimeout(()=>setParam(\"fluid_volume\",this.value),40)'></label>");
		html.Append("<label>OPL3 vol<input id='mx-opl3v' type='range' min='0' max='100' oninput='clearTimeout(this._t);this._t=setTimeout(()=>setParam(\"opl3_volume\",this.value),40)'></label>");
		html.Append("<label>MT-32 pan<input id='mx-mt32p' type='range' min='-100' max='100' oninput='clearTimeout(this._t);this._t=setTimeout(()=>setParam(\"mt32_pan\",this.value),40)'></label>");
		html.Append("<label>FluidSynth pan<input id='mx-fluidp' type='range' min='-100' max='100' oninput='clearTimeout(this._t);this._t=setTimeout(()=>setParam(\"fluid_pan\",this.value),40)'></label>");
		html.Append("</div></section>");

		// Audio render performance - redesigned
		html.Append("<section><h2>Audio Performance</h2>");
		// Status pill + translated metrics
		html.Append("<div style='display:flex;gap:10px;align-items:center;flex-wrap:wrap;margin-bottom:12px;'>");
		html.Append("<span id='mx-status' style='font-weight:700;font-size:13px;padding:4px 14px;border-radius:999px;background:#14532d;border:1px solid #16a34a;color:#4ade80;'>&#9679; OK</span>");
		html.Append("<span style='font-size:12px;color:#94a3b8;'>Latency <strong id='mx-lat'>-</strong> ms &bull; Headroom <strong id='mx-head'>-</strong>% &bull; CPU <strong id='mx-cpu' style='color:#22c55e;'>-</strong>%<span id='mx-cpu-hint' style='display:none;margin-left:8px;font-size:11px;'></span></span>");
		html.Append("</div>");
		// Technical detail (small)
		html.Append("<div style='font-size:11px;color:#475569;margin-bottom:12px;display:flex;gap:14px;flex-wrap:wrap;'>");
		html.Append("<span>Render <b id='mx-render' style='color:#64748b;'>-</b>&micro;s</span>");
		html.Append("<span>Avg <b id='mx-avg' style='color:#64748b;'>-</b>&micro;s</span>");
		html.Append("<span>Peak <b id='mx-peak' style='color:#64748b;'>-</b>&micro;s</span>");
		html.Append("<span>Deadline <b id='mx-deadline' style='color:#64748b;'>-</b>&micro;s</span>");
		html.Append("</div>");
		// Legend
		html.Append("<div style='display:flex;gap:10px;flex-wrap:wrap;margin-bottom:10px;font-size:11px;'>");
		html.Append("<span><span style='display:inline-block;width:10px;height:10px;border-radius:2px;background:#3b82f6;vertical-align:middle;margin-right:3px;'></span>MT-32</span>");
		html.Append("<span><span style='display:inline-block;width:10px;height:10px;border-radius:2px;background:#22c55e;vertical-align:middle;margin-right:3px;'></span>FluidSynth</span>");
		html.Append("<span><span style='display:inline-block;width:10px;height:10px;border-radius:2px;background:#f59e0b;vertical-align:middle;margin-right:3px;'></span>OPL3</span>");
		html.Append("<span><span style='display:inline-block;width:10px;height:10px;border-radius:2px;background:#a855f7;vertical-align:middle;margin-right:3px;'></span>Layered</span>");
		html.Append("<span><span style='display:inline-block;width:10px;height:10px;border-radius:2px;background:#475569;vertical-align:middle;margin-right:3px;'></span>None</span>");
		html.Append("<span style='color:#64748b;font-size:10px;margin-left:4px;'>(dbl-click=mute, scroll=vol)</span>");
		html.Append("</div>");
		// Card grid (replaces table)
		html.Append("<div id='mx-ch' style='display:grid;grid-template-columns:repeat(auto-fill,minmax(180px,1fr));gap:8px;margin-bottom:10px;'></div>");
		// Buttons
		// Buttons: edit group | file group (Save primary)
		html.Append("<div style='display:flex;gap:8px;flex-wrap:wrap;align-items:center;'>");
		html.Append("<div style='display:flex;gap:4px;'>");
		html.Append("<button onclick='setAllLayer(true)'>Layer All</button>");
		html.Append("<button onclick='setAllLayer(false)'>Unlayer All</button>");
		html.Append("<button onclick='resetCCFilters()'>Reset CC</button>");
		html.Append("<button onclick='resetChVol()'>Reset Vol</button>");
		html.Append("</div>");
		html.Append("<span style='border-left:1px solid #334155;height:22px;display:inline-block;margin:0 2px;'></span>");
		html.Append("<div style='display:flex;gap:4px;'>");
		html.Append("<button onclick='savePreset()' style='background:#1d4ed8;border-color:#3b82f6;color:#fff;font-weight:600;'>&#128190; Save</button>");
		html.Append("<button onclick='loadPreset()' style='border-color:#334155;'>&#128194; Load</button>");
		html.Append("</div></div></section>");

		// Post-mix audio effects
		html.Append("<section><h2>Audio Effects</h2><div class='grid'>");
		html.Append("<label>EQ<select id='mx-fx-eq' onchange=\"setParam('fx_eq_enabled',this.value)\"><option value='on'>On</option><option value='off'>Off</option></select></label>");
		html.Append("<label>Bass (dB)<input id='mx-fx-bass' type='number' min='-12' max='12' step='1' oninput=\"setParam('fx_eq_bass',this.value)\"></label>");
		html.Append("<label>Treble (dB)<input id='mx-fx-treble' type='number' min='-12' max='12' step='1' oninput=\"setParam('fx_eq_treble',this.value)\"></label>");
		html.Append("<label>Limiter<select id='mx-fx-lim' onchange=\"setParam('fx_limiter_enabled',this.value)\"><option value='on'>On</option><option value='off'>Off</option></select></label>");
		html.Append("<label>Reverb<select id='mx-fx-rev' onchange=\"setParam('fx_reverb_enabled',this.value)\"><option value='on'>On</option><option value='off'>Off</option></select></label>");
		html.Append("<label>Room % <input id='mx-fx-room' type='range' min='0' max='100' oninput=\"setParam('fx_reverb_room',this.value);document.getElementById('mx-fx-room-val').textContent=this.value\"> <span id='mx-fx-room-val'>50</span></label>");
		html.Append("<label>Damp % <input id='mx-fx-damp' type='range' min='0' max='100' oninput=\"setParam('fx_reverb_damp',this.value);document.getElementById('mx-fx-damp-val').textContent=this.value\"> <span id='mx-fx-damp-val'>50</span></label>");
		html.Append("<label>Wet %  <input id='mx-fx-wet'  type='range' min='0' max='100' oninput=\"setParam('fx_reverb_wet',this.value);document.getElementById('mx-fx-wet-val').textContent=this.value\"> <span id='mx-fx-wet-val'>33</span></label>");
		html.Append("</div></section>");

		// MIDI Thru
		html.Append("<section><h2>MIDI Routing</h2><div class='grid'>");
		html.Append("<label>MIDI Thru<select id='mx-midi-thru' onchange=\"setRtParam('midi_thru',this.value)\"><option value='on'>On</option><option value='off'>Off</option></select></label>");
		html.Append("</div></section>");

		html.Append("<p id='mx-msg' style='color:#64748b;'></p>");

		// JavaScript
		html.Append("<script>");
		// helpers
		html.Append("function setParam(p,v){_qs('/api/mixer/set','param='+p+'&value='+encodeURIComponent(v),function(r){if(!r||!r.ok)showToast('Error',false);});}");
		html.Append("function setRtParam(p,v){_qs('/api/runtime/set','param='+p+'&value='+encodeURIComponent(v),function(r){if(r&&r.ok)loadStatus();else showToast('Error',false);});}"); 
		html.Append("function setEnabled(v){setParam('enabled',v==='1'?'on':'off');}");
		html.Append("function setPreset(v){_qs('/api/mixer/preset','preset='+v,function(r){if(r&&r.ok)loadStatus();else showToast('Error',false);});}");
		html.Append("function savePreset(){_qs('/api/router/save','',function(r){if(r&&r.ok)showToast('Preset saved');else showToast('Save failed',false);});}");
		html.Append("function loadPreset(){_qs('/api/router/load','',function(r){if(r&&r.ok){loadStatus();showToast('Preset loaded');}else showToast('Load failed',false);});}");

		// channel engine/remap
		html.Append("function setChEngine(ch,eng){_qs('/api/mixer/set','param=channel_engine&value='+ch+','+eng,function(r){if(r&&r.ok)loadStatus();else showToast('Error',false);});}");
		html.Append("function setChRemap(ch,dst){var n=parseInt(dst,10);if(n<1||n>16)return;");
		html.Append("_qs('/api/mixer/set','param=channel_remap&value='+ch+','+n,function(r){if(r&&r.ok)loadStatus();else showToast('Error',false);});}");
		html.Append("function setChVol(ch,vol){_qs('/api/mixer/set','param=channel_volume&value='+ch+','+vol,function(r){if(!r||!r.ok)showToast('Error',false);});}");

		// layer
		html.Append("function setChLayer(ch,on){_qs('/api/mixer/set','param=channel_layer&value='+ch+','+(on?'on':'off'),function(r){if(r&&r.ok)loadStatus();else showToast('Error',false);});}");
		html.Append("function setAllLayer(on){_qs('/api/mixer/set','param=all_layer&value='+(on?'on':'off'),function(r){if(r&&r.ok)loadStatus();else showToast('Error',false);});}");
		html.Append("function resetCCFilters(){_qs('/api/mixer/set','param=cc_filter_reset&value=1',function(r){if(r&&r.ok)showToast('CC filters reset');else showToast('Error',false);});}");
		html.Append("function resetChVol(){_qs('/api/mixer/set','param=channel_volume_reset&value=1',function(r){if(r&&r.ok){loadStatus();showToast('Vol reset');}else showToast('Error',false);});}");

		// render
		html.Append("function mxEngineKey(n){return n==='MT-32'?'mt32':(n==='FluidSynth'?'fluidsynth':(n==='OPL3'?'opl3':'none'));}");
		html.Append("function mxEngineColor(n,layered){return layered?'#a855f7':(n==='MT-32'?'#3b82f6':(n==='FluidSynth'?'#22c55e':(n==='OPL3'?'#f59e0b':'#475569')));}");
		html.Append("function mxEngineLabel(n,layered){return layered?'L':(n==='MT-32'?'MT':(n==='FluidSynth'?'SF':(n==='OPL3'?'OPL':'--')));}");
		html.Append("function renderRouteMap(chs){var mp=document.getElementById('mx-route-map');if(!mp)return;mp.innerHTML='';for(var i=0;i<chs.length;i++){var c=chs[i];var chip=document.createElement('div');var col=mxEngineColor(c.engine,!!c.layered);chip.style.cssText='width:40px;height:40px;border-radius:6px;background:'+col+';display:flex;flex-direction:column;align-items:center;justify-content:center;cursor:default;flex-shrink:0;position:relative;';var chnum=document.createElement('span');chnum.style.cssText='font-size:12px;font-weight:bold;color:#fff;line-height:1;';chnum.textContent=c.ch;chip.appendChild(chnum);var eng=document.createElement('span');eng.style.cssText='font-size:8px;color:rgba(255,255,255,0.75);line-height:1;margin-top:2px;';eng.textContent=mxEngineLabel(c.engine,!!c.layered);chip.appendChild(eng);if(c.remap!==c.ch){var rm=document.createElement('span');rm.style.cssText='font-size:7px;color:rgba(255,255,255,0.6);line-height:1;margin-top:1px;';rm.textContent='\u2192'+c.remap;chip.appendChild(rm);}chip.title='Ch '+c.ch+': '+(c.layered?'Layered (both)':c.engine)+(c.remap!==c.ch?' \u2192ch'+c.remap:'')+' Vol:'+c.vol+'%';mp.appendChild(chip);}}");
		html.Append("function renderChannels(chs){renderRouteMap(chs);var tb=document.getElementById('mx-ch');tb.innerHTML='';var isLight=document.body.classList.contains('light');var cardBg=isLight?'#f8fafc':'#1e293b';var insClr=isLight?'#0f172a':'#e2e8f0';var vuBg=isLight?'#e2e8f0':'#0f172a';var ebBdr=isLight?'#cbd5e1':'#334155';var mutedClr=isLight?'#4b5563':'#64748b';var detClr=isLight?'#4b5563':'#475569';");
		html.Append("for(var i=0;i<chs.length;i++){var c=chs[i];");
		// Engine accent color — muted, used only for left border stripe
		html.Append("var ec=c.layered?'#a78bfa':(c.engine==='MT-32'?'#60a5fa':(c.engine==='FluidSynth'?'#34d399':(c.engine==='OPL3'?'#fbbf24':'#475569')));");
		// Card: neutral bg always; id for glow animation; accent only on left border
		html.Append("var card=document.createElement('div');card.id='mxcd-'+c.ch;");
		html.Append("card.style.cssText='border-left:3px solid '+ec+';background:'+cardBg+';border-radius:8px;padding:8px 10px;display:flex;flex-direction:column;gap:4px;transition:box-shadow 0.12s,opacity 0.15s;';");
		// Row 1: CH number (tiny) + instrument name (bold, dominant) + engine tag (right, subtle)
		html.Append("var r1=document.createElement('div');r1.style.cssText='display:flex;align-items:baseline;gap:5px;min-width:0;';");
		html.Append("var chb=document.createElement('span');chb.style.cssText='font-size:9px;font-weight:700;color:'+ec+';letter-spacing:0.5px;flex-shrink:0;';chb.textContent='CH'+c.ch;r1.appendChild(chb);");
		html.Append("var ins=document.createElement('span');ins.style.cssText='flex:1;font-size:13px;font-weight:600;color:'+insClr+';overflow:hidden;text-overflow:ellipsis;white-space:nowrap;min-width:0;';ins.textContent=c.instrument||'\\u2014';ins.title=c.instrument||'';r1.appendChild(ins);");
		// Engine toggle: minimal, muted by default, accent on hover
		html.Append("var eb=document.createElement('button');eb.style.cssText='font-size:9px;padding:1px 5px;border-radius:4px;background:transparent;border:1px solid '+ebBdr+';color:'+mutedClr+';cursor:pointer;flex-shrink:0;transition:border-color 0.15s,color 0.15s;';");
		html.Append("eb.textContent=c.engine==='MT-32'?'MT-32':(c.engine==='OPL3'?'OPL':'SF');eb.dataset.ch=c.ch;eb.dataset.eng=mxEngineKey(c.engine);");
		html.Append("eb.onmouseenter=(function(a){return function(){this.style.borderColor=a;this.style.color=a;};})(ec);eb.onmouseleave=(function(bd,cl){return function(){this.style.borderColor=bd;this.style.color=cl;};})(ebBdr,mutedClr);");
		html.Append("eb.onclick=(function(b){return function(){var ne=b.dataset.eng==='mt32'?'fluidsynth':(b.dataset.eng==='fluidsynth'?'opl3':'mt32');setChEngine(b.dataset.ch,ne);};})(eb);");
		html.Append("r1.appendChild(eb);card.appendChild(r1);");
		// Row 2: VU meter — taller, no border, thicker peak hold bar
		html.Append("var vu=document.createElement('div');vu.style.cssText='position:relative;height:20px;background:'+vuBg+';border-radius:4px;overflow:hidden;';");
		html.Append("var mff=document.createElement('div');mff.id='mxf-'+c.ch;mff.style.cssText='position:absolute;left:0;top:0;bottom:0;width:0%;background:#22c55e;border-radius:4px;';");
		html.Append("var mpp=document.createElement('div');mpp.id='mxp-'+c.ch;mpp.style.cssText='position:absolute;top:0;bottom:0;width:3px;background:rgba(255,255,255,0.9);left:0%;border-radius:2px;box-shadow:0 0 4px rgba(255,255,255,0.6);';");
		html.Append("vu.appendChild(mff);vu.appendChild(mpp);card.appendChild(vu);");
		// Row 3: Vol slider — label hidden at 100% to reduce noise
		html.Append("var vrow=document.createElement('div');vrow.style.cssText='display:flex;align-items:center;gap:4px;';");
		html.Append("var vlbl=document.createElement('span');vlbl.style.cssText='font-size:9px;color:'+detClr+';letter-spacing:0.4px;flex-shrink:0;width:22px;';vlbl.textContent='VOL';vrow.appendChild(vlbl);");
		html.Append("var vsl=document.createElement('input');vsl.type='range';vsl.min=0;vsl.max=100;var initVol=c.vol!=null?c.vol:100;vsl.value=initVol;vsl.style.cssText='flex:1;cursor:pointer;';vsl.dataset.ch=c.ch;");
		html.Append("var vlab=document.createElement('span');vlab.style.cssText='font-size:10px;color:#94a3b8;width:32px;text-align:right;flex-shrink:0;';vlab.textContent=initVol===100?'':initVol+'%';");
		html.Append("vsl.oninput=(function(l,ch){return function(){var v=parseInt(this.value);setChVol(ch,v);l.textContent=v===100?'':v+'%';};})(vlab,c.ch);");
		html.Append("vsl.onwheel=(function(l,ch){return function(e){e.preventDefault();var nv=Math.max(0,Math.min(100,parseInt(this.value)+(e.deltaY<0?1:-1)));this.value=nv;l.textContent=nv===100?'':nv+'%';setChVol(ch,nv);};})(vlab,c.ch);");
		html.Append("vrow.appendChild(vsl);vrow.appendChild(vlab);card.appendChild(vrow);");
		// Row 4: Remap + Layer (compact detail row)
		html.Append("var dr=document.createElement('div');dr.style.cssText='display:flex;align-items:center;gap:8px;';");
		html.Append("var rsp=document.createElement('span');rsp.style.cssText='font-size:10px;color:'+detClr+';';rsp.textContent='\\u2192';dr.appendChild(rsp);");
		html.Append("var ri=document.createElement('input');ri.type='number';ri.min=1;ri.max=16;ri.value=c.remap;ri.style.cssText='width:38px;font-size:10px;padding:2px 4px;height:20px;border-radius:4px;';ri.dataset.ch=c.ch;");
		html.Append("ri.onchange=function(){setChRemap(this.dataset.ch,this.value);};dr.appendChild(ri);");
		html.Append("var ll=document.createElement('label');ll.style.cssText='display:flex;align-items:center;gap:3px;font-size:10px;color:'+detClr+';cursor:pointer;flex-direction:row;';");
		html.Append("var cb=document.createElement('input');cb.type='checkbox';cb.checked=!!c.layered;cb.dataset.ch=c.ch;");
		html.Append("cb.onchange=function(){setChLayer(this.dataset.ch,this.checked);};ll.appendChild(cb);ll.appendChild(document.createTextNode('Layer'));dr.appendChild(ll);card.appendChild(dr);");
		// Double-click to mute / unmute — dims card for physical feedback
		html.Append("card.ondblclick=(function(sl,l,ch,crd){return function(e){if(e.target.tagName==='INPUT'||e.target.tagName==='BUTTON')return;");
		html.Append("if(parseInt(sl.value)>0){sl.dataset.pv=sl.value;sl.value=0;l.textContent='0%';setChVol(ch,0);crd.style.opacity='0.45';}");
		html.Append("else{var pv=sl.dataset.pv||100;sl.value=pv;l.textContent=pv===100?'':pv+'%';setChVol(ch,pv);crd.style.opacity='1';}");
		html.Append("};})(vsl,vlab,c.ch,card);tb.appendChild(card);}}");

		// load status
		html.Append("function loadStatus(){_qs('/api/mixer/status','',function(d){if(!d)return;");
		html.Append("document.getElementById('mx-preset').value=d.preset;");
		html.Append("document.getElementById('mx-enabled').value=d.enabled?'1':'0';");
		html.Append("document.getElementById('mx-mt32v').value=Math.round(d.mt32_volume*100);");
		html.Append("document.getElementById('mx-fluidv').value=Math.round(d.fluid_volume*100);");
		html.Append("var opl3Vol=document.getElementById('mx-opl3v');if(opl3Vol)opl3Vol.value=Math.round((d.ymfm_volume||0)*100);");
		html.Append("document.getElementById('mx-mt32p').value=Math.round(d.mt32_pan*100);");
		html.Append("document.getElementById('mx-fluidp').value=Math.round(d.fluid_pan*100);");
		html.Append("document.getElementById('mx-render').textContent=d.render_us;");
		html.Append("document.getElementById('mx-avg').textContent=d.render_avg_us;");
		html.Append("document.getElementById('mx-peak').textContent=d.render_peak_us;");
		html.Append("document.getElementById('mx-deadline').textContent=d.deadline_us;");
html.Append("var cpu=d.cpu_load;document.getElementById('mx-cpu').textContent=cpu;");
                html.Append("var cpuEl=document.getElementById('mx-cpu');var hint=document.getElementById('mx-cpu-hint');");
                html.Append("cpuEl.style.color=cpu>=85?'#ef4444':cpu>=65?'#f59e0b':'#22c55e';");
                html.Append("if(cpu>=85){hint.textContent='\u26a0 underrun risk \u2014 increase chunk_size';hint.style.color='#ef4444';hint.style.display='inline';}");
                html.Append("else if(cpu>=65){hint.textContent='approaching limit';hint.style.color='#f59e0b';hint.style.display='inline';}");
                html.Append("else{hint.style.display='none';}");
		html.Append("var fxEq=document.getElementById('mx-fx-eq');if(fxEq)fxEq.value=d.fx_eq_enabled?'on':'off';");
		html.Append("var fxBass=document.getElementById('mx-fx-bass');if(fxBass)fxBass.value=d.fx_eq_bass;");
		html.Append("var fxTreb=document.getElementById('mx-fx-treble');if(fxTreb)fxTreb.value=d.fx_eq_treble;");
		html.Append("var fxLim=document.getElementById('mx-fx-lim');if(fxLim)fxLim.value=d.fx_limiter_enabled?'on':'off';");
		html.Append("var fxRev=document.getElementById('mx-fx-rev');if(fxRev)fxRev.value=d.fx_reverb_enabled?'on':'off';");
		html.Append("var fxRoom=document.getElementById('mx-fx-room');if(fxRoom){fxRoom.value=d.fx_reverb_room;document.getElementById('mx-fx-room-val').textContent=d.fx_reverb_room;}");
		html.Append("var fxDamp=document.getElementById('mx-fx-damp');if(fxDamp){fxDamp.value=d.fx_reverb_damp;document.getElementById('mx-fx-damp-val').textContent=d.fx_reverb_damp;}");
		html.Append("var fxWet=document.getElementById('mx-fx-wet');if(fxWet){fxWet.value=d.fx_reverb_wet;document.getElementById('mx-fx-wet-val').textContent=d.fx_reverb_wet;}");
		html.Append("var mxThru=document.getElementById('mx-midi-thru');if(mxThru)mxThru.value=d.midi_thru_enabled?'on':'off';");
		html.Append("if(d.channels)renderChannels(d.channels);});}loadStatus();setInterval(loadStatus,5000);");
		// WebSocket for real-time channel meters
		html.Append("var _mxLv=new Array(16).fill(0),_mxPk=new Array(16).fill(0),_mxPa=new Array(16).fill(-9999);");
		html.Append("var _mxTgt=new Array(17).fill(0),_mxPt=new Array(17).fill(0);");
		html.Append("function _mxRf(ts){for(var i=0;i<16;i++){var ch=i+1;var t=_mxTgt[ch];");
		html.Append("_mxLv[i]=t>_mxLv[i]?_mxLv[i]+(t-_mxLv[i])*0.4:_mxLv[i]+(t-_mxLv[i])*0.06;");
		html.Append("if(_mxPt[ch]>_mxPk[i]){_mxPk[i]=_mxPt[ch];_mxPa[i]=ts;}else if(ts-_mxPa[i]>1500)_mxPk[i]*=0.95;");
		html.Append("var lp=_mxLv[i]*100;var col=lp>75?'#ef4444':lp>45?'#f59e0b':'#22c55e';");
		html.Append("var f=document.getElementById('mxf-'+ch);if(f){f.style.width=lp.toFixed(1)+'%';f.style.background=col;}");
		html.Append("var p=document.getElementById('mxp-'+ch);if(p)p.style.left=(_mxPk[i]*100).toFixed(1)+'%';");
		// Card glow: intensity tracks level — silent=none, loud=red — silent = none, loud = red
		html.Append("var cd=document.getElementById('mxcd-'+ch);if(cd&&cd.style.opacity!=='0.45'){if(lp>75)cd.style.boxShadow='0 0 10px 2px rgba(239,68,68,0.45)';else if(lp>45)cd.style.boxShadow='0 0 8px 2px rgba(245,158,11,0.35)';else if(lp>5)cd.style.boxShadow='0 0 5px 1px rgba(34,197,94,0.2)';else cd.style.boxShadow='none';}}");
		html.Append("requestAnimationFrame(_mxRf);}requestAnimationFrame(_mxRf);");
		html.Append("(function(){var ws=null,_rt=0;function conn(){ws=new WebSocket('ws://'+location.hostname+':8765/');");
		html.Append("ws.onmessage=function(e){try{var d=JSON.parse(e.data);if(d.channels)for(var i=0;i<d.channels.length;i++){var ch=d.channels[i];_mxTgt[ch.ch]=Math.max(0,Math.min(1,ch.lv||0));_mxPt[ch.ch]=Math.max(0,Math.min(1,ch.pk||0));}}catch(x){}};");
		html.Append("ws.onclose=function(){_rt=Math.min((_rt||500)*2,8000);setTimeout(conn,_rt);};ws.onerror=function(){ws.close();};}conn();})();");

		html.Append("</script></main></body></html>");

		if (!html.bOk)
			return HTTPInternalServerError;
		*pLength = html.Length();
		*ppContentType = "text/html; charset=utf-8";
		return HTTPOK;
}

THTTPStatus CWebDaemon::BuildMonitorPage(u8* pBuffer, unsigned* pLength, const char** ppContentType)
{
		HtmlWriter html(pBuffer, *pLength);
		html.Append("<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width, initial-scale=1'>");
		html.Append("<title>mt32-pi monitor</title><link rel='stylesheet' href='/app.css'></head><body><main>");
		html.Append("<script src='/app.js'></script>");
		html.Append("<h1>MIDI Monitor</h1>");
		html.Append("<p>Live log of incoming MIDI events. Shows the last 64 messages received by the active synth.</p>");
		html.Append("<nav><a href='/'>Status</a><a href='/sound'>Sound</a><a href='/config'>Config</a><a href='/sequencer'>Sequencer</a><a href='/mixer'>Mixer</a><a href='/monitor'>Monitor</a></nav>");

		// Filter controls
		html.Append("<section><h2>Filters</h2><div class='grid'>");
		html.Append("<label>Channel<select id='mn-ch'><option value='0'>All channels</option>");
		for (int i = 1; i <= 16; ++i)
		{
			CString Opt; Opt.Format("<option value='%d'>Ch %d</option>", i, i);
			html.Append(static_cast<const char*>(Opt));
		}
		html.Append("</select></label>");
		html.Append("<label>Type<select id='mn-type'>");
		html.Append("<option value=''>All types</option>");
		html.Append("<option value='Note On'>Note On</option>");
		html.Append("<option value='Note Off'>Note Off</option>");
		html.Append("<option value='CC'>CC</option>");
		html.Append("<option value='Prog Change'>Prog Change</option>");
		html.Append("<option value='Pitch Bend'>Pitch Bend</option>");
		html.Append("<option value='Aftertouch'>Aftertouch</option>");
		html.Append("<option value='SysEx'>SysEx</option>");
		html.Append("</select></label>");
		html.Append("<label style='align-self:end;'><button onclick='clearLog()'>Clear Log</button></label>");
		html.Append("<label style='align-self:end;'><label><input type='checkbox' id='mn-pause'> Pause</label></label>");
		html.Append("</div></section>");

		// Event table
		html.Append("<section><h2>Events <span id='mn-count' style='font-size:12px;color:#64748b;'></span></h2>");
		html.Append("<div style='overflow-x:auto;'><table style='width:100%;border-collapse:collapse;font-size:12px;font-family:monospace;'><thead><tr>");
		html.Append("<th style='text-align:left;padding:4px 8px;border-bottom:2px solid #334155;color:#93c5fd;min-width:80px;'>Time (ms)</th>");
		html.Append("<th style='text-align:left;padding:4px 8px;border-bottom:2px solid #334155;color:#93c5fd;'>Ch</th>");
		html.Append("<th style='text-align:left;padding:4px 8px;border-bottom:2px solid #334155;color:#93c5fd;'>Type</th>");
		html.Append("<th style='text-align:left;padding:4px 8px;border-bottom:2px solid #334155;color:#93c5fd;'>Data 1</th>");
		html.Append("<th style='text-align:left;padding:4px 8px;border-bottom:2px solid #334155;color:#93c5fd;'>Data 2</th>");
		html.Append("<th style='text-align:left;padding:4px 8px;border-bottom:2px solid #334155;color:#93c5fd;'>Detail</th>");
		html.Append("</tr></thead><tbody id='mn-body'></tbody></table></div></section>");

		// Note names for display
		html.Append("<script>");
		html.Append("var _nn=['C','C#','D','D#','E','F','F#','G','G#','A','A#','B'];");
		html.Append("function noteName(n){return _nn[n%12]+(Math.floor(n/12)-1);}");

		// CC name lookup (common ones)
		html.Append("var _cc={1:'Mod',7:'Vol',10:'Pan',11:'Exp',64:'Sustain',91:'Reverb',93:'Chorus',121:'Reset',123:'All Off'};");
		html.Append("function ccName(n){return _cc[n]?_cc[n]+' ('+n+')':String(n);}");

		// detail column
		html.Append("function detail(e){");
		html.Append("var type=e.type,d1=e.d1,d2=e.d2;");
		html.Append("if(type==='Note On'||type==='Note Off')return noteName(parseInt(d1))+' vel='+d2;");
		html.Append("if(type==='CC')return ccName(parseInt(d1))+' val='+d2;");
		html.Append("if(type==='Prog Change')return 'Prog '+d1;");
		html.Append("if(type==='Pitch Bend'){var v=(parseInt(d2)<<7|parseInt(d1))-8192;return 'bend='+v;}");
		html.Append("if(type==='SysEx')return d2;");
		html.Append("return d1+' '+d2;}");

		// type color
		html.Append("var _tc={'Note On':'#4ade80','Note Off':'#94a3b8','CC':'#fbbf24','Prog Change':'#c084fc','Pitch Bend':'#38bdf8','Aftertouch':'#fb923c','SysEx':'#a78bfa'};");
		html.Append("function typeColor(t){return _tc[t]||'#e2e8f0';}");

		// render table
		html.Append("var _evs=[];");
		html.Append("function renderTable(){");
		html.Append("var fch=parseInt(document.getElementById('mn-ch').value,10);");
		html.Append("var fty=document.getElementById('mn-type').value;");
		html.Append("var tb=document.getElementById('mn-body');tb.innerHTML='';");
		html.Append("var shown=0;");
		html.Append("for(var i=_evs.length-1;i>=0;i--){");  // newest first
		html.Append("var e=_evs[i];");
		html.Append("if(fch&&parseInt(e.ch,10)!==fch)continue;");
		html.Append("if(fty&&e.type!==fty)continue;");
		html.Append("var tr=document.createElement('tr');");
		html.Append("tr.style.background=shown%2===0?'#111827':'#0b1220';");
		html.Append("var cells=[e.ts,e.ch,e.type,e.d1,e.d2,detail(e)];");
		html.Append("for(var j=0;j<cells.length;j++){var td=document.createElement('td');td.style.padding='4px 8px';td.style.borderBottom='1px solid #1e293b';");
		html.Append("if(j===2){td.style.color=typeColor(e.type);td.style.fontWeight='bold';}");
		html.Append("td.textContent=cells[j];tr.appendChild(td);}");
		html.Append("tb.appendChild(tr);shown++;}");
		html.Append("document.getElementById('mn-count').textContent='('+shown+' shown, '+_evs.length+' total)';}");

		// fetch log
		html.Append("function fetchLog(){if(document.getElementById('mn-pause').checked)return;");
		html.Append("_qs('/api/midi/log','',function(d){if(!d||!d.events)return;_evs=d.events;");
		html.Append("_evs.sort(function(a,b){return parseInt(a.ts)-parseInt(b.ts);});renderTable();});}");

		// clear
		html.Append("function clearLog(){_qs('/api/midi/log/clear','',function(){_evs=[];renderTable();});}");

		// filter change re-renders immediately
		html.Append("document.getElementById('mn-ch').onchange=renderTable;");
		html.Append("document.getElementById('mn-type').onchange=renderTable;");

		// poll every 500ms
		html.Append("fetchLog();setInterval(fetchLog,500);");
		html.Append("</script></main></body></html>");

		if (!html.bOk)
			return HTTPInternalServerError;
		*pLength = html.Length();
		*ppContentType = "text/html; charset=utf-8";
		return HTTPOK;
}

THTTPStatus CWebDaemon::BuildSoundPage(u8* pBuffer, unsigned* pLength, const char** ppContentType)
{
		HtmlWriter html(pBuffer, *pLength);
		html.Append("<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width, initial-scale=1'>");
		html.Append("<title>mt32-pi sound</title><link rel='stylesheet' href='/app.css'></head><body><main>");
		html.Append("<script src='/app.js'></script>");

		const bool bMT32Active = std::strcmp(m_pMT32Pi->GetActiveSynthName(), "MT-32") == 0;
		const bool bYmfmActive = std::strcmp(m_pMT32Pi->GetActiveSynthName(), "OPL3") == 0;
		const bool bSFActive   = !bMT32Active && !bYmfmActive;
		const int nROMSetIndex = m_pMT32Pi->GetMT32ROMSetIndex();
		const int nMasterVolume = m_pMT32Pi->GetMasterVolume();
		const size_t nCurrentSoundFontIndex = m_pMT32Pi->GetCurrentSoundFontIndex();
		const size_t nSoundFontCount = m_pMT32Pi->GetSoundFontCount();
		bool bReverbActive = false;
		float nReverbRoom = 0.0f;
		float nReverbLevel = 0.0f;
		float nReverbDamping = 0.0f;
		float nReverbWidth = 0.0f;
		bool bChorusActive = false;
		float nChorusDepth = 0.0f;
		float nChorusLevel = 0.0f;
		int   nChorusVoices = 1;
		float nChorusSpeed = 0.0f;
		float nSFGain = 0.0f;
		const bool bHasSoundFontFX = m_pMT32Pi->GetSoundFontFXState(bReverbActive, nReverbRoom, nReverbLevel,
		                                                             nReverbDamping, nReverbWidth,
		                                                             bChorusActive, nChorusDepth, nChorusLevel,
		                                                             nChorusVoices, nChorusSpeed, nSFGain);

		// MT-32 Sound Parameters
		float fMT32ReverbGain = m_pMT32Pi->GetMT32ReverbOutputGain();
		bool bMT32ReverbActive = m_pMT32Pi->IsMT32ReverbActive();
		bool bMT32NiceAmp = m_pMT32Pi->IsMT32NiceAmpRamp();
		bool bMT32NicePan = m_pMT32Pi->IsMT32NicePanning();
		bool bMT32NiceMix = m_pMT32Pi->IsMT32NicePartialMixing();
		int nMT32DACMode = m_pMT32Pi->GetMT32DACMode();
		int nMT32MIDIDelay = m_pMT32Pi->GetMT32MIDIDelayMode();
		int nMT32AnalogMode = m_pMT32Pi->GetMT32AnalogMode();
		int nMT32RendererType = m_pMT32Pi->GetMT32RendererType();
		int nMT32PartialCount = m_pMT32Pi->GetMT32PartialCount();

		CString MasterVolume; MasterVolume.Format("%d", nMasterVolume);
		CString ReverbRoom;    ReverbRoom.Format("%.1f",   bHasSoundFontFX ? nReverbRoom    : 0.0f);
		CString ReverbLevel;   ReverbLevel.Format("%.1f",  bHasSoundFontFX ? nReverbLevel   : 0.0f);
		CString ReverbDamping; ReverbDamping.Format("%.1f",bHasSoundFontFX ? nReverbDamping : 0.0f);
		CString ReverbWidth;   ReverbWidth.Format("%.0f",  bHasSoundFontFX ? nReverbWidth   : 0.0f);
		CString ChorusDepth;   ChorusDepth.Format("%.0f",  bHasSoundFontFX ? nChorusDepth   : 0.0f);
		CString ChorusLevel;   ChorusLevel.Format("%.2f",  bHasSoundFontFX ? nChorusLevel   : 0.0f);
		CString ChorusVoices;  ChorusVoices.Format("%d",   bHasSoundFontFX ? nChorusVoices  : 1);
		CString ChorusSpeed;   ChorusSpeed.Format("%.2f",  bHasSoundFontFX ? nChorusSpeed   : 0.0f);
		CString SFGain;        SFGain.Format("%.2f",       bHasSoundFontFX ? nSFGain        : 0.0f);

		html.Append("<h1>Sound control</h1><p>Live adjustments for synthesis engines and effects, no restart needed.</p>");
		html.Append("<nav><a href='/'>Status</a><a href='/sound'>Sound</a><a href='/config'>Config</a><a href='/sequencer'>Sequencer</a><a href='/mixer'>Mixer</a><a href='/monitor'>Monitor</a></nav>");
		html.Append("<div class='statusbar'><div class='pill'>Active synth: <strong id='rt_active_synth_label'>");
		AppendEscaped(html, m_pMT32Pi->GetActiveSynthName());
		html.Append("</strong></div><div class='pill'>MT-32 ROM: <strong id='rt_mt32_rom_name'>");
		AppendEscaped(html, m_pMT32Pi->GetCurrentMT32ROMName());
		html.Append("</strong></div><div class='pill'>SoundFont: <strong id='rt_soundfont_name'>");
		AppendEscaped(html, m_pMT32Pi->GetCurrentSoundFontName());
		html.Append("</strong></div><div class='pill'>Mixer: <strong id='rt_mixer_label'>-</strong></div></div>");
		html.Append("<div id='mx-banner' style='display:none;background:#1e3a5f;border:1px solid #22d3ee;border-radius:8px;padding:8px 12px;margin:8px 0;color:#93c5fd;font-size:13px;'>&#9432; Mixer dual mode: both engines processing audio. Changing active synth switches to single-engine mode.</div>");
		html.Append("<div class='tabbar'><button class='tabbtn");
		html.Append(bMT32Active ? " active" : "");
		html.Append("' type='button' id='tab-mt32'>MT-32</button><button class='tabbtn");
		html.Append(bSFActive ? " active" : "");
		html.Append("' type='button' id='tab-sf'>SoundFont</button><button class='tabbtn");
		html.Append(bYmfmActive ? " active" : "");
		html.Append("' type='button' id='tab-opl3'>OPL3</button></div>");
		html.Append("<section><h2>Engine &amp; bank</h2><div class='grid'>");
		html.Append("<label>Active synth<select id='rt_active_synth'><option value='mt32'");
		html.Append(SelectedAttr(bMT32Active));
		html.Append(">MT-32</option><option value='soundfont'");
		html.Append(SelectedAttr(bSFActive));
		html.Append(">SoundFont</option><option value='opl3'");
		html.Append(SelectedAttr(bYmfmActive));
		html.Append(">OPL3</option></select></label>");
		html.Append("<label>Master volume <input id='rt_master_volume' type='range' min='0' max='100' step='1' value='");
		AppendEscaped(html, MasterVolume);
		html.Append("'><span id='rt_master_volume_val'>");
		AppendEscaped(html, MasterVolume);
		html.Append("</span></label>");
		html.Append("</div></section>");

		CString MT32ReverbGain; MT32ReverbGain.Format("%.2f", fMT32ReverbGain);
		CString MT32PartialCount; MT32PartialCount.Format("%d", nMT32PartialCount);
		
		html.Append("<section id='mt32-section'><h2>MT-32</h2><div class='grid'>");
		html.Append("<span class='subgroup-title'>ROM &amp; bank</span>");
		html.Append("<label>ROM set MT-32<select id='rt_mt32_rom_set'><option value='mt32_old'");
		html.Append(SelectedAttr(nROMSetIndex == static_cast<int>(TMT32ROMSet::MT32Old)));
		html.Append(">MT-32 old</option><option value='mt32_new'");
		html.Append(SelectedAttr(nROMSetIndex == static_cast<int>(TMT32ROMSet::MT32New)));
		html.Append(">MT-32 new</option><option value='cm32l'");
		html.Append(SelectedAttr(nROMSetIndex == static_cast<int>(TMT32ROMSet::CM32L)));
		html.Append(">CM-32L</option></select></label>");
		html.Append("<label>Current ROM<input value='");
		AppendEscaped(html, m_pMT32Pi->GetCurrentMT32ROMName());
		html.Append("' disabled></label>");
		html.Append("<span class='subgroup-title'>Reverb</span>");
		html.Append("<label>Reverb<select id='rt_mt32_reverb_active'><option value='off'");
		html.Append(SelectedAttr(!bMT32ReverbActive));
		html.Append(">off</option><option value='on'");
		html.Append(SelectedAttr(bMT32ReverbActive));
		html.Append(">on</option></select></label>");
		html.Append("<label>Reverb gain <input id='rt_mt32_reverb_gain' type='range' min='0' max='4' step='0.2' value='");
		AppendEscaped(html, MT32ReverbGain);
		html.Append("'><span id='rt_mt32_reverb_gain_val'>");
		AppendEscaped(html, MT32ReverbGain);
		html.Append("</span></label>");
		html.Append("<span class='subgroup-title'>Emulation enhancements</span>");
		html.Append("<label>Nice Amp<select id='rt_mt32_nice_amp'><option value='off'");
		html.Append(SelectedAttr(!bMT32NiceAmp));
		html.Append(">off</option><option value='on'");
		html.Append(SelectedAttr(bMT32NiceAmp));
		html.Append(">on</option></select></label>");
		html.Append("<label>Nice Pan<select id='rt_mt32_nice_pan'><option value='off'");
		html.Append(SelectedAttr(!bMT32NicePan));
		html.Append(">off</option><option value='on'");
		html.Append(SelectedAttr(bMT32NicePan));
		html.Append(">on</option></select></label>");
		html.Append("<label>Nice Mix<select id='rt_mt32_nice_mix'><option value='off'");
		html.Append(SelectedAttr(!bMT32NiceMix));
		html.Append(">off</option><option value='on'");
		html.Append(SelectedAttr(bMT32NiceMix));
		html.Append(">on</option></select></label>");
		html.Append("<span class='subgroup-title'>Advanced emulation</span>");
		html.Append("<label>DAC<select id='rt_mt32_dac_mode'><option value='0'");
		html.Append(SelectedAttr(nMT32DACMode == 0));
		html.Append(">NICE</option><option value='1'");
		html.Append(SelectedAttr(nMT32DACMode == 1));
		html.Append(">PURE</option><option value='2'");
		html.Append(SelectedAttr(nMT32DACMode == 2));
		html.Append(">GEN1</option><option value='3'");
		html.Append(SelectedAttr(nMT32DACMode == 3));
		html.Append(">GEN2</option></select></label>");
		html.Append("<label>MIDI Delay<select id='rt_mt32_midi_delay'><option value='0'");
		html.Append(SelectedAttr(nMT32MIDIDelay == 0));
		html.Append(">IMMD</option><option value='1'");
		html.Append(SelectedAttr(nMT32MIDIDelay == 1));
		html.Append(">SHORT</option><option value='2'");
		html.Append(SelectedAttr(nMT32MIDIDelay == 2));
		html.Append(">ALL</option></select></label>");
		html.Append("<label>Analog<select id='rt_mt32_analog_mode'><option value='0'");
		html.Append(SelectedAttr(nMT32AnalogMode == 0));
		html.Append(">DIG</option><option value='1'");
		html.Append(SelectedAttr(nMT32AnalogMode == 1));
		html.Append(">COARSE</option><option value='2'");
		html.Append(SelectedAttr(nMT32AnalogMode == 2));
		html.Append(">ACCUR</option><option value='3'");
		html.Append(SelectedAttr(nMT32AnalogMode == 3));
		html.Append(">OVR</option></select></label>");
		html.Append("<label>Renderer<select id='rt_mt32_renderer_type'><option value='0'");
		html.Append(SelectedAttr(nMT32RendererType == 0));
		html.Append(">I16</option><option value='1'");
		html.Append(SelectedAttr(nMT32RendererType == 1));
		html.Append(">F32</option></select></label>");
		html.Append("<label>Partials <input id='rt_mt32_partial_count' type='number' min='8' max='256' value='");
		AppendEscaped(html, MT32PartialCount);
		html.Append("'></label>");
		html.Append("</div></section>");

		html.Append("<section id='sf-section'><h2>SoundFont</h2><div class='grid'>");
		html.Append("<label>SoundFont<select id='rt_soundfont_index'>");
		for (size_t i = 0; i < nSoundFontCount; ++i)
		{
			CString Index; Index.Format("%d", static_cast<int>(i));
			html.Append("<option value='");
			html.Append(Index);
			html.Append("'");
			html.Append(SelectedAttr(i == nCurrentSoundFontIndex));
			html.Append(">");
			const char* pSoundFontName = m_pMT32Pi->GetSoundFontName(i);
			AppendEscaped(html, pSoundFontName ? pSoundFontName : "(unnamed)");
			html.Append("</option>");
		}
		if (nSoundFontCount == 0)
			html.Append("<option value='0'>No SoundFonts</option>");
		html.Append("</select></label>");
		html.Append("<button type='button' id='sf-fav-btn' title='Toggle favorite' style='align-self:end;padding:4px 10px;font-size:18px;line-height:1;background:#0b1220;border:1px solid #334155;border-radius:6px;color:#facc15;cursor:pointer;'>&#9734;</button>");
		html.Append("<div style='grid-column:1/-1;'><div id='sf-favs-wrap' style='display:none;'><div style='font-size:11px;color:#64748b;margin-bottom:4px;'>Favorites</div><div id='sf-favs' style='display:flex;flex-wrap:wrap;gap:4px;'></div></div></div>");
		html.Append("<div id='sf-preview' style='grid-column:1/-1;background:#0b1220;border:1px solid #1e293b;border-radius:8px;padding:8px 12px;font-size:12px;color:#94a3b8;display:none;'>");
		html.Append("<strong id='sf-prev-name' style='color:#7dd3fc;'></strong>");
		html.Append("<div id='sf-prev-path' style='color:#4b5563;margin-top:2px;word-break:break-all;'></div>");
		html.Append("<div id='sf-prev-size' style='margin-top:2px;'></div>");
		html.Append("</div>");
		html.Append("<label>Gain <input id='rt_sf_gain' type='range' min='0' max='5' step='0.05' value='");
		AppendEscaped(html, SFGain);
		html.Append("'><span id='rt_sf_gain_val'>");
		AppendEscaped(html, SFGain);
		html.Append("</span></label>");
		html.Append("<span class='subgroup-title'>Reverb</span>");
		html.Append("<label>Reverb<select id='rt_sf_reverb_active'><option value='off'");
		html.Append(SelectedAttr(!bReverbActive));
		html.Append(">off</option><option value='on'");
		html.Append(SelectedAttr(bReverbActive));
		html.Append(">on</option></select></label>");
		html.Append("<label>Reverb room <input id='rt_sf_reverb_room' type='range' min='0' max='1' step='0.1' value='");
		AppendEscaped(html, ReverbRoom);
		html.Append("'><span id='rt_sf_reverb_room_val'>");
		AppendEscaped(html, ReverbRoom);
		html.Append("</span></label>");
		html.Append("<label>Reverb level <input id='rt_sf_reverb_level' type='range' min='0' max='1' step='0.1' value='");
		AppendEscaped(html, ReverbLevel);
		html.Append("'><span id='rt_sf_reverb_level_val'>");
		AppendEscaped(html, ReverbLevel);
		html.Append("</span></label>");
		html.Append("<label>Reverb damping <input id='rt_sf_reverb_damping' type='range' min='0' max='1' step='0.1' value='");
		AppendEscaped(html, ReverbDamping);
		html.Append("'><span id='rt_sf_reverb_damping_val'>");
		AppendEscaped(html, ReverbDamping);
		html.Append("</span></label>");
		html.Append("<label>Reverb width <input id='rt_sf_reverb_width' type='range' min='0' max='100' step='1' value='");
		AppendEscaped(html, ReverbWidth);
		html.Append("'><span id='rt_sf_reverb_width_val'>");
		AppendEscaped(html, ReverbWidth);
		html.Append("</span></label>");
		html.Append("<span class='subgroup-title'>Chorus</span>");
		html.Append("<label>Chorus<select id='rt_sf_chorus_active'><option value='off'");
		html.Append(SelectedAttr(!bChorusActive));
		html.Append(">off</option><option value='on'");
		html.Append(SelectedAttr(bChorusActive));
		html.Append(">on</option></select></label>");
		html.Append("<label>Chorus depth <input id='rt_sf_chorus_depth' type='range' min='0' max='20' step='1' value='");
		AppendEscaped(html, ChorusDepth);
		html.Append("'><span id='rt_sf_chorus_depth_val'>");
		AppendEscaped(html, ChorusDepth);
		html.Append("</span></label>");
		html.Append("<label>Chorus level <input id='rt_sf_chorus_level' type='range' min='0' max='1' step='0.01' value='");
		AppendEscaped(html, ChorusLevel);
		html.Append("'><span id='rt_sf_chorus_level_val'>");
		AppendEscaped(html, ChorusLevel);
		html.Append("</span></label>");
		html.Append("<label>Chorus voices <input id='rt_sf_chorus_voices' type='number' min='1' max='99' value='");
		AppendEscaped(html, ChorusVoices);
		html.Append("'></label>");
		html.Append("<label>Chorus speed (Hz) <input id='rt_sf_chorus_speed' type='range' min='0.29' max='5' step='0.01' value='");
		AppendEscaped(html, ChorusSpeed);
		html.Append("'><span id='rt_sf_chorus_speed_val'>");
		AppendEscaped(html, ChorusSpeed);
		html.Append("</span></label>");
		html.Append("<label>Tuning <select id='rt_sf_tuning'>");
		html.Append("<option value='0'>Equal</option>");
		html.Append("<option value='1'>Werckmeister III</option>");
		html.Append("<option value='2'>Kirnberger III</option>");
		html.Append("<option value='3'>Meantone 1/4</option>");
		html.Append("<option value='4'>Pythagorean</option>");
		html.Append("<option value='5'>Just Intonation</option>");
		html.Append("<option value='6'>Vallotti</option>");
		html.Append("</select></label>");
		html.Append("<label>Polyphony <input id='rt_sf_polyphony' type='number' min='1' max='512' value='200'></label>");
		html.Append("<label>Channel types<div id='sf_chtypes' style='display:flex;gap:3px;flex-wrap:wrap;margin-top:4px;'></div></label>");
		html.Append("</div><div id='rtStatus' style='margin-top:10px;color:#86efac;'></div></section>");

		html.Append("<section id='opl3-section'><h2>OPL3/ymfm</h2><div class='grid'>");
		html.Append("<label>Bank<input id='rt_opl3_bank' disabled placeholder='(none=built-in GM)'></label>");
		html.Append("<label>Chip mode<input id='rt_opl3_chip' disabled></label>");
		html.Append("</div></section>");

		html.Append("<script>const rs=document.getElementById('rtStatus');"
			"const setText=(id,v)=>{const e=document.getElementById(id);if(e)e.textContent=v;};"
			"const setDisabled=(id,b)=>{const e=document.getElementById(id);if(e)e.disabled=!!b;};"
			"const setSectionHidden=(id,b)=>{const e=document.getElementById(id);if(e)e.classList.toggle('section-hidden',!!b);};"
			"const setTabActive=(id,b)=>{const e=document.getElementById(id);if(e)e.classList.toggle('active',!!b);};");
		html.Append("const applyRuntimeState=(j)=>{const mt32=j.active_synth==='MT-32';const sf=j.active_synth==='SoundFont';const opl3=j.active_synth==='OPL3';setText('rt_active_synth_label',j.active_synth||'-');setText('rt_mt32_rom_name',j.mt32_rom_name||'-');setText('rt_soundfont_name',j.soundfont_name||'-');setText('rt_master_volume_val',j.master_volume);setText('rt_sf_gain_val',Number(j.sf_gain).toFixed(2));setText('rt_sf_reverb_room_val',Number(j.sf_reverb_room).toFixed(1));setText('rt_sf_reverb_level_val',Number(j.sf_reverb_level).toFixed(1));setText('rt_sf_reverb_damping_val',Number(j.sf_reverb_damping).toFixed(1));setText('rt_sf_reverb_width_val',Math.round(Number(j.sf_reverb_width)));setText('rt_sf_chorus_depth_val',Math.round(Number(j.sf_chorus_depth)));setText('rt_sf_chorus_level_val',Number(j.sf_chorus_level).toFixed(2));setText('rt_sf_chorus_speed_val',Number(j.sf_chorus_speed).toFixed(2));setText('rt_mt32_reverb_gain_val',Number(j.mt32_reverb_gain).toFixed(2));const synth=document.getElementById('rt_active_synth');if(synth)synth.value=mt32?'mt32':(sf?'soundfont':'opl3');const rom=document.getElementById('rt_mt32_rom_set');if(rom&&j.mt32_rom_set>=0)rom.value=j.mt32_rom_set===0?'mt32_old':(j.mt32_rom_set===1?'mt32_new':'cm32l');const sfSel=document.getElementById('rt_soundfont_index');if(sfSel&&j.soundfont_index>=0)sfSel.value=String(j.soundfont_index);const vol=document.getElementById('rt_master_volume');if(vol)vol.value=j.master_volume;const sfg=document.getElementById('rt_sf_gain');if(sfg)sfg.value=Number(j.sf_gain).toFixed(2);const rta=document.getElementById('rt_sf_reverb_active');if(rta)rta.value=j.sf_reverb_active?'on':'off';const rtr=document.getElementById('rt_sf_reverb_room');if(rtr)rtr.value=Number(j.sf_reverb_room).toFixed(1);const rtl=document.getElementById('rt_sf_reverb_level');if(rtl)rtl.value=Number(j.sf_reverb_level).toFixed(1);const rtd=document.getElementById('rt_sf_reverb_damping');if(rtd)rtd.value=Number(j.sf_reverb_damping).toFixed(1);const rtw=document.getElementById('rt_sf_reverb_width');if(rtw)rtw.value=Math.round(Number(j.sf_reverb_width));const cta=document.getElementById('rt_sf_chorus_active');if(cta)cta.value=j.sf_chorus_active?'on':'off';const ctd=document.getElementById('rt_sf_chorus_depth');if(ctd)ctd.value=Math.round(Number(j.sf_chorus_depth));const ctl=document.getElementById('rt_sf_chorus_level');if(ctl)ctl.value=Number(j.sf_chorus_level).toFixed(2);const ctv=document.getElementById('rt_sf_chorus_voices');if(ctv)ctv.value=j.sf_chorus_voices;const cts=document.getElementById('rt_sf_chorus_speed');if(cts)cts.value=Number(j.sf_chorus_speed).toFixed(2);const sft=document.getElementById('rt_sf_tuning');if(sft)sft.value=j.sf_tuning;const sfp=document.getElementById('rt_sf_polyphony');if(sfp)sfp.value=j.sf_polyphony;if(j.sf_percussion_mask!==undefined){var cg=document.getElementById('sf_chtypes');if(cg){if(!cg.children.length){for(var ci=0;ci<16;ci++){var btn=document.createElement('button');btn.type='button';btn.id='sf_ch'+ci;btn.style.cssText='min-width:30px;padding:2px 4px;font-size:11px;border:1px solid #334155;border-radius:4px;cursor:pointer;';btn.dataset.ch=ci;btn.addEventListener('click',function(){var c=Number(this.dataset.ch);var cur=Number(this.dataset.drum);rtApply('sf_channel_type',c+','+(cur?0:1));});cg.appendChild(btn);}}for(var ci=0;ci<16;ci++){var b=document.getElementById('sf_ch'+ci);if(b){var isDrum=(j.sf_percussion_mask>>ci)&1;b.dataset.drum=isDrum;b.textContent=(ci+1)+(isDrum?'D':'M');b.style.background=isDrum?'#f59e0b':'#334155';b.style.color=isDrum?'#000':'#e2e8f0';b.disabled=!sf;}}}}const m32rg=document.getElementById('rt_mt32_reverb_gain');if(m32rg)m32rg.value=Number(j.mt32_reverb_gain).toFixed(2);const m32ra=document.getElementById('rt_mt32_reverb_active');if(m32ra)m32ra.value=j.mt32_reverb_active?'on':'off';const m32na=document.getElementById('rt_mt32_nice_amp');if(m32na)m32na.value=j.mt32_nice_amp?'on':'off';const m32np=document.getElementById('rt_mt32_nice_pan');if(m32np)m32np.value=j.mt32_nice_pan?'on':'off';const m32nm=document.getElementById('rt_mt32_nice_mix');if(m32nm)m32nm.value=j.mt32_nice_mix?'on':'off';const m32dc=document.getElementById('rt_mt32_dac_mode');if(m32dc)m32dc.value=j.mt32_dac_mode;const m32md=document.getElementById('rt_mt32_midi_delay');if(m32md)m32md.value=j.mt32_midi_delay;const m32an=document.getElementById('rt_mt32_analog_mode');if(m32an)m32an.value=j.mt32_analog_mode;const m32rd=document.getElementById('rt_mt32_renderer_type');if(m32rd)m32rd.value=j.mt32_renderer_type;const m32pc=document.getElementById('rt_mt32_partial_count');if(m32pc)m32pc.value=j.mt32_partial_count;setDisabled('rt_mt32_rom_set',!mt32);setDisabled('rt_mt32_reverb_gain',!mt32);setDisabled('rt_mt32_reverb_active',!mt32);setDisabled('rt_mt32_nice_amp',!mt32);setDisabled('rt_mt32_nice_pan',!mt32);setDisabled('rt_mt32_nice_mix',!mt32);setDisabled('rt_mt32_dac_mode',!mt32);setDisabled('rt_mt32_midi_delay',!mt32);setDisabled('rt_mt32_analog_mode',!mt32);setDisabled('rt_mt32_renderer_type',!mt32);setDisabled('rt_mt32_partial_count',!mt32);setDisabled('rt_soundfont_index',!sf);setDisabled('rt_sf_gain',!sf);setDisabled('rt_sf_reverb_active',!sf);setDisabled('rt_sf_reverb_room',!sf);setDisabled('rt_sf_reverb_level',!sf);setDisabled('rt_sf_reverb_damping',!sf);setDisabled('rt_sf_reverb_width',!sf);setDisabled('rt_sf_chorus_active',!sf);setDisabled('rt_sf_chorus_depth',!sf);setDisabled('rt_sf_chorus_level',!sf);setDisabled('rt_sf_chorus_voices',!sf);setDisabled('rt_sf_chorus_speed',!sf);setDisabled('rt_sf_tuning',!sf);setDisabled('rt_sf_polyphony',!sf);setSectionHidden('mt32-section',!mt32);setSectionHidden('sf-section',!sf);setSectionHidden('opl3-section',!opl3);setTabActive('tab-mt32',mt32);setTabActive('tab-sf',sf);setTabActive('tab-opl3',opl3);const rtOpl3Bank=document.getElementById('rt_opl3_bank');if(rtOpl3Bank)rtOpl3Bank.value=j.ymfm_bank_name||'';const rtOpl3Chip=document.getElementById('rt_opl3_chip');if(rtOpl3Chip)rtOpl3Chip.value=j.ymfm_chip_is_opl3?'OPL3':'OPL2';var dual=!!j.mixer_dual_mode;if(dual){document.querySelectorAll('#mt32-section input,#mt32-section select,#sf-section input,#sf-section select,#opl3-section input,#opl3-section select').forEach(function(e){e.disabled=false;});setSectionHidden('mt32-section',false);setSectionHidden('sf-section',false);setSectionHidden('opl3-section',false);}var mxl=document.getElementById('rt_mixer_label');if(mxl){var pn=['Single MT-32','Single FluidSynth','Single OPL3','Split GM','Custom'];if(j.mixer_enabled){mxl.textContent=pn[j.mixer_preset]||'On';mxl.style.color='#86efac';}else{mxl.textContent='OFF';mxl.style.color='#64748b';}}var mxb=document.getElementById('mx-banner');if(mxb)mxb.style.display=dual?'block':'none';};");
		html.Append("const rtRefresh=async()=>{try{const r=await fetch('/api/runtime/status',{cache:'no-store'});if(!r.ok)throw new Error('http');const j=await r.json();applyRuntimeState(j);}catch(err){if(rs)rs.textContent='Error reading runtime status';}};");
		html.Append("const rtApply=async(param,value,bNotify=true)=>{if(!rs)return false;rs.textContent='Applying...';const body=new URLSearchParams({param,value:String(value)});try{const r=await fetch('/api/runtime/set',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:body.toString()});const j=await r.json();if(!r.ok||!j.ok){rs.textContent='Could not apply '+param;if(bNotify)showToast('Error: '+param,false);return false;}try{applyRuntimeState(j);}catch(applyErr){rs.textContent='Applied: '+param+' (refreshing...)';}if(rs.textContent==='Applying...')rs.textContent='Applied: '+param;if(bNotify)showToast('Applied: '+param);return true;}catch(err){rs.textContent='Error applying '+param;if(bNotify)showToast('Error',false);return false;}};");
		html.Append("const setActiveSynthUI=(value)=>{const synth=document.getElementById('rt_active_synth');if(synth)synth.value=value;setTabActive('tab-mt32',value==='mt32');setTabActive('tab-sf',value==='soundfont');setTabActive('tab-opl3',value==='opl3');};");
		html.Append("const applyActiveSynth=async(value)=>{setActiveSynthUI(value);const ok=await rtApply('active_synth',value,false);await rtRefresh();const lbl=document.getElementById('rt_active_synth_label');const current=lbl?lbl.textContent:'';const applied=(value==='mt32'&&current==='MT-32')||(value==='soundfont'&&current==='SoundFont')||(value==='opl3'&&current==='OPL3');if(applied){if(rs)rs.textContent='Applied: active_synth';return true;}if(rs)rs.textContent='Could not apply active_synth';if(!ok)showToast('Error: active_synth',false);return false;};");
		html.Append("const bindChange=(id,param)=>{const el=document.getElementById(id);if(!el)return;el.addEventListener('change',()=>rtApply(param,el.value));};const bindRange=(id,param,formatter)=>{const el=document.getElementById(id);if(!el)return;el.addEventListener('input',()=>{if(formatter)formatter(el.value);});el.addEventListener('change',()=>rtApply(param,el.value));};");
		html.Append("const loadSFPreview=(idx)=>{const pv=document.getElementById('sf-preview');if(!pv)return;pv.style.display='none';if(idx<0)return;");
		html.Append("_qs('/api/soundfont/info','index='+idx,function(j){if(!j)return;");
		html.Append("var n=document.getElementById('sf-prev-name');if(n)n.textContent=j.name||'';");
		html.Append("var pa=document.getElementById('sf-prev-path');if(pa)pa.textContent=j.path||'';");
		html.Append("var sz=document.getElementById('sf-prev-size');if(sz)sz.textContent=j.size_kb>0?j.size_kb+' KB':'';");
		html.Append("pv.style.display='';});};");
		html.Append("const sfSel=document.getElementById('rt_soundfont_index');if(sfSel){sfSel.addEventListener('change',function(){loadSFPreview(Number(this.value));updateFavBtn();});}");
		html.Append("const FAV_KEY='sfFavorites';");
		html.Append("const loadFavs=()=>{try{return JSON.parse(localStorage.getItem(FAV_KEY)||'[]');}catch(e){return[];}};");
		html.Append("const saveFavs=(a)=>{try{localStorage.setItem(FAV_KEY,JSON.stringify(a));}catch(e){}};");
		html.Append("const currentSFName=()=>{const s=document.getElementById('rt_soundfont_index');if(!s)return'';const o=s.options[s.selectedIndex];return o?o.textContent.trim():'';};");
		html.Append("const currentSFIndex=()=>{const s=document.getElementById('rt_soundfont_index');return s?Number(s.value):-1;};");
		html.Append("const updateFavBtn=()=>{const nm=currentSFName();const favs=loadFavs();const btn=document.getElementById('sf-fav-btn');if(!btn)return;const isFav=favs.some(f=>f.name===nm);btn.innerHTML=isFav?'&#9733;':'&#9734;';btn.title=isFav?'Remove from favorites':'Add to favorites';};");
		html.Append("const renderFavs=()=>{const favs=loadFavs();const wrap=document.getElementById('sf-favs-wrap');const cont=document.getElementById('sf-favs');if(!wrap||!cont)return;cont.innerHTML='';if(!favs.length){wrap.style.display='none';return;}wrap.style.display='';favs.forEach((f,fi)=>{const btn=document.createElement('button');btn.type='button';btn.title=f.path||f.name;btn.style.cssText='padding:3px 8px;font-size:12px;background:#1e293b;border:1px solid #334155;border-radius:5px;color:#7dd3fc;cursor:pointer;display:flex;align-items:center;gap:4px;';const star=document.createElement('span');star.textContent='\u2605';star.style.color='#facc15';btn.appendChild(star);btn.appendChild(document.createTextNode(f.name));btn.addEventListener('click',()=>{const sel=document.getElementById('rt_soundfont_index');if(!sel)return;for(let i=0;i<sel.options.length;i++){if(sel.options[i].textContent.trim()===f.name){sel.value=String(i);loadSFPreview(i);rtApply('soundfont_index',String(i));updateFavBtn();return;}}showToast('SoundFont not found: '+f.name,false);});const rm=document.createElement('button');rm.type='button';rm.title='Remove';rm.textContent='\u00d7';rm.style.cssText='margin-left:2px;background:none;border:none;color:#f87171;cursor:pointer;font-size:14px;padding:0 2px;';rm.addEventListener('click',(e)=>{e.stopPropagation();const a=loadFavs();a.splice(fi,1);saveFavs(a);renderFavs();updateFavBtn();});btn.appendChild(rm);cont.appendChild(btn);});};");
		html.Append("const sfFavBtn=document.getElementById('sf-fav-btn');if(sfFavBtn){sfFavBtn.addEventListener('click',()=>{const nm=currentSFName();if(!nm)return;const idx=currentSFIndex();let favs=loadFavs();const pos=favs.findIndex(f=>f.name===nm);if(pos>=0){favs.splice(pos,1);}else{_qs('/api/soundfont/info','index='+idx,function(j){favs.push({name:nm,path:j?j.path:''});saveFavs(favs);renderFavs();updateFavBtn();});return;}saveFavs(favs);renderFavs();updateFavBtn();});}");
		html.Append("renderFavs();updateFavBtn();");
		html.Append("const tabMT32=document.getElementById('tab-mt32');if(tabMT32)tabMT32.addEventListener('click',()=>applyActiveSynth('mt32'));const tabSF=document.getElementById('tab-sf');if(tabSF)tabSF.addEventListener('click',()=>applyActiveSynth('soundfont'));const tabOPL3=document.getElementById('tab-opl3');if(tabOPL3)tabOPL3.addEventListener('click',()=>applyActiveSynth('opl3'));const activeSynthSel=document.getElementById('rt_active_synth');if(activeSynthSel)activeSynthSel.addEventListener('change',()=>applyActiveSynth(activeSynthSel.value));");
		html.Append("bindChange('rt_mt32_rom_set','mt32_rom_set');bindChange('rt_soundfont_index','soundfont_index');bindChange('rt_sf_reverb_active','sf_reverb_active');bindChange('rt_sf_chorus_active','sf_chorus_active');bindChange('rt_sf_chorus_voices','sf_chorus_voices');bindChange('rt_sf_tuning','sf_tuning');bindChange('rt_sf_polyphony','sf_polyphony');bindChange('rt_mt32_reverb_active','mt32_reverb_active');bindChange('rt_mt32_nice_amp','mt32_nice_amp');bindChange('rt_mt32_nice_pan','mt32_nice_pan');bindChange('rt_mt32_nice_mix','mt32_nice_mix');bindChange('rt_mt32_dac_mode','mt32_dac_mode');bindChange('rt_mt32_midi_delay','mt32_midi_delay');bindChange('rt_mt32_analog_mode','mt32_analog_mode');bindChange('rt_mt32_renderer_type','mt32_renderer_type');bindChange('rt_mt32_partial_count','mt32_partial_count');");
		html.Append("bindRange('rt_master_volume','master_volume',(v)=>setText('rt_master_volume_val',v));bindRange('rt_sf_gain','sf_gain',(v)=>setText('rt_sf_gain_val',Number(v).toFixed(2)));bindRange('rt_sf_reverb_room','sf_reverb_room',(v)=>setText('rt_sf_reverb_room_val',Number(v).toFixed(1)));bindRange('rt_sf_reverb_level','sf_reverb_level',(v)=>setText('rt_sf_reverb_level_val',Number(v).toFixed(1)));bindRange('rt_sf_reverb_damping','sf_reverb_damping',(v)=>setText('rt_sf_reverb_damping_val',Number(v).toFixed(1)));bindRange('rt_sf_reverb_width','sf_reverb_width',(v)=>setText('rt_sf_reverb_width_val',Math.round(Number(v))));bindRange('rt_sf_chorus_depth','sf_chorus_depth',(v)=>setText('rt_sf_chorus_depth_val',Math.round(Number(v))));bindRange('rt_sf_chorus_level','sf_chorus_level',(v)=>setText('rt_sf_chorus_level_val',Number(v).toFixed(2)));bindRange('rt_sf_chorus_speed','sf_chorus_speed',(v)=>setText('rt_sf_chorus_speed_val',Number(v).toFixed(2)));bindRange('rt_mt32_reverb_gain','mt32_reverb_gain',(v)=>setText('rt_mt32_reverb_gain_val',Number(v).toFixed(2)));rtRefresh();setInterval(rtRefresh,3000);</script>");
		html.Append("</main></body></html>");

		if (!html.bOk)
			return HTTPInternalServerError;
		*pLength = html.Length();
		*ppContentType = "text/html; charset=utf-8";
		return HTTPOK;
}

THTTPStatus CWebDaemon::BuildConfigPage(u8* pBuffer, unsigned* pLength, const char** ppContentType)
{
		const CConfig* pConfig = m_pMT32Pi->GetConfig();
		if (!pConfig)
			return HTTPInternalServerError;

		HtmlWriter html(pBuffer, *pLength);
		html.Append("<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width, initial-scale=1'>");
		html.Append("<title>mt32-pi config</title><link rel='stylesheet' href='/app.css'></head><body><main>");
		html.Append("<script src='/app.js'></script>");
		html.Append("<h1>Configure mt32-pi</h1><p>Saves changes to <code>mt32-pi.cfg</code> and creates a backup <code>mt32-pi.cfg.bak</code>.</p>");
		html.Append("<nav><a href='/'>Status</a><a href='/sound'>Sound</a><a href='/config'>Config</a><a href='/sequencer'>Sequencer</a><a href='/mixer'>Mixer</a><a href='/monitor'>Monitor</a></nav>");
		html.Append("<form id='cfgForm'>");

		// ---- [system] ----
		CString SysI2CBaud; SysI2CBaud.Format("%d", pConfig->SystemI2CBaudRate);
		CString SysPowerSave; SysPowerSave.Format("%d", pConfig->SystemPowerSaveTimeout);
		html.Append("<section><h2>System</h2><div class='grid'>");
		html.Append("<label>Default synth<small>mt32: MT-32 via Munt emulator; soundfont: FluidSynth; ymfm: OPL3/OPL2 chip emulation. Falls back to first available synth if chosen one is unavailable.</small><select name='default_synth'>");
		html.Append("<option value='mt32'"); html.Append(SelectedAttr(pConfig->SystemDefaultSynth == CConfig::TSystemDefaultSynth::MT32)); html.Append(">MT-32</option>");
		html.Append("<option value='soundfont'"); html.Append(SelectedAttr(pConfig->SystemDefaultSynth == CConfig::TSystemDefaultSynth::SoundFont)); html.Append(">SoundFont</option>");
		html.Append("<option value='ymfm'"); html.Append(SelectedAttr(pConfig->SystemDefaultSynth == CConfig::TSystemDefaultSynth::Ymfm)); html.Append(">OPL3/ymfm</option>");
		html.Append("</select></label>");
		html.Append("<label>Verbose<small>on: more info on LCD at boot and on errors. May hide the boot logo on small displays.</small><select name='system_verbose'><option value='off'"); html.Append(SelectedAttr(!pConfig->SystemVerbose)); html.Append(">off</option><option value='on'"); html.Append(SelectedAttr(pConfig->SystemVerbose)); html.Append(">on</option></select></label>");
		html.Append("<label>USB<small>on: enables USB support (MIDI, keyboards, etc.). Disable to speed up boot if USB is not needed.</small><select name='system_usb'><option value='on'"); html.Append(SelectedAttr(pConfig->SystemUSB)); html.Append(">on</option><option value='off'"); html.Append(SelectedAttr(!pConfig->SystemUSB)); html.Append(">off</option></select></label>");
		html.Append("<label>I2C baud rate<small>Bus speed in Hz. 400000 = fast mode. Use 1000000 for high-res graphic LCDs. Range: 100000-1000000</small><input name='system_i2c_baud_rate' type='number' value='"); AppendEscaped(html, SysI2CBaud); html.Append("'></label>");
		html.Append("<label>Power save timeout (s)<small>Seconds of silence before slowing CPU and turning off backlight. 0 = disabled. Range: 0-3600</small><input name='system_power_save_timeout' type='number' value='"); AppendEscaped(html, SysPowerSave); html.Append("'></label>");
		html.Append("</div></section>");

		// ---- [audio] ----
		CString AudioSR; AudioSR.Format("%d", pConfig->AudioSampleRate);
		CString AudioCS; AudioCS.Format("%d", pConfig->AudioChunkSize);
		html.Append("<section><h2>Audio</h2><div class='grid'>");
		html.Append("<label>Output device<small>pwm: headphone jack (Pi 3B/4); hdmi: HDMI audio; i2s: external I2S DAC (HiFiBerry, etc.)</small><select name='audio_output_device'>");
		html.Append("<option value='pwm'");  html.Append(SelectedAttr(pConfig->AudioOutputDevice == CConfig::TAudioOutputDevice::PWM));  html.Append(">PWM</option>");
		html.Append("<option value='hdmi'"); html.Append(SelectedAttr(pConfig->AudioOutputDevice == CConfig::TAudioOutputDevice::HDMI)); html.Append(">HDMI</option>");
		html.Append("<option value='i2s'");  html.Append(SelectedAttr(pConfig->AudioOutputDevice == CConfig::TAudioOutputDevice::I2S));  html.Append(">I2S</option>");
		html.Append("</select></label>");
		html.Append("<label>Sample rate (Hz)<small>PWM: 22050-192000; HDMI: 48000 only; I2S: depends on DAC. MT-32 uses 32000 Hz internally (resampled). Range: 32000-192000</small><input name='audio_sample_rate' type='number' value='"); AppendEscaped(html, AudioSR); html.Append("'></label>");
		html.Append("<label>Chunk size<small>Samples per audio buffer. Lower = lower latency. Min: PWM=2, I2S=32, HDMI=384 (multiple). Latency = chunk/2/Hz*1000ms. Range: 2-2048</small><input name='audio_chunk_size' type='number' value='"); AppendEscaped(html, AudioCS); html.Append("'></label>");
		html.Append("<label>Reversed stereo<small>on: swaps left/right channels. Use if hardware has channels connected in reverse.</small><select name='audio_reversed_stereo'><option value='off'"); html.Append(SelectedAttr(!pConfig->AudioReversedStereo)); html.Append(">off</option><option value='on'"); html.Append(SelectedAttr(pConfig->AudioReversedStereo)); html.Append(">on</option></select></label>");
		html.Append("</div></section>");

		// ---- [midi] ----
		CString MIDIGPIOBaud; MIDIGPIOBaud.Format("%d", pConfig->MIDIGPIOBaudRate);
		CString MIDIUSBBaud;  MIDIUSBBaud.Format("%d", pConfig->MIDIUSBSerialBaudRate);
		html.Append("<section><h2>MIDI</h2><div class='grid'>");
		html.Append("<label>GPIO baud rate<small>Baud rate for GPIO MIDI. Standard DIN MIDI: 31250. SoftMPU serial mode: 38400. Range: 300-4000000</small><input name='midi_gpio_baud_rate' type='number' value='"); AppendEscaped(html, MIDIGPIOBaud); html.Append("'></label>");
		html.Append("<label>GPIO thru<small>on: retransmits on GPIO Tx everything received on Rx. Useful for debugging or passing MIDI to another synth.</small><select name='midi_gpio_thru'><option value='off'"); html.Append(SelectedAttr(!pConfig->MIDIGPIOThru)); html.Append(">off</option><option value='on'"); html.Append(SelectedAttr(pConfig->MIDIGPIOThru)); html.Append(">on</option></select></label>");
		html.Append("<label>USB serial baud rate<small>Baud rate for MIDI via USB-serial adapter. SoftMPU serial mode: 38400. Range: 9600-115200</small><input name='midi_usb_serial_baud_rate' type='number' value='"); AppendEscaped(html, MIDIUSBBaud); html.Append("'></label>");
		html.Append("</div></section>");

		// ---- [control] ----
		CString CtrlSwitchTO; CtrlSwitchTO.Format("%d", pConfig->ControlSwitchTimeout);
		html.Append("<section><h2>Control</h2><div class='grid'>");
		html.Append("<label>Control scheme<small>none: no physical controls; simple_buttons: 4-button scheme; simple_encoder: 2 buttons + rotary encoder</small><select name='control_scheme'>");
		html.Append("<option value='none'"); html.Append(SelectedAttr(pConfig->ControlScheme == CConfig::TControlScheme::None)); html.Append(">none</option>");
		html.Append("<option value='simple_buttons'"); html.Append(SelectedAttr(pConfig->ControlScheme == CConfig::TControlScheme::SimpleButtons)); html.Append(">simple_buttons</option>");
		html.Append("<option value='simple_encoder'"); html.Append(SelectedAttr(pConfig->ControlScheme == CConfig::TControlScheme::SimpleEncoder)); html.Append(">simple_encoder</option>");
		html.Append("</select></label>");
		html.Append("<label>Encoder type<small>Gray-code cycle per click. quarter: 4 clicks = 1 step; half: 2 clicks; full: 1 click = 1 step. Depends on your encoder hardware.</small><select name='encoder_type'>");
		html.Append("<option value='quarter'"); html.Append(SelectedAttr(pConfig->ControlEncoderType == CConfig::TEncoderType::Quarter)); html.Append(">quarter</option>");
		html.Append("<option value='half'");    html.Append(SelectedAttr(pConfig->ControlEncoderType == CConfig::TEncoderType::Half));    html.Append(">half</option>");
		html.Append("<option value='full'");    html.Append(SelectedAttr(pConfig->ControlEncoderType == CConfig::TEncoderType::Full));    html.Append(">full</option>");
		html.Append("</select></label>");
		html.Append("<label>Encoder reversed<small>on: reverses encoder direction if CLK/DAT are connected backwards.</small><select name='encoder_reversed'><option value='off'"); html.Append(SelectedAttr(!pConfig->ControlEncoderReversed)); html.Append(">off</option><option value='on'"); html.Append(SelectedAttr(pConfig->ControlEncoderReversed)); html.Append(">on</option></select></label>");
		html.Append("<label>MiSTer<small>on: enables I2C interface to control mt32-pi from the MiSTer FPGA OSD via additional hardware.</small><select name='control_mister'><option value='off'"); html.Append(SelectedAttr(!pConfig->ControlMister)); html.Append(">off</option><option value='on'"); html.Append(SelectedAttr(pConfig->ControlMister)); html.Append(">on</option></select></label>");
		html.Append("<label>Switch timeout (s)<small>Seconds to wait before loading the SoundFont when switching with the physical button. Range: 0-3600</small><input name='control_switch_timeout' type='number' value='"); AppendEscaped(html, CtrlSwitchTO); html.Append("'></label>");
		html.Append("</div></section>");

		// ---- [mt32emu] ----
		CString MT32Gain; MT32Gain.Format("%.2f", pConfig->MT32EmuGain);
		CString MT32RevGain; MT32RevGain.Format("%.2f", pConfig->MT32EmuReverbGain);
		html.Append("<section><h2>MT-32 emulator (defaults)</h2><div class='grid'>");
		html.Append("<label>Gain<small>Synthesizer output gain. 1.0 = no change. Independent of MIDI volume. Range: 0.0-256.0</small><input name='mt32emu_gain' type='number' step='0.01' min='0' max='8' value='"); AppendEscaped(html, MT32Gain); html.Append("'></label>");
		html.Append("<label>Reverb gain<small>Gain applied only to the MT-32 wet reverb channel. Range: 0.0 and upwards.</small><input name='mt32emu_reverb_gain' type='number' step='0.01' min='0' max='8' value='"); AppendEscaped(html, MT32RevGain); html.Append("'></label>");
		html.Append("<label>Resampler quality<small>none: no resampling (requires sample_rate=32000); fastest/fast: quick; good: balanced; best: highest quality, most CPU</small><select name='mt32emu_resampler_quality'>");
		html.Append("<option value='none'");    html.Append(SelectedAttr(pConfig->MT32EmuResamplerQuality == CConfig::TMT32EmuResamplerQuality::None));    html.Append(">none</option>");
		html.Append("<option value='fastest'"); html.Append(SelectedAttr(pConfig->MT32EmuResamplerQuality == CConfig::TMT32EmuResamplerQuality::Fastest)); html.Append(">fastest</option>");
		html.Append("<option value='fast'");    html.Append(SelectedAttr(pConfig->MT32EmuResamplerQuality == CConfig::TMT32EmuResamplerQuality::Fast));    html.Append(">fast</option>");
		html.Append("<option value='good'");    html.Append(SelectedAttr(pConfig->MT32EmuResamplerQuality == CConfig::TMT32EmuResamplerQuality::Good));    html.Append(">good</option>");
		html.Append("<option value='best'");    html.Append(SelectedAttr(pConfig->MT32EmuResamplerQuality == CConfig::TMT32EmuResamplerQuality::Best));    html.Append(">best</option>");
		html.Append("</select></label>");
		html.Append("<label>MIDI channels<small>standard: parts 1-8 on MIDI channels 2-9, rhythm=10; alternate: parts 1-8 on channels 1-8, rhythm=10</small><select name='mt32emu_midi_channels'>");
		html.Append("<option value='standard'");  html.Append(SelectedAttr(pConfig->MT32EmuMIDIChannels == CConfig::TMT32EmuMIDIChannels::Standard));  html.Append(">standard</option>");
		html.Append("<option value='alternate'"); html.Append(SelectedAttr(pConfig->MT32EmuMIDIChannels == CConfig::TMT32EmuMIDIChannels::Alternate)); html.Append(">alternate</option>");
		html.Append("</select></label>");
		html.Append("<label>ROM set<small>Boot ROM set. old: MT-32 v1; new: MT-32 v2; cm32l: Roland CM-32L; any: first available; all: all found</small><select name='mt32emu_rom_set'>");
		html.Append("<option value='old'");   html.Append(SelectedAttr(pConfig->MT32EmuROMSet == CConfig::TMT32EmuROMSet::MT32Old)); html.Append(">old (MT-32)</option>");
		html.Append("<option value='new'");   html.Append(SelectedAttr(pConfig->MT32EmuROMSet == CConfig::TMT32EmuROMSet::MT32New)); html.Append(">new (MT-32)</option>");
		html.Append("<option value='cm32l'"); html.Append(SelectedAttr(pConfig->MT32EmuROMSet == CConfig::TMT32EmuROMSet::CM32L));  html.Append(">cm32l</option>");
		html.Append("<option value='any'");   html.Append(SelectedAttr(pConfig->MT32EmuROMSet == CConfig::TMT32EmuROMSet::Any));    html.Append(">any</option>");
		html.Append("<option value='all'");   html.Append(SelectedAttr(pConfig->MT32EmuROMSet == CConfig::TMT32EmuROMSet::All));    html.Append(">all</option>");
		html.Append("</select></label>");
		html.Append("<label>Reversed stereo<small>on: swaps L/R so MT-32 panning matches SoundFont. Also changeable at runtime via SysEx.</small><select name='mt32emu_reversed_stereo'><option value='off'"); html.Append(SelectedAttr(!pConfig->MT32EmuReversedStereo)); html.Append(">off</option><option value='on'"); html.Append(SelectedAttr(pConfig->MT32EmuReversedStereo)); html.Append(">on</option></select></label>");
		html.Append("</div></section>");

		// ---- [fluidsynth] ----
		CString FSSoundFont; FSSoundFont.Format("%d", pConfig->FluidSynthSoundFont);
		CString FSPolyphony; FSPolyphony.Format("%d", pConfig->FluidSynthPolyphony);
		CString FSGain;      FSGain.Format("%.2f", pConfig->FluidSynthDefaultGain);
		CString FSRevDamp;   FSRevDamp.Format("%.2f", pConfig->FluidSynthDefaultReverbDamping);
		CString FSRevLevel;  FSRevLevel.Format("%.2f", pConfig->FluidSynthDefaultReverbLevel);
		CString FSRevRoom;   FSRevRoom.Format("%.2f", pConfig->FluidSynthDefaultReverbRoomSize);
		CString FSRevWidth;  FSRevWidth.Format("%.2f", pConfig->FluidSynthDefaultReverbWidth);
		CString FSChrDepth;  FSChrDepth.Format("%.2f", pConfig->FluidSynthDefaultChorusDepth);
		CString FSChrLevel;  FSChrLevel.Format("%.2f", pConfig->FluidSynthDefaultChorusLevel);
		CString FSChrVoices; FSChrVoices.Format("%d", pConfig->FluidSynthDefaultChorusVoices);
		CString FSChrSpeed;  FSChrSpeed.Format("%.2f", pConfig->FluidSynthDefaultChorusSpeed);
		html.Append("<section><h2>SoundFont (defaults)</h2><div class='grid'>");
		html.Append("<label>SoundFont index<small>0-based index sorted alphabetically from soundfonts/. 0=first, 1=second, etc.</small><input name='fs_soundfont' type='number' min='0' value='"); AppendEscaped(html, FSSoundFont); html.Append("'></label>");
		html.Append("<label>Polyphony<small>Max simultaneous voices. Reduce if distortion occurs. Pi4/overclocked can support higher values. Range: 1-65535</small><input name='fs_polyphony' type='number' min='1' value='"); AppendEscaped(html, FSPolyphony); html.Append("'></label>");
		html.Append("<label>Gain<small>FluidSynth master volume gain. See fluidsettings.xml for details.</small><input name='fs_gain' type='number' step='0.01' min='0' value='"); AppendEscaped(html, FSGain); html.Append("'></label>");
		html.Append("<label>Reverb<small>on: enable reverb by default. Can be overridden per SoundFont with a .cfg file next to the .sf2</small><select name='fs_reverb'><option value='on'"); html.Append(SelectedAttr(pConfig->FluidSynthDefaultReverbActive)); html.Append(">on</option><option value='off'"); html.Append(SelectedAttr(!pConfig->FluidSynthDefaultReverbActive)); html.Append(">off</option></select></label>");
		html.Append("<label>Reverb damping<small>High-frequency absorption. Range: 0.0-1.0</small><input name='fs_reverb_damping' type='number' step='0.01' min='0' max='1' value='"); AppendEscaped(html, FSRevDamp); html.Append("'></label>");
		html.Append("<label>Reverb level<small>Wet mix level. Range: 0.0-1.0</small><input name='fs_reverb_level' type='number' step='0.01' min='0' value='"); AppendEscaped(html, FSRevLevel); html.Append("'></label>");
		html.Append("<label>Reverb room size<small>Room size. Higher values = more cavernous. Range: 0.0-1.0</small><input name='fs_reverb_room_size' type='number' step='0.01' min='0' max='1' value='"); AppendEscaped(html, FSRevRoom); html.Append("'></label>");
		html.Append("<label>Reverb width<small>Stereo width. 0.0 = mono, 100.0 = maximum. Range: 0.0-100.0</small><input name='fs_reverb_width' type='number' step='0.01' min='0' value='"); AppendEscaped(html, FSRevWidth); html.Append("'></label>");
		html.Append("<label>Chorus<small>on: enable chorus by default. Can be overridden per SoundFont with a .cfg file next to the .sf2</small><select name='fs_chorus'><option value='on'"); html.Append(SelectedAttr(pConfig->FluidSynthDefaultChorusActive)); html.Append(">on</option><option value='off'"); html.Append(SelectedAttr(!pConfig->FluidSynthDefaultChorusActive)); html.Append(">off</option></select></label>");
		html.Append("<label>Chorus depth<small>Modulation depth (ms). Typical values: 0-20</small><input name='fs_chorus_depth' type='number' step='0.01' min='0' value='"); AppendEscaped(html, FSChrDepth); html.Append("'></label>");
		html.Append("<label>Chorus level<small>Mix level. Typical values: 0.0-10.0</small><input name='fs_chorus_level' type='number' step='0.01' min='0' value='"); AppendEscaped(html, FSChrLevel); html.Append("'></label>");
		html.Append("<label>Chorus voices<small>Number of modulator voices. Typical values: 1-99</small><input name='fs_chorus_voices' type='number' min='0' value='"); AppendEscaped(html, FSChrVoices); html.Append("'></label>");
		html.Append("<label>Chorus speed<small>Modulation speed in Hz. Range: 0.29-5.0</small><input name='fs_chorus_speed' type='number' step='0.01' min='0' value='"); AppendEscaped(html, FSChrSpeed); html.Append("'></label>");
		html.Append("</div></section>");

		// ---- [ymfm] ----
		html.Append("<section><h2>OPL3/ymfm (defaults)</h2><div class='grid'>");
		html.Append("<label>Chip mode<small>opl3: 18-voice stereo OPL3; opl2: 9-voice mono OPL2 (backward compat)</small><select name='ymfm_chip'>");
		html.Append("<option value='opl3'"); html.Append(SelectedAttr(pConfig->YmfmChip == CConfig::TYmfmChip::OPL3)); html.Append(">opl3</option>");
		html.Append("<option value='opl2'"); html.Append(SelectedAttr(pConfig->YmfmChip == CConfig::TYmfmChip::OPL2)); html.Append(">opl2</option>");
		html.Append("</select></label>");
		html.Append("<label>Bank file<small>Path to .wopl or .op2 bank file relative to SD root. Empty = built-in GM patches.</small><input name='ymfm_bank_file' value='"); AppendEscaped(html, pConfig->YmfmBankFile); html.Append("'></label>");
		html.Append("</div></section>");

		// ---- [lcd] ----
		CString LCDWidth; LCDWidth.Format("%d", pConfig->LCDWidth);
		CString LCDHeight; LCDHeight.Format("%d", pConfig->LCDHeight);
		CString I2CAddr; I2CAddr.Format("%x", pConfig->LCDI2CLCDAddress);
		html.Append("<section><h2>LCD / OLED display</h2><div class='grid'>");
		html.Append("<label>LCD type<small>none: no LCD; hd44780_4bit: 4-bit GPIO char LCD; hd44780_i2c: I2C char LCD; sh1106_i2c: 1.3 inch OLED; ssd1306_i2c: 0.96 inch OLED</small><select name='lcd_type'>");
		html.Append("<option value='none'");        html.Append(SelectedAttr(pConfig->LCDType == CConfig::TLCDType::None));          html.Append(">none</option>");
		html.Append("<option value='hd44780_4bit'"); html.Append(SelectedAttr(pConfig->LCDType == CConfig::TLCDType::HD44780FourBit)); html.Append(">hd44780_4bit</option>");
		html.Append("<option value='hd44780_i2c'"); html.Append(SelectedAttr(pConfig->LCDType == CConfig::TLCDType::HD44780I2C));    html.Append(">hd44780_i2c</option>");
		html.Append("<option value='sh1106_i2c'");  html.Append(SelectedAttr(pConfig->LCDType == CConfig::TLCDType::SH1106I2C));     html.Append(">sh1106_i2c</option>");
		html.Append("<option value='ssd1306_i2c'"); html.Append(SelectedAttr(pConfig->LCDType == CConfig::TLCDType::SSD1306I2C));    html.Append(">ssd1306_i2c</option>");
		html.Append("</select></label>");
		html.Append("<label>LCD width<small>Width in characters (LCD) or pixels (OLED). SSD1305: use 132. Range: 20-132</small><input name='lcd_width' type='number' value='"); AppendEscaped(html, LCDWidth); html.Append("'></label>");
		html.Append("<label>LCD height<small>Height in characters (LCD) or pixels (OLED). See docs for your model. Range: 2-64</small><input name='lcd_height' type='number' value='"); AppendEscaped(html, LCDHeight); html.Append("'></label>");
		html.Append("<label>LCD I2C address (hex)<small>Hex address without 0x prefix. Most common: 3c or 3d. Check your display datasheet.</small><input name='lcd_i2c_address' value='"); AppendEscaped(html, I2CAddr); html.Append("'></label>");
		html.Append("<label>Rotation<small>normal: no rotation; inverted: 180 degree rotation. Graphic LCDs only (sh1106, ssd1306).</small><select name='lcd_rotation'>");
		html.Append("<option value='normal'");   html.Append(SelectedAttr(pConfig->LCDRotation == CConfig::TLCDRotation::Normal));   html.Append(">normal</option>");
		html.Append("<option value='inverted'"); html.Append(SelectedAttr(pConfig->LCDRotation == CConfig::TLCDRotation::Inverted)); html.Append(">inverted</option>");
		html.Append("</select></label>");
		html.Append("<label>Mirror<small>normal: no mirror; mirrored: horizontal reflection. Graphic LCDs only (sh1106, ssd1306).</small><select name='lcd_mirror'>");
		html.Append("<option value='normal'");   html.Append(SelectedAttr(pConfig->LCDMirror == CConfig::TLCDMirror::Normal));   html.Append(">normal</option>");
		html.Append("<option value='mirrored'"); html.Append(SelectedAttr(pConfig->LCDMirror == CConfig::TLCDMirror::Mirrored)); html.Append(">mirrored</option>");
		html.Append("</select></label>");
		html.Append("</div></section>");

		// ---- [network] ----
		CString WebPort; WebPort.Format("%d", pConfig->NetworkWebServerPort);
		CString IP; pConfig->NetworkIPAddress.Format(&IP);
		CString Subnet; pConfig->NetworkSubnetMask.Format(&Subnet);
		CString GW; pConfig->NetworkDefaultGateway.Format(&GW);
		CString DNS; pConfig->NetworkDNSServer.Format(&DNS);
		html.Append("<section><h2>Network</h2><div class='grid'>");
		html.Append("<label>Mode<small>off: no network; ethernet: Ethernet (Pi 3B/3B+ requires USB); wifi: Wi-Fi (configure SSID in WiFi section below)</small><select name='network_mode'><option value='off'"); html.Append(SelectedAttr(pConfig->NetworkMode == CConfig::TNetworkMode::Off)); html.Append(">off</option>");
		html.Append("<option value='ethernet'"); html.Append(SelectedAttr(pConfig->NetworkMode == CConfig::TNetworkMode::Ethernet)); html.Append(">ethernet</option>");
		html.Append("<option value='wifi'");     html.Append(SelectedAttr(pConfig->NetworkMode == CConfig::TNetworkMode::WiFi));     html.Append(">wifi</option></select></label>");
		html.Append("<label>DHCP<small>on: automatic IP via DHCP; off: use static IP, subnet, gateway and DNS values below.</small><select name='network_dhcp'><option value='on'"); html.Append(SelectedAttr(pConfig->NetworkDHCP)); html.Append(">on</option><option value='off'"); html.Append(SelectedAttr(!pConfig->NetworkDHCP)); html.Append(">off</option></select></label>");
		html.Append("<label>IP address<input name='network_ip' value='"); AppendEscaped(html, IP); html.Append("'></label>");
		html.Append("<label>Subnet mask<input name='network_subnet' value='"); AppendEscaped(html, Subnet); html.Append("'></label>");
		html.Append("<label>Default gateway<input name='network_gateway' value='"); AppendEscaped(html, GW); html.Append("'></label>");
		html.Append("<label>DNS server<input name='network_dns' value='"); AppendEscaped(html, DNS); html.Append("'></label>");
		html.Append("<label>Hostname<input name='network_hostname' value='"); AppendEscaped(html, pConfig->NetworkHostname); html.Append("'></label>");
		html.Append("<label>RTP MIDI<small>on: enable RTP-MIDI/AppleMIDI server (macOS, rtpMIDI on Windows).</small><select name='network_rtp_midi'><option value='on'"); html.Append(SelectedAttr(pConfig->NetworkRTPMIDI)); html.Append(">on</option><option value='off'"); html.Append(SelectedAttr(!pConfig->NetworkRTPMIDI)); html.Append(">off</option></select></label>");
		html.Append("<label>UDP MIDI<small>on: enable simple UDP MIDI server on port 1999.</small><select name='network_udp_midi'><option value='on'"); html.Append(SelectedAttr(pConfig->NetworkUDPMIDI)); html.Append(">on</option><option value='off'"); html.Append(SelectedAttr(!pConfig->NetworkUDPMIDI)); html.Append(">off</option></select></label>");
		html.Append("<label>FTP<small>on: enable FTP server to transfer files (ROMs, SoundFonts) over the network.</small><select name='network_ftp'><option value='on'"); html.Append(SelectedAttr(pConfig->NetworkFTPServer)); html.Append(">on</option><option value='off'"); html.Append(SelectedAttr(!pConfig->NetworkFTPServer)); html.Append(">off</option></select></label>");
		html.Append("<label>FTP username<small>FTP server username. Default: mt32-pi</small><input name='network_ftp_username' value='"); AppendEscaped(html, pConfig->NetworkFTPUsername); html.Append("'></label>");
		html.Append("<label>FTP password<small>FTP server password. Default: mt32-pi</small><input name='network_ftp_password' type='password' value='"); AppendEscaped(html, pConfig->NetworkFTPPassword); html.Append("'></label>");
		html.Append("<label>Web<select name='network_web'><option value='on'"); html.Append(SelectedAttr(pConfig->NetworkWebServer)); html.Append(">on</option><option value='off'"); html.Append(SelectedAttr(!pConfig->NetworkWebServer)); html.Append(">off</option></select></label>");
		html.Append("<label>Web port<small>TCP port for the web server. Default: 80. Restart to apply changes.</small><input name='network_web_port' type='number' value='"); AppendEscaped(html, WebPort); html.Append("'></label>");
		CString OSCPort; OSCPort.Format("%d", pConfig->NetworkOSCPort);
		html.Append("<label>OSC<small>on: enable OSC (Open Sound Control) UDP server for MIDI and control messages.</small><select name='network_osc'><option value='on'"); html.Append(SelectedAttr(pConfig->NetworkOSC)); html.Append(">on</option><option value='off'"); html.Append(SelectedAttr(!pConfig->NetworkOSC)); html.Append(">off</option></select></label>");
		html.Append("<label>OSC port<small>UDP port for the OSC server. Default: 8000. Restart to apply changes.</small><input name='network_osc_port' type='number' value='"); AppendEscaped(html, OSCPort); html.Append("'></label>");
		html.Append("</div></section>");

		html.Append("<section id='wifi-section' class='section-hidden'><h2>WiFi</h2><p>Credentials for connecting to a wireless network. Saved to <code>wpa_supplicant.conf</code>.</p>");
		html.Append("<div class='grid'>");
		html.Append("<label>Country (ISO 3166-1 alpha-2)<input id='wifi_country' maxlength='2' placeholder='ES' autocomplete='off'></label>");
		html.Append("<label>SSID<input id='wifi_ssid' autocomplete='off'></label>");
		html.Append("<label>Password (PSK)<input id='wifi_psk' type='password' autocomplete='new-password' placeholder='leave empty to keep current'></label>");
		html.Append("</div><div style='margin-top:12px;'>");
		html.Append("<button class='primary' type='button' onclick='saveWifi()'>Save WiFi</button> ");
		html.Append("<span id='wifi_status' style='color:#93c5fd;'></span></div></section>");

		html.Append("<button class='primary' type='submit'>Save config</button> <button class='warn' type='button' id='rebootBtn'>Restart Pi</button> <span id='status'></span>");
		html.Append("</form>");
		html.Append("<script>const f=document.getElementById('cfgForm');const s=document.getElementById('status');const rb=document.getElementById('rebootBtn');");
		html.Append("f.addEventListener('submit',async(e)=>{e.preventDefault();s.textContent='Saving...';const body=new URLSearchParams(new FormData(f));try{const r=await fetch('/api/config/save',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:body.toString()});const j=await r.json();s.textContent=j.message||'OK';}catch(err){s.textContent='Error saving config';}});");
		html.Append("rb.addEventListener('click',async()=>{if(!confirm('Restart mt32-pi now?'))return;s.textContent='Restarting\u2026';rb.disabled=true;try{await fetch('/api/system/reboot',{method:'POST'});}catch(e){}s.textContent='Restarting\u2026 reconnect in ~20s';});");
		html.Append("const mEl=document.querySelector('select[name=\"network_mode\"]');const wSec=document.getElementById('wifi-section');");
		html.Append("function _chkWifi(){if(wSec)wSec.classList.toggle('section-hidden',!mEl||mEl.value!=='wifi');}");
		html.Append("if(mEl)mEl.addEventListener('change',_chkWifi);_chkWifi();");
		html.Append("fetch('/api/wifi/read').then(r=>r.json()).then(j=>{const ss=document.getElementById('wifi_ssid');const co=document.getElementById('wifi_country');if(ss)ss.value=j.ssid||'';if(co)co.value=j.country||'';}).catch(()=>{});");
		html.Append("function saveWifi(){const ws=document.getElementById('wifi_status');const ss=document.getElementById('wifi_ssid');const co=document.getElementById('wifi_country');const pk=document.getElementById('wifi_psk');");
		html.Append("if(!ss||!ss.value.trim()){if(ws)ws.textContent='SSID required';return;}if(!co||!co.value.trim()){if(ws)ws.textContent='Country required';return;}");
		html.Append("if(ws)ws.textContent='Saving...';const body=new URLSearchParams({wifi_ssid:ss.value,wifi_psk:pk?pk.value:'',wifi_country:co.value});");
		html.Append("fetch('/api/wifi/save',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:body.toString()}).then(r=>r.json()).then(j=>{if(ws)ws.textContent=j.ok?'Saved. Restart to apply.':(j.message||'Error');}).catch(()=>{if(ws)ws.textContent='Error saving WiFi';});}</script>");
		html.Append("</main></body></html>");

		if (!html.bOk)
			return HTTPInternalServerError;
		*pLength = html.Length();
		*ppContentType = "text/html; charset=utf-8";
		return HTTPOK;
}

THTTPStatus CWebDaemon::BuildStatusPage(u8* pBuffer, unsigned* pLength, const char** ppContentType)
{
	const CConfig* pConfig = m_pMT32Pi->GetConfig();
	if (!pConfig)
		return HTTPInternalServerError;

	CString IPAddress;
	m_pMT32Pi->FormatIPAddress(IPAddress);

	CString I2CAddress;
	I2CAddress.Format("0x%x", pConfig->LCDI2CLCDAddress);

	CString SoundFontIndex;
	SoundFontIndex.Format("%u / %u", static_cast<unsigned>(m_pMT32Pi->GetCurrentSoundFontIndex()), static_cast<unsigned>(m_pMT32Pi->GetSoundFontCount()));

	HtmlWriter html(pBuffer, *pLength);
	html.Append("<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width, initial-scale=1'><title>mt32-pi status</title><link rel='stylesheet' href='/app.css'></head><body><main>");
	html.Append("<script src='/app.js'></script>");
	html.Append("<h1>mt32-pi</h1><p>Live status of system, network and synthesizers.</p>");
	html.Append("<nav><a href='/'>Status</a><a href='/sound'>Sound</a><a href='/config'>Config</a><a href='/sequencer'>Sequencer</a><a href='/mixer'>Mixer</a><a href='/monitor'>Monitor</a></nav>");
	html.Append("<div class='hero'>");
	html.Append("<div class='pill'>IP: ");
		AppendEscaped(html, IPAddress);
	html.Append("</div><div class='pill'>Active synth: ");
		AppendEscaped(html, m_pMT32Pi->GetActiveSynthName());
	html.Append("</div></div><div class='grid'>");

	AppendSectionStart(html, "System");
	AppendRow(html, "Active synth", m_pMT32Pi->GetActiveSynthName());
	AppendRow(html, "Default synth", DefaultSynthText(pConfig->SystemDefaultSynth));
	AppendRow(html, "USB", BoolText(pConfig->SystemUSB));
	AppendIntRow(html, "I2C baud", pConfig->SystemI2CBaudRate);
	AppendIntRow(html, "Power save timeout", pConfig->SystemPowerSaveTimeout);
	AppendSectionEnd(html);

	AppendSectionStart(html, "Network");
	AppendRow(html, "Interface", m_pMT32Pi->GetNetworkInterfaceName());
	AppendRow(html, "Network ready", BoolText(m_pMT32Pi->IsNetworkReady()));
	AppendRow(html, "Mode", NetworkModeText(pConfig->NetworkMode));
	AppendRow(html, "DHCP", BoolText(pConfig->NetworkDHCP));
	AppendRow(html, "Hostname", pConfig->NetworkHostname);
	AppendRow(html, "Current IP", IPAddress);
	AppendRow(html, "RTP-MIDI", BoolText(pConfig->NetworkRTPMIDI));
	AppendRow(html, "UDP MIDI", BoolText(pConfig->NetworkUDPMIDI));
	AppendRow(html, "FTP", BoolText(pConfig->NetworkFTPServer));
	AppendRow(html, "Web", BoolText(pConfig->NetworkWebServer));
	AppendIntRow(html, "Web port", pConfig->NetworkWebServerPort);
	AppendRow(html, "OSC", BoolText(pConfig->NetworkOSC));
	AppendIntRow(html, "OSC port", pConfig->NetworkOSCPort);
	AppendSectionEnd(html);

	AppendSectionStart(html, "Audio & control");
	AppendRow(html, "Audio output", AudioOutputText(pConfig->AudioOutputDevice));
	AppendIntRow(html, "Sample rate", pConfig->AudioSampleRate);
	AppendIntRow(html, "Chunk size", pConfig->AudioChunkSize);
	AppendRow(html, "Reversed stereo", BoolText(pConfig->AudioReversedStereo));
	AppendRow(html, "Control", ControlSchemeText(pConfig->ControlScheme));
	AppendRow(html, "Encoder", EncoderTypeText(pConfig->ControlEncoderType));
	AppendRow(html, "Encoder reversed", BoolText(pConfig->ControlEncoderReversed));
	AppendRow(html, "MiSTer", BoolText(pConfig->ControlMister));
	AppendSectionEnd(html);

	AppendSectionStart(html, "Display");
	AppendRow(html, "Type", LCDTypeText(pConfig->LCDType));
	AppendIntRow(html, "Width", pConfig->LCDWidth);
	AppendIntRow(html, "Height", pConfig->LCDHeight);
	AppendRow(html, "I2C address", I2CAddress);
	AppendRow(html, "Rotation", RotationText(pConfig->LCDRotation));
	AppendRow(html, "Mirror", MirrorText(pConfig->LCDMirror));
	AppendSectionEnd(html);

	AppendSectionStart(html, "MT-32");
	AppendRow(html, "Available", BoolText(m_pMT32Pi->HasMT32Synth()));
	AppendRow(html, "Current ROM", m_pMT32Pi->GetCurrentMT32ROMName());
	AppendRow(html, "ROM set config", MT32ROMSetText(pConfig->MT32EmuROMSet));
	AppendFloatRow(html, "Gain", pConfig->MT32EmuGain);
	AppendFloatRow(html, "Reverb gain", pConfig->MT32EmuReverbGain);
	AppendRow(html, "Reversed stereo", BoolText(pConfig->MT32EmuReversedStereo));
	AppendSectionEnd(html);

	AppendSectionStart(html, "SoundFont");
	AppendRow(html, "Available", BoolText(m_pMT32Pi->HasSoundFontSynth()));
	AppendRow(html, "Current name", m_pMT32Pi->GetCurrentSoundFontName());
	AppendRow(html, "Current path", m_pMT32Pi->GetCurrentSoundFontPath());
	AppendRow(html, "Index / total", SoundFontIndex);
	AppendIntRow(html, "Polyphony", pConfig->FluidSynthPolyphony);
	AppendFloatRow(html, "Gain", pConfig->FluidSynthDefaultGain);
	AppendRow(html, "Reverb", BoolText(pConfig->FluidSynthDefaultReverbActive));
	AppendRow(html, "Chorus", BoolText(pConfig->FluidSynthDefaultChorusActive));
	AppendSectionEnd(html);

	{
		const CMT32Pi::TSystemState st = m_pMT32Pi->GetSystemState();
		AppendSectionStart(html, "OPL3 / ymfm");
		AppendRow(html, "Available", BoolText(st.bYmfmAvailable));
		AppendRow(html, "Current bank", st.pYmfmBankName ? st.pYmfmBankName : "");
		AppendRow(html, "Chip mode", st.bYmfmChipIsOPL3 ? "OPL3" : "OPL2");
		AppendRow(html, "Configured bank", pConfig->YmfmBankFile);
		AppendRow(html, "Configured chip", pConfig->YmfmChip == CConfig::TYmfmChip::OPL3 ? "OPL3" : "OPL2");
		AppendSectionEnd(html);
	}

	// Mixer status section
	{
		const CMT32Pi::TMixerStatus ms = m_pMT32Pi->GetMixerStatus();
		static const char* PresetNames[] = {"Single MT-32", "Single FluidSynth", "Single OPL3", "Split GM", "Custom"};
		const char* pPresetName = (ms.nPreset >= 0 && ms.nPreset <= 4) ? PresetNames[ms.nPreset] : "Unknown";
		html.Append("<section><h2>Mixer <a href='/mixer' style='font-size:13px;font-weight:normal;margin-left:8px;'>&#8594; Go</a></h2><table>");
		AppendRow(html, "Enabled", BoolText(ms.bEnabled));
		AppendRow(html, "Preset", pPresetName);
		AppendRow(html, "Dual mode", BoolText(ms.bDualMode));
		AppendSectionEnd(html);
	}

	html.Append("<section><h2>Sequencer <a href='/sequencer' style='font-size:13px;font-weight:normal;margin-left:8px;'>&#8594; Go</a></h2>");
	html.Append("<span id='idx-seq-badge' class='badge'>Loading...</span>");
	html.Append("<p id='idx-seq-file' style='margin-top:8px;color:#64748b;word-break:break-all;font-size:12px;margin-bottom:4px;'>&#8212;</p>");
	html.Append("<div class='seq-prog-bg'><div id='idx-prog' class='seq-prog-fill'></div></div>");
	html.Append("<p id='idx-seq-time' style='font-size:12px;margin:2px 0 0;color:#64748b;'>0:00 / 0:00</p>");
	html.Append("<div style='display:flex;gap:6px;flex-wrap:wrap;margin-top:10px;'>");
	html.Append("<button onclick='idxPrev()' title='Previous'>&#9664;&#9664;</button>");
	html.Append("<button id='idx-pp-btn' class='primary' onclick='idxPP()'>&#9654; Play</button>");
	html.Append("<button id='idx-stop-btn' onclick='idxStop()' style='display:none;'>&#9646; Stop</button>");
	html.Append("<button onclick='idxNext()' title='Next'>&#9654;&#9654;</button>");
	html.Append("</div></section>");
	html.Append("<section><h2>Audio profiler</h2><table>");
	html.Append("<tr><th>Total</th><td id='idx-prof-total'>0 us</td></tr>");
	html.Append("<tr><th>Average</th><td id='idx-prof-avg'>0 us</td></tr>");
	html.Append("<tr><th>Deadline</th><td id='idx-prof-deadline'>0 us</td></tr>");
	html.Append("<tr><th>CPU load</th><td id='idx-prof-cpu'>0%</td></tr>");
	html.Append("<tr><th>MT-32</th><td id='idx-prof-mt32'>0 us (0%)</td></tr>");
	html.Append("<tr><th>FluidSynth</th><td id='idx-prof-fluid'>0 us (0%)</td></tr>");
	html.Append("<tr><th>OPL3</th><td id='idx-prof-opl3'>0 us (0%)</td></tr>");
	html.Append("<tr><th>Mixer</th><td id='idx-prof-mixer'>0 us (0%)</td></tr>");
	html.Append("</table></section>");

	// ---- MIDI Recorder section ----
	html.Append("<section><h2>MIDI Recorder</h2>");
	html.Append("<span id='idx-rec-badge' class='badge'>Stopped</span>");
	html.Append("<p style='margin-top:8px;color:#64748b;font-size:12px;margin-bottom:10px;'>Records live MIDI input to SD:recording_NNN.mid (Standard MIDI File Type\u00a00).</p>");
	html.Append("<button id='idx-rec-btn' class='primary' onclick='toggleRecord()'>&#9210; Start Recording</button>");
	html.Append("</section>");

	html.Append("<section><h2>Live MIDI</h2><table><tr><th>API</th><td><code>/api/midi</code></td></tr><tr><th>Status</th><td id='midi-status'>Loading...</td></tr></table><div class='meter-grid' id='midi-grid'></div></section>");
	html.Append("<section><h2>Active keyboard</h2><canvas id='kb-canvas' height='64'></canvas></section>");
	html.Append("<section><h2>Piano roll</h2><canvas id='pr-canvas' height='160'></canvas></section>");

	html.Append("<script>");
	html.Append("const grid=document.getElementById('midi-grid');const st=document.getElementById('midi-status');");
	html.Append("for(let i=1;i<=16;i++){const row=document.createElement('div');row.className='meter';row.innerHTML='<span class=\"meter-label\" id=\"mlbl-'+i+'\">CH'+String(i).padStart(2,'0')+'</span><div class=\"meter-bar\"><div class=\"meter-fill\" id=\"fill-'+i+'\"></div><div class=\"meter-peak\" id=\"peak-'+i+'\"></div></div>';grid.appendChild(row);}");
	html.Append("const _f=[],_p=[],_lt=new Array(16).fill(0),_lv=new Array(16).fill(0),_pt=new Array(16).fill(0),_pk=new Array(16).fill(0),_pa=new Array(16).fill(-9999);");
	html.Append("for(let i=1;i<=16;i++){_f.push(document.getElementById('fill-'+i));_p.push(document.getElementById('peak-'+i));}");
	// Piano roll state: ring buffer of snapshots
	html.Append("const PR_COLS=120,PR_ROWS=128;");
	html.Append("const _prBuf=new Uint8Array(PR_COLS*PR_ROWS);var _prCol=0;");
	html.Append("const _kbCanvas=document.getElementById('kb-canvas');");
	html.Append("const _prCanvas=document.getElementById('pr-canvas');");
	html.Append("const _kbCtx=_kbCanvas?_kbCanvas.getContext('2d'):null;");
	html.Append("const _prCtx=_prCanvas?_prCanvas.getContext('2d'):null;");
	// Source colors: 0=off, 1=physical(green), 2=player(blue), 3=webui(orange)
	html.Append("const SRC_COLORS=['','#4ade80','#60a5fa','#fb923c'];");
	// Engine colors: 0=MT-32(cyan), 1=FluidSynth(magenta), 2=OPL3(amber)
	html.Append("const ENG_COLORS=['#22d3ee','#c084fc','#f59e0b'];");
	html.Append("var _chEng=new Array(16).fill(0);var _isMixer=false;");
	// WHITE_ST: semitone offsets for the 7 white keys in an octave
	// BLACK_ST: semitone offsets + fractional x position (0-7 white-key units) for 5 black keys
	html.Append("const W_ST=[0,2,4,5,7,9,11];");
	html.Append("const B_ST=[1,3,6,8,10];const B_XF=[0.6,1.6,3.6,4.6,5.6];");
	html.Append("function _noteClr(ch,n){if(_isMixer)return ENG_COLORS[_chEng[ch]]||'#22d3ee';return SRC_COLORS[src_arr[ch][n]]||'#60a5fa';}");
	html.Append("function _drawKb(){if(!_kbCtx)return;const W=_kbCanvas.width,H=_kbCanvas.height;const kbLt=document.body.classList.contains('light');");
	html.Append("const octaves=10,ww=W/(octaves*7);_kbCtx.fillStyle=kbLt?'#334155':'#0a1020';_kbCtx.fillRect(0,0,W,H);");
	html.Append("for(let o=0;o<octaves;o++){for(let wi=0;wi<7;wi++){const n=o*12+W_ST[wi];if(n>=128)continue;");
	html.Append("const x=(o*7+wi)*ww;let clr=kbLt?'#f1f5f9':'#cbd5e1';let hitCh=-1;");
	html.Append("for(let ch=0;ch<16;ch++){if(_notes[ch][n]){clr=_noteClr(ch,n);hitCh=ch;break;}}");
	html.Append("_kbCtx.fillStyle=clr;_kbCtx.fillRect(x+0.5,H*0.15,ww-1,H*0.84);");
	html.Append("if(hitCh>=0){_kbCtx.fillStyle='#fff';_kbCtx.font='bold '+Math.max(7,ww*0.6|0)+'px sans-serif';_kbCtx.textAlign='center';");
	html.Append("_kbCtx.fillText(''+(hitCh+1),x+ww/2,H*0.85);}}}");
	html.Append("for(let o=0;o<octaves;o++){for(let bi=0;bi<5;bi++){const n=o*12+B_ST[bi];if(n>=128)continue;");
	html.Append("const x=(o*7+B_XF[bi])*ww;let clr='#1e293b';");
	html.Append("for(let ch=0;ch<16;ch++){if(_notes[ch][n]){clr=_noteClr(ch,n);break;}}");
	html.Append("_kbCtx.fillStyle=clr;_kbCtx.fillRect(x,0,ww*0.55,H*0.58);}}}");
	html.Append("var _notes=Array.from({length:16},()=>new Uint8Array(128));");
	html.Append("var src_arr=Array.from({length:16},()=>new Uint8Array(128));");
	html.Append("function _drawPR(){if(!_prCtx)return;const W=_prCanvas.width,H=_prCanvas.height;const prLt=document.body.classList.contains('light');");
	html.Append("const rowH=H/128;const colW=W/PR_COLS;");
	html.Append("_prCtx.fillStyle=prLt?'#e2e8f0':'#0a1020';_prCtx.fillRect(0,0,W,H);");
	html.Append("for(let col=0;col<PR_COLS;col++){const ci=(_prCol+col)%PR_COLS;");
	html.Append("for(let note=0;note<128;note++){const v=_prBuf[ci*PR_ROWS+note];if(!v)continue;");
	html.Append("_prCtx.fillStyle=_isMixer?ENG_COLORS[v-1]||'#22d3ee':SRC_COLORS[v]||'#60a5fa';");
	html.Append("_prCtx.fillRect(col*colW,H-(note+1)*rowH,colW-0.5,rowH-0.5);}}}");
	html.Append("const ATK=0.3,DCY=0.07,PH=1200,PD=0.97;function _rf(ts){for(let i=0;i<16;i++){const t=_lt[i];_lv[i]=t>_lv[i]?_lv[i]+(t-_lv[i])*ATK:_lv[i]+(t-_lv[i])*DCY;if(_pt[i]>_pk[i]){_pk[i]=_pt[i];_pa[i]=ts;}else if(ts-_pa[i]>PH)_pk[i]*=PD;if(_f[i])_f[i].style.width=(_lv[i]*100).toFixed(1)+'%';if(_p[i])_p[i].style.left=(_pk[i]*100).toFixed(1)+'%';}");
	html.Append("_drawKb();_drawPR();requestAnimationFrame(_rf);}");
	html.Append("function _resizeCanvases(){if(_kbCanvas){_kbCanvas.width=_kbCanvas.offsetWidth||640;_kbCanvas.height=64;}if(_prCanvas){_prCanvas.width=_prCanvas.offsetWidth||640;_prCanvas.height=160;}}");
	html.Append("_resizeCanvases();window.addEventListener('resize',_resizeCanvases);");
	html.Append("requestAnimationFrame(_rf);");
	html.Append("function applyWS(d){");
	html.Append("if(d.channels){var _pn=['Single MT-32','Single Fluid','Single OPL3','Split GM','Custom'];st.textContent='Synth: '+(d.synth||'?')+(d.mixer?' ['+(_pn[d.preset]||'Mixer')+']':'')+' | WS';for(var i=0;i<d.channels.length;i++){var ch=d.channels[i];_lt[ch.ch]=Math.max(0,Math.min(1,ch.lv||0));_pt[ch.ch]=Math.max(0,Math.min(1,ch.pk||0));if(ch.eng!==undefined)_chEng[ch.ch]=ch.eng;}if(d.mixer!==undefined)_isMixer=d.mixer;");
	html.Append("var EN=['M','F','O'];for(var i=0;i<16;i++){var lb=document.getElementById('mlbl-'+(i+1));if(lb){var tag=_isMixer?' '+(EN[_chEng[i]]||'?'):'';lb.textContent='CH'+String(i+1).padStart(2,'0')+tag;lb.style.color=_isMixer?(ENG_COLORS[_chEng[i]]||'#93c5fd'):'#93c5fd';}}}");
	html.Append("if(d.notes){for(var ch=0;ch<16;ch++){_notes[ch].fill(0);if(d.notes[ch])for(var j=0;j<d.notes[ch].length;j++){const n=d.notes[ch][j];_notes[ch][n]=1;src_arr[ch][n]=(d.src&&d.src[ch])||1;}}");
	html.Append("_prCol=(_prCol+1)%PR_COLS;_prBuf.fill(0,_prCol*PR_ROWS,(_prCol+1)*PR_ROWS);");
	html.Append("for(var ch=0;ch<16;ch++){if(d.notes[ch])for(var j=0;j<d.notes[ch].length;j++){const n=d.notes[ch][j];_prBuf[_prCol*PR_ROWS+n]=_isMixer?(_chEng[ch]+1):Math.max(_prBuf[_prCol*PR_ROWS+n],(d.src&&d.src[ch])||1);}}}");
	html.Append("var b=document.getElementById('idx-seq-badge');if(b){var st2=d.loading?'loading':(d.paused?'paused':(d.playing?'playing':(d.finished?'finished':'stopped')));_idxSt=st2;");
	html.Append("b.textContent=st2==='loading'?'Loading\u2026':(st2==='playing'?'Playing':(st2==='paused'?'Paused':(st2==='finished'?'Finished':'Stopped')));");
	html.Append("b.className='badge'+(st2==='playing'?' playing':(st2==='paused'?' paused':(st2==='finished'?' finished':(st2==='loading'?' loading':''))));");
	html.Append("var ppb=document.getElementById('idx-pp-btn'),stb=document.getElementById('idx-stop-btn');");
	html.Append("if(ppb){ppb.textContent=st2==='playing'?'\\u23F8 Pause':(st2==='paused'?'\\u25B6 Resume':'\\u25B6 Play');}");
	html.Append("if(stb)stb.style.display=(st2==='playing'||st2==='paused')?'':'none';}");
	html.Append("var sf=document.getElementById('idx-seq-file');if(sf)sf.textContent=(d.file||'').replace(/^(SD:|USB:)/,'')||'\\u2014';");
	html.Append("var pt=document.getElementById('idx-prof-total'),pa=document.getElementById('idx-prof-avg'),pd=document.getElementById('idx-prof-deadline'),pc=document.getElementById('idx-prof-cpu'),pm=document.getElementById('idx-prof-mt32'),pf=document.getElementById('idx-prof-fluid'),po=document.getElementById('idx-prof-opl3'),px=document.getElementById('idx-prof-mixer');");
	html.Append("if(pt)pt.textContent=(d.render_us||0)+' us';if(pa)pa.textContent=(d.render_avg_us||0)+' us';if(pd)pd.textContent=((d.deadline_us!==undefined?d.deadline_us:0))+' us';if(pc)pc.textContent=((d.cpu_load!==undefined?d.cpu_load:0))+'%';");
	html.Append("if(pm)pm.textContent=(d.mt32_render_us||0)+' us ('+((d.mt32_cpu!==undefined?d.mt32_cpu:0))+'%)';if(pf)pf.textContent=(d.fluid_render_us||0)+' us ('+((d.fluid_cpu!==undefined?d.fluid_cpu:0))+'%)';if(po)po.textContent=(d.ymfm_render_us||0)+' us ('+((d.ymfm_cpu!==undefined?d.ymfm_cpu:0))+'%)';if(px)px.textContent=(d.mixer_render_us||0)+' us ('+((d.mixer_cpu!==undefined?d.mixer_cpu:0))+'%)';");
	// Recorder badge + button
	html.Append("var rb=document.getElementById('idx-rec-btn'),rbg=document.getElementById('idx-rec-badge');");
	html.Append("if(rb){var rec=!!d.recording;rb.textContent=rec?'\\u23F9 Stop Recording':'\\u23FA Start Recording';rb.className=rec?'danger':'primary';}");
	html.Append("if(rbg){rbg.textContent=rec?'\\u25CF Recording':'Stopped';rbg.className='badge'+(rec?' recording':'');}");
	html.Append("var dur=d.duration_ms||1;var elp=d.finished?d.duration_ms:(d.elapsed_ms||0);");
	html.Append("var pp=document.getElementById('idx-prog');if(pp)pp.style.width=Math.min(100,elp/dur*100).toFixed(1)+'%';");
	html.Append("var tt=document.getElementById('idx-seq-time');if(tt)tt.textContent=fmt(elp)+' / '+fmt(d.duration_ms);}");
	html.Append("var _idxSt='stopped';");
	html.Append("function idxPrev(){_qs('/api/sequencer/prev','',function(){});}");
	html.Append("function idxNext(){_qs('/api/sequencer/next','',function(){});}");
	html.Append("function idxStop(){_qs('/api/sequencer/stop','',function(){});}");
	html.Append("function idxPP(){if(_idxSt==='playing')_qs('/api/sequencer/pause','',function(){});else if(_idxSt==='paused')_qs('/api/sequencer/resume','',function(){});else _qs('/api/sequencer/next','',function(){});}");
	html.Append("function toggleRecord(){var btn=document.getElementById('idx-rec-btn');if(btn&&btn.classList.contains('danger'))_qs('/api/recorder/stop','',function(){});else _qs('/api/recorder/start','',function(){});}");
	html.Append("(function(){var ws=null,_rt=0;function conn(){ws=new WebSocket('ws://'+location.hostname+':8765/');ws.onmessage=function(e){try{applyWS(JSON.parse(e.data));}catch(x){}};ws.onclose=function(){_rt=Math.min((_rt||500)*2,8000);setTimeout(conn,_rt);};ws.onerror=function(){ws.close();};}conn();})();");
	html.Append("</script>");

	html.Append("</div></main></body></html>");

	if (!html.bOk)
	{
		LOGERR("Increase web content buffer (html writer overflow)");
		return HTTPInternalServerError;
	}
	*pLength = html.Length();
	*ppContentType = "text/html; charset=utf-8";
	return HTTPOK;
}
