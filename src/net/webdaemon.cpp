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
#include <cstring>

#include <fatfs/ff.h>

#include "mt32pi.h"
#include "net/webdaemon.h"

LOGMODULE("httpd");

namespace
{
	constexpr u16 DefaultPort = 80;
	constexpr unsigned MaxContentSize = 16384;

	constexpr const char* ConfigPath = "SD:mt32-pi.cfg";
	constexpr const char* ConfigTempPath = "SD:mt32-pi.cfg.new";
	constexpr const char* ConfigBackupPath = "SD:mt32-pi.cfg.bak";

	enum class TConfigSection
	{
		None,
		System,
		Control,
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
		if (std::strcmp(pTrimmed, "[control]") == 0)
			return TConfigSection::Control;
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
	const bool bIsStatusAPIPath = strcmp(pPath, "/api/status") == 0;
	const bool bIsMIDIAPIPath = strcmp(pPath, "/api/midi") == 0;
 	const bool bIsConfigSavePath = strcmp(pPath, "/api/config/save") == 0;
	const bool bIsSystemRebootPath = strcmp(pPath, "/api/system/reboot") == 0;

	if (!bIsIndexPath && !bIsConfigPagePath && !bIsStatusAPIPath && !bIsMIDIAPIPath && !bIsConfigSavePath && !bIsSystemRebootPath)
		return HTTPNotFound;

	if (!m_pMT32Pi)
		return HTTPInternalServerError;

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

	const CConfig* pConfig = m_pMT32Pi->GetConfig();
	if (!pConfig)
		return HTTPInternalServerError;

