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

	void AppendEscaped(CString& Out, const char* pText)
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
				case '&':
					Out += "&amp;";
					break;
				case '<':
					Out += "&lt;";
					break;
				case '>':
					Out += "&gt;";
					break;
				case '"':
					Out += "&quot;";
					break;
				default:
					Out += *pCurrent;
					break;
			}
		}
	}

	void AppendRow(CString& Out, const char* pLabel, const char* pValue)
	{
		Out += "<tr><th>";
		AppendEscaped(Out, pLabel);
		Out += "</th><td>";
		AppendEscaped(Out, pValue);
		Out += "</td></tr>";
	}

	void AppendIntRow(CString& Out, const char* pLabel, int nValue)
	{
		CString Value;
		Value.Format("%d", nValue);
		AppendRow(Out, pLabel, Value);
	}

	void AppendFloatRow(CString& Out, const char* pLabel, float nValue)
	{
		CString Value;
		Value.Format("%.2f", nValue);
		AppendRow(Out, pLabel, Value);
	}

	void AppendSectionStart(CString& Out, const char* pTitle)
	{
		Out += "<section><h2>";
		AppendEscaped(Out, pTitle);
		Out += "</h2><table>";
	}

	void AppendSectionEnd(CString& Out)
	{
		Out += "</table></section>";
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

	const bool bIsIndexPath = strcmp(pPath, "/") == 0 || strcmp(pPath, "/index.html") == 0;
	const bool bIsConfigPagePath = strcmp(pPath, "/config") == 0;
	const bool bIsSoundPagePath = strcmp(pPath, "/sound") == 0;
	const bool bIsStatusAPIPath = strcmp(pPath, "/api/status") == 0;
	const bool bIsMIDIAPIPath = strcmp(pPath, "/api/midi") == 0;
 	const bool bIsConfigSavePath = strcmp(pPath, "/api/config/save") == 0;
	const bool bIsRuntimeStatusPath = strcmp(pPath, "/api/runtime/status") == 0;
	const bool bIsRuntimeSetPath = strcmp(pPath, "/api/runtime/set") == 0;
	const bool bIsSystemRebootPath = strcmp(pPath, "/api/system/reboot") == 0;
	const bool bIsSeqStatusPath    = strcmp(pPath, "/api/sequencer/status") == 0;
	const bool bIsSeqPlayPath      = strcmp(pPath, "/api/sequencer/play") == 0;
	const bool bIsSeqStopPath      = strcmp(pPath, "/api/sequencer/stop") == 0;
	const bool bIsSeqFilesPath      = strcmp(pPath, "/api/sequencer/files") == 0;
	const bool bIsSeqLoopPath       = strcmp(pPath, "/api/sequencer/loop")  == 0;
	const bool bIsMidiNotePath      = strcmp(pPath, "/api/midi/note")        == 0;
	const bool bIsMidiRawPath       = strcmp(pPath, "/api/midi/raw")         == 0;
	const bool bIsSequencerPagePath = strcmp(pPath, "/sequencer") == 0;
	const bool bIsAppCSSPath        = strcmp(pPath, "/app.css")       == 0;
	const bool bIsAppJSPath         = strcmp(pPath, "/app.js")        == 0;
	const bool bIsWifiReadPath      = strcmp(pPath, "/api/wifi/read") == 0;
	const bool bIsWifiSavePath      = strcmp(pPath, "/api/wifi/save") == 0;
	const bool bIsMixerStatusPath   = strcmp(pPath, "/api/mixer/status") == 0;
	const bool bIsMixerSetPath      = strcmp(pPath, "/api/mixer/set") == 0;
	const bool bIsMixerPresetPath   = strcmp(pPath, "/api/mixer/preset") == 0;
	const bool bIsMixerPagePath     = strcmp(pPath, "/mixer") == 0;

	if (!bIsIndexPath && !bIsConfigPagePath && !bIsSoundPagePath && !bIsStatusAPIPath && !bIsMIDIAPIPath && !bIsConfigSavePath && !bIsRuntimeStatusPath && !bIsRuntimeSetPath && !bIsSystemRebootPath && !bIsSeqStatusPath && !bIsSeqPlayPath && !bIsSeqStopPath && !bIsSeqFilesPath && !bIsSeqLoopPath && !bIsMidiNotePath && !bIsMidiRawPath && !bIsSequencerPagePath && !bIsAppCSSPath && !bIsAppJSPath && !bIsWifiReadPath && !bIsWifiSavePath && !bIsMixerStatusPath && !bIsMixerSetPath && !bIsMixerPresetPath && !bIsMixerPagePath)
		return HTTPNotFound;

	if (!m_pMT32Pi)
		return HTTPInternalServerError;

	// ---- GET /api/sequencer/status ----
	if (bIsSeqStatusPath)
	{
		const CMT32Pi::TSequencerStatus s = m_pMT32Pi->GetSequencerStatus();
		CString JSON;
		JSON += "{";
		AppendJSONPairBool(JSON, "playing", s.bPlaying);
		AppendJSONPairBool(JSON, "loop_enabled", s.bLoopEnabled);
		AppendJSONPairBool(JSON, "finished", s.bFinished);
		AppendJSONPair(JSON, "file", s.pFile ? s.pFile : "");
		AppendJSONPairInt(JSON, "event_count", static_cast<int>(s.nEventCount));
		AppendJSONPairInt(JSON, "duration_ms", static_cast<int>(s.nDurationMs));
		AppendJSONPairInt(JSON, "elapsed_ms", static_cast<int>(s.nElapsedMs), false);
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
		AppendJSONPairFloat(JSON, "mt32_pan", ms.fMT32Pan);
		AppendJSONPairFloat(JSON, "fluid_pan", ms.fFluidPan);

		// Audio render performance
		AppendJSONPairInt(JSON, "render_us", static_cast<int>(ms.nRenderUs));
		AppendJSONPairInt(JSON, "render_avg_us", static_cast<int>(ms.nRenderAvgUs));
		AppendJSONPairInt(JSON, "render_peak_us", static_cast<int>(ms.nRenderPeakUs));
		AppendJSONPairInt(JSON, "deadline_us", static_cast<int>(ms.nDeadlineUs));
		AppendJSONPairInt(JSON, "cpu_load", static_cast<int>(ms.nCpuLoadPercent));

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
		 || !GetFormValue(pFormData, "network_web_port", NetworkWebPort, sizeof(NetworkWebPort)))
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
		 || !CConfig::ParseOption(NetworkWebPort, &ParsedWebPort))
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

		CString JSON;
		JSON += "{";
		AppendJSONPairBool(JSON, "ok", bApplied);
		AppendJSONPair(JSON, "active_synth", m_pMT32Pi->GetActiveSynthName());
		AppendJSONPair(JSON, "mt32_rom_name", m_pMT32Pi->GetCurrentMT32ROMName());
		AppendJSONPair(JSON, "soundfont_name", m_pMT32Pi->GetCurrentSoundFontName());
		AppendJSONPairInt(JSON, "mt32_rom_set", m_pMT32Pi->GetMT32ROMSetIndex());
		AppendJSONPairInt(JSON, "soundfont_index", static_cast<int>(m_pMT32Pi->GetCurrentSoundFontIndex()));
		AppendJSONPairInt(JSON, "soundfont_count", static_cast<int>(m_pMT32Pi->GetSoundFontCount()));
		AppendJSONPairInt(JSON, "master_volume", m_pMT32Pi->GetMasterVolume());
		AppendJSONPairBool(JSON, "sf_available", bHasSoundFontFX);
		AppendJSONPairFloat(JSON, "sf_gain", bHasSoundFontFX ? nSFGain : 0.0f);
		AppendJSONPairBool(JSON, "sf_reverb_active", bHasSoundFontFX ? bReverbActive : false);
		AppendJSONPairFloat(JSON, "sf_reverb_room", bHasSoundFontFX ? nReverbRoom : 0.0f);
		AppendJSONPairFloat(JSON, "sf_reverb_level", bHasSoundFontFX ? nReverbLevel : 0.0f);
		AppendJSONPairFloat(JSON, "sf_reverb_damping", bHasSoundFontFX ? nReverbDamping : 0.0f);
		AppendJSONPairFloat(JSON, "sf_reverb_width", bHasSoundFontFX ? nReverbWidth : 0.0f);
		AppendJSONPairBool(JSON, "sf_chorus_active", bHasSoundFontFX ? bChorusActive : false);
		AppendJSONPairFloat(JSON, "sf_chorus_depth", bHasSoundFontFX ? nChorusDepth : 0.0f);
		AppendJSONPairFloat(JSON, "sf_chorus_level", bHasSoundFontFX ? nChorusLevel : 0.0f);
		AppendJSONPairInt(JSON, "sf_chorus_voices", bHasSoundFontFX ? nChorusVoices : 1);
		AppendJSONPairFloat(JSON, "sf_chorus_speed", bHasSoundFontFX ? nChorusSpeed : 0.0f);
		
		// MT-32 parameters
		AppendJSONPairFloat(JSON, "mt32_reverb_gain", m_pMT32Pi->GetMT32ReverbOutputGain());
		AppendJSONPairBool(JSON, "mt32_reverb_active", m_pMT32Pi->IsMT32ReverbActive());
		AppendJSONPairBool(JSON, "mt32_nice_amp", m_pMT32Pi->IsMT32NiceAmpRamp());
		AppendJSONPairBool(JSON, "mt32_nice_pan", m_pMT32Pi->IsMT32NicePanning());
		AppendJSONPairBool(JSON, "mt32_nice_mix", m_pMT32Pi->IsMT32NicePartialMixing());
		AppendJSONPairInt(JSON, "mt32_dac_mode", m_pMT32Pi->GetMT32DACMode());
		AppendJSONPairInt(JSON, "mt32_midi_delay", m_pMT32Pi->GetMT32MIDIDelayMode());
		AppendJSONPairInt(JSON, "mt32_analog_mode", m_pMT32Pi->GetMT32AnalogMode());
		AppendJSONPairInt(JSON, "mt32_renderer_type", m_pMT32Pi->GetMT32RendererType());
		AppendJSONPairInt(JSON, "mt32_partial_count", m_pMT32Pi->GetMT32PartialCount());
		{
			const CMT32Pi::TMixerStatus mxs = m_pMT32Pi->GetMixerStatus();
			AppendJSONPairBool(JSON, "mixer_enabled", mxs.bEnabled);
			AppendJSONPairInt(JSON, "mixer_preset", mxs.nPreset);
			AppendJSONPairBool(JSON, "mixer_dual_mode", mxs.bDualMode, false);
		}
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

		CString JSON;
		JSON += "{";
		AppendJSONPair(JSON, "active_synth", m_pMT32Pi->GetActiveSynthName());
		AppendJSONPair(JSON, "mt32_rom_name", m_pMT32Pi->GetCurrentMT32ROMName());
		AppendJSONPair(JSON, "soundfont_name", m_pMT32Pi->GetCurrentSoundFontName());
		AppendJSONPairInt(JSON, "mt32_rom_set", m_pMT32Pi->GetMT32ROMSetIndex());
		AppendJSONPairInt(JSON, "soundfont_index", static_cast<int>(m_pMT32Pi->GetCurrentSoundFontIndex()));
		AppendJSONPairInt(JSON, "soundfont_count", static_cast<int>(m_pMT32Pi->GetSoundFontCount()));
		AppendJSONPairInt(JSON, "master_volume", m_pMT32Pi->GetMasterVolume());
		AppendJSONPairBool(JSON, "sf_available", bHasSoundFontFX);
		AppendJSONPairFloat(JSON, "sf_gain", bHasSoundFontFX ? nSFGain : 0.0f);
		AppendJSONPairBool(JSON, "sf_reverb_active", bHasSoundFontFX ? bReverbActive : false);
		AppendJSONPairFloat(JSON, "sf_reverb_room", bHasSoundFontFX ? nReverbRoom : 0.0f);
		AppendJSONPairFloat(JSON, "sf_reverb_level", bHasSoundFontFX ? nReverbLevel : 0.0f);
		AppendJSONPairFloat(JSON, "sf_reverb_damping", bHasSoundFontFX ? nReverbDamping : 0.0f);
		AppendJSONPairFloat(JSON, "sf_reverb_width", bHasSoundFontFX ? nReverbWidth : 0.0f);
		AppendJSONPairBool(JSON, "sf_chorus_active", bHasSoundFontFX ? bChorusActive : false);
		AppendJSONPairFloat(JSON, "sf_chorus_depth", bHasSoundFontFX ? nChorusDepth : 0.0f);
		AppendJSONPairFloat(JSON, "sf_chorus_level", bHasSoundFontFX ? nChorusLevel : 0.0f);
		AppendJSONPairInt(JSON, "sf_chorus_voices", bHasSoundFontFX ? nChorusVoices : 1);
		AppendJSONPairFloat(JSON, "sf_chorus_speed", bHasSoundFontFX ? nChorusSpeed : 0.0f);
		
		// MT-32 parameters
		AppendJSONPairFloat(JSON, "mt32_reverb_gain", m_pMT32Pi->GetMT32ReverbOutputGain());
		AppendJSONPairBool(JSON, "mt32_reverb_active", m_pMT32Pi->IsMT32ReverbActive());
		AppendJSONPairBool(JSON, "mt32_nice_amp", m_pMT32Pi->IsMT32NiceAmpRamp());
		AppendJSONPairBool(JSON, "mt32_nice_pan", m_pMT32Pi->IsMT32NicePanning());
		AppendJSONPairBool(JSON, "mt32_nice_mix", m_pMT32Pi->IsMT32NicePartialMixing());
		AppendJSONPairInt(JSON, "mt32_dac_mode", m_pMT32Pi->GetMT32DACMode());
		AppendJSONPairInt(JSON, "mt32_midi_delay", m_pMT32Pi->GetMT32MIDIDelayMode());
		AppendJSONPairInt(JSON, "mt32_analog_mode", m_pMT32Pi->GetMT32AnalogMode());
		AppendJSONPairInt(JSON, "mt32_renderer_type", m_pMT32Pi->GetMT32RendererType());
		AppendJSONPairInt(JSON, "mt32_partial_count", m_pMT32Pi->GetMT32PartialCount());
		{
			const CMT32Pi::TMixerStatus mxs = m_pMT32Pi->GetMixerStatus();
			AppendJSONPairBool(JSON, "mixer_enabled", mxs.bEnabled);
			AppendJSONPairInt(JSON, "mixer_preset", mxs.nPreset);
			AppendJSONPairBool(JSON, "mixer_dual_mode", mxs.bDualMode, false);
		}
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
		CString IPAddress;
		m_pMT32Pi->FormatIPAddress(IPAddress);

		CString JSON;
		JSON += "{";
		AppendJSONPair(JSON, "active_synth", m_pMT32Pi->GetActiveSynthName());
		AppendJSONPair(JSON, "network_interface", m_pMT32Pi->GetNetworkInterfaceName());
		AppendJSONPairBool(JSON, "network_ready", m_pMT32Pi->IsNetworkReady());
		AppendJSONPair(JSON, "ip", IPAddress);
		AppendJSONPair(JSON, "hostname", pConfig->NetworkHostname);
		AppendJSONPairInt(JSON, "web_port", pConfig->NetworkWebServerPort);
		AppendJSONPair(JSON, "mt32_rom_name", m_pMT32Pi->GetCurrentMT32ROMName());
		AppendJSONPair(JSON, "soundfont_name", m_pMT32Pi->GetCurrentSoundFontName());
		AppendJSONPair(JSON, "soundfont_path", m_pMT32Pi->GetCurrentSoundFontPath());
		AppendJSONPairInt(JSON, "soundfont_index", static_cast<int>(m_pMT32Pi->GetCurrentSoundFontIndex()));
		AppendJSONPairInt(JSON, "soundfont_count", static_cast<int>(m_pMT32Pi->GetSoundFontCount()), false);
		JSON += "}";

		const unsigned nBodyLength = JSON.GetLength();
		if (*pLength < nBodyLength)
			return HTTPInternalServerError;

		memcpy(pBuffer, static_cast<const char*>(JSON), nBodyLength);
		*pLength = nBodyLength;
		*ppContentType = "application/json; charset=utf-8";
		return HTTPOK;
	}

	if (bIsAppCSSPath)
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
			".badge.playing{background:#14532d;border-color:#16a34a;}.badge.finished{background:#44403c;border-color:#a8a29e;}"
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
			"#msg{color:#93c5fd;min-height:20px;}label small{display:block;font-size:.72em;color:#64748b;line-height:1.3;margin-bottom:2px;}";

		const unsigned nLen = static_cast<unsigned>(std::strlen(pCSS));
		if (*pLength < nLen) return HTTPInternalServerError;
		memcpy(pBuffer, pCSS, nLen);
		*pLength = nLen;
		*ppContentType = "text/css; charset=utf-8";
		return HTTPOK;
	}

	if (bIsAppJSPath)
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
			"});})();";

		const unsigned nLen = static_cast<unsigned>(std::strlen(pJS));
		if (*pLength < nLen) return HTTPInternalServerError;
		memcpy(pBuffer, pJS, nLen);
		*pLength = nLen;
		*ppContentType = "application/javascript; charset=utf-8";
		return HTTPOK;
	}

	if (bIsSequencerPagePath)
	{
		CString HTML;
		HTML += "<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width, initial-scale=1'>";
		HTML += "<title>mt32-pi sequencer</title><link rel='stylesheet' href='/app.css'></head><body><main>";
		HTML += "<script src='/app.js'></script>";
		HTML += "<h1>MIDI Sequencer</h1><p>Plays MIDI files from SD card or USB.</p>";
		HTML += "<nav><a href='/'>Status</a><a href='/sound'>Sound</a><a href='/config'>Config</a><a href='/sequencer'>Sequencer</a><a href='/mixer'>Mixer</a></nav>";
		HTML += "<section><h2>File</h2><div class='grid'>";
		HTML += "<label>MIDI file<select id='seq-file'><option value=''>Loading...</option></select></label></div>";
		HTML += "<div style='margin-top:12px;display:flex;gap:8px;flex-wrap:wrap;'>";
		HTML += "<button class='primary' onclick='doPlay()'>&#9654; Play</button>";
		HTML += "<button class='danger' onclick='doStop()'>&#9646;&#9646; Stop</button>";
		HTML += "<button id='seq-loop-btn' onclick='toggleLoop()' title='Loop: OFF'>&#8635; Loop</button>";
		HTML += "<button onclick='loadFiles()'>&#8635; Refresh list</button></div></section>";
		HTML += "<section><h2>Status</h2>";
		HTML += "<span id='seq-badge' class='badge'>Stopped</span>";
		HTML += "<p id='seq-cur-file' style='margin-top:8px;color:#64748b;word-break:break-all;'>&#8212;</p>";
		HTML += "<div class='prog-bg'><div id='prog' class='prog-fill'></div></div>";
		HTML += "<p id='seq-time' style='font-size:13px;'>0:00 / 0:00</p>";
		HTML += "<p id='msg'></p></section>";

		HTML += "<script>";
		// --- Status helpers ---
		HTML += "function applyStatus(d){if(!d)return;var b=document.getElementById('seq-badge');";
		HTML += "var st=d.playing?'playing':(d.finished?'finished':'stopped');";
		HTML += "b.textContent=st==='playing'?'Playing':(st==='finished'?'Finished':'Stopped');"; 
		HTML += "b.className='badge'+(st==='playing'?' playing':(st==='finished'?' finished':''));";
		HTML += "document.getElementById('seq-cur-file').textContent=d.file||'\\u2014';";
		HTML += "var dur=d.duration_ms||1;var elp=st==='finished'?d.duration_ms:(d.elapsed_ms||0);";
		HTML += "document.getElementById('prog').style.width=Math.min(100,elp/dur*100).toFixed(1)+'%';";
		HTML += "document.getElementById('seq-time').textContent=fmt(elp)+' / '+fmt(d.duration_ms);";
		HTML += "var lb=document.getElementById('seq-loop-btn');";
		HTML += "if(lb){lb.className=d.loop_enabled?'loop-on':'';lb.title=d.loop_enabled?'Loop: ON':'Loop: OFF';}}";
		// --- Self-scheduling poll fallback (via HTTP when WS not connected) ---
		HTML += "var _wsOk=false;";
		HTML += "function schedPoll(){if(_wsOk)return;setTimeout(function(){_qs('/api/sequencer/status','',function(d){applyStatus(d);if(!_wsOk)schedPoll();});},1000);}";
		// --- WebSocket for status updates ---
		HTML += "(function(){var ws=null,_rt=0;";
		HTML += "function wsConnect(){ws=new WebSocket('ws://'+location.hostname+':8765/');";
		HTML += "ws.onopen=function(){_wsOk=true;};";
		HTML += "ws.onmessage=function(e){try{applyStatus(JSON.parse(e.data));}catch(x){}};";
		HTML += "ws.onclose=function(){_wsOk=false;schedPoll();_rt=Math.min((_rt||500)*2,8000);setTimeout(wsConnect,_rt);};";
		HTML += "ws.onerror=function(){ws.close();};}";
		HTML += "wsConnect();})();";
		// --- File list ---
		HTML += "function loadFiles(){var sel=document.getElementById('seq-file');sel.innerHTML='<option value=\"\">Loading...</option>';";
		HTML += "_qs('/api/sequencer/files','',function(files){sel.innerHTML='';if(!files||!files.length){sel.innerHTML='<option value=\"\">No MIDI files</option>';return;}";
		HTML += "for(var i=0;i<files.length;i++){var o=document.createElement('option');o.value=files[i];o.textContent=files[i].replace(/^(SD:|USB:)/,'');sel.appendChild(o);}});}";
		// --- Controls ---
		HTML += "function doPlay(){var f=document.getElementById('seq-file').value;";
		HTML += "if(!f){document.getElementById('msg').textContent='Select a file first.';return;}";
		HTML += "document.getElementById('msg').textContent='Starting...';";		HTML += "_qs('/api/sequencer/play','file='+encodeURIComponent(f),function(j){document.getElementById('msg').textContent=j&&j.ok?'OK.':'Error.';});}";
		HTML += "function doStop(){document.getElementById('msg').textContent='Stopping...';"; 
		HTML += "_qs('/api/sequencer/stop','',function(){document.getElementById('msg').textContent='Stopped.';});}";
		HTML += "function toggleLoop(){var lb=document.getElementById('seq-loop-btn');var lc=lb&&lb.className==='loop-on';";
		HTML += "_qs('/api/sequencer/loop','enabled='+(lc?'off':'on'),function(){_qs('/api/sequencer/status','',function(d){applyStatus(d);});});}";
		HTML += "loadFiles();schedPoll();";
		HTML += "</script></main></body></html>";

		const unsigned nBodyLength = HTML.GetLength();
		if (*pLength < nBodyLength)
			return HTTPInternalServerError;

		memcpy(pBuffer, static_cast<const char*>(HTML), nBodyLength);
		*pLength = nBodyLength;
		*ppContentType = "text/html; charset=utf-8";
		return HTTPOK;
	}

	if (bIsMixerPagePath)
	{
		CString HTML;
		HTML += "<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width, initial-scale=1'>";
		HTML += "<title>mt32-pi mixer</title><link rel='stylesheet' href='/app.css'></head><body><main>";
		HTML += "<script src='/app.js'></script>";
		HTML += "<h1>MIDI Mixer / Router</h1>";
		HTML += "<p>Route MIDI channels to engines and remap channel numbers.</p>";
		HTML += "<nav><a href='/'>Status</a><a href='/sound'>Sound</a><a href='/config'>Config</a><a href='/sequencer'>Sequencer</a><a href='/mixer'>Mixer</a></nav>";

		// Preset + Enable section
		HTML += "<section><h2>Mode</h2><div class='grid'>";
		HTML += "<label>Preset<select id='mx-preset' onchange='setPreset(this.value)'>";
		HTML += "<option value='0'>Single MT-32</option><option value='1'>Single FluidSynth</option>";
		HTML += "<option value='2'>Split GM</option><option value='3'>Custom</option></select></label>";
		HTML += "<label>Dual mode<select id='mx-enabled' onchange='setEnabled(this.value)'>";
		HTML += "<option value='1'>Enabled</option><option value='0'>Disabled</option></select></label>";
		HTML += "</div></section>";

		// Volume / Pan
		HTML += "<section><h2>Engine Levels</h2><div class='grid'>";
		HTML += "<label>MT-32 vol<input id='mx-mt32v' type='range' min='0' max='100' oninput='setParam(\"mt32_volume\",this.value)'></label>";
		HTML += "<label>FluidSynth vol<input id='mx-fluidv' type='range' min='0' max='100' oninput='setParam(\"fluid_volume\",this.value)'></label>";
		HTML += "<label>MT-32 pan<input id='mx-mt32p' type='range' min='-100' max='100' oninput='setParam(\"mt32_pan\",this.value)'></label>";
		HTML += "<label>FluidSynth pan<input id='mx-fluidp' type='range' min='-100' max='100' oninput='setParam(\"fluid_pan\",this.value)'></label>";
		HTML += "</div></section>";

		// Audio render performance
		HTML += "<section><h2>Audio Performance</h2><div class='grid'>";
		HTML += "<label>Render <span id='mx-render'>-</span> &micro;s</label>";
		HTML += "<label>Average <span id='mx-avg'>-</span> &micro;s</label>";
		HTML += "<label>Peak <span id='mx-peak'>-</span> &micro;s</label>";
		HTML += "<label>Deadline <span id='mx-deadline'>-</span> &micro;s</label>";
		HTML += "<label>CPU load <strong id='mx-cpu'>-</strong>%</label>";
		HTML += "</div></section>";

		// Channel routing table
		HTML += "<section><h2>Channel Routing</h2>";
		HTML += "<table style='width:100%;border-collapse:collapse;'><thead><tr>";
		HTML += "<th style='text-align:left;padding:4px;border-bottom:2px solid #334155;color:#93c5fd;'>CH</th>";
		HTML += "<th style='text-align:left;padding:4px;border-bottom:2px solid #334155;color:#93c5fd;'>Engine</th>";
		HTML += "<th style='text-align:left;padding:4px;border-bottom:2px solid #334155;color:#93c5fd;'>Remap&rarr;</th>";
		HTML += "<th style='text-align:left;padding:4px;border-bottom:2px solid #334155;color:#93c5fd;'>Layer</th>";
		HTML += "<th style='text-align:left;padding:4px;border-bottom:2px solid #334155;color:#93c5fd;'>Instrument</th>";
		HTML += "<th style='text-align:left;padding:4px;border-bottom:2px solid #334155;color:#93c5fd;min-width:80px;'>Activity</th>";
		HTML += "</tr></thead><tbody id='mx-ch'></tbody></table>";
		HTML += "<div style='margin-top:8px;'><button onclick='setAllLayer(true)'>Layer All</button> ";
		HTML += "<button onclick='setAllLayer(false)'>Unlayer All</button> ";
		HTML += "<button onclick='resetCCFilters()'>Reset CC Filters</button></div></section>";

		HTML += "<p id='mx-msg' style='color:#64748b;'></p>";

		// JavaScript
		HTML += "<script>";
		// helpers
		HTML += "function msg(t){document.getElementById('mx-msg').textContent=t;setTimeout(function(){document.getElementById('mx-msg').textContent='';},2000);}";
		HTML += "function setParam(p,v){_qs('/api/mixer/set','param='+p+'&value='+encodeURIComponent(v),function(r){if(!r||!r.ok)msg('Error');});}";
		HTML += "function setEnabled(v){setParam('enabled',v==='1'?'on':'off');}";
		HTML += "function setPreset(v){_qs('/api/mixer/preset','preset='+v,function(r){if(r&&r.ok)loadStatus();else msg('Error');});}";

		// channel engine/remap
		HTML += "function setChEngine(ch,eng){_qs('/api/mixer/set','param=channel_engine&value='+ch+','+eng,function(r){if(r&&r.ok)loadStatus();else msg('Error');});}";
		HTML += "function setChRemap(ch,dst){var n=parseInt(dst,10);if(n<1||n>16)return;";
		HTML += "_qs('/api/mixer/set','param=channel_remap&value='+ch+','+n,function(r){if(r&&r.ok)loadStatus();else msg('Error');});}";

		// layer
		HTML += "function setChLayer(ch,on){_qs('/api/mixer/set','param=channel_layer&value='+ch+','+(on?'on':'off'),function(r){if(r&&r.ok)loadStatus();else msg('Error');});}";
		HTML += "function setAllLayer(on){_qs('/api/mixer/set','param=all_layer&value='+(on?'on':'off'),function(r){if(r&&r.ok)loadStatus();else msg('Error');});}";
		HTML += "function resetCCFilters(){_qs('/api/mixer/set','param=cc_filter_reset&value=1',function(r){if(r&&r.ok)msg('CC filters reset');else msg('Error');});}";

		// render
		HTML += "function renderChannels(chs){var tb=document.getElementById('mx-ch');tb.innerHTML='';";
		HTML += "for(var i=0;i<chs.length;i++){var c=chs[i];var tr=document.createElement('tr');";
		HTML += "var bg=i%2===0?'#111827':'#0b1220';tr.style.background=bg;";

		// CH number cell
		HTML += "var td1=document.createElement('td');td1.style.padding='6px 4px';td1.style.borderBottom='1px solid #1e293b';";
		HTML += "td1.textContent=c.ch;tr.appendChild(td1);";

		// Engine select cell
		HTML += "var td2=document.createElement('td');td2.style.padding='6px 4px';td2.style.borderBottom='1px solid #1e293b';";
		HTML += "var sel=document.createElement('select');sel.dataset.ch=c.ch;";
		HTML += "var o1=document.createElement('option');o1.value='mt32';o1.textContent='MT-32';if(c.engine==='MT-32')o1.selected=true;sel.appendChild(o1);";
		HTML += "var o2=document.createElement('option');o2.value='fluidsynth';o2.textContent='FluidSynth';if(c.engine==='FluidSynth')o2.selected=true;sel.appendChild(o2);";
		HTML += "sel.onchange=function(){setChEngine(this.dataset.ch,this.value);};";
		HTML += "td2.appendChild(sel);tr.appendChild(td2);";

		// Remap input cell
		HTML += "var td3=document.createElement('td');td3.style.padding='6px 4px';td3.style.borderBottom='1px solid #1e293b';";
		HTML += "var inp=document.createElement('input');inp.type='number';inp.min=1;inp.max=16;inp.value=c.remap;";
		HTML += "inp.style.width='60px';inp.dataset.ch=c.ch;";
		HTML += "inp.onchange=function(){setChRemap(this.dataset.ch,this.value);};";
		HTML += "td3.appendChild(inp);tr.appendChild(td3);";

		// Layer checkbox cell
		HTML += "var td4=document.createElement('td');td4.style.padding='6px 4px';td4.style.borderBottom='1px solid #1e293b';";
		HTML += "var cb=document.createElement('input');cb.type='checkbox';cb.checked=!!c.layered;cb.dataset.ch=c.ch;";
		HTML += "cb.onchange=function(){setChLayer(this.dataset.ch,this.checked);};";
		HTML += "td4.appendChild(cb);tr.appendChild(td4);";

		// Instrument name cell
		HTML += "var td5i=document.createElement('td');td5i.style.padding='6px 4px';td5i.style.borderBottom='1px solid #1e293b';";
		HTML += "td5i.style.fontSize='11px';td5i.style.color='#94a3b8';td5i.style.maxWidth='120px';td5i.style.overflow='hidden';td5i.style.textOverflow='ellipsis';td5i.style.whiteSpace='nowrap';";
		HTML += "td5i.textContent=c.instrument||'\\u2014';tr.appendChild(td5i);";

		// Activity meter cell
		HTML += "var td5=document.createElement('td');td5.style.padding='6px 4px';td5.style.borderBottom='1px solid #1e293b';";
		HTML += "td5.innerHTML='<div class=\"meter-bar\" style=\"height:8px;\"><div class=\"meter-fill\" id=\"mxf-'+c.ch+'\"></div><div class=\"meter-peak\" id=\"mxp-'+c.ch+'\"></div></div>';";
		HTML += "tr.appendChild(td5);";

		HTML += "tb.appendChild(tr);}}";

		// load status
		HTML += "function loadStatus(){_qs('/api/mixer/status','',function(d){if(!d)return;";
		HTML += "document.getElementById('mx-preset').value=d.preset;";
		HTML += "document.getElementById('mx-enabled').value=d.enabled?'1':'0';";
		HTML += "document.getElementById('mx-mt32v').value=Math.round(d.mt32_volume*100);";
		HTML += "document.getElementById('mx-fluidv').value=Math.round(d.fluid_volume*100);";
		HTML += "document.getElementById('mx-mt32p').value=Math.round(d.mt32_pan*100);";
		HTML += "document.getElementById('mx-fluidp').value=Math.round(d.fluid_pan*100);";
		HTML += "document.getElementById('mx-render').textContent=d.render_us;";
		HTML += "document.getElementById('mx-avg').textContent=d.render_avg_us;";
		HTML += "document.getElementById('mx-peak').textContent=d.render_peak_us;";
		HTML += "document.getElementById('mx-deadline').textContent=d.deadline_us;";
		HTML += "document.getElementById('mx-cpu').textContent=d.cpu_load;";
		HTML += "renderChannels(d.channels);});}";

		HTML += "loadStatus();setInterval(loadStatus,2000);";

		// WebSocket for real-time channel meters
		HTML += "var _mxLv=new Array(16).fill(0),_mxPk=new Array(16).fill(0),_mxPa=new Array(16).fill(-9999);";
		HTML += "var _mxTgt=new Array(16).fill(0),_mxPt=new Array(16).fill(0);";
		HTML += "function _mxRf(ts){for(var i=0;i<16;i++){var t=_mxTgt[i];_mxLv[i]=t>_mxLv[i]?_mxLv[i]+(t-_mxLv[i])*0.3:_mxLv[i]+(t-_mxLv[i])*0.07;";
		HTML += "if(_mxPt[i]>_mxPk[i]){_mxPk[i]=_mxPt[i];_mxPa[i]=ts;}else if(ts-_mxPa[i]>1200)_mxPk[i]*=0.97;";
		HTML += "var f=document.getElementById('mxf-'+i);if(f)f.style.width=(_mxLv[i]*100).toFixed(1)+'%';";
		HTML += "var p=document.getElementById('mxp-'+i);if(p)p.style.left=(_mxPk[i]*100).toFixed(1)+'%';}";
		HTML += "requestAnimationFrame(_mxRf);}requestAnimationFrame(_mxRf);";
		HTML += "(function(){var ws=null,_rt=0;function conn(){ws=new WebSocket('ws://'+location.hostname+':8765/');";
		HTML += "ws.onmessage=function(e){try{var d=JSON.parse(e.data);if(d.channels)for(var i=0;i<d.channels.length;i++){var ch=d.channels[i];_mxTgt[ch.ch]=Math.max(0,Math.min(1,ch.lv||0));_mxPt[ch.ch]=Math.max(0,Math.min(1,ch.pk||0));}}catch(x){}};";
		HTML += "ws.onclose=function(){_rt=Math.min((_rt||500)*2,8000);setTimeout(conn,_rt);};ws.onerror=function(){ws.close();};}conn();})();";

		HTML += "</script></main></body></html>";

		const unsigned nMixerLen = HTML.GetLength();
		if (*pLength < nMixerLen)
			return HTTPInternalServerError;

		memcpy(pBuffer, static_cast<const char*>(HTML), nMixerLen);
		*pLength = nMixerLen;
		*ppContentType = "text/html; charset=utf-8";
		return HTTPOK;
	}

	if (bIsSoundPagePath)
	{
		CString HTML;
		HTML += "<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width, initial-scale=1'>";
		HTML += "<title>mt32-pi sound</title><link rel='stylesheet' href='/app.css'></head><body><main>";
		HTML += "<script src='/app.js'></script>";

		const bool bMT32Active = std::strcmp(m_pMT32Pi->GetActiveSynthName(), "MT-32") == 0;
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

		HTML += "<h1>Sound control</h1><p>Live adjustments for synthesis engines and effects, no restart needed.</p>";
		HTML += "<nav><a href='/'>Status</a><a href='/sound'>Sound</a><a href='/config'>Config</a><a href='/sequencer'>Sequencer</a><a href='/mixer'>Mixer</a></nav>";
		HTML += "<div class='statusbar'><div class='pill'>Active synth: <strong id='rt_active_synth_label'>";
		AppendEscaped(HTML, m_pMT32Pi->GetActiveSynthName());
		HTML += "</strong></div><div class='pill'>MT-32 ROM: <strong id='rt_mt32_rom_name'>";
		AppendEscaped(HTML, m_pMT32Pi->GetCurrentMT32ROMName());
		HTML += "</strong></div><div class='pill'>SoundFont: <strong id='rt_soundfont_name'>";
		AppendEscaped(HTML, m_pMT32Pi->GetCurrentSoundFontName());
		HTML += "</strong></div><div class='pill'>Mixer: <strong id='rt_mixer_label'>-</strong></div></div>";
		HTML += "<div id='mx-banner' style='display:none;background:#1e3a5f;border:1px solid #22d3ee;border-radius:8px;padding:8px 12px;margin:8px 0;color:#93c5fd;font-size:13px;'>&#9432; Mixer dual mode: both engines processing audio. Changing active synth switches to single-engine mode.</div>";
		HTML += "<div class='tabbar'><button class='tabbtn";
		HTML += bMT32Active ? " active" : "";
		HTML += "' type='button' id='tab-mt32'>MT-32</button><button class='tabbtn";
		HTML += !bMT32Active ? " active" : "";
		HTML += "' type='button' id='tab-sf'>SoundFont</button></div>";
		HTML += "<section><h2>Engine &amp; bank</h2><div class='grid'>";
		HTML += "<label>Active synth<select id='rt_active_synth'><option value='mt32'";
		HTML += SelectedAttr(bMT32Active);
		HTML += ">MT-32</option><option value='soundfont'";
		HTML += SelectedAttr(!bMT32Active);
		HTML += ">SoundFont</option></select></label>";
		HTML += "<label>Master volume <input id='rt_master_volume' type='range' min='0' max='100' step='1' value='";
		AppendEscaped(HTML, MasterVolume);
		HTML += "'><span id='rt_master_volume_val'>";
		AppendEscaped(HTML, MasterVolume);
		HTML += "</span></label>";
		HTML += "</div></section>";

		CString MT32ReverbGain; MT32ReverbGain.Format("%.2f", fMT32ReverbGain);
		CString MT32PartialCount; MT32PartialCount.Format("%d", nMT32PartialCount);
		
		HTML += "<section id='mt32-section'><h2>MT-32</h2><div class='grid'>";
		HTML += "<span class='subgroup-title'>ROM &amp; bank</span>";
		HTML += "<label>ROM set MT-32<select id='rt_mt32_rom_set'><option value='mt32_old'";
		HTML += SelectedAttr(nROMSetIndex == static_cast<int>(TMT32ROMSet::MT32Old));
		HTML += ">MT-32 old</option><option value='mt32_new'";
		HTML += SelectedAttr(nROMSetIndex == static_cast<int>(TMT32ROMSet::MT32New));
		HTML += ">MT-32 new</option><option value='cm32l'";
		HTML += SelectedAttr(nROMSetIndex == static_cast<int>(TMT32ROMSet::CM32L));
		HTML += ">CM-32L</option></select></label>";
		HTML += "<label>Current ROM<input value='";
		AppendEscaped(HTML, m_pMT32Pi->GetCurrentMT32ROMName());
		HTML += "' disabled></label>";
		HTML += "<span class='subgroup-title'>Reverb</span>";
		HTML += "<label>Reverb<select id='rt_mt32_reverb_active'><option value='off'";
		HTML += SelectedAttr(!bMT32ReverbActive);
		HTML += ">off</option><option value='on'";
		HTML += SelectedAttr(bMT32ReverbActive);
		HTML += ">on</option></select></label>";
		HTML += "<label>Reverb gain <input id='rt_mt32_reverb_gain' type='range' min='0' max='4' step='0.2' value='";
		AppendEscaped(HTML, MT32ReverbGain);
		HTML += "'><span id='rt_mt32_reverb_gain_val'>";
		AppendEscaped(HTML, MT32ReverbGain);
		HTML += "</span></label>";
		HTML += "<span class='subgroup-title'>Emulation enhancements</span>";
		HTML += "<label>Nice Amp<select id='rt_mt32_nice_amp'><option value='off'";
		HTML += SelectedAttr(!bMT32NiceAmp);
		HTML += ">off</option><option value='on'";
		HTML += SelectedAttr(bMT32NiceAmp);
		HTML += ">on</option></select></label>";
		HTML += "<label>Nice Pan<select id='rt_mt32_nice_pan'><option value='off'";
		HTML += SelectedAttr(!bMT32NicePan);
		HTML += ">off</option><option value='on'";
		HTML += SelectedAttr(bMT32NicePan);
		HTML += ">on</option></select></label>";
		HTML += "<label>Nice Mix<select id='rt_mt32_nice_mix'><option value='off'";
		HTML += SelectedAttr(!bMT32NiceMix);
		HTML += ">off</option><option value='on'";
		HTML += SelectedAttr(bMT32NiceMix);
		HTML += ">on</option></select></label>";
		HTML += "<span class='subgroup-title'>Advanced emulation</span>";
		HTML += "<label>DAC<select id='rt_mt32_dac_mode'><option value='0'";
		HTML += SelectedAttr(nMT32DACMode == 0);
		HTML += ">NICE</option><option value='1'";
		HTML += SelectedAttr(nMT32DACMode == 1);
		HTML += ">PURE</option><option value='2'";
		HTML += SelectedAttr(nMT32DACMode == 2);
		HTML += ">GEN1</option><option value='3'";
		HTML += SelectedAttr(nMT32DACMode == 3);
		HTML += ">GEN2</option></select></label>";
		HTML += "<label>MIDI Delay<select id='rt_mt32_midi_delay'><option value='0'";
		HTML += SelectedAttr(nMT32MIDIDelay == 0);
		HTML += ">IMMD</option><option value='1'";
		HTML += SelectedAttr(nMT32MIDIDelay == 1);
		HTML += ">SHORT</option><option value='2'";
		HTML += SelectedAttr(nMT32MIDIDelay == 2);
		HTML += ">ALL</option></select></label>";
		HTML += "<label>Analog<select id='rt_mt32_analog_mode'><option value='0'";
		HTML += SelectedAttr(nMT32AnalogMode == 0);
		HTML += ">DIG</option><option value='1'";
		HTML += SelectedAttr(nMT32AnalogMode == 1);
		HTML += ">COARSE</option><option value='2'";
		HTML += SelectedAttr(nMT32AnalogMode == 2);
		HTML += ">ACCUR</option><option value='3'";
		HTML += SelectedAttr(nMT32AnalogMode == 3);
		HTML += ">OVR</option></select></label>";
		HTML += "<label>Renderer<select id='rt_mt32_renderer_type'><option value='0'";
		HTML += SelectedAttr(nMT32RendererType == 0);
		HTML += ">I16</option><option value='1'";
		HTML += SelectedAttr(nMT32RendererType == 1);
		HTML += ">F32</option></select></label>";
		HTML += "<label>Partials <input id='rt_mt32_partial_count' type='number' min='8' max='256' value='";
		AppendEscaped(HTML, MT32PartialCount);
		HTML += "'></label>";
		HTML += "</div></section>";

		HTML += "<section id='sf-section'><h2>SoundFont</h2><div class='grid'>";
		HTML += "<label>SoundFont<select id='rt_soundfont_index'>";
		for (size_t i = 0; i < nSoundFontCount; ++i)
		{
			CString Index; Index.Format("%d", static_cast<int>(i));
			HTML += "<option value='";
			HTML += Index;
			HTML += "'";
			HTML += SelectedAttr(i == nCurrentSoundFontIndex);
			HTML += ">";
			const char* pSoundFontName = m_pMT32Pi->GetSoundFontName(i);
			AppendEscaped(HTML, pSoundFontName ? pSoundFontName : "(unnamed)");
			HTML += "</option>";
		}
		if (nSoundFontCount == 0)
			HTML += "<option value='0'>No SoundFonts</option>";
		HTML += "</select></label>";
		HTML += "<label>Gain <input id='rt_sf_gain' type='range' min='0' max='5' step='0.05' value='";
		AppendEscaped(HTML, SFGain);
		HTML += "'><span id='rt_sf_gain_val'>";
		AppendEscaped(HTML, SFGain);
		HTML += "</span></label>";
		HTML += "<span class='subgroup-title'>Reverb</span>";
		HTML += "<label>Reverb<select id='rt_sf_reverb_active'><option value='off'";
		HTML += SelectedAttr(!bReverbActive);
		HTML += ">off</option><option value='on'";
		HTML += SelectedAttr(bReverbActive);
		HTML += ">on</option></select></label>";
		HTML += "<label>Reverb room <input id='rt_sf_reverb_room' type='range' min='0' max='1' step='0.1' value='";
		AppendEscaped(HTML, ReverbRoom);
		HTML += "'><span id='rt_sf_reverb_room_val'>";
		AppendEscaped(HTML, ReverbRoom);
		HTML += "</span></label>";
		HTML += "<label>Reverb level <input id='rt_sf_reverb_level' type='range' min='0' max='1' step='0.1' value='";
		AppendEscaped(HTML, ReverbLevel);
		HTML += "'><span id='rt_sf_reverb_level_val'>";
		AppendEscaped(HTML, ReverbLevel);
		HTML += "</span></label>";
		HTML += "<label>Reverb damping <input id='rt_sf_reverb_damping' type='range' min='0' max='1' step='0.1' value='";
		AppendEscaped(HTML, ReverbDamping);
		HTML += "'><span id='rt_sf_reverb_damping_val'>";
		AppendEscaped(HTML, ReverbDamping);
		HTML += "</span></label>";
		HTML += "<label>Reverb width <input id='rt_sf_reverb_width' type='range' min='0' max='100' step='1' value='";
		AppendEscaped(HTML, ReverbWidth);
		HTML += "'><span id='rt_sf_reverb_width_val'>";
		AppendEscaped(HTML, ReverbWidth);
		HTML += "</span></label>";
		HTML += "<span class='subgroup-title'>Chorus</span>";
		HTML += "<label>Chorus<select id='rt_sf_chorus_active'><option value='off'";
		HTML += SelectedAttr(!bChorusActive);
		HTML += ">off</option><option value='on'";
		HTML += SelectedAttr(bChorusActive);
		HTML += ">on</option></select></label>";
		HTML += "<label>Chorus depth <input id='rt_sf_chorus_depth' type='range' min='0' max='20' step='1' value='";
		AppendEscaped(HTML, ChorusDepth);
		HTML += "'><span id='rt_sf_chorus_depth_val'>";
		AppendEscaped(HTML, ChorusDepth);
		HTML += "</span></label>";
		HTML += "<label>Chorus level <input id='rt_sf_chorus_level' type='range' min='0' max='1' step='0.01' value='";
		AppendEscaped(HTML, ChorusLevel);
		HTML += "'><span id='rt_sf_chorus_level_val'>";
		AppendEscaped(HTML, ChorusLevel);
		HTML += "</span></label>";
		HTML += "<label>Chorus voices <input id='rt_sf_chorus_voices' type='number' min='1' max='99' value='";
		AppendEscaped(HTML, ChorusVoices);
		HTML += "'></label>";
		HTML += "<label>Chorus speed (Hz) <input id='rt_sf_chorus_speed' type='range' min='0.29' max='5' step='0.01' value='";
		AppendEscaped(HTML, ChorusSpeed);
		HTML += "'><span id='rt_sf_chorus_speed_val'>";
		AppendEscaped(HTML, ChorusSpeed);
		HTML += "</span></label>";
		HTML += "</div><div id='rtStatus' style='margin-top:10px;color:#86efac;'></div></section>";
		HTML += "<script>const rs=document.getElementById('rtStatus');"
			"const setText=(id,v)=>{const e=document.getElementById(id);if(e)e.textContent=v;};"
			"const setDisabled=(id,b)=>{const e=document.getElementById(id);if(e)e.disabled=!!b;};"
			"const setSectionHidden=(id,b)=>{const e=document.getElementById(id);if(e)e.classList.toggle('section-hidden',!!b);};"
			"const setTabActive=(id,b)=>{const e=document.getElementById(id);if(e)e.classList.toggle('active',!!b);};";
		HTML += "const applyRuntimeState=(j)=>{const mt32=j.active_synth==='MT-32';const sf=j.active_synth==='SoundFont';setText('rt_active_synth_label',j.active_synth||'-');setText('rt_mt32_rom_name',j.mt32_rom_name||'-');setText('rt_soundfont_name',j.soundfont_name||'-');setText('rt_master_volume_val',j.master_volume);setText('rt_sf_gain_val',Number(j.sf_gain).toFixed(2));setText('rt_sf_reverb_room_val',Number(j.sf_reverb_room).toFixed(1));setText('rt_sf_reverb_level_val',Number(j.sf_reverb_level).toFixed(1));setText('rt_sf_reverb_damping_val',Number(j.sf_reverb_damping).toFixed(1));setText('rt_sf_reverb_width_val',Math.round(Number(j.sf_reverb_width)));setText('rt_sf_chorus_depth_val',Math.round(Number(j.sf_chorus_depth)));setText('rt_sf_chorus_level_val',Number(j.sf_chorus_level).toFixed(2));setText('rt_sf_chorus_speed_val',Number(j.sf_chorus_speed).toFixed(2));setText('rt_mt32_reverb_gain_val',Number(j.mt32_reverb_gain).toFixed(2));const synth=document.getElementById('rt_active_synth');if(synth)synth.value=mt32?'mt32':'soundfont';const rom=document.getElementById('rt_mt32_rom_set');if(rom&&j.mt32_rom_set>=0)rom.value=j.mt32_rom_set===0?'mt32_old':(j.mt32_rom_set===1?'mt32_new':'cm32l');const sfSel=document.getElementById('rt_soundfont_index');if(sfSel&&j.soundfont_index>=0)sfSel.value=String(j.soundfont_index);const vol=document.getElementById('rt_master_volume');if(vol)vol.value=j.master_volume;const sfg=document.getElementById('rt_sf_gain');if(sfg)sfg.value=Number(j.sf_gain).toFixed(2);const rta=document.getElementById('rt_sf_reverb_active');if(rta)rta.value=j.sf_reverb_active?'on':'off';const rtr=document.getElementById('rt_sf_reverb_room');if(rtr)rtr.value=Number(j.sf_reverb_room).toFixed(1);const rtl=document.getElementById('rt_sf_reverb_level');if(rtl)rtl.value=Number(j.sf_reverb_level).toFixed(1);const rtd=document.getElementById('rt_sf_reverb_damping');if(rtd)rtd.value=Number(j.sf_reverb_damping).toFixed(1);const rtw=document.getElementById('rt_sf_reverb_width');if(rtw)rtw.value=Math.round(Number(j.sf_reverb_width));const cta=document.getElementById('rt_sf_chorus_active');if(cta)cta.value=j.sf_chorus_active?'on':'off';const ctd=document.getElementById('rt_sf_chorus_depth');if(ctd)ctd.value=Math.round(Number(j.sf_chorus_depth));const ctl=document.getElementById('rt_sf_chorus_level');if(ctl)ctl.value=Number(j.sf_chorus_level).toFixed(2);const ctv=document.getElementById('rt_sf_chorus_voices');if(ctv)ctv.value=j.sf_chorus_voices;const cts=document.getElementById('rt_sf_chorus_speed');if(cts)cts.value=Number(j.sf_chorus_speed).toFixed(2);const m32rg=document.getElementById('rt_mt32_reverb_gain');if(m32rg)m32rg.value=Number(j.mt32_reverb_gain).toFixed(2);const m32ra=document.getElementById('rt_mt32_reverb_active');if(m32ra)m32ra.value=j.mt32_reverb_active?'on':'off';const m32na=document.getElementById('rt_mt32_nice_amp');if(m32na)m32na.value=j.mt32_nice_amp?'on':'off';const m32np=document.getElementById('rt_mt32_nice_pan');if(m32np)m32np.value=j.mt32_nice_pan?'on':'off';const m32nm=document.getElementById('rt_mt32_nice_mix');if(m32nm)m32nm.value=j.mt32_nice_mix?'on':'off';const m32dc=document.getElementById('rt_mt32_dac_mode');if(m32dc)m32dc.value=j.mt32_dac_mode;const m32md=document.getElementById('rt_mt32_midi_delay');if(m32md)m32md.value=j.mt32_midi_delay;const m32an=document.getElementById('rt_mt32_analog_mode');if(m32an)m32an.value=j.mt32_analog_mode;const m32rd=document.getElementById('rt_mt32_renderer_type');if(m32rd)m32rd.value=j.mt32_renderer_type;const m32pc=document.getElementById('rt_mt32_partial_count');if(m32pc)m32pc.value=j.mt32_partial_count;setDisabled('rt_mt32_rom_set',!mt32);setDisabled('rt_mt32_reverb_gain',!mt32);setDisabled('rt_mt32_reverb_active',!mt32);setDisabled('rt_mt32_nice_amp',!mt32);setDisabled('rt_mt32_nice_pan',!mt32);setDisabled('rt_mt32_nice_mix',!mt32);setDisabled('rt_mt32_dac_mode',!mt32);setDisabled('rt_mt32_midi_delay',!mt32);setDisabled('rt_mt32_analog_mode',!mt32);setDisabled('rt_mt32_renderer_type',!mt32);setDisabled('rt_mt32_partial_count',!mt32);setDisabled('rt_soundfont_index',!sf);setDisabled('rt_sf_gain',!sf);setDisabled('rt_sf_reverb_active',!sf);setDisabled('rt_sf_reverb_room',!sf);setDisabled('rt_sf_reverb_level',!sf);setDisabled('rt_sf_reverb_damping',!sf);setDisabled('rt_sf_reverb_width',!sf);setDisabled('rt_sf_chorus_active',!sf);setDisabled('rt_sf_chorus_depth',!sf);setDisabled('rt_sf_chorus_level',!sf);setDisabled('rt_sf_chorus_voices',!sf);setDisabled('rt_sf_chorus_speed',!sf);setSectionHidden('mt32-section',!mt32);setSectionHidden('sf-section',!sf);setTabActive('tab-mt32',mt32);setTabActive('tab-sf',sf);var dual=!!j.mixer_dual_mode;if(dual){document.querySelectorAll('#mt32-section input,#mt32-section select,#sf-section input,#sf-section select').forEach(function(e){e.disabled=false;});setSectionHidden('mt32-section',false);setSectionHidden('sf-section',false);setTabActive('tab-mt32',true);setTabActive('tab-sf',true);}var mxl=document.getElementById('rt_mixer_label');if(mxl){var pn=['All MT-32','All FluidSynth','Split GM','Custom'];if(j.mixer_enabled){mxl.textContent=pn[j.mixer_preset]||'On';mxl.style.color='#86efac';}else{mxl.textContent='OFF';mxl.style.color='#64748b';}}var mxb=document.getElementById('mx-banner');if(mxb)mxb.style.display=dual?'block':'none';};";
		HTML += "const rtRefresh=async()=>{try{const r=await fetch('/api/runtime/status',{cache:'no-store'});if(!r.ok)throw new Error('http');const j=await r.json();applyRuntimeState(j);}catch(err){if(rs)rs.textContent='Error reading runtime status';}};";
		HTML += "const rtApply=async(param,value)=>{if(!rs)return;rs.textContent='Applying...';const body=new URLSearchParams({param,value:String(value)});try{const r=await fetch('/api/runtime/set',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:body.toString()});const j=await r.json();if(!r.ok||!j.ok){rs.textContent='Could not apply '+param;return;}applyRuntimeState(j);rs.textContent='Applied: '+param;}catch(err){rs.textContent='Error applying '+param;}};";
		HTML += "const bindChange=(id,param)=>{const el=document.getElementById(id);if(!el)return;el.addEventListener('change',()=>rtApply(param,el.value));};const bindRange=(id,param,formatter)=>{const el=document.getElementById(id);if(!el)return;el.addEventListener('input',()=>{if(formatter)formatter(el.value);});el.addEventListener('change',()=>rtApply(param,el.value));};";
		HTML += "const tabMT32=document.getElementById('tab-mt32');if(tabMT32)tabMT32.addEventListener('click',()=>rtApply('active_synth','mt32'));const tabSF=document.getElementById('tab-sf');if(tabSF)tabSF.addEventListener('click',()=>rtApply('active_synth','soundfont'));";
		HTML += "bindChange('rt_active_synth','active_synth');bindChange('rt_mt32_rom_set','mt32_rom_set');bindChange('rt_soundfont_index','soundfont_index');bindChange('rt_sf_reverb_active','sf_reverb_active');bindChange('rt_sf_chorus_active','sf_chorus_active');bindChange('rt_sf_chorus_voices','sf_chorus_voices');bindChange('rt_mt32_reverb_active','mt32_reverb_active');bindChange('rt_mt32_nice_amp','mt32_nice_amp');bindChange('rt_mt32_nice_pan','mt32_nice_pan');bindChange('rt_mt32_nice_mix','mt32_nice_mix');bindChange('rt_mt32_dac_mode','mt32_dac_mode');bindChange('rt_mt32_midi_delay','mt32_midi_delay');bindChange('rt_mt32_analog_mode','mt32_analog_mode');bindChange('rt_mt32_renderer_type','mt32_renderer_type');bindChange('rt_mt32_partial_count','mt32_partial_count');";
		HTML += "bindRange('rt_master_volume','master_volume',(v)=>setText('rt_master_volume_val',v));bindRange('rt_sf_gain','sf_gain',(v)=>setText('rt_sf_gain_val',Number(v).toFixed(2)));bindRange('rt_sf_reverb_room','sf_reverb_room',(v)=>setText('rt_sf_reverb_room_val',Number(v).toFixed(1)));bindRange('rt_sf_reverb_level','sf_reverb_level',(v)=>setText('rt_sf_reverb_level_val',Number(v).toFixed(1)));bindRange('rt_sf_reverb_damping','sf_reverb_damping',(v)=>setText('rt_sf_reverb_damping_val',Number(v).toFixed(1)));bindRange('rt_sf_reverb_width','sf_reverb_width',(v)=>setText('rt_sf_reverb_width_val',Math.round(Number(v))));bindRange('rt_sf_chorus_depth','sf_chorus_depth',(v)=>setText('rt_sf_chorus_depth_val',Math.round(Number(v))));bindRange('rt_sf_chorus_level','sf_chorus_level',(v)=>setText('rt_sf_chorus_level_val',Number(v).toFixed(2)));bindRange('rt_sf_chorus_speed','sf_chorus_speed',(v)=>setText('rt_sf_chorus_speed_val',Number(v).toFixed(2)));bindRange('rt_mt32_reverb_gain','mt32_reverb_gain',(v)=>setText('rt_mt32_reverb_gain_val',Number(v).toFixed(2)));rtRefresh();setInterval(rtRefresh,3000);</script>";
		HTML += "</main></body></html>";

		const unsigned nBodyLength = HTML.GetLength();
		if (*pLength < nBodyLength)
			return HTTPInternalServerError;

		memcpy(pBuffer, static_cast<const char*>(HTML), nBodyLength);
		*pLength = nBodyLength;
		*ppContentType = "text/html; charset=utf-8";
		return HTTPOK;
	}

	if (bIsConfigPagePath)
	{
		CString HTML;
		HTML += "<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width, initial-scale=1'>";
		HTML += "<title>mt32-pi config</title><link rel='stylesheet' href='/app.css'></head><body><main>";
		HTML += "<script src='/app.js'></script>";
		HTML += "<h1>Configure mt32-pi</h1><p>Saves changes to <code>mt32-pi.cfg</code> and creates a backup <code>mt32-pi.cfg.bak</code>.</p>";
		HTML += "<nav><a href='/'>Status</a><a href='/sound'>Sound</a><a href='/config'>Config</a><a href='/sequencer'>Sequencer</a><a href='/mixer'>Mixer</a></nav>";
		HTML += "<form id='cfgForm'>";

		// ---- [system] ----
		CString SysI2CBaud; SysI2CBaud.Format("%d", pConfig->SystemI2CBaudRate);
		CString SysPowerSave; SysPowerSave.Format("%d", pConfig->SystemPowerSaveTimeout);
		HTML += "<section><h2>System</h2><div class='grid'>";
		HTML += "<label>Default synth<small>mt32: MT-32 via Munt emulator; soundfont: FluidSynth. Falls back to first available synth if chosen one is unavailable.</small><select name='default_synth'>";
		HTML += "<option value='mt32'"; HTML += SelectedAttr(pConfig->SystemDefaultSynth == CConfig::TSystemDefaultSynth::MT32); HTML += ">MT-32</option>";
		HTML += "<option value='soundfont'"; HTML += SelectedAttr(pConfig->SystemDefaultSynth == CConfig::TSystemDefaultSynth::SoundFont); HTML += ">SoundFont</option>";
		HTML += "</select></label>";
		HTML += "<label>Verbose<small>on: more info on LCD at boot and on errors. May hide the boot logo on small displays.</small><select name='system_verbose'><option value='off'"; HTML += SelectedAttr(!pConfig->SystemVerbose); HTML += ">off</option><option value='on'"; HTML += SelectedAttr(pConfig->SystemVerbose); HTML += ">on</option></select></label>";
		HTML += "<label>USB<small>on: enables USB support (MIDI, keyboards, etc.). Disable to speed up boot if USB is not needed.</small><select name='system_usb'><option value='on'"; HTML += SelectedAttr(pConfig->SystemUSB); HTML += ">on</option><option value='off'"; HTML += SelectedAttr(!pConfig->SystemUSB); HTML += ">off</option></select></label>";
		HTML += "<label>I2C baud rate<small>Bus speed in Hz. 400000 = fast mode. Use 1000000 for high-res graphic LCDs. Range: 100000-1000000</small><input name='system_i2c_baud_rate' type='number' value='"; AppendEscaped(HTML, SysI2CBaud); HTML += "'></label>";
		HTML += "<label>Power save timeout (s)<small>Seconds of silence before slowing CPU and turning off backlight. 0 = disabled. Range: 0-3600</small><input name='system_power_save_timeout' type='number' value='"; AppendEscaped(HTML, SysPowerSave); HTML += "'></label>";
		HTML += "</div></section>";

		// ---- [audio] ----
		CString AudioSR; AudioSR.Format("%d", pConfig->AudioSampleRate);
		CString AudioCS; AudioCS.Format("%d", pConfig->AudioChunkSize);
		HTML += "<section><h2>Audio</h2><div class='grid'>";
		HTML += "<label>Output device<small>pwm: headphone jack (Pi 3B/4); hdmi: HDMI audio; i2s: external I2S DAC (HiFiBerry, etc.)</small><select name='audio_output_device'>";
		HTML += "<option value='pwm'";  HTML += SelectedAttr(pConfig->AudioOutputDevice == CConfig::TAudioOutputDevice::PWM);  HTML += ">PWM</option>";
		HTML += "<option value='hdmi'"; HTML += SelectedAttr(pConfig->AudioOutputDevice == CConfig::TAudioOutputDevice::HDMI); HTML += ">HDMI</option>";
		HTML += "<option value='i2s'";  HTML += SelectedAttr(pConfig->AudioOutputDevice == CConfig::TAudioOutputDevice::I2S);  HTML += ">I2S</option>";
		HTML += "</select></label>";
		HTML += "<label>Sample rate (Hz)<small>PWM: 22050-192000; HDMI: 48000 only; I2S: depends on DAC. MT-32 uses 32000 Hz internally (resampled). Range: 32000-192000</small><input name='audio_sample_rate' type='number' value='"; AppendEscaped(HTML, AudioSR); HTML += "'></label>";
		HTML += "<label>Chunk size<small>Samples per audio buffer. Lower = lower latency. Min: PWM=2, I2S=32, HDMI=384 (multiple). Latency = chunk/2/Hz*1000ms. Range: 2-2048</small><input name='audio_chunk_size' type='number' value='"; AppendEscaped(HTML, AudioCS); HTML += "'></label>";
		HTML += "<label>Reversed stereo<small>on: swaps left/right channels. Use if hardware has channels connected in reverse.</small><select name='audio_reversed_stereo'><option value='off'"; HTML += SelectedAttr(!pConfig->AudioReversedStereo); HTML += ">off</option><option value='on'"; HTML += SelectedAttr(pConfig->AudioReversedStereo); HTML += ">on</option></select></label>";
		HTML += "</div></section>";

		// ---- [midi] ----
		CString MIDIGPIOBaud; MIDIGPIOBaud.Format("%d", pConfig->MIDIGPIOBaudRate);
		CString MIDIUSBBaud;  MIDIUSBBaud.Format("%d", pConfig->MIDIUSBSerialBaudRate);
		HTML += "<section><h2>MIDI</h2><div class='grid'>";
		HTML += "<label>GPIO baud rate<small>Baud rate for GPIO MIDI. Standard DIN MIDI: 31250. SoftMPU serial mode: 38400. Range: 300-4000000</small><input name='midi_gpio_baud_rate' type='number' value='"; AppendEscaped(HTML, MIDIGPIOBaud); HTML += "'></label>";
		HTML += "<label>GPIO thru<small>on: retransmits on GPIO Tx everything received on Rx. Useful for debugging or passing MIDI to another synth.</small><select name='midi_gpio_thru'><option value='off'"; HTML += SelectedAttr(!pConfig->MIDIGPIOThru); HTML += ">off</option><option value='on'"; HTML += SelectedAttr(pConfig->MIDIGPIOThru); HTML += ">on</option></select></label>";
		HTML += "<label>USB serial baud rate<small>Baud rate for MIDI via USB-serial adapter. SoftMPU serial mode: 38400. Range: 9600-115200</small><input name='midi_usb_serial_baud_rate' type='number' value='"; AppendEscaped(HTML, MIDIUSBBaud); HTML += "'></label>";
		HTML += "</div></section>";

		// ---- [control] ----
		CString CtrlSwitchTO; CtrlSwitchTO.Format("%d", pConfig->ControlSwitchTimeout);
		HTML += "<section><h2>Control</h2><div class='grid'>";
		HTML += "<label>Control scheme<small>none: no physical controls; simple_buttons: 4-button scheme; simple_encoder: 2 buttons + rotary encoder</small><select name='control_scheme'>";
		HTML += "<option value='none'"; HTML += SelectedAttr(pConfig->ControlScheme == CConfig::TControlScheme::None); HTML += ">none</option>";
		HTML += "<option value='simple_buttons'"; HTML += SelectedAttr(pConfig->ControlScheme == CConfig::TControlScheme::SimpleButtons); HTML += ">simple_buttons</option>";
		HTML += "<option value='simple_encoder'"; HTML += SelectedAttr(pConfig->ControlScheme == CConfig::TControlScheme::SimpleEncoder); HTML += ">simple_encoder</option>";
		HTML += "</select></label>";
		HTML += "<label>Encoder type<small>Gray-code cycle per click. quarter: 4 clicks = 1 step; half: 2 clicks; full: 1 click = 1 step. Depends on your encoder hardware.</small><select name='encoder_type'>";
		HTML += "<option value='quarter'"; HTML += SelectedAttr(pConfig->ControlEncoderType == CConfig::TEncoderType::Quarter); HTML += ">quarter</option>";
		HTML += "<option value='half'";    HTML += SelectedAttr(pConfig->ControlEncoderType == CConfig::TEncoderType::Half);    HTML += ">half</option>";
		HTML += "<option value='full'";    HTML += SelectedAttr(pConfig->ControlEncoderType == CConfig::TEncoderType::Full);    HTML += ">full</option>";
		HTML += "</select></label>";
		HTML += "<label>Encoder reversed<small>on: reverses encoder direction if CLK/DAT are connected backwards.</small><select name='encoder_reversed'><option value='off'"; HTML += SelectedAttr(!pConfig->ControlEncoderReversed); HTML += ">off</option><option value='on'"; HTML += SelectedAttr(pConfig->ControlEncoderReversed); HTML += ">on</option></select></label>";
		HTML += "<label>MiSTer<small>on: enables I2C interface to control mt32-pi from the MiSTer FPGA OSD via additional hardware.</small><select name='control_mister'><option value='off'"; HTML += SelectedAttr(!pConfig->ControlMister); HTML += ">off</option><option value='on'"; HTML += SelectedAttr(pConfig->ControlMister); HTML += ">on</option></select></label>";
		HTML += "<label>Switch timeout (s)<small>Seconds to wait before loading the SoundFont when switching with the physical button. Range: 0-3600</small><input name='control_switch_timeout' type='number' value='"; AppendEscaped(HTML, CtrlSwitchTO); HTML += "'></label>";
		HTML += "</div></section>";

		// ---- [mt32emu] ----
		CString MT32Gain; MT32Gain.Format("%.2f", pConfig->MT32EmuGain);
		CString MT32RevGain; MT32RevGain.Format("%.2f", pConfig->MT32EmuReverbGain);
		HTML += "<section><h2>MT-32 emulator (defaults)</h2><div class='grid'>";
		HTML += "<label>Gain<small>Synthesizer output gain. 1.0 = no change. Independent of MIDI volume. Range: 0.0-256.0</small><input name='mt32emu_gain' type='number' step='0.01' min='0' max='8' value='"; AppendEscaped(HTML, MT32Gain); HTML += "'></label>";
		HTML += "<label>Reverb gain<small>Gain applied only to the MT-32 wet reverb channel. Range: 0.0 and upwards.</small><input name='mt32emu_reverb_gain' type='number' step='0.01' min='0' max='8' value='"; AppendEscaped(HTML, MT32RevGain); HTML += "'></label>";
		HTML += "<label>Resampler quality<small>none: no resampling (requires sample_rate=32000); fastest/fast: quick; good: balanced; best: highest quality, most CPU</small><select name='mt32emu_resampler_quality'>";
		HTML += "<option value='none'";    HTML += SelectedAttr(pConfig->MT32EmuResamplerQuality == CConfig::TMT32EmuResamplerQuality::None);    HTML += ">none</option>";
		HTML += "<option value='fastest'"; HTML += SelectedAttr(pConfig->MT32EmuResamplerQuality == CConfig::TMT32EmuResamplerQuality::Fastest); HTML += ">fastest</option>";
		HTML += "<option value='fast'";    HTML += SelectedAttr(pConfig->MT32EmuResamplerQuality == CConfig::TMT32EmuResamplerQuality::Fast);    HTML += ">fast</option>";
		HTML += "<option value='good'";    HTML += SelectedAttr(pConfig->MT32EmuResamplerQuality == CConfig::TMT32EmuResamplerQuality::Good);    HTML += ">good</option>";
		HTML += "<option value='best'";    HTML += SelectedAttr(pConfig->MT32EmuResamplerQuality == CConfig::TMT32EmuResamplerQuality::Best);    HTML += ">best</option>";
		HTML += "</select></label>";
		HTML += "<label>MIDI channels<small>standard: parts 1-8 on MIDI channels 2-9, rhythm=10; alternate: parts 1-8 on channels 1-8, rhythm=10</small><select name='mt32emu_midi_channels'>";
		HTML += "<option value='standard'";  HTML += SelectedAttr(pConfig->MT32EmuMIDIChannels == CConfig::TMT32EmuMIDIChannels::Standard);  HTML += ">standard</option>";
		HTML += "<option value='alternate'"; HTML += SelectedAttr(pConfig->MT32EmuMIDIChannels == CConfig::TMT32EmuMIDIChannels::Alternate); HTML += ">alternate</option>";
		HTML += "</select></label>";
		HTML += "<label>ROM set<small>Boot ROM set. old: MT-32 v1; new: MT-32 v2; cm32l: Roland CM-32L; any: first available; all: all found</small><select name='mt32emu_rom_set'>";
		HTML += "<option value='old'";   HTML += SelectedAttr(pConfig->MT32EmuROMSet == CConfig::TMT32EmuROMSet::MT32Old); HTML += ">old (MT-32)</option>";
		HTML += "<option value='new'";   HTML += SelectedAttr(pConfig->MT32EmuROMSet == CConfig::TMT32EmuROMSet::MT32New); HTML += ">new (MT-32)</option>";
		HTML += "<option value='cm32l'"; HTML += SelectedAttr(pConfig->MT32EmuROMSet == CConfig::TMT32EmuROMSet::CM32L);  HTML += ">cm32l</option>";
		HTML += "<option value='any'";   HTML += SelectedAttr(pConfig->MT32EmuROMSet == CConfig::TMT32EmuROMSet::Any);    HTML += ">any</option>";
		HTML += "<option value='all'";   HTML += SelectedAttr(pConfig->MT32EmuROMSet == CConfig::TMT32EmuROMSet::All);    HTML += ">all</option>";
		HTML += "</select></label>";
		HTML += "<label>Reversed stereo<small>on: swaps L/R so MT-32 panning matches SoundFont. Also changeable at runtime via SysEx.</small><select name='mt32emu_reversed_stereo'><option value='off'"; HTML += SelectedAttr(!pConfig->MT32EmuReversedStereo); HTML += ">off</option><option value='on'"; HTML += SelectedAttr(pConfig->MT32EmuReversedStereo); HTML += ">on</option></select></label>";
		HTML += "</div></section>";

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
		HTML += "<section><h2>SoundFont (defaults)</h2><div class='grid'>";
		HTML += "<label>SoundFont index<small>0-based index sorted alphabetically from soundfonts/. 0=first, 1=second, etc.</small><input name='fs_soundfont' type='number' min='0' value='"; AppendEscaped(HTML, FSSoundFont); HTML += "'></label>";
		HTML += "<label>Polyphony<small>Max simultaneous voices. Reduce if distortion occurs. Pi4/overclocked can support higher values. Range: 1-65535</small><input name='fs_polyphony' type='number' min='1' value='"; AppendEscaped(HTML, FSPolyphony); HTML += "'></label>";
		HTML += "<label>Gain<small>FluidSynth master volume gain. See fluidsettings.xml for details.</small><input name='fs_gain' type='number' step='0.01' min='0' value='"; AppendEscaped(HTML, FSGain); HTML += "'></label>";
		HTML += "<label>Reverb<small>on: enable reverb by default. Can be overridden per SoundFont with a .cfg file next to the .sf2</small><select name='fs_reverb'><option value='on'"; HTML += SelectedAttr(pConfig->FluidSynthDefaultReverbActive); HTML += ">on</option><option value='off'"; HTML += SelectedAttr(!pConfig->FluidSynthDefaultReverbActive); HTML += ">off</option></select></label>";
		HTML += "<label>Reverb damping<small>High-frequency absorption. Range: 0.0-1.0</small><input name='fs_reverb_damping' type='number' step='0.01' min='0' max='1' value='"; AppendEscaped(HTML, FSRevDamp); HTML += "'></label>";
		HTML += "<label>Reverb level<small>Wet mix level. Range: 0.0-1.0</small><input name='fs_reverb_level' type='number' step='0.01' min='0' value='"; AppendEscaped(HTML, FSRevLevel); HTML += "'></label>";
		HTML += "<label>Reverb room size<small>Room size. Higher values = more cavernous. Range: 0.0-1.0</small><input name='fs_reverb_room_size' type='number' step='0.01' min='0' max='1' value='"; AppendEscaped(HTML, FSRevRoom); HTML += "'></label>";
		HTML += "<label>Reverb width<small>Stereo width. 0.0 = mono, 100.0 = maximum. Range: 0.0-100.0</small><input name='fs_reverb_width' type='number' step='0.01' min='0' value='"; AppendEscaped(HTML, FSRevWidth); HTML += "'></label>";
		HTML += "<label>Chorus<small>on: enable chorus by default. Can be overridden per SoundFont with a .cfg file next to the .sf2</small><select name='fs_chorus'><option value='on'"; HTML += SelectedAttr(pConfig->FluidSynthDefaultChorusActive); HTML += ">on</option><option value='off'"; HTML += SelectedAttr(!pConfig->FluidSynthDefaultChorusActive); HTML += ">off</option></select></label>";
		HTML += "<label>Chorus depth<small>Modulation depth (ms). Typical values: 0-20</small><input name='fs_chorus_depth' type='number' step='0.01' min='0' value='"; AppendEscaped(HTML, FSChrDepth); HTML += "'></label>";
		HTML += "<label>Chorus level<small>Mix level. Typical values: 0.0-10.0</small><input name='fs_chorus_level' type='number' step='0.01' min='0' value='"; AppendEscaped(HTML, FSChrLevel); HTML += "'></label>";
		HTML += "<label>Chorus voices<small>Number of modulator voices. Typical values: 1-99</small><input name='fs_chorus_voices' type='number' min='0' value='"; AppendEscaped(HTML, FSChrVoices); HTML += "'></label>";
		HTML += "<label>Chorus speed<small>Modulation speed in Hz. Range: 0.29-5.0</small><input name='fs_chorus_speed' type='number' step='0.01' min='0' value='"; AppendEscaped(HTML, FSChrSpeed); HTML += "'></label>";
		HTML += "</div></section>";

		// ---- [lcd] ----
		CString LCDWidth; LCDWidth.Format("%d", pConfig->LCDWidth);
		CString LCDHeight; LCDHeight.Format("%d", pConfig->LCDHeight);
		CString I2CAddr; I2CAddr.Format("%x", pConfig->LCDI2CLCDAddress);
		HTML += "<section><h2>LCD / OLED display</h2><div class='grid'>";
		HTML += "<label>LCD type<small>none: no LCD; hd44780_4bit: 4-bit GPIO char LCD; hd44780_i2c: I2C char LCD; sh1106_i2c: 1.3 inch OLED; ssd1306_i2c: 0.96 inch OLED</small><select name='lcd_type'>";
		HTML += "<option value='none'";        HTML += SelectedAttr(pConfig->LCDType == CConfig::TLCDType::None);          HTML += ">none</option>";
		HTML += "<option value='hd44780_4bit'"; HTML += SelectedAttr(pConfig->LCDType == CConfig::TLCDType::HD44780FourBit); HTML += ">hd44780_4bit</option>";
		HTML += "<option value='hd44780_i2c'"; HTML += SelectedAttr(pConfig->LCDType == CConfig::TLCDType::HD44780I2C);    HTML += ">hd44780_i2c</option>";
		HTML += "<option value='sh1106_i2c'";  HTML += SelectedAttr(pConfig->LCDType == CConfig::TLCDType::SH1106I2C);     HTML += ">sh1106_i2c</option>";
		HTML += "<option value='ssd1306_i2c'"; HTML += SelectedAttr(pConfig->LCDType == CConfig::TLCDType::SSD1306I2C);    HTML += ">ssd1306_i2c</option>";
		HTML += "</select></label>";
		HTML += "<label>LCD width<small>Width in characters (LCD) or pixels (OLED). SSD1305: use 132. Range: 20-132</small><input name='lcd_width' type='number' value='"; AppendEscaped(HTML, LCDWidth); HTML += "'></label>";
		HTML += "<label>LCD height<small>Height in characters (LCD) or pixels (OLED). See docs for your model. Range: 2-64</small><input name='lcd_height' type='number' value='"; AppendEscaped(HTML, LCDHeight); HTML += "'></label>";
		HTML += "<label>LCD I2C address (hex)<small>Hex address without 0x prefix. Most common: 3c or 3d. Check your display datasheet.</small><input name='lcd_i2c_address' value='"; AppendEscaped(HTML, I2CAddr); HTML += "'></label>";
		HTML += "<label>Rotation<small>normal: no rotation; inverted: 180 degree rotation. Graphic LCDs only (sh1106, ssd1306).</small><select name='lcd_rotation'>";
		HTML += "<option value='normal'";   HTML += SelectedAttr(pConfig->LCDRotation == CConfig::TLCDRotation::Normal);   HTML += ">normal</option>";
		HTML += "<option value='inverted'"; HTML += SelectedAttr(pConfig->LCDRotation == CConfig::TLCDRotation::Inverted); HTML += ">inverted</option>";
		HTML += "</select></label>";
		HTML += "<label>Mirror<small>normal: no mirror; mirrored: horizontal reflection. Graphic LCDs only (sh1106, ssd1306).</small><select name='lcd_mirror'>";
		HTML += "<option value='normal'";   HTML += SelectedAttr(pConfig->LCDMirror == CConfig::TLCDMirror::Normal);   HTML += ">normal</option>";
		HTML += "<option value='mirrored'"; HTML += SelectedAttr(pConfig->LCDMirror == CConfig::TLCDMirror::Mirrored); HTML += ">mirrored</option>";
		HTML += "</select></label>";
		HTML += "</div></section>";

		// ---- [network] ----
		CString WebPort; WebPort.Format("%d", pConfig->NetworkWebServerPort);
		CString IP; pConfig->NetworkIPAddress.Format(&IP);
		CString Subnet; pConfig->NetworkSubnetMask.Format(&Subnet);
		CString GW; pConfig->NetworkDefaultGateway.Format(&GW);
		CString DNS; pConfig->NetworkDNSServer.Format(&DNS);
		HTML += "<section><h2>Network</h2><div class='grid'>";
		HTML += "<label>Mode<small>off: no network; ethernet: Ethernet (Pi 3B/3B+ requires USB); wifi: Wi-Fi (configure SSID in WiFi section below)</small><select name='network_mode'><option value='off'"; HTML += SelectedAttr(pConfig->NetworkMode == CConfig::TNetworkMode::Off); HTML += ">off</option>";
		HTML += "<option value='ethernet'"; HTML += SelectedAttr(pConfig->NetworkMode == CConfig::TNetworkMode::Ethernet); HTML += ">ethernet</option>";
		HTML += "<option value='wifi'";     HTML += SelectedAttr(pConfig->NetworkMode == CConfig::TNetworkMode::WiFi);     HTML += ">wifi</option></select></label>";
		HTML += "<label>DHCP<small>on: automatic IP via DHCP; off: use static IP, subnet, gateway and DNS values below.</small><select name='network_dhcp'><option value='on'"; HTML += SelectedAttr(pConfig->NetworkDHCP); HTML += ">on</option><option value='off'"; HTML += SelectedAttr(!pConfig->NetworkDHCP); HTML += ">off</option></select></label>";
		HTML += "<label>IP address<input name='network_ip' value='"; AppendEscaped(HTML, IP); HTML += "'></label>";
		HTML += "<label>Subnet mask<input name='network_subnet' value='"; AppendEscaped(HTML, Subnet); HTML += "'></label>";
		HTML += "<label>Default gateway<input name='network_gateway' value='"; AppendEscaped(HTML, GW); HTML += "'></label>";
		HTML += "<label>DNS server<input name='network_dns' value='"; AppendEscaped(HTML, DNS); HTML += "'></label>";
		HTML += "<label>Hostname<input name='network_hostname' value='"; AppendEscaped(HTML, pConfig->NetworkHostname); HTML += "'></label>";
		HTML += "<label>RTP MIDI<small>on: enable RTP-MIDI/AppleMIDI server (macOS, rtpMIDI on Windows).</small><select name='network_rtp_midi'><option value='on'"; HTML += SelectedAttr(pConfig->NetworkRTPMIDI); HTML += ">on</option><option value='off'"; HTML += SelectedAttr(!pConfig->NetworkRTPMIDI); HTML += ">off</option></select></label>";
		HTML += "<label>UDP MIDI<small>on: enable simple UDP MIDI server on port 1999.</small><select name='network_udp_midi'><option value='on'"; HTML += SelectedAttr(pConfig->NetworkUDPMIDI); HTML += ">on</option><option value='off'"; HTML += SelectedAttr(!pConfig->NetworkUDPMIDI); HTML += ">off</option></select></label>";
		HTML += "<label>FTP<small>on: enable FTP server to transfer files (ROMs, SoundFonts) over the network.</small><select name='network_ftp'><option value='on'"; HTML += SelectedAttr(pConfig->NetworkFTPServer); HTML += ">on</option><option value='off'"; HTML += SelectedAttr(!pConfig->NetworkFTPServer); HTML += ">off</option></select></label>";
		HTML += "<label>FTP username<small>FTP server username. Default: mt32-pi</small><input name='network_ftp_username' value='"; AppendEscaped(HTML, pConfig->NetworkFTPUsername); HTML += "'></label>";
		HTML += "<label>FTP password<small>FTP server password. Default: mt32-pi</small><input name='network_ftp_password' type='password' value='"; AppendEscaped(HTML, pConfig->NetworkFTPPassword); HTML += "'></label>";
		HTML += "<label>Web<select name='network_web'><option value='on'"; HTML += SelectedAttr(pConfig->NetworkWebServer); HTML += ">on</option><option value='off'"; HTML += SelectedAttr(!pConfig->NetworkWebServer); HTML += ">off</option></select></label>";
		HTML += "<label>Web port<small>TCP port for the web server. Default: 80. Restart to apply changes.</small><input name='network_web_port' type='number' value='"; AppendEscaped(HTML, WebPort); HTML += "'></label>";
		HTML += "</div></section>";

		HTML += "<section id='wifi-section' class='section-hidden'><h2>WiFi</h2><p>Credentials for connecting to a wireless network. Saved to <code>wpa_supplicant.conf</code>.</p>";
		HTML += "<div class='grid'>";
		HTML += "<label>Country (ISO 3166-1 alpha-2)<input id='wifi_country' maxlength='2' placeholder='ES' autocomplete='off'></label>";
		HTML += "<label>SSID<input id='wifi_ssid' autocomplete='off'></label>";
		HTML += "<label>Password (PSK)<input id='wifi_psk' type='password' autocomplete='new-password' placeholder='leave empty to keep current'></label>";
		HTML += "</div><div style='margin-top:12px;'>";
		HTML += "<button class='primary' type='button' onclick='saveWifi()'>Save WiFi</button> ";
		HTML += "<span id='wifi_status' style='color:#93c5fd;'></span></div></section>";

		HTML += "<button class='primary' type='submit'>Save config</button> <button class='warn' type='button' id='rebootBtn'>Restart Pi</button> <span id='status'></span>";
		HTML += "</form>";
		HTML += "<script>const f=document.getElementById('cfgForm');const s=document.getElementById('status');const rb=document.getElementById('rebootBtn');";
		HTML += "f.addEventListener('submit',async(e)=>{e.preventDefault();s.textContent='Saving...';const body=new URLSearchParams(new FormData(f));try{const r=await fetch('/api/config/save',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:body.toString()});const j=await r.json();s.textContent=j.message||'OK';}catch(err){s.textContent='Error saving config';}});";
		HTML += "rb.addEventListener('click',async()=>{if(!confirm('Restart mt32-pi now?'))return;s.textContent='Restarting\u2026';rb.disabled=true;try{await fetch('/api/system/reboot',{method:'POST'});}catch(e){}s.textContent='Restarting\u2026 reconnect in ~20s';});";
		HTML += "const mEl=document.querySelector('select[name=\"network_mode\"]');const wSec=document.getElementById('wifi-section');";
		HTML += "function _chkWifi(){if(wSec)wSec.classList.toggle('section-hidden',!mEl||mEl.value!=='wifi');}";
		HTML += "if(mEl)mEl.addEventListener('change',_chkWifi);_chkWifi();";
		HTML += "fetch('/api/wifi/read').then(r=>r.json()).then(j=>{const ss=document.getElementById('wifi_ssid');const co=document.getElementById('wifi_country');if(ss)ss.value=j.ssid||'';if(co)co.value=j.country||'';}).catch(()=>{});";
		HTML += "function saveWifi(){const ws=document.getElementById('wifi_status');const ss=document.getElementById('wifi_ssid');const co=document.getElementById('wifi_country');const pk=document.getElementById('wifi_psk');";
		HTML += "if(!ss||!ss.value.trim()){if(ws)ws.textContent='SSID required';return;}if(!co||!co.value.trim()){if(ws)ws.textContent='Country required';return;}";
		HTML += "if(ws)ws.textContent='Saving...';const body=new URLSearchParams({wifi_ssid:ss.value,wifi_psk:pk?pk.value:'',wifi_country:co.value});";
		HTML += "fetch('/api/wifi/save',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:body.toString()}).then(r=>r.json()).then(j=>{if(ws)ws.textContent=j.ok?'Saved. Restart to apply.':(j.message||'Error');}).catch(()=>{if(ws)ws.textContent='Error saving WiFi';});}</script>";
		HTML += "</main></body></html>";

		const unsigned nBodyLength = HTML.GetLength();
		if (*pLength < nBodyLength)
			return HTTPInternalServerError;

		memcpy(pBuffer, static_cast<const char*>(HTML), nBodyLength);
		*pLength = nBodyLength;
		*ppContentType = "text/html; charset=utf-8";
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

	CString IPAddress;
	m_pMT32Pi->FormatIPAddress(IPAddress);

	CString I2CAddress;
	I2CAddress.Format("0x%x", pConfig->LCDI2CLCDAddress);

	CString SoundFontIndex;
	SoundFontIndex.Format("%u / %u", static_cast<unsigned>(m_pMT32Pi->GetCurrentSoundFontIndex()), static_cast<unsigned>(m_pMT32Pi->GetSoundFontCount()));

	CString HTML;
	HTML += "<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width, initial-scale=1'><title>mt32-pi status</title><link rel='stylesheet' href='/app.css'></head><body><main>";
	HTML += "<script src='/app.js'></script>";
	HTML += "<h1>mt32-pi</h1><p>Live status of system, network and synthesizers.</p>";
	HTML += "<nav><a href='/'>Status</a><a href='/sound'>Sound</a><a href='/config'>Config</a><a href='/sequencer'>Sequencer</a><a href='/mixer'>Mixer</a></nav>";
	HTML += "<div class='hero'>";
	HTML += "<div class='pill'>IP: ";
		AppendEscaped(HTML, IPAddress);
	HTML += "</div><div class='pill'>Active synth: ";
		AppendEscaped(HTML, m_pMT32Pi->GetActiveSynthName());
	HTML += "</div></div><div class='grid'>";

	AppendSectionStart(HTML, "System");
	AppendRow(HTML, "Active synth", m_pMT32Pi->GetActiveSynthName());
	AppendRow(HTML, "Default synth", DefaultSynthText(pConfig->SystemDefaultSynth));
	AppendRow(HTML, "USB", BoolText(pConfig->SystemUSB));
	AppendIntRow(HTML, "I2C baud", pConfig->SystemI2CBaudRate);
	AppendIntRow(HTML, "Power save timeout", pConfig->SystemPowerSaveTimeout);
	AppendSectionEnd(HTML);

	AppendSectionStart(HTML, "Network");
	AppendRow(HTML, "Interface", m_pMT32Pi->GetNetworkInterfaceName());
	AppendRow(HTML, "Network ready", BoolText(m_pMT32Pi->IsNetworkReady()));
	AppendRow(HTML, "Mode", NetworkModeText(pConfig->NetworkMode));
	AppendRow(HTML, "DHCP", BoolText(pConfig->NetworkDHCP));
	AppendRow(HTML, "Hostname", pConfig->NetworkHostname);
	AppendRow(HTML, "Current IP", IPAddress);
	AppendRow(HTML, "RTP-MIDI", BoolText(pConfig->NetworkRTPMIDI));
	AppendRow(HTML, "UDP MIDI", BoolText(pConfig->NetworkUDPMIDI));
	AppendRow(HTML, "FTP", BoolText(pConfig->NetworkFTPServer));
	AppendRow(HTML, "Web", BoolText(pConfig->NetworkWebServer));
	AppendIntRow(HTML, "Web port", pConfig->NetworkWebServerPort);
	AppendSectionEnd(HTML);

	AppendSectionStart(HTML, "Audio & control");
	AppendRow(HTML, "Audio output", AudioOutputText(pConfig->AudioOutputDevice));
	AppendIntRow(HTML, "Sample rate", pConfig->AudioSampleRate);
	AppendIntRow(HTML, "Chunk size", pConfig->AudioChunkSize);
	AppendRow(HTML, "Reversed stereo", BoolText(pConfig->AudioReversedStereo));
	AppendRow(HTML, "Control", ControlSchemeText(pConfig->ControlScheme));
	AppendRow(HTML, "Encoder", EncoderTypeText(pConfig->ControlEncoderType));
	AppendRow(HTML, "Encoder reversed", BoolText(pConfig->ControlEncoderReversed));
	AppendRow(HTML, "MiSTer", BoolText(pConfig->ControlMister));
	AppendSectionEnd(HTML);

	AppendSectionStart(HTML, "Display");
	AppendRow(HTML, "Type", LCDTypeText(pConfig->LCDType));
	AppendIntRow(HTML, "Width", pConfig->LCDWidth);
	AppendIntRow(HTML, "Height", pConfig->LCDHeight);
	AppendRow(HTML, "I2C address", I2CAddress);
	AppendRow(HTML, "Rotation", RotationText(pConfig->LCDRotation));
	AppendRow(HTML, "Mirror", MirrorText(pConfig->LCDMirror));
	AppendSectionEnd(HTML);

	AppendSectionStart(HTML, "MT-32");
	AppendRow(HTML, "Available", BoolText(m_pMT32Pi->HasMT32Synth()));
	AppendRow(HTML, "Current ROM", m_pMT32Pi->GetCurrentMT32ROMName());
	AppendRow(HTML, "ROM set config", MT32ROMSetText(pConfig->MT32EmuROMSet));
	AppendFloatRow(HTML, "Gain", pConfig->MT32EmuGain);
	AppendFloatRow(HTML, "Reverb gain", pConfig->MT32EmuReverbGain);
	AppendRow(HTML, "Reversed stereo", BoolText(pConfig->MT32EmuReversedStereo));
	AppendSectionEnd(HTML);

	AppendSectionStart(HTML, "SoundFont");
	AppendRow(HTML, "Available", BoolText(m_pMT32Pi->HasSoundFontSynth()));
	AppendRow(HTML, "Current name", m_pMT32Pi->GetCurrentSoundFontName());
	AppendRow(HTML, "Current path", m_pMT32Pi->GetCurrentSoundFontPath());
	AppendRow(HTML, "Index / total", SoundFontIndex);
	AppendIntRow(HTML, "Polyphony", pConfig->FluidSynthPolyphony);
	AppendFloatRow(HTML, "Gain", pConfig->FluidSynthDefaultGain);
	AppendRow(HTML, "Reverb", BoolText(pConfig->FluidSynthDefaultReverbActive));
	AppendRow(HTML, "Chorus", BoolText(pConfig->FluidSynthDefaultChorusActive));
	AppendSectionEnd(HTML);

	// Mixer status section
	{
		const CMT32Pi::TMixerStatus ms = m_pMT32Pi->GetMixerStatus();
		static const char* PresetNames[] = {"All MT-32", "All FluidSynth", "Split GM", "Custom"};
		const char* pPresetName = (ms.nPreset >= 0 && ms.nPreset <= 3) ? PresetNames[ms.nPreset] : "Unknown";
		HTML += "<section><h2>Mixer <a href='/mixer' style='font-size:13px;font-weight:normal;margin-left:8px;'>&#8594; Go</a></h2><table>";
		AppendRow(HTML, "Enabled", BoolText(ms.bEnabled));
		AppendRow(HTML, "Preset", pPresetName);
		AppendRow(HTML, "Dual mode", BoolText(ms.bDualMode));
		AppendSectionEnd(HTML);
	}

	HTML += "<section><h2>Sequencer <a href='/sequencer' style='font-size:13px;font-weight:normal;margin-left:8px;'>&#8594; Go</a></h2>";
	HTML += "<span id='idx-seq-badge' class='badge'>Loading...</span>";
	HTML += "<p id='idx-seq-file' style='margin-top:8px;color:#64748b;word-break:break-all;font-size:12px;margin-bottom:4px;'>&#8212;</p>";
	HTML += "<div class='seq-prog-bg'><div id='idx-prog' class='seq-prog-fill'></div></div>";
	HTML += "<p id='idx-seq-time' style='font-size:12px;margin:2px 0 0;color:#64748b;'>0:00 / 0:00</p></section>";
	HTML += "<section><h2>Live MIDI</h2><table><tr><th>API</th><td><code>/api/midi</code></td></tr><tr><th>Status</th><td id='midi-status'>Loading...</td></tr></table><div class='meter-grid' id='midi-grid'></div></section>";
	HTML += "<section><h2>Active keyboard</h2><canvas id='kb-canvas' height='64'></canvas></section>";
	HTML += "<section><h2>Piano roll</h2><canvas id='pr-canvas' height='160'></canvas></section>";

	HTML += "<script>";
	HTML += "const grid=document.getElementById('midi-grid');const st=document.getElementById('midi-status');";
	HTML += "for(let i=1;i<=16;i++){const row=document.createElement('div');row.className='meter';row.innerHTML='<span class=\"meter-label\" id=\"mlbl-'+i+'\">CH'+String(i).padStart(2,'0')+'</span><div class=\"meter-bar\"><div class=\"meter-fill\" id=\"fill-'+i+'\"></div><div class=\"meter-peak\" id=\"peak-'+i+'\"></div></div>';grid.appendChild(row);}";
	HTML += "const _f=[],_p=[],_lt=new Array(16).fill(0),_lv=new Array(16).fill(0),_pt=new Array(16).fill(0),_pk=new Array(16).fill(0),_pa=new Array(16).fill(-9999);";
	HTML += "for(let i=1;i<=16;i++){_f.push(document.getElementById('fill-'+i));_p.push(document.getElementById('peak-'+i));}";
	// Piano roll state: ring buffer of snapshots
	HTML += "const PR_COLS=120,PR_ROWS=128;";
	HTML += "const _prBuf=new Uint8Array(PR_COLS*PR_ROWS);var _prCol=0;";
	HTML += "const _kbCanvas=document.getElementById('kb-canvas');";
	HTML += "const _prCanvas=document.getElementById('pr-canvas');";
	HTML += "const _kbCtx=_kbCanvas?_kbCanvas.getContext('2d'):null;";
	HTML += "const _prCtx=_prCanvas?_prCanvas.getContext('2d'):null;";
	// Source colors: 0=off, 1=physical(green), 2=player(blue), 3=webui(orange)
	HTML += "const SRC_COLORS=['','#4ade80','#60a5fa','#fb923c'];";
	// Engine colors: 0=MT-32(cyan), 1=FluidSynth(magenta)
	HTML += "const ENG_COLORS=['#22d3ee','#c084fc'];";
	HTML += "var _chEng=new Array(16).fill(0);var _isMixer=false;";
	// WHITE_ST: semitone offsets for the 7 white keys in an octave
	// BLACK_ST: semitone offsets + fractional x position (0-7 white-key units) for 5 black keys
	HTML += "const W_ST=[0,2,4,5,7,9,11];";
	HTML += "const B_ST=[1,3,6,8,10];const B_XF=[0.6,1.6,3.6,4.6,5.6];";
	HTML += "function _noteClr(ch,n){if(_isMixer)return ENG_COLORS[_chEng[ch]]||'#22d3ee';return SRC_COLORS[src_arr[ch][n]]||'#60a5fa';}";
	HTML += "function _drawKb(){if(!_kbCtx)return;const W=_kbCanvas.width,H=_kbCanvas.height;";
	HTML += "const octaves=10,ww=W/(octaves*7);_kbCtx.fillStyle='#0a1020';_kbCtx.fillRect(0,0,W,H);";
	HTML += "for(let o=0;o<octaves;o++){for(let wi=0;wi<7;wi++){const n=o*12+W_ST[wi];if(n>=128)continue;";
	HTML += "const x=(o*7+wi)*ww;let clr='#cbd5e1';let hitCh=-1;";
	HTML += "for(let ch=0;ch<16;ch++){if(_notes[ch][n]){clr=_noteClr(ch,n);hitCh=ch;break;}}";
	HTML += "_kbCtx.fillStyle=clr;_kbCtx.fillRect(x+0.5,H*0.15,ww-1,H*0.84);";
	HTML += "if(hitCh>=0){_kbCtx.fillStyle='#fff';_kbCtx.font='bold '+Math.max(7,ww*0.6|0)+'px sans-serif';_kbCtx.textAlign='center';";
	HTML += "_kbCtx.fillText(''+(hitCh+1),x+ww/2,H*0.85);}}}";
	HTML += "for(let o=0;o<octaves;o++){for(let bi=0;bi<5;bi++){const n=o*12+B_ST[bi];if(n>=128)continue;";
	HTML += "const x=(o*7+B_XF[bi])*ww;let clr='#1e293b';";
	HTML += "for(let ch=0;ch<16;ch++){if(_notes[ch][n]){clr=_noteClr(ch,n);break;}}";
	HTML += "_kbCtx.fillStyle=clr;_kbCtx.fillRect(x,0,ww*0.55,H*0.58);}}}";
	HTML += "var _notes=Array.from({length:16},()=>new Uint8Array(128));";
	HTML += "var src_arr=Array.from({length:16},()=>new Uint8Array(128));";
	HTML += "function _drawPR(){if(!_prCtx)return;const W=_prCanvas.width,H=_prCanvas.height;";
	HTML += "const rowH=H/128;const colW=W/PR_COLS;";
	HTML += "_prCtx.fillStyle='#0a1020';_prCtx.fillRect(0,0,W,H);";
	HTML += "for(let col=0;col<PR_COLS;col++){const ci=(_prCol+col)%PR_COLS;";
	HTML += "for(let note=0;note<128;note++){const v=_prBuf[ci*PR_ROWS+note];if(!v)continue;";
	HTML += "_prCtx.fillStyle=_isMixer?ENG_COLORS[v-1]||'#22d3ee':SRC_COLORS[v]||'#60a5fa';";
	HTML += "_prCtx.fillRect(col*colW,H-(note+1)*rowH,colW-0.5,rowH-0.5);}}}";
	HTML += "const ATK=0.3,DCY=0.07,PH=1200,PD=0.97;function _rf(ts){for(let i=0;i<16;i++){const t=_lt[i];_lv[i]=t>_lv[i]?_lv[i]+(t-_lv[i])*ATK:_lv[i]+(t-_lv[i])*DCY;if(_pt[i]>_pk[i]){_pk[i]=_pt[i];_pa[i]=ts;}else if(ts-_pa[i]>PH)_pk[i]*=PD;if(_f[i])_f[i].style.width=(_lv[i]*100).toFixed(1)+'%';if(_p[i])_p[i].style.left=(_pk[i]*100).toFixed(1)+'%';}";
	HTML += "_drawKb();_drawPR();requestAnimationFrame(_rf);}";
	HTML += "function _resizeCanvases(){if(_kbCanvas){_kbCanvas.width=_kbCanvas.offsetWidth||640;_kbCanvas.height=64;}if(_prCanvas){_prCanvas.width=_prCanvas.offsetWidth||640;_prCanvas.height=160;}}";
	HTML += "_resizeCanvases();window.addEventListener('resize',_resizeCanvases);";
	HTML += "requestAnimationFrame(_rf);";
	HTML += "function applyWS(d){";
	HTML += "if(d.channels){var _pn=['All MT-32','All Fluid','Split GM','Custom'];st.textContent='Synth: '+(d.synth||'?')+(d.mixer?' ['+(_pn[d.preset]||'Mixer')+']':'')+' | WS';for(var i=0;i<d.channels.length;i++){var ch=d.channels[i];_lt[ch.ch]=Math.max(0,Math.min(1,ch.lv||0));_pt[ch.ch]=Math.max(0,Math.min(1,ch.pk||0));if(ch.eng!==undefined)_chEng[ch.ch]=ch.eng;}if(d.mixer!==undefined)_isMixer=d.mixer;";
	HTML += "var EN=['M','F'];for(var i=0;i<16;i++){var lb=document.getElementById('mlbl-'+(i+1));if(lb){var tag=_isMixer?' '+EN[_chEng[i]]:'';lb.textContent='CH'+String(i+1).padStart(2,'0')+tag;lb.style.color=_isMixer?ENG_COLORS[_chEng[i]]:'#93c5fd';}}}";
	HTML += "if(d.notes){for(var ch=0;ch<16;ch++){_notes[ch].fill(0);if(d.notes[ch])for(var j=0;j<d.notes[ch].length;j++){const n=d.notes[ch][j];_notes[ch][n]=1;src_arr[ch][n]=(d.src&&d.src[ch])||1;}}";
	HTML += "_prCol=(_prCol+1)%PR_COLS;_prBuf.fill(0,_prCol*PR_ROWS,(_prCol+1)*PR_ROWS);";
	HTML += "for(var ch=0;ch<16;ch++){if(d.notes[ch])for(var j=0;j<d.notes[ch].length;j++){const n=d.notes[ch][j];_prBuf[_prCol*PR_ROWS+n]=_isMixer?(_chEng[ch]+1):Math.max(_prBuf[_prCol*PR_ROWS+n],(d.src&&d.src[ch])||1);}}}";
	HTML += "var b=document.getElementById('idx-seq-badge');if(b){var st2=d.playing?'playing':(d.finished?'finished':'stopped');b.textContent=st2==='playing'?'Playing':(st2==='finished'?'Finished':'Stopped');b.className='badge'+(st2==='playing'?' playing':(st2==='finished'?' finished':''));}";
	HTML += "var sf=document.getElementById('idx-seq-file');if(sf)sf.textContent=d.file||'\\u2014';";
	HTML += "var dur=d.duration_ms||1;var elp=d.finished?d.duration_ms:(d.elapsed_ms||0);";
	HTML += "var pp=document.getElementById('idx-prog');if(pp)pp.style.width=Math.min(100,elp/dur*100).toFixed(1)+'%';";
	HTML += "var tt=document.getElementById('idx-seq-time');if(tt)tt.textContent=fmt(elp)+' / '+fmt(d.duration_ms);}";
	HTML += "(function(){var ws=null,_rt=0;function conn(){ws=new WebSocket('ws://'+location.hostname+':8765/');ws.onmessage=function(e){try{applyWS(JSON.parse(e.data));}catch(x){}};ws.onclose=function(){_rt=Math.min((_rt||500)*2,8000);setTimeout(conn,_rt);};ws.onerror=function(){ws.close();};}conn();})();";
	HTML += "</script>";

	HTML += "</div></main></body></html>";

	const unsigned nBodyLength = HTML.GetLength();
	if (*pLength < nBodyLength)
	{
		LOGERR("Increase web content buffer to at least %u bytes", nBodyLength);
		return HTTPInternalServerError;
	}

	memcpy(pBuffer, static_cast<const char*>(HTML), nBodyLength);
	*pLength = nBodyLength;
	*ppContentType = "text/html; charset=utf-8";
	return HTTPOK;
}