	if (bIsConfigSavePath)
	{
		if (!pFormData || !*pFormData)
			return HTTPBadRequest;

		char DefaultSynth[32];
		char ControlScheme[32];
		char EncoderType[32];
		char EncoderReversed[16];
		char LCDType[32];
		char LCDWidth[16];
		char LCDHeight[16];
		char LCDAddress[16];
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
		char NetworkWeb[16];
		char NetworkWebPort[16];

		if (!GetFormValue(pFormData, "default_synth", DefaultSynth, sizeof(DefaultSynth))
		 || !GetFormValue(pFormData, "control_scheme", ControlScheme, sizeof(ControlScheme))
		 || !GetFormValue(pFormData, "encoder_type", EncoderType, sizeof(EncoderType))
		 || !GetFormValue(pFormData, "encoder_reversed", EncoderReversed, sizeof(EncoderReversed))
		 || !GetFormValue(pFormData, "lcd_type", LCDType, sizeof(LCDType))
		 || !GetFormValue(pFormData, "lcd_width", LCDWidth, sizeof(LCDWidth))
		 || !GetFormValue(pFormData, "lcd_height", LCDHeight, sizeof(LCDHeight))
		 || !GetFormValue(pFormData, "lcd_i2c_address", LCDAddress, sizeof(LCDAddress))
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
		 || !GetFormValue(pFormData, "network_web", NetworkWeb, sizeof(NetworkWeb))
		 || !GetFormValue(pFormData, "network_web_port", NetworkWebPort, sizeof(NetworkWebPort)))
		{
			return HTTPBadRequest;
		}

		CConfig::TSystemDefaultSynth ParsedDefaultSynth;
		CConfig::TControlScheme ParsedControlScheme;
		CConfig::TEncoderType ParsedEncoderType;
		bool ParsedEncoderReversed = false;
		CConfig::TLCDType ParsedLCDType;
		int ParsedLCDWidth = 0;
		int ParsedLCDHeight = 0;
		int ParsedLCDAddress = 0;
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
		bool ParsedWeb = false;
		int ParsedWebPort = 0;

		if (!CConfig::ParseOption(DefaultSynth, &ParsedDefaultSynth)
		 || !CConfig::ParseOption(ControlScheme, &ParsedControlScheme)
		 || !CConfig::ParseOption(EncoderType, &ParsedEncoderType)
		 || !CConfig::ParseOption(EncoderReversed, &ParsedEncoderReversed)
		 || !CConfig::ParseOption(LCDType, &ParsedLCDType)
		 || !CConfig::ParseOption(LCDWidth, &ParsedLCDWidth)
		 || !CConfig::ParseOption(LCDHeight, &ParsedLCDHeight)
		 || !CConfig::ParseOption(LCDAddress, &ParsedLCDAddress, true)
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
		 || !CConfig::ParseOption(NetworkWeb, &ParsedWeb)
		 || !CConfig::ParseOption(NetworkWebPort, &ParsedWebPort))
		{
			return HTTPBadRequest;
		}

		TReplacement Replacements[] = {
			{TConfigSection::System, "default_synth", DefaultSynth, false},
			{TConfigSection::Control, "scheme", ControlScheme, false},
			{TConfigSection::Control, "encoder_type", EncoderType, false},
			{TConfigSection::Control, "encoder_reversed", EncoderReversed, false},
			{TConfigSection::LCD, "type", LCDType, false},
			{TConfigSection::LCD, "width", LCDWidth, false},
			{TConfigSection::LCD, "height", LCDHeight, false},
			{TConfigSection::LCD, "i2c_lcd_address", LCDAddress, false},
			{TConfigSection::Network, "mode", NetworkMode, false},
			{TConfigSection::Network, "dhcp", NetworkDHCP, false},
			{TConfigSection::Network, "ip_address", IPAddress, false},
			{TConfigSection::Network, "subnet_mask", SubnetMask, false},
			{TConfigSection::Network, "default_gateway", DefaultGateway, false},
			{TConfigSection::Network, "dns_server", DNSServer, false},
			{TConfigSection::Network, "hostname", Hostname, false},
			{TConfigSection::Network, "rtp_midi", NetworkRTPMIDI, false},
			{TConfigSection::Network, "udp_midi", NetworkUDPMIDI, false},
			{TConfigSection::Network, "ftp", NetworkFTP, false},
			{TConfigSection::Network, "web", NetworkWeb, false},
			{TConfigSection::Network, "web_port", NetworkWebPort, false},
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

	if (bIsConfigPagePath)
	{
		CString HTML;
		HTML += "<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width, initial-scale=1'>";
		HTML += "<title>mt32-pi config</title><style>";
		HTML += "body{font:14px/1.45 system-ui,sans-serif;margin:0;background:#0f172a;color:#e2e8f0;}";
		HTML += "main{max-width:1000px;margin:0 auto;padding:24px;}h1{margin:0 0 8px;}h2{font-size:18px;margin:0 0 12px;}";
		HTML += "p{margin:0 0 16px;color:#94a3b8;}section{background:#111827;border:1px solid #334155;border-radius:14px;padding:16px;margin-bottom:14px;}";
		HTML += ".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(220px,1fr));gap:10px;}label{display:flex;flex-direction:column;gap:4px;color:#cbd5e1;}";
		HTML += "input,select,button{background:#0b1220;color:#e2e8f0;border:1px solid #334155;border-radius:8px;padding:8px;}";
		HTML += "button{cursor:pointer;}button.primary{background:#1d4ed8;border-color:#1d4ed8;}button.warn{background:#7f1d1d;border-color:#7f1d1d;}";
		HTML += "#status{margin-top:10px;color:#93c5fd;}a{color:#93c5fd;}";
		HTML += "</style></head><body><main>";
		HTML += "<h1>Configurar mt32-pi</h1><p>Guarda cambios en <code>mt32-pi.cfg</code> y crea copia de seguridad <code>mt32-pi.cfg.bak</code>.</p>";
		HTML += "<form id='cfgForm'>";

		HTML += "<section><h2>Sistema y control</h2><div class='grid'>";
		HTML += "<label>Default synth<select name='default_synth'>";
		HTML += "<option value='mt32'";
		HTML += SelectedAttr(pConfig->SystemDefaultSynth == CConfig::TSystemDefaultSynth::MT32);
		HTML += ">MT-32</option><option value='soundfont'";
		HTML += SelectedAttr(pConfig->SystemDefaultSynth == CConfig::TSystemDefaultSynth::SoundFont);
		HTML += ">SoundFont</option></select></label>";

		HTML += "<label>Control scheme<select name='control_scheme'>";
		HTML += "<option value='none'";
		HTML += SelectedAttr(pConfig->ControlScheme == CConfig::TControlScheme::None);
		HTML += ">none</option><option value='simple_buttons'";
		HTML += SelectedAttr(pConfig->ControlScheme == CConfig::TControlScheme::SimpleButtons);
		HTML += ">simple_buttons</option><option value='simple_encoder'";
		HTML += SelectedAttr(pConfig->ControlScheme == CConfig::TControlScheme::SimpleEncoder);
		HTML += ">simple_encoder</option></select></label>";

		HTML += "<label>Encoder type<select name='encoder_type'>";
		HTML += "<option value='quarter'";
		HTML += SelectedAttr(pConfig->ControlEncoderType == CConfig::TEncoderType::Quarter);
		HTML += ">quarter</option><option value='half'";
		HTML += SelectedAttr(pConfig->ControlEncoderType == CConfig::TEncoderType::Half);
		HTML += ">half</option><option value='full'";
		HTML += SelectedAttr(pConfig->ControlEncoderType == CConfig::TEncoderType::Full);
		HTML += ">full</option></select></label>";

		HTML += "<label>Encoder reversed<select name='encoder_reversed'><option value='off'";
		HTML += SelectedAttr(!pConfig->ControlEncoderReversed);
		HTML += ">off</option><option value='on'";
		HTML += SelectedAttr(pConfig->ControlEncoderReversed);
		HTML += ">on</option></select></label>";
		HTML += "</div></section>";

		CString Width; Width.Format("%d", pConfig->LCDWidth);
		CString Height; Height.Format("%d", pConfig->LCDHeight);
		CString I2CAddr; I2CAddr.Format("%x", pConfig->LCDI2CLCDAddress);

		HTML += "<section><h2>Pantalla LCD/OLED</h2><div class='grid'>";
		HTML += "<label>LCD type<select name='lcd_type'>";
		HTML += "<option value='none'"; HTML += SelectedAttr(pConfig->LCDType == CConfig::TLCDType::None); HTML += ">none</option>";
		HTML += "<option value='hd44780_4bit'"; HTML += SelectedAttr(pConfig->LCDType == CConfig::TLCDType::HD44780FourBit); HTML += ">hd44780_4bit</option>";
		HTML += "<option value='hd44780_i2c'"; HTML += SelectedAttr(pConfig->LCDType == CConfig::TLCDType::HD44780I2C); HTML += ">hd44780_i2c</option>";
		HTML += "<option value='sh1106_i2c'"; HTML += SelectedAttr(pConfig->LCDType == CConfig::TLCDType::SH1106I2C); HTML += ">sh1106_i2c</option>";
		HTML += "<option value='ssd1306_i2c'"; HTML += SelectedAttr(pConfig->LCDType == CConfig::TLCDType::SSD1306I2C); HTML += ">ssd1306_i2c</option>";
		HTML += "</select></label>";
		HTML += "<label>LCD width<input name='lcd_width' value='"; AppendEscaped(HTML, Width); HTML += "'></label>";
		HTML += "<label>LCD height<input name='lcd_height' value='"; AppendEscaped(HTML, Height); HTML += "'></label>";
		HTML += "<label>LCD I2C address (hex)<input name='lcd_i2c_address' value='"; AppendEscaped(HTML, I2CAddr); HTML += "'></label>";
		HTML += "</div></section>";

		CString WebPort; WebPort.Format("%d", pConfig->NetworkWebServerPort);
		CString IP; pConfig->NetworkIPAddress.Format(&IP);
		CString Subnet; pConfig->NetworkSubnetMask.Format(&Subnet);
		CString GW; pConfig->NetworkDefaultGateway.Format(&GW);
		CString DNS; pConfig->NetworkDNSServer.Format(&DNS);

		HTML += "<section><h2>Red</h2><div class='grid'>";
		HTML += "<label>Mode<select name='network_mode'><option value='off'"; HTML += SelectedAttr(pConfig->NetworkMode == CConfig::TNetworkMode::Off); HTML += ">off</option><option value='ethernet'";
		HTML += SelectedAttr(pConfig->NetworkMode == CConfig::TNetworkMode::Ethernet);
		HTML += ">ethernet</option><option value='wifi'";
		HTML += SelectedAttr(pConfig->NetworkMode == CConfig::TNetworkMode::WiFi);
		HTML += ">wifi</option></select></label>";
		HTML += "<label>DHCP<select name='network_dhcp'><option value='on'"; HTML += SelectedAttr(pConfig->NetworkDHCP); HTML += ">on</option><option value='off'"; HTML += SelectedAttr(!pConfig->NetworkDHCP); HTML += ">off</option></select></label>";
		HTML += "<label>IP address<input name='network_ip' value='"; AppendEscaped(HTML, IP); HTML += "'></label>";
		HTML += "<label>Subnet mask<input name='network_subnet' value='"; AppendEscaped(HTML, Subnet); HTML += "'></label>";
		HTML += "<label>Default gateway<input name='network_gateway' value='"; AppendEscaped(HTML, GW); HTML += "'></label>";
		HTML += "<label>DNS server<input name='network_dns' value='"; AppendEscaped(HTML, DNS); HTML += "'></label>";
		HTML += "<label>Hostname<input name='network_hostname' value='"; AppendEscaped(HTML, pConfig->NetworkHostname); HTML += "'></label>";
		HTML += "<label>RTP MIDI<select name='network_rtp_midi'><option value='on'"; HTML += SelectedAttr(pConfig->NetworkRTPMIDI); HTML += ">on</option><option value='off'"; HTML += SelectedAttr(!pConfig->NetworkRTPMIDI); HTML += ">off</option></select></label>";
		HTML += "<label>UDP MIDI<select name='network_udp_midi'><option value='on'"; HTML += SelectedAttr(pConfig->NetworkUDPMIDI); HTML += ">on</option><option value='off'"; HTML += SelectedAttr(!pConfig->NetworkUDPMIDI); HTML += ">off</option></select></label>";
		HTML += "<label>FTP<select name='network_ftp'><option value='on'"; HTML += SelectedAttr(pConfig->NetworkFTPServer); HTML += ">on</option><option value='off'"; HTML += SelectedAttr(!pConfig->NetworkFTPServer); HTML += ">off</option></select></label>";
		HTML += "<label>Web<select name='network_web'><option value='on'"; HTML += SelectedAttr(pConfig->NetworkWebServer); HTML += ">on</option><option value='off'"; HTML += SelectedAttr(!pConfig->NetworkWebServer); HTML += ">off</option></select></label>";
		HTML += "<label>Web port<input name='network_web_port' value='"; AppendEscaped(HTML, WebPort); HTML += "'></label>";
		HTML += "</div></section>";

		HTML += "<button class='primary' type='submit'>Guardar config</button> <button class='warn' type='button' id='rebootBtn'>Reiniciar Pi</button> <span id='status'></span>";
		HTML += "</form><p><a href='/'>Volver al estado</a></p>";
		HTML += "<script>const f=document.getElementById('cfgForm');const s=document.getElementById('status');const rb=document.getElementById('rebootBtn');f.addEventListener('submit',async(e)=>{e.preventDefault();s.textContent='Guardando...';const body=new URLSearchParams(new FormData(f));try{const r=await fetch('/api/config/save',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:body.toString()});const j=await r.json();s.textContent=j.message||'OK';}catch(err){s.textContent='Error guardando config';}});rb.addEventListener('click',async()=>{if(!confirm('Reiniciar mt32-pi ahora?'))return;try{await fetch('/api/system/reboot',{method:'POST'});s.textContent='Reinicio solicitado';}catch(err){s.textContent='Error solicitando reinicio';}});</script>";
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
	HTML += "<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width, initial-scale=1'>";
	HTML += "<title>mt32-pi status</title><style>";
	HTML += "body{font:14px/1.45 system-ui,sans-serif;margin:0;background:#0f172a;color:#e2e8f0;}";
	HTML += "main{max-width:1040px;margin:0 auto;padding:24px;}";
	HTML += "h1{margin:0 0 8px;font-size:32px;}h2{margin:0 0 12px;font-size:18px;}p{margin:0 0 16px;color:#94a3b8;}";
	HTML += ".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(280px,1fr));gap:16px;}";
	HTML += "section{background:#111827;border:1px solid #334155;border-radius:14px;padding:16px;}";
	HTML += "table{width:100%;border-collapse:collapse;}th,td{padding:6px 0;vertical-align:top;border-bottom:1px solid #1e293b;}";
	HTML += "th{width:44%;text-align:left;color:#93c5fd;font-weight:600;padding-right:12px;}td{color:#e5e7eb;}";
	HTML += ".hero{display:flex;flex-wrap:wrap;gap:12px;margin:16px 0 20px;}";
	HTML += ".pill{background:#1e293b;border:1px solid #475569;border-radius:999px;padding:8px 12px;color:#f8fafc;}";
	HTML += ".meter-grid{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:10px;max-width:100%;}";
	HTML += ".meter{display:flex;align-items:center;gap:8px;min-width:0;}";
	HTML += ".meter-label{width:32px;flex:0 0 32px;color:#93c5fd;font-size:12px;}";
	HTML += ".meter-bar{position:relative;flex:1;min-width:0;height:10px;background:#0b1220;border:1px solid #334155;border-radius:999px;overflow:hidden;}";
	HTML += ".meter-fill{position:absolute;left:0;top:0;bottom:0;background:linear-gradient(90deg,#22d3ee,#4ade80,#facc15,#f97316);width:0%;}";
	HTML += ".meter-peak{position:absolute;top:-1px;bottom:-1px;width:2px;background:#f8fafc;left:0%;}";
	HTML += "@media (max-width: 860px){.meter-grid{grid-template-columns:1fr;}}";
	HTML += "code{font:12px ui-monospace,monospace;color:#bfdbfe;}a{color:#93c5fd;}";
	HTML += "</style></head><body><main>";
	HTML += "<h1>mt32-pi</h1><p>Estado en vivo del sistema, red y sintetizadores.</p><div class='hero'>";
	HTML += "<div class='pill'>IP: ";
		AppendEscaped(HTML, IPAddress);
	HTML += "</div><div class='pill'>Synth activo: ";
		AppendEscaped(HTML, m_pMT32Pi->GetActiveSynthName());
	HTML += "</div><div class='pill'>Web: /health</div><div class='pill'><a href='/config'>Config editor</a></div></div><div class='grid'>";

	AppendSectionStart(HTML, "Sistema");
	AppendRow(HTML, "Synth activo", m_pMT32Pi->GetActiveSynthName());
	AppendRow(HTML, "Synth por defecto", DefaultSynthText(pConfig->SystemDefaultSynth));
	AppendRow(HTML, "USB", BoolText(pConfig->SystemUSB));
	AppendIntRow(HTML, "I2C baud", pConfig->SystemI2CBaudRate);
	AppendIntRow(HTML, "Power save timeout", pConfig->SystemPowerSaveTimeout);
	AppendSectionEnd(HTML);

	AppendSectionStart(HTML, "Red");
	AppendRow(HTML, "Interfaz", m_pMT32Pi->GetNetworkInterfaceName());
	AppendRow(HTML, "Network ready", BoolText(m_pMT32Pi->IsNetworkReady()));
	AppendRow(HTML, "Modo", NetworkModeText(pConfig->NetworkMode));
	AppendRow(HTML, "DHCP", BoolText(pConfig->NetworkDHCP));
	AppendRow(HTML, "Hostname", pConfig->NetworkHostname);
	AppendRow(HTML, "IP actual", IPAddress);
	AppendRow(HTML, "RTP-MIDI", BoolText(pConfig->NetworkRTPMIDI));
	AppendRow(HTML, "UDP MIDI", BoolText(pConfig->NetworkUDPMIDI));
	AppendRow(HTML, "FTP", BoolText(pConfig->NetworkFTPServer));
	AppendRow(HTML, "Web", BoolText(pConfig->NetworkWebServer));
	AppendIntRow(HTML, "Web port", pConfig->NetworkWebServerPort);
	AppendSectionEnd(HTML);

	AppendSectionStart(HTML, "Audio y control");
	AppendRow(HTML, "Salida audio", AudioOutputText(pConfig->AudioOutputDevice));
	AppendIntRow(HTML, "Sample rate", pConfig->AudioSampleRate);
	AppendIntRow(HTML, "Chunk size", pConfig->AudioChunkSize);
	AppendRow(HTML, "Stereo invertido", BoolText(pConfig->AudioReversedStereo));
	AppendRow(HTML, "Control", ControlSchemeText(pConfig->ControlScheme));
	AppendRow(HTML, "Encoder", EncoderTypeText(pConfig->ControlEncoderType));
	AppendRow(HTML, "Encoder reversed", BoolText(pConfig->ControlEncoderReversed));
	AppendRow(HTML, "MiSTer", BoolText(pConfig->ControlMister));
	AppendSectionEnd(HTML);

	AppendSectionStart(HTML, "Pantalla");
	AppendRow(HTML, "Tipo", LCDTypeText(pConfig->LCDType));
	AppendIntRow(HTML, "Width", pConfig->LCDWidth);
	AppendIntRow(HTML, "Height", pConfig->LCDHeight);
	AppendRow(HTML, "I2C address", I2CAddress);
	AppendRow(HTML, "Rotation", RotationText(pConfig->LCDRotation));
	AppendRow(HTML, "Mirror", MirrorText(pConfig->LCDMirror));
	AppendSectionEnd(HTML);

	AppendSectionStart(HTML, "MT-32");
	AppendRow(HTML, "Disponible", BoolText(m_pMT32Pi->HasMT32Synth()));
	AppendRow(HTML, "ROM actual", m_pMT32Pi->GetCurrentMT32ROMName());
	AppendRow(HTML, "ROM set config", MT32ROMSetText(pConfig->MT32EmuROMSet));
	AppendFloatRow(HTML, "Gain", pConfig->MT32EmuGain);
	AppendFloatRow(HTML, "Reverb gain", pConfig->MT32EmuReverbGain);
	AppendRow(HTML, "Reversed stereo", BoolText(pConfig->MT32EmuReversedStereo));
	AppendSectionEnd(HTML);

	AppendSectionStart(HTML, "SoundFont");
	AppendRow(HTML, "Disponible", BoolText(m_pMT32Pi->HasSoundFontSynth()));
	AppendRow(HTML, "Nombre actual", m_pMT32Pi->GetCurrentSoundFontName());
	AppendRow(HTML, "Ruta actual", m_pMT32Pi->GetCurrentSoundFontPath());
	AppendRow(HTML, "Indice / total", SoundFontIndex);
	AppendIntRow(HTML, "Polyphony", pConfig->FluidSynthPolyphony);
	AppendFloatRow(HTML, "Gain", pConfig->FluidSynthDefaultGain);
	AppendRow(HTML, "Reverb", BoolText(pConfig->FluidSynthDefaultReverbActive));
	AppendRow(HTML, "Chorus", BoolText(pConfig->FluidSynthDefaultChorusActive));
	AppendSectionEnd(HTML);

	HTML += "<section><h2>MIDI en vivo</h2><table><tr><th>API</th><td><code>/api/midi</code></td></tr><tr><th>Estado</th><td id='midi-status'>Cargando...</td></tr></table><div class='meter-grid' id='midi-grid'></div></section>";

	HTML += "<script>";
	HTML += "const grid=document.getElementById('midi-grid');const st=document.getElementById('midi-status');";
	HTML += "for(let i=1;i<=16;i++){const row=document.createElement('div');row.className='meter';row.innerHTML='<span class=\"meter-label\">CH'+String(i).padStart(2,'0')+'</span><div class=\"meter-bar\"><div class=\"meter-fill\" id=\"fill-'+i+'\"></div><div class=\"meter-peak\" id=\"peak-'+i+'\"></div></div>';grid.appendChild(row);}";
	HTML += "async function tick(){try{const r=await fetch('/api/midi',{cache:'no-store'});if(!r.ok)throw new Error('http '+r.status);const d=await r.json();st.textContent='Synth: '+d.active_synth+' | actualizado';for(const ch of d.channels){const i=ch.channel;const lv=Math.max(0,Math.min(1,ch.level||0));const pk=Math.max(0,Math.min(1,ch.peak||0));document.getElementById('fill-'+i).style.width=(lv*100).toFixed(1)+'%';document.getElementById('peak-'+i).style.left=(pk*100).toFixed(1)+'%';}}catch(e){st.textContent='Error leyendo /api/midi';}}";
	HTML += "tick();setInterval(tick,300);";
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
