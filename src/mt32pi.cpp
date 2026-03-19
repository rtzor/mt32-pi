//
// mt32pi.cpp
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

#include <circle/memory.h>
#include <circle/serial.h>
#include <circle/sound/hdmisoundbasedevice.h>
#include <circle/sound/i2ssoundbasedevice.h>
#include <circle/sound/pwmsoundbasedevice.h>
#include <fatfs/ff.h>

#include <cstdarg>

#include "lcd/drivers/hd44780.h"
#include "lcd/drivers/ssd1306.h"
#include "lcd/ui.h"
#include "mt32pi.h"

#define MT32_PI_NAME "mt32-pi"
LOGMODULE(MT32_PI_NAME);
const char MT32PiFullName[] = MT32_PI_NAME " " MT32_PI_VERSION;

const char WLANFirmwarePath[] = "SD:firmware/";
const char WLANConfigFile[]   = "SD:wpa_supplicant.conf";

constexpr u32 LCDUpdatePeriodMillis                = 16;
constexpr u32 MisterUpdatePeriodMillis             = 50;
constexpr u32 LEDTimeoutMillis                     = 50;
constexpr u32 ActiveSenseTimeoutMillis             = 330;

constexpr float Sample24BitMax = (1 << 24 - 1) - 1;

enum class TCustomSysExCommand : u8
{
	Reboot                = 0x00,
	SwitchMT32ROMSet      = 0x01,
	SwitchSoundFont       = 0x02,
	SwitchSynth           = 0x03,
	SetMT32ReversedStereo = 0x04,
};

CMT32Pi* CMT32Pi::s_pThis = nullptr;

CMT32Pi::CMT32Pi(CI2CMaster* pI2CMaster, CSPIMaster* pSPIMaster, CInterruptSystem* pInterrupt, CGPIOManager* pGPIOManager, CSerialDevice* pSerialDevice, CUSBHCIDevice* pUSBHCI)
	: CMultiCoreSupport(CMemorySystem::Get()),
	  CMIDIParser(),

	  m_pConfig(CConfig::Get()),

	  m_pTimer(CTimer::Get()),
	  m_pActLED(CActLED::Get()),

	  m_pI2CMaster(pI2CMaster),
	  m_pSPIMaster(pSPIMaster),
	  m_pInterrupt(pInterrupt),
	  m_pGPIOManager(pGPIOManager),
	  m_pSerial(pSerialDevice),
	  m_pUSBHCI(pUSBHCI),
	  m_USBFileSystem{},
	  m_bUSBAvailable(false),

	  m_pNet(nullptr),
	  m_pNetDevice(nullptr),
	  m_WLAN(WLANFirmwarePath),
	  m_WPASupplicant(WLANConfigFile),
	  m_bNetworkReady(false),
	  m_pAppleMIDIParticipant(nullptr),
	  m_pUDPMIDIReceiver(nullptr),
	  m_pFTPDaemon(nullptr),
	  m_pWebDaemon(nullptr),
	  m_pWebSocketDaemon(nullptr),

	  m_pLCD(nullptr),
	  m_nLCDUpdateTime(0),
#ifdef MONITOR_TEMPERATURE
	  m_nTempUpdateTime(0),
#endif

	  m_pControl(nullptr),
	  m_MisterControl(pI2CMaster, m_EventQueue),
	  m_nMisterUpdateTime(0),

	  m_bDeferredSoundFontSwitchFlag(false),
	  m_nDeferredSoundFontSwitchIndex(0),
	  m_nDeferredSoundFontSwitchTime(0),

	  m_bSerialMIDIAvailable(false),
	  m_bSerialMIDIEnabled(false),
	  m_pUSBMIDIDevice(nullptr),
	  m_pUSBSerialDevice(nullptr),
	  m_pUSBMassStorageDevice(nullptr),

	  m_bActiveSenseFlag(false),
	  m_nActiveSenseTime(0),

	  m_bRunning(true),
	  m_bUITaskDone(false),
	  m_bLEDOn(false),
	  m_nLEDOnTime(0),

	  m_pSound(nullptr),
	  m_pPisound(nullptr),

	  m_nMasterVolume(100),
	  m_pCurrentSynth(nullptr),
	  m_pMT32Synth(nullptr),
	  m_pSoundFontSynth(nullptr),
	  m_bMixerEnabled(false),
	  m_nRenderUs(0),
	  m_nRenderAvgUs(0),
	  m_nRenderPeakUs(0),
	  m_nDeadlineUs(0),
	  m_bAutoReducePartials(true),
	  m_bMenuLongPressConsumed(false),

	  m_pFluidSequencer(nullptr),
	  m_nTempoMultiplier(1.0),
	  m_bSeqLoopEnabled(false),
	  m_bSeqIsPlaying(false),
	  m_bSeqFinished(false),
	  m_nSeqElapsedUs(0),
	  m_nSeqDurationUs(0),
	  m_nSeqEventCount(0),
	  m_nSeqFileSizeKB(0)
{
	s_pThis = this;
	m_szSeqCurrentFile[0] = '\0';
	memset(m_activeNotes, 0, sizeof(m_activeNotes));
	m_eMidiSource = static_cast<u8>(EMidiSource::Physical);
}

CMT32Pi::~CMT32Pi()
{
	delete m_pFluidSequencer;
}

bool CMT32Pi::Initialize(bool bSerialMIDIAvailable)
{
	m_bSerialMIDIAvailable = bSerialMIDIAvailable;
	m_bSerialMIDIEnabled = bSerialMIDIAvailable;

	switch (m_pConfig->LCDType)
	{
		case CConfig::TLCDType::HD44780FourBit:
			m_pLCD = new CHD44780FourBit(m_pConfig->LCDWidth, m_pConfig->LCDHeight);
			break;

		case CConfig::TLCDType::HD44780I2C:
			m_pLCD = new CHD44780I2C(m_pI2CMaster, m_pConfig->LCDI2CLCDAddress, m_pConfig->LCDWidth, m_pConfig->LCDHeight);
			break;

		case CConfig::TLCDType::SH1106I2C:
			m_pLCD = new CSH1106(m_pI2CMaster, m_pConfig->LCDI2CLCDAddress, m_pConfig->LCDWidth, m_pConfig->LCDHeight, m_pConfig->LCDRotation);
			break;

		case CConfig::TLCDType::SSD1306I2C:
			m_pLCD = new CSSD1306(m_pI2CMaster, m_pConfig->LCDI2CLCDAddress, m_pConfig->LCDWidth, m_pConfig->LCDHeight, m_pConfig->LCDRotation, m_pConfig->LCDMirror);
			break;

		default:
			break;
	}

	if (m_pLCD)
	{
		if (m_pLCD->Initialize())
		{
			CLogger::Get()->RegisterPanicHandler(PanicHandler);

			// Splash screen
			if (m_pLCD->GetType() == CLCD::TType::Graphical && !m_pConfig->SystemVerbose)
				m_pLCD->DrawImage(TImage::MT32PiLogo, true);
			else
			{
				const u8 nOffsetX = CUserInterface::CenterMessageOffset(*m_pLCD, MT32PiFullName);
				m_pLCD->Print(MT32PiFullName, nOffsetX, 0, false, true);
			}
		}
		else
		{
			LOGWARN("LCD init failed; invalid dimensions?");
			delete m_pLCD;
			m_pLCD = nullptr;
		}
	}

#if !defined(__aarch64__) || !defined(LEAVE_QEMU_ON_HALT)
	// The USB driver is not supported under 64-bit QEMU, so
	// the initialization must be skipped in this case, or an
	// exit happens here under 64-bit QEMU.
	LCDLog(TLCDLogType::Startup, "Init USB");
	if (m_pConfig->SystemUSB && m_pUSBHCI->Initialize())
	{
		m_bUSBAvailable = true;

		// Perform an initial Plug and Play update to initialize devices early
		UpdateUSB(true);
	}
#endif

	LCDLog(TLCDLogType::Startup, "Init Network");
	InitNetwork();

	if (m_pConfig->NetworkWebServer && m_pConfig->NetworkMode == CConfig::TNetworkMode::Off)
	{
		LOGWARN("Web server is enabled in config, but network mode is off; disabling web server");
		m_pConfig->NetworkWebServer = false;
	}

	// Check for Blokas Pisound, but only when not using 4-bit HD44780 (GPIO pin conflict)
	if (m_pConfig->LCDType != CConfig::TLCDType::HD44780FourBit)
	{
		m_pPisound = new CPisound(m_pSPIMaster, m_pGPIOManager, m_pConfig->AudioSampleRate);
		if (m_pPisound->Initialize())
		{
			LOGWARN("Blokas Pisound detected");
			m_pPisound->RegisterMIDIReceiveHandler(IRQMIDIReceiveHandler);
			m_bSerialMIDIEnabled = false;
		}
		else
		{
			delete m_pPisound;
			m_pPisound = nullptr;
		}
	}

	// Queue size of just one chunk
	unsigned int nQueueSize = m_pConfig->AudioChunkSize;
	TSoundFormat Format = TSoundFormat::SoundFormatSigned24;

	switch (m_pConfig->AudioOutputDevice)
	{
		case CConfig::TAudioOutputDevice::PWM:
			LCDLog(TLCDLogType::Startup, "Init audio (PWM)");
			m_pSound = new CPWMSoundBaseDevice(m_pInterrupt, m_pConfig->AudioSampleRate, m_pConfig->AudioChunkSize);
			break;

		case CConfig::TAudioOutputDevice::HDMI:
		{
			LCDLog(TLCDLogType::Startup, "Init audio (HDMI)");

			// Chunk size must be a multiple of 384
			const unsigned int nChunkSize = Utility::RoundToNearestMultiple(m_pConfig->AudioChunkSize, IEC958_SUBFRAMES_PER_BLOCK);
			nQueueSize = nChunkSize;

			m_pSound = new CHDMISoundBaseDevice(m_pInterrupt, m_pConfig->AudioSampleRate, nChunkSize);
			break;
		}

		case CConfig::TAudioOutputDevice::I2S:
		{
			LCDLog(TLCDLogType::Startup, "Init audio (I2S)");

			// Pisound provides clock
			const bool bSlave = m_pPisound != nullptr;

			// Don't probe if using Pisound
			CI2CMaster* const pI2CMaster = bSlave ? nullptr : m_pI2CMaster;

			m_pSound = new CI2SSoundBaseDevice(m_pInterrupt, m_pConfig->AudioSampleRate, m_pConfig->AudioChunkSize, bSlave, pI2CMaster);
			Format = TSoundFormat::SoundFormatSigned24_32;

			break;
		}
	}

	m_pSound->SetWriteFormat(Format);
	if (!m_pSound->AllocateQueueFrames(nQueueSize))
		LOGPANIC("Failed to allocate sound queue");

	LCDLog(TLCDLogType::Startup, "Init controls");
	if (m_pConfig->ControlScheme == CConfig::TControlScheme::SimpleButtons)
		m_pControl = new CControlSimpleButtons(m_EventQueue);
	else if (m_pConfig->ControlScheme == CConfig::TControlScheme::SimpleEncoder)
		m_pControl = new CControlSimpleEncoder(m_EventQueue, m_pConfig->ControlEncoderType, m_pConfig->ControlEncoderReversed);

	if (m_pControl && !m_pControl->Initialize())
	{
		LOGWARN("Control init failed");
		delete m_pControl;
		m_pControl = nullptr;
	}

	LCDLog(TLCDLogType::Startup, "Init mt32emu");
	InitMT32Synth();

	LCDLog(TLCDLogType::Startup, "Init FluidSynth");
	InitSoundFontSynth();

	// Set initial synthesizer
	if (m_pConfig->SystemDefaultSynth == CConfig::TSystemDefaultSynth::MT32)
		m_pCurrentSynth = m_pMT32Synth;
	else if (m_pConfig->SystemDefaultSynth == CConfig::TSystemDefaultSynth::SoundFont)
		m_pCurrentSynth = m_pSoundFontSynth;

	if (!m_pCurrentSynth)
	{
		LOGERR("Preferred synth failed to initialize successfully");

		// Activate any working synth
		if (m_pMT32Synth)
			m_pCurrentSynth = m_pMT32Synth;
		else if (m_pSoundFontSynth)
			m_pCurrentSynth = m_pSoundFontSynth;
		else
		{
			LOGPANIC("No synths available; ROMs/SoundFonts not found");
			return false;
		}
	}

	// Initialize MIDI Router + Audio Mixer
	SetupMixerRouting();

	if (m_pPisound)
		LOGNOTE("Using Pisound MIDI interface");
	else if (m_bSerialMIDIEnabled)
		LOGNOTE("Using serial MIDI interface");

	CCPUThrottle::Get()->DumpStatus();
	SetPowerSaveTimeout(m_pConfig->SystemPowerSaveTimeout);

	// Clear LCD
	if (m_pLCD)
		m_pLCD->Clear();

	// Start audio
	m_pSound->Start();

	// Start other cores
	if (!CMultiCoreSupport::Initialize())
		return false;

	return true;
}

bool CMT32Pi::InitNetwork()
{
	assert(m_pNet == nullptr);

	TNetDeviceType NetDeviceType = NetDeviceTypeUnknown;

	if (m_pConfig->NetworkMode == CConfig::TNetworkMode::WiFi)
	{
		LOGNOTE("Initializing Wi-Fi");

		if (m_WLAN.Initialize() && m_WPASupplicant.Initialize())
			NetDeviceType = NetDeviceTypeWLAN;
		else
			LOGERR("Failed to initialize Wi-Fi");
	}
	else if (m_pConfig->NetworkMode == CConfig::TNetworkMode::Ethernet)
	{
		LOGNOTE("Initializing Ethernet");
		NetDeviceType = NetDeviceTypeEthernet;
	}

	if (NetDeviceType != NetDeviceTypeUnknown)
	{
		if (m_pConfig->NetworkDHCP)
			m_pNet = new CNetSubSystem(0, 0, 0, 0, m_pConfig->NetworkHostname, NetDeviceType);
		else
			m_pNet = new CNetSubSystem(
				m_pConfig->NetworkIPAddress.Get(),
				m_pConfig->NetworkSubnetMask.Get(),
				m_pConfig->NetworkDefaultGateway.Get(),
				m_pConfig->NetworkDNSServer.Get(),
				m_pConfig->NetworkHostname,
				NetDeviceType
			);

		if (!m_pNet->Initialize(false))
		{
			LOGERR("Failed to initialize network subsystem");
			delete m_pNet;
			m_pNet = nullptr;
		}

		m_pNetDevice = CNetDevice::GetNetDevice(NetDeviceType);
	}

	return m_pNet != nullptr;
}

bool CMT32Pi::InitMT32Synth()
{
	assert(m_pMT32Synth == nullptr);

	m_pMT32Synth = new CMT32Synth(m_pConfig->AudioSampleRate, m_pConfig->MT32EmuGain, m_pConfig->MT32EmuReverbGain, m_pConfig->MT32EmuResamplerQuality);
	if (!m_pMT32Synth->Initialize())
	{
		LOGWARN("mt32emu init failed; no ROMs present?");
		delete m_pMT32Synth;
		m_pMT32Synth = nullptr;
		return false;
	}

	// Set initial MT-32 channel assignment from config
	if (m_pConfig->MT32EmuMIDIChannels == CMT32Synth::TMIDIChannels::Alternate)
		m_pMT32Synth->SetMIDIChannels(m_pConfig->MT32EmuMIDIChannels);

	// Set MT-32 reversed stereo option from config
	m_pMT32Synth->SetReversedStereo(m_pConfig->MT32EmuReversedStereo);

	m_pMT32Synth->SetUserInterface(&m_UserInterface);

	return true;
}

bool CMT32Pi::InitSoundFontSynth()
{
	assert(m_pSoundFontSynth == nullptr);

	m_pSoundFontSynth = new CSoundFontSynth(m_pConfig->AudioSampleRate);
	if (!m_pSoundFontSynth->Initialize())
	{
		LOGWARN("FluidSynth init failed; no SoundFonts present?");
		delete m_pSoundFontSynth;
		m_pSoundFontSynth = nullptr;
		return false;
	}

	m_pSoundFontSynth->SetUserInterface(&m_UserInterface);

	return true;
}

void CMT32Pi::FormatIPAddress(CString& Out) const
{
	Out = "Unavailable";

	if (!m_pNet)
		return;

	m_pNet->GetConfig()->GetIPAddress()->Format(&Out);
}

const char* CMT32Pi::GetActiveSynthName() const
{
	if (m_pCurrentSynth == m_pMT32Synth)
		return "MT-32";
	if (m_pCurrentSynth == m_pSoundFontSynth)
		return "SoundFont";
	return "Unavailable";
}

const char* CMT32Pi::GetCurrentMT32ROMName() const
{
	return m_pMT32Synth ? m_pMT32Synth->GetControlROMName() : "Unavailable";
}

const char* CMT32Pi::GetCurrentSoundFontName() const
{
	return m_pSoundFontSynth ? m_pSoundFontSynth->GetSoundFontManager().GetSoundFontName(m_pSoundFontSynth->GetSoundFontIndex()) : "Unavailable";
}

const char* CMT32Pi::GetCurrentSoundFontPath() const
{
	return m_pSoundFontSynth ? m_pSoundFontSynth->GetSoundFontManager().GetSoundFontPath(m_pSoundFontSynth->GetSoundFontIndex()) : "Unavailable";
}

size_t CMT32Pi::GetCurrentSoundFontIndex() const
{
	return m_pSoundFontSynth ? m_pSoundFontSynth->GetSoundFontIndex() : 0;
}

const char* CMT32Pi::GetSoundFontName(size_t nIndex) const
{
	return m_pSoundFontSynth ? m_pSoundFontSynth->GetSoundFontManager().GetSoundFontName(nIndex) : nullptr;
}

size_t CMT32Pi::GetSoundFontCount() const
{
	return m_pSoundFontSynth ? m_pSoundFontSynth->GetSoundFontManager().GetSoundFontCount() : 0;
}

int CMT32Pi::GetMT32ROMSetIndex() const
{
	return m_pMT32Synth ? static_cast<int>(m_pMT32Synth->GetROMSet()) : -1;
}

bool CMT32Pi::GetSoundFontFXState(bool& bReverbActive, float& nReverbRoomSize, float& nReverbLevel,
                                  float& nReverbDamping, float& nReverbWidth,
                                  bool& bChorusActive, float& nChorusDepth, float& nChorusLevel,
                                  int& nChorusVoices, float& nChorusSpeed, float& nGain) const
{
	if (!m_pSoundFontSynth)
		return false;

	bReverbActive   = m_pSoundFontSynth->GetReverbActive();
	nReverbRoomSize = m_pSoundFontSynth->GetReverbRoomSize();
	nReverbLevel    = m_pSoundFontSynth->GetReverbLevel();
	nReverbDamping  = m_pSoundFontSynth->GetReverbDamping();
	nReverbWidth    = m_pSoundFontSynth->GetReverbWidth();
	bChorusActive   = m_pSoundFontSynth->GetChorusActive();
	nChorusDepth    = m_pSoundFontSynth->GetChorusDepth();
	nChorusLevel    = m_pSoundFontSynth->GetChorusLevel();
	nChorusVoices   = m_pSoundFontSynth->GetChorusVoices();
	nChorusSpeed    = m_pSoundFontSynth->GetChorusSpeed();
	nGain           = m_pSoundFontSynth->GetGain();
	return true;
}

bool CMT32Pi::SetActiveSynth(TSynth Synth)
{
	SwitchSynth(Synth);

	if (Synth == TSynth::MT32)
		return m_pCurrentSynth == m_pMT32Synth;
	if (Synth == TSynth::SoundFont)
		return m_pCurrentSynth == m_pSoundFontSynth;

	return false;
}

bool CMT32Pi::SetMT32ROMSet(TMT32ROMSet ROMSet)
{
	if (!m_pMT32Synth)
		return false;

	if (ROMSet >= TMT32ROMSet::Any)
		return false;

	SwitchMT32ROMSet(ROMSet);
	return m_pMT32Synth->GetROMSet() == ROMSet;
}

bool CMT32Pi::SetSoundFontIndex(size_t nIndex)
{
	if (!m_pSoundFontSynth)
		return false;

	const size_t nSoundFontCount = m_pSoundFontSynth->GetSoundFontManager().GetSoundFontCount();
	if (nIndex >= nSoundFontCount)
		return false;

	SwitchSoundFont(nIndex);
	return m_pSoundFontSynth->GetSoundFontIndex() == nIndex;
}

bool CMT32Pi::SetMasterVolumePercent(int nVolume)
{
	SetMasterVolume(Utility::Clamp(nVolume, 0, 100));
	return true;
}

bool CMT32Pi::SetSoundFontReverbActive(bool bActive)
{
	if (!m_pSoundFontSynth)
		return false;

	m_pSoundFontSynth->SetReverbActive(bActive);
	return true;
}

bool CMT32Pi::SetSoundFontReverbRoomSize(float nRoomSize)
{
	if (!m_pSoundFontSynth)
		return false;

	m_pSoundFontSynth->SetReverbRoomSize(Utility::Clamp(nRoomSize, 0.0f, 1.0f));
	return true;
}

bool CMT32Pi::SetSoundFontReverbLevel(float nLevel)
{
	if (!m_pSoundFontSynth)
		return false;

	m_pSoundFontSynth->SetReverbLevel(Utility::Clamp(nLevel, 0.0f, 1.0f));
	return true;
}

bool CMT32Pi::SetSoundFontChorusActive(bool bActive)
{
	if (!m_pSoundFontSynth)
		return false;

	m_pSoundFontSynth->SetChorusActive(bActive);
	return true;
}

bool CMT32Pi::SetSoundFontChorusDepth(float nDepth)
{
	if (!m_pSoundFontSynth)
		return false;

	m_pSoundFontSynth->SetChorusDepth(Utility::Clamp(nDepth, 0.0f, 20.0f));
	return true;
}

bool CMT32Pi::SetSoundFontChorusLevel(float nLevel)
{
	if (!m_pSoundFontSynth)
		return false;

	m_pSoundFontSynth->SetChorusLevel(Utility::Clamp(nLevel, 0.0f, 1.0f));
	return true;
}

bool CMT32Pi::SetSoundFontChorusVoices(int nVoices)
{
	if (!m_pSoundFontSynth)
		return false;

	m_pSoundFontSynth->SetChorusVoices(Utility::Clamp(nVoices, 1, 99));
	return true;
}

bool CMT32Pi::SetSoundFontChorusSpeed(float nSpeed)
{
	if (!m_pSoundFontSynth)
		return false;

	m_pSoundFontSynth->SetChorusSpeed(Utility::Clamp(nSpeed, 0.29f, 5.0f));
	return true;
}

bool CMT32Pi::SetSoundFontReverbDamping(float nDamping)
{
	if (!m_pSoundFontSynth)
		return false;

	m_pSoundFontSynth->SetReverbDamping(Utility::Clamp(nDamping, 0.0f, 1.0f));
	return true;
}

bool CMT32Pi::SetSoundFontReverbWidth(float nWidth)
{
	if (!m_pSoundFontSynth)
		return false;

	m_pSoundFontSynth->SetReverbWidth(Utility::Clamp(nWidth, 0.0f, 100.0f));
	return true;
}

bool CMT32Pi::SetSoundFontGain(float nGain)
{
	if (!m_pSoundFontSynth)
		return false;

	m_pSoundFontSynth->SetGain(Utility::Clamp(nGain, 0.0f, 5.0f));
	return true;
}

bool CMT32Pi::SetSoundFontTuning(int nPreset)
{
	if (!m_pSoundFontSynth)
		return false;

	m_pSoundFontSynth->SetTuning(nPreset);
	return true;
}

int CMT32Pi::GetSoundFontTuning() const
{
	if (!m_pSoundFontSynth)
		return 0;

	return m_pSoundFontSynth->GetTuning();
}

const char* CMT32Pi::GetSoundFontTuningName() const
{
	return CSoundFontSynth::GetTuningName(GetSoundFontTuning());
}

bool CMT32Pi::SetSoundFontPolyphony(int nPolyphony)
{
	if (!m_pSoundFontSynth)
		return false;

	m_pSoundFontSynth->SetPolyphony(nPolyphony);
	return true;
}

int CMT32Pi::GetSoundFontPolyphony() const
{
	if (!m_pSoundFontSynth)
		return 0;

	return m_pSoundFontSynth->GetPolyphony();
}

bool CMT32Pi::SetSoundFontChannelType(int nChannel, int nType)
{
	if (!m_pSoundFontSynth)
		return false;

	m_pSoundFontSynth->SetChannelType(nChannel, nType);
	return true;
}

u16 CMT32Pi::GetSoundFontPercussionMask() const
{
	if (!m_pSoundFontSynth)
		return (1 << 9);

	return m_pSoundFontSynth->GetPercussionMask();
}

// ========== MT-32 Sound Parameters ==========
float CMT32Pi::GetMT32ReverbOutputGain() const
{
	if (!m_pMT32Synth)
		return 1.0f;

	return m_pMT32Synth->GetReverbOutputGain();
}

bool CMT32Pi::SetMT32ReverbOutputGain(float nGain)
{
	if (!m_pMT32Synth)
		return false;

	m_pMT32Synth->SetReverbOutputGain(Utility::Clamp(nGain, 0.0f, 4.0f));
	return true;
}

bool CMT32Pi::IsMT32ReverbActive() const
{
	if (!m_pMT32Synth)
		return false;

	return m_pMT32Synth->GetReverbEnabled();
}

bool CMT32Pi::SetMT32ReverbActive(bool bActive)
{
	if (!m_pMT32Synth)
		return false;

	m_pMT32Synth->SetReverbEnabled(bActive);
	return true;
}

bool CMT32Pi::IsMT32NiceAmpRamp() const
{
	if (!m_pMT32Synth)
		return false;

	return m_pMT32Synth->GetNiceAmpRamp();
}

bool CMT32Pi::SetMT32NiceAmpRamp(bool bEnabled)
{
	if (!m_pMT32Synth)
		return false;

	m_pMT32Synth->SetNiceAmpRamp(bEnabled);
	return true;
}

bool CMT32Pi::IsMT32NicePanning() const
{
	if (!m_pMT32Synth)
		return false;

	return m_pMT32Synth->GetNicePanning();
}

bool CMT32Pi::SetMT32NicePanning(bool bEnabled)
{
	if (!m_pMT32Synth)
		return false;

	m_pMT32Synth->SetNicePanning(bEnabled);
	return true;
}

bool CMT32Pi::IsMT32NicePartialMixing() const
{
	if (!m_pMT32Synth)
		return false;

	return m_pMT32Synth->GetNicePartialMixing();
}

bool CMT32Pi::SetMT32NicePartialMixing(bool bEnabled)
{
	if (!m_pMT32Synth)
		return false;

	m_pMT32Synth->SetNicePartialMixing(bEnabled);
	return true;
}

int CMT32Pi::GetMT32DACMode() const
{
	if (!m_pMT32Synth)
		return 0;

	return static_cast<int>(m_pMT32Synth->GetDACInputMode());
}

bool CMT32Pi::SetMT32DACMode(int nMode)
{
	if (!m_pMT32Synth)
		return false;

	if (nMode < 0 || nMode > 3)
		return false;

	m_pMT32Synth->SetDACInputMode(static_cast<MT32Emu::DACInputMode>(nMode));
	return true;
}

int CMT32Pi::GetMT32MIDIDelayMode() const
{
	if (!m_pMT32Synth)
		return 0;

	return static_cast<int>(m_pMT32Synth->GetMIDIDelayMode());
}

bool CMT32Pi::SetMT32MIDIDelayMode(int nMode)
{
	if (!m_pMT32Synth)
		return false;

	if (nMode < 0 || nMode > 2)
		return false;

	m_pMT32Synth->SetMIDIDelayMode(static_cast<MT32Emu::MIDIDelayMode>(nMode));
	return true;
}

int CMT32Pi::GetMT32AnalogMode() const
{
	if (!m_pMT32Synth)
		return 0;

	return static_cast<int>(m_pMT32Synth->GetAnalogOutputMode());
}

bool CMT32Pi::SetMT32AnalogMode(int nMode)
{
	if (!m_pMT32Synth)
		return false;

	if (nMode < 0 || nMode > 3)
		return false;

	return m_pMT32Synth->SetAnalogOutputMode(static_cast<MT32Emu::AnalogOutputMode>(nMode));
}

int CMT32Pi::GetMT32RendererType() const
{
	if (!m_pMT32Synth)
		return 0;

	return static_cast<int>(m_pMT32Synth->GetRendererType());
}

bool CMT32Pi::SetMT32RendererType(int nType)
{
	if (!m_pMT32Synth)
		return false;

	if (nType < 0 || nType > 1)
		return false;

	return m_pMT32Synth->SetRendererType(static_cast<MT32Emu::RendererType>(nType));
}

int CMT32Pi::GetMT32PartialCount() const
{
	if (!m_pMT32Synth)
		return 0;

	return static_cast<int>(m_pMT32Synth->GetPartialCount());
}

bool CMT32Pi::SetMT32PartialCount(int nCount)
{
	if (!m_pMT32Synth)
		return false;

	if (nCount < 8 || nCount > 256)
		return false;

	return m_pMT32Synth->SetPartialCount(static_cast<u32>(nCount));
}

void CMT32Pi::GetMIDIChannelLevels(float* pOutLevels, float* pOutPeaks) const
{
	if (!pOutLevels || !pOutPeaks)
		return;

	for (size_t nChannel = 0; nChannel < 16; ++nChannel)
	{
		pOutLevels[nChannel] = 0.0f;
		pOutPeaks[nChannel] = 0.0f;
	}

	// In mixer dual mode, merge levels from both synths
	if (m_bMixerEnabled && m_MIDIRouter.IsDualMode())
	{
		const unsigned nTicks = CTimer::GetClockTicks();
		float mt32Levels[16] = {}, mt32Peaks[16] = {};
		float fluidLevels[16] = {}, fluidPeaks[16] = {};

		if (m_pMT32Synth)
		{
			m_pMT32Synth->m_Lock.Acquire();
			m_pMT32Synth->m_MIDIMonitor.GetChannelLevels(nTicks, mt32Levels, mt32Peaks);
			m_pMT32Synth->m_Lock.Release();
		}
		if (m_pSoundFontSynth)
		{
			m_pSoundFontSynth->m_Lock.Acquire();
			m_pSoundFontSynth->m_MIDIMonitor.GetChannelLevels(nTicks, fluidLevels, fluidPeaks);
			m_pSoundFontSynth->m_Lock.Release();
		}

		for (size_t i = 0; i < 16; ++i)
		{
			pOutLevels[i] = Utility::Max(mt32Levels[i], fluidLevels[i]);
			pOutPeaks[i]  = Utility::Max(mt32Peaks[i],  fluidPeaks[i]);
		}
		return;
	}

	if (!m_pCurrentSynth)
		return;

	CSynthBase* const pSynth = m_pCurrentSynth;
	pSynth->m_Lock.Acquire();
	pSynth->m_MIDIMonitor.GetChannelLevels(CTimer::GetClockTicks(), pOutLevels, pOutPeaks);
	pSynth->m_Lock.Release();
}

void CMT32Pi::GetActiveNotes(u8 out[16][128]) const
{
	memcpy(out, m_activeNotes, sizeof(m_activeNotes));
}

CMT32Pi::TSystemState CMT32Pi::GetSystemState() const
{
	TSystemState s;

	s.Sequencer = GetSequencerStatus();
	s.Mixer     = GetMixerStatus();

	// Synth identity
	s.pActiveSynthName  = GetActiveSynthName();
	s.pMT32ROMName      = GetCurrentMT32ROMName();
	s.nMT32ROMSetIndex  = GetMT32ROMSetIndex();
	s.pSoundFontName    = GetCurrentSoundFontName();
	s.pSoundFontPath    = GetCurrentSoundFontPath();
	s.nSoundFontIndex   = GetCurrentSoundFontIndex();
	s.nSoundFontCount   = GetSoundFontCount();
	s.nMasterVolume     = GetMasterVolume();

	// SoundFont FX
	s.bSFFXAvailable = GetSoundFontFXState(
		s.bSFReverbActive, s.fSFReverbRoom, s.fSFReverbLevel,
		s.fSFReverbDamping, s.fSFReverbWidth,
		s.bSFChorusActive, s.fSFChorusDepth, s.fSFChorusLevel,
		s.nSFChorusVoices, s.fSFChorusSpeed, s.fSFGain);

	s.nSFTuning       = GetSoundFontTuning();
	s.pSFTuningName   = GetSoundFontTuningName();
	s.nSFPolyphony    = GetSoundFontPolyphony();
	s.nSFPercussionMask = GetSoundFontPercussionMask();

	// MT-32 parameters
	s.fMT32ReverbGain         = GetMT32ReverbOutputGain();
	s.bMT32ReverbActive       = IsMT32ReverbActive();
	s.bMT32NiceAmpRamp        = IsMT32NiceAmpRamp();
	s.bMT32NicePanning        = IsMT32NicePanning();
	s.bMT32NicePartialMixing  = IsMT32NicePartialMixing();
	s.nMT32DACMode            = GetMT32DACMode();
	s.nMT32MIDIDelayMode      = GetMT32MIDIDelayMode();
	s.nMT32AnalogMode         = GetMT32AnalogMode();
	s.nMT32RendererType       = GetMT32RendererType();
	s.nMT32PartialCount       = GetMT32PartialCount();

	// Network
	s.bNetworkReady         = m_bNetworkReady;
	s.pNetworkInterfaceName = GetNetworkDeviceShortName();
	{
		CString ip;
		FormatIPAddress(ip);
		strncpy(s.IPAddress, static_cast<const char*>(ip), sizeof(s.IPAddress) - 1);
		s.IPAddress[sizeof(s.IPAddress) - 1] = '\0';
	}

	// MIDI activity levels
	GetMIDIChannelLevels(s.MIDILevels, s.MIDIPeaks);

	return s;
}

void CMT32Pi::MainTask()
{
	CScheduler* const pScheduler = CScheduler::Get();

	LOGNOTE("Main task on Core 0 starting up");

	Awaken();

	while (m_bRunning)
	{
		// Process MIDI data
		UpdateMIDI();

		// Process network packets
		UpdateNetwork();

		// Update controls
		if (m_pControl)
			m_pControl->Update();

		// Process events
		ProcessEventQueue();

		const unsigned int nTicks = m_pTimer->GetTicks();

		// Update activity LED
		if (m_bLEDOn && (nTicks - m_nLEDOnTime) >= MSEC2HZ(LEDTimeoutMillis))
		{
			m_pActLED->Off();
			m_bLEDOn = false;
		}

		// Check for active sensing timeout
		if (m_bActiveSenseFlag && (nTicks > m_nActiveSenseTime) && (nTicks - m_nActiveSenseTime) >= MSEC2HZ(ActiveSenseTimeoutMillis))
		{
			m_pCurrentSynth->AllSoundOff();
			m_bActiveSenseFlag = false;
			LOGNOTE("Active sense timeout - turning notes off");
		}

		// Update power management
		if (m_pCurrentSynth->IsActive())
			Awaken();

#ifdef MONITOR_TEMPERATURE
		if (nTicks - m_nTempUpdateTime >= MSEC2HZ(5000))
		{
			const unsigned int nTemp = CCPUThrottle::Get()->GetTemperature();
			LOGDBG("Temperature: %dC", nTemp);
			LCDLog(TLCDLogType::Notice, "Temp: %dC", nTemp);
			m_nTempUpdateTime = nTicks;
		}
#endif

		CPower::Update();

		// Check for deferred SoundFont switch
		if (m_bDeferredSoundFontSwitchFlag)
		{
			// Delay switch if scrolling a long SoundFont name
			if (m_UserInterface.IsScrolling())
				m_nDeferredSoundFontSwitchTime = nTicks;
			else if ((nTicks - m_nDeferredSoundFontSwitchTime) >= static_cast<unsigned int>(m_pConfig->ControlSwitchTimeout) * HZ)
			{
				SwitchSoundFont(m_nDeferredSoundFontSwitchIndex);
				m_bDeferredSoundFontSwitchFlag = false;

				// Trigger an awaken so we don't immediately go to sleep
				Awaken();
			}
		}

		// Check for USB PnP events
		UpdateUSB();

		// Allow other tasks to run
		pScheduler->Yield();
	}


	// Stop audio
	m_pSound->Cancel();

	// Wait for UI task to finish
	while (!m_bUITaskDone)
		;
}

void CMT32Pi::UITask()
{
	LOGNOTE("UI task on Core 1 starting up");

	const bool bMisterEnabled = m_pConfig->ControlMister;

	// Nothing for this core to do; bail out
	if (!(m_pLCD || bMisterEnabled))
	{
		m_bUITaskDone = true;
		return;
	}

	// Display current MT-32 ROM version/SoundFont
	m_pCurrentSynth->ReportStatus();

	while (m_bRunning)
	{
		const unsigned int nTicks = CTimer::GetClockTicks();

		// Update LCD
		if (m_pLCD && (nTicks - m_nLCDUpdateTime) >= Utility::MillisToTicks(LCDUpdatePeriodMillis))
		{
			m_UserInterface.Update(*m_pLCD, *m_pCurrentSynth, nTicks);
			m_nLCDUpdateTime = nTicks;
		}

		// Poll MiSTer interface
		if (bMisterEnabled && (nTicks - m_nMisterUpdateTime) >= Utility::MillisToTicks(MisterUpdatePeriodMillis))
		{
			TMisterStatus Status{TMisterSynth::Unknown, 0xFF, 0xFF};

			if (m_pCurrentSynth == m_pMT32Synth)
				Status.Synth = TMisterSynth::MT32;
			else if (m_pCurrentSynth == m_pSoundFontSynth)
				Status.Synth = TMisterSynth::SoundFont;

			if (m_pMT32Synth)
				Status.MT32ROMSet = static_cast<u8>(m_pMT32Synth->GetROMSet());

			if (m_pSoundFontSynth)
				Status.SoundFontIndex = m_pSoundFontSynth->GetSoundFontIndex();

			m_MisterControl.Update(Status);
			m_nMisterUpdateTime = nTicks;
		}
	}

	// Clear screen
	if (m_pLCD)
		m_pLCD->Clear();

	m_bUITaskDone = true;
}

// AudioTask — runs exclusively on Core 2.
//
// Timing model:
//   Each iteration produces nFrames of audio to keep the DMA queue full.
//   nDeadline (µs) = nFrames / SampleRate — the hard real-time budget.
//   The entire render (synth + optional mixer) must complete before the
//   hardware DMA underruns.  m_nRenderAvgUs (EMA, 1/16 alpha) is exposed
//   to Core 0 for monitoring via GetMixerStatus() / GetSystemState().
void CMT32Pi::AudioTask()
{
	LOGNOTE("Audio task on Core 2 starting up");

	constexpr u8 nChannels = 2;

	// Circle's "fast path" for I2S 24-bit really expects 32-bit samples
	const bool bI2S = m_pConfig->AudioOutputDevice == CConfig::TAudioOutputDevice::I2S;
	const bool bReversedStereo = m_pConfig->AudioReversedStereo;
	const u8 nBytesPerSample = bI2S ? sizeof(s32) : (sizeof(s8) * 3);
	const u8 nBytesPerFrame = 2 * nBytesPerSample;

	const size_t nQueueSizeFrames = m_pSound->GetQueueSizeFrames();

	// Extra byte so that we can write to the 24-bit buffer with overlapping 32-bit writes (efficiency)
	float FloatBuffer[nQueueSizeFrames * nChannels];
s8 IntBuffer[nQueueSizeFrames * nBytesPerFrame + (bI2S ? 0 : 1)];

	while (m_bRunning)
	{
		const size_t nFrames = nQueueSizeFrames - m_pSound->GetQueueFramesAvail();
		const size_t nWriteBytes = nFrames * nBytesPerFrame;

		// Compute deadline for this chunk (µs)
		const unsigned nDeadline = static_cast<unsigned>(nFrames * 1000000ULL / m_pConfig->AudioSampleRate);
		m_nDeadlineUs = nDeadline;

		// Measure render time
		const unsigned nStart = CTimer::GetClockTicks();

		if (m_bMixerEnabled)
			m_AudioMixer.Render(FloatBuffer, nFrames);
		else
			m_pCurrentSynth->Render(FloatBuffer, nFrames);

		const unsigned nElapsed = CTimer::GetClockTicks() - nStart;
		m_nRenderUs = nElapsed;

		// Exponential moving average (alpha ≈ 1/16)
		m_nRenderAvgUs = m_nRenderAvgUs - (m_nRenderAvgUs >> 4) + (nElapsed >> 4);

		// Track peak
		if (nElapsed > m_nRenderPeakUs)
			m_nRenderPeakUs = nElapsed;

		// Auto polyphony reduction: if avg > 90% of deadline, halve MT-32 partials
		if (m_bAutoReducePartials && nDeadline > 0 && m_nRenderAvgUs > (nDeadline * 9 / 10))
		{
			const int nCurrent = GetMT32PartialCount();
			if (nCurrent > 8)
				SetMT32PartialCount(nCurrent / 2);
			m_bAutoReducePartials = false;  // one-shot per overload episode
		}

		if (bReversedStereo)
		{
			// Convert to signed 24-bit integers with channel swap
			for (size_t i = 0; i < nFrames * nChannels; i += nChannels)
			{
				s32* const pLeftSample = reinterpret_cast<s32*>(IntBuffer + i * nBytesPerSample);
				s32* const pRightSample = reinterpret_cast<s32*>(IntBuffer + (i + 1) * nBytesPerSample);
				*pLeftSample = FloatBuffer[i + 1] * Sample24BitMax;
				*pRightSample = FloatBuffer[i] * Sample24BitMax;
			}
		}
		else
		{
			// Convert to signed 24-bit integers
			for (size_t i = 0; i < nFrames * nChannels; ++i)
			{
				s32* const pSample = reinterpret_cast<s32*>(IntBuffer + i * nBytesPerSample);
				*pSample = FloatBuffer[i] * Sample24BitMax;
			}
		}

		const int nResult = m_pSound->Write(IntBuffer, nWriteBytes);
		if (nResult != static_cast<int>(nWriteBytes))
			LOGERR("Sound data dropped");
	}
}

void CMT32Pi::SequencerPlayFile(const char* pPath)
{
	if (!pPath || !*pPath)
		return;

	// Requires FluidSynth available (for fluid_player MIDI parsing)
	if (!m_pSoundFontSynth)
	{
		LOGWARN("SequencerPlayFile: FluidSynth not available");
		return;
	}

	// Allocate FluidSequencer once
	if (!m_pFluidSequencer)
	{
		m_pFluidSequencer = new CFluidSequencer();
		if (!m_pFluidSequencer)
			return;
		if (!m_pFluidSequencer->Initialize(m_pSoundFontSynth->GetFluidSynth()))
		{
			delete m_pFluidSequencer;
			m_pFluidSequencer = nullptr;
			LOGWARN("FluidSequencer init failed");
			return;
		}
	}

	// Play file via fluid_player
	if (!m_pFluidSequencer->Play(pPath))
	{
		LOGWARN("FluidSequencer: failed to play %s", pPath);
		return;
	}

	// Set loop mode: FluidSynth uses 1 = play once, -1 = infinite
	m_pFluidSequencer->SetLoop(m_bSeqLoopEnabled ? -1 : 1);

	// Update status fields
	__builtin_strncpy(m_szSeqCurrentFile, pPath, SeqPathMax - 1);
	m_szSeqCurrentFile[SeqPathMax - 1] = '\0';
	m_nSeqEventCount = 0;
	m_nSeqDurationUs = 0;
	m_nSeqElapsedUs  = 0;

	// Get file size in KB
	{
		FILINFO fno;
		m_nSeqFileSizeKB = (f_stat(pPath, &fno) == FR_OK) ? static_cast<u32>(fno.fsize / 1024) : 0;
	}

	m_bSeqIsPlaying  = true;
	m_bSeqFinished   = false;
	m_nTempoMultiplier = 1.0;

	// Show LCD notice
	const char* pBaseName = strrchr(pPath, '/');
	pBaseName = pBaseName ? pBaseName + 1 : pPath;
	LCDLog(TLCDLogType::Notice, "[SEQ] %s", pBaseName);
}

void CMT32Pi::SequencerStop()
{
	if (m_pFluidSequencer)
	{
		m_pFluidSequencer->Stop();
		m_bSeqIsPlaying = false;
		m_bSeqFinished  = false;
	}
}

CMT32Pi::TSequencerStatus CMT32Pi::GetSequencerStatus() const
{
	TSequencerStatus s;
	s.bLoopEnabled  = m_bSeqLoopEnabled;
	s.pFile         = m_szSeqCurrentFile;
	s.nFileSizeKB   = m_nSeqFileSizeKB;

	if (m_pFluidSequencer)
	{
		s.bPlaying         = m_pFluidSequencer->IsPlaying();
		s.bFinished        = m_pFluidSequencer->IsFinished();
		s.nEventCount      = 0;
		s.nCurrentTick     = m_pFluidSequencer->GetCurrentTick();
		s.nTotalTicks      = m_pFluidSequencer->GetTotalTicks();
		s.nBPM             = m_pFluidSequencer->GetBPM();
		s.nDivision        = m_pFluidSequencer->GetDivision();
		s.nTempoMultiplier = m_nTempoMultiplier;
		s.pDiag            = m_pFluidSequencer->GetDiag();

		// Estimate duration/elapsed from ticks and BPM
		const int nBPM = s.nBPM > 0 ? s.nBPM : 120;
		const int nDiv = s.nDivision > 0 ? s.nDivision : 480;
		const double nMsPerTick = 60000.0 / (nBPM * nDiv);
		s.nDurationMs = static_cast<u32>(s.nTotalTicks * nMsPerTick);
		s.nElapsedMs  = static_cast<u32>(s.nCurrentTick * nMsPerTick);
	}
	else
	{
		s.bPlaying         = false;
		s.bFinished        = false;
		s.nEventCount      = 0;
		s.nDurationMs      = 0;
		s.nElapsedMs       = 0;
		s.nCurrentTick     = 0;
		s.nTotalTicks      = 0;
		s.nBPM             = 0;
		s.nDivision        = 0;
		s.nTempoMultiplier = 1.0;
		s.pDiag            = nullptr;
	}

	return s;
}

void CMT32Pi::SetSequencerLoop(bool bLoop)
{
	m_bSeqLoopEnabled = bLoop;
	if (m_pFluidSequencer)
		m_pFluidSequencer->SetLoop(bLoop ? -1 : 0);
}

bool CMT32Pi::SequencerSeek(int nTicks)
{
	if (!m_pFluidSequencer)
		return false;
	return m_pFluidSequencer->Seek(nTicks);
}

bool CMT32Pi::SequencerSetTempoMultiplier(double nMultiplier)
{
	if (!m_pFluidSequencer)
		return false;
	m_nTempoMultiplier = nMultiplier;
	return m_pFluidSequencer->SetTempoMultiplier(nMultiplier);
}

bool CMT32Pi::SequencerSetTempoBPM(double nBPM)
{
	if (!m_pFluidSequencer)
		return false;
	return m_pFluidSequencer->SetTempoBPM(nBPM);
}

void CMT32Pi::SendRawMIDI(const u8* pData, size_t nSize)
{
	m_WebMIDIRxBuffer.Enqueue(pData, nSize);
}

void CMT32Pi::GetMIDIFileListJSON(CString& outJSON) const
{
	outJSON = "[";
	bool bFirst = true;

	const char* const Dirs[] = { "SD:", "USB:" };
	for (const char* pDir : Dirs)
	{
		DIR dir;
		if (f_opendir(&dir, pDir) != FR_OK)
			continue;

		FILINFO fi;
		while (f_readdir(&dir, &fi) == FR_OK && fi.fname[0])
		{
			const size_t nLen = strlen(fi.fname);
			if (nLen < 5)
				continue;
			const char* pExt = fi.fname + nLen - 4;
			if (strcasecmp(pExt, ".mid") != 0 && strcasecmp(pExt, ".midi") != 0)
				continue;

			if (!bFirst) outJSON += ",";
			bFirst = false;

			char szFull[SeqPathMax];
			snprintf(szFull, sizeof(szFull), "%s/%s", pDir, fi.fname);

			outJSON += "\""; 
			outJSON += szFull;
			outJSON += "\"";
		}
		f_closedir(&dir);
	}

	outJSON += "]";
}

void CMT32Pi::Run(unsigned nCore)
{
	// Assign tasks to different CPU cores
	switch (nCore)
	{
		case 0:
			return MainTask();

		case 1:
			return UITask();

		case 2:
			return AudioTask();

		default:
			break;
	}
}

void CMT32Pi::OnEnterPowerSavingMode()
{
	CPower::OnEnterPowerSavingMode();
	m_pSound->Cancel();
	m_UserInterface.EnterPowerSavingMode();
}

void CMT32Pi::OnExitPowerSavingMode()
{
	CPower::OnExitPowerSavingMode();
	m_pSound->Start();
	m_UserInterface.ExitPowerSavingMode();
}

void CMT32Pi::OnThrottleDetected()
{
	CPower::OnThrottleDetected();
	LCDLog(TLCDLogType::Warning, "CPU throttl! Chk PSU");
}

void CMT32Pi::OnUnderVoltageDetected()
{
	CPower::OnUnderVoltageDetected();
	LCDLog(TLCDLogType::Warning, "Low voltage! Chk PSU");
}

void CMT32Pi::OnShortMessage(u32 nMessage)
{
	// Active sensing
	if (nMessage == 0xFE)
	{
		m_bActiveSenseFlag = true;
		return;
	}

	// Track active notes for WebSocket snapshot
	const u8 status = nMessage & 0xFF;
	const u8 type   = status & 0xF0;
	const u8 ch     = status & 0x0F;
	if (type == 0x90)
	{
		const u8 note = (nMessage >> 8) & 0x7F;
		const u8 vel  = (nMessage >> 16) & 0x7F;
		m_activeNotes[ch][note] = (vel > 0) ? m_eMidiSource : 0;
	}
	else if (type == 0x80)
	{
		const u8 note = (nMessage >> 8) & 0x7F;
		m_activeNotes[ch][note] = 0;
	}

	// Flash LED for channel messages
	if ((nMessage & 0xFF) < 0xF0)
		LEDOn();

	if (m_bMixerEnabled)
		m_MIDIRouter.RouteShortMessage(nMessage);
	else
		m_pCurrentSynth->HandleMIDIShortMessage(nMessage);

	// Wake from power saving mode if necessary
	Awaken();
}

void CMT32Pi::OnSysExMessage(const u8* pData, size_t nSize)
{
	// Flash LED
	LEDOn();

	// If we don't consume the SysEx message, forward it to the synthesizer
	if (!ParseCustomSysEx(pData, nSize))
	{
		if (m_bMixerEnabled)
			m_MIDIRouter.RouteSysEx(pData, nSize);
		else
			m_pCurrentSynth->HandleMIDISysExMessage(pData, nSize);
	}

	// Wake from power saving mode if necessary
	Awaken();
}

void CMT32Pi::OnUnexpectedStatus()
{
	CMIDIParser::OnUnexpectedStatus();
	if (m_pConfig->SystemVerbose)
		LCDLog(TLCDLogType::Warning, "Unexp. MIDI status!");
}

void CMT32Pi::OnSysExOverflow()
{
	CMIDIParser::OnSysExOverflow();
	LCDLog(TLCDLogType::Error, "SysEx overflow!");
}

void CMT32Pi::OnAppleMIDIConnect(const CIPAddress* pIPAddress, const char* pName)
{
	if (!m_pLCD)
		return;

	LCDLog(TLCDLogType::Notice, "%s connected!", pName);
}

void CMT32Pi::OnAppleMIDIDisconnect(const CIPAddress* pIPAddress, const char* pName)
{
	if (!m_pLCD)
		return;

	LCDLog(TLCDLogType::Notice, "%s disconnected!", pName);
}

bool CMT32Pi::ParseCustomSysEx(const u8* pData, size_t nSize)
{
	if (nSize < 4)
		return false;

	// 'Educational' manufacturer
	if (pData[1] != 0x7D)
		return false;

	const auto Command = static_cast<TCustomSysExCommand>(pData[2]);

	// Reboot (F0 7D 00 F7)
	if (nSize == 4 && Command == TCustomSysExCommand::Reboot)
	{
		LOGNOTE("Reboot command received");
		m_bRunning = false;
		return true;
	}

	if (nSize != 5)
		return false;

	const u8 nParameter = pData[3];
	switch (Command)
	{
		// Switch MT-32 ROM set (F0 7D 01 xx F7)
		case TCustomSysExCommand::SwitchMT32ROMSet:
		{
			const TMT32ROMSet NewROMSet = static_cast<TMT32ROMSet>(nParameter);
			if (NewROMSet < TMT32ROMSet::Any)
				SwitchMT32ROMSet(NewROMSet);
			return true;
		}

		// Switch SoundFont (F0 7D 02 xx F7)
		case TCustomSysExCommand::SwitchSoundFont:
			SwitchSoundFont(nParameter);
			return true;

		// Switch synthesizer (F0 7D 03 xx F7)
		case TCustomSysExCommand::SwitchSynth:
		{
			SwitchSynth(static_cast<TSynth>(nParameter));
			return true;
		}

		// Swap MT-32 stereo channels (F0 7D 04 xx F7)
		case TCustomSysExCommand::SetMT32ReversedStereo:
		{
			if (m_pMT32Synth)
				m_pMT32Synth->SetReversedStereo(nParameter);
			return true;
		}

		default:
			return false;
	}
}

void CMT32Pi::UpdateUSB(bool bStartup)
{
	if (!m_bUSBAvailable || !m_pUSBHCI->UpdatePlugAndPlay())
		return;

	Awaken();

	CUSBBulkOnlyMassStorageDevice* pUSBMassStorageDevice = static_cast<CUSBBulkOnlyMassStorageDevice*>(CDeviceNameService::Get()->GetDevice("umsd1", TRUE));

	if (!m_pUSBMassStorageDevice && pUSBMassStorageDevice)
	{
		// USB disk was attached
		LOGNOTE("USB mass storage device attached");

		if (f_mount(&m_USBFileSystem, "USB:", 1) != FR_OK)
			LOGERR("Failed to mount USB mass storage device");
		else
		{
			if (!bStartup)
			{
				LCDLog(TLCDLogType::Spinner, "MT-32 ROM rescan");
				if (m_pMT32Synth)
					m_pMT32Synth->GetROMManager().ScanROMs();
				else
					InitMT32Synth();

				LCDLog(TLCDLogType::Spinner, "SoundFont rescan");
				if (m_pSoundFontSynth)
					m_pSoundFontSynth->GetSoundFontManager().ScanSoundFonts();
				else
					InitSoundFontSynth();

				if (m_pSoundFontSynth)
					LCDLog(TLCDLogType::Notice, "%d SoundFonts avail", m_pSoundFontSynth->GetSoundFontManager().GetSoundFontCount());
			}
		}
	}
	else if (m_pUSBMassStorageDevice && !pUSBMassStorageDevice)
	{
		// USB disk was removed
		LOGNOTE("USB mass storage device removed");

		f_unmount("USB:");

		// Only need to rescan SoundFonts on storage removal; MT-32 ROMs are kept in memory
		if (m_pSoundFontSynth)
		{
			LCDLog(TLCDLogType::Spinner, "SoundFont rescan");
			m_pSoundFontSynth->GetSoundFontManager().ScanSoundFonts();
			LCDLog(TLCDLogType::Notice, "%d SoundFonts avail", m_pSoundFontSynth->GetSoundFontManager().GetSoundFontCount());
		}
	}
	m_pUSBMassStorageDevice = pUSBMassStorageDevice;

	if (!m_pUSBMIDIDevice && (m_pUSBMIDIDevice = static_cast<CUSBMIDIDevice*>(CDeviceNameService::Get()->GetDevice("umidi1", FALSE))))
	{
		m_pUSBMIDIDevice->RegisterRemovedHandler(USBMIDIDeviceRemovedHandler, &m_pUSBMIDIDevice);
		m_pUSBMIDIDevice->RegisterPacketHandler(USBMIDIPacketHandler);
		LOGNOTE("Using USB MIDI interface");
		m_bSerialMIDIEnabled = false;
	}

	if (!m_pUSBSerialDevice && (m_pUSBSerialDevice = static_cast<CUSBSerialDevice*>(CDeviceNameService::Get()->GetDevice("utty1", FALSE))))
	{
		m_pUSBSerialDevice->SetBaudRate(m_pConfig->MIDIUSBSerialBaudRate);
		m_pUSBSerialDevice->RegisterRemovedHandler(USBMIDIDeviceRemovedHandler, &m_pUSBSerialDevice);
		LOGNOTE("Using USB serial interface");
		m_bSerialMIDIEnabled = false;
	}
}

void CMT32Pi::UpdateNetwork()
{
	if (!m_pNet)
		return;

	bool bNetIsRunning = false;
	if (m_pConfig->NetworkMode == CConfig::TNetworkMode::Ethernet)
	{
		if (m_pConfig->NetworkDHCP)
			bNetIsRunning = !m_pNet->GetConfig()->GetIPAddress()->IsNull();
		else
			bNetIsRunning = true;
	}
	else
		bNetIsRunning = m_pNet->IsRunning();

	if (!m_bNetworkReady && bNetIsRunning)
	{
		m_bNetworkReady = true;

		CString IPString;
		m_pNet->GetConfig()->GetIPAddress()->Format(&IPString);

		LOGNOTE("Network up and running at: %s", static_cast<const char *>(IPString));
		LCDLog(TLCDLogType::Notice, "%s: %s", GetNetworkDeviceShortName(), static_cast<const char*>(IPString));

		if (m_pConfig->NetworkRTPMIDI && !m_pAppleMIDIParticipant)
		{
			m_pAppleMIDIParticipant = new CAppleMIDIParticipant(&m_Random, this);
			if (!m_pAppleMIDIParticipant->Initialize())
			{
				LOGERR("Failed to init AppleMIDI receiver");
				delete m_pAppleMIDIParticipant;
				m_pAppleMIDIParticipant = nullptr;
			}
			else
				LOGNOTE("AppleMIDI receiver initialized");
		}

		if (m_pConfig->NetworkUDPMIDI && !m_pUDPMIDIReceiver)
		{
			m_pUDPMIDIReceiver = new CUDPMIDIReceiver(this);
			if (!m_pUDPMIDIReceiver->Initialize())
			{
				LOGERR("Failed to init UDP MIDI receiver");
				delete m_pUDPMIDIReceiver;
				m_pUDPMIDIReceiver = nullptr;
			}
			else
				LOGNOTE("UDP MIDI receiver initialized");
		}

		if (m_pConfig->NetworkFTPServer && !m_pFTPDaemon)
		{
			m_pFTPDaemon = new CFTPDaemon(m_pConfig->NetworkFTPUsername, m_pConfig->NetworkFTPPassword);
			if (!m_pFTPDaemon->Initialize())
			{
				LOGERR("Failed to init FTP daemon");
				delete m_pFTPDaemon;
				m_pFTPDaemon = nullptr;
			}
			else
				LOGNOTE("FTP daemon initialized");
		}

		if (m_pConfig->NetworkWebServer && !m_pWebDaemon)
		{
			const int nRequestedPort = m_pConfig->NetworkWebServerPort;
			const int nPort = Utility::Clamp(nRequestedPort, 1, 65535);

			if (nRequestedPort != nPort)
				LOGWARN("Invalid web_port %d, clamping to %d", nRequestedPort, nPort);

			m_pWebDaemon = new CWebDaemon(m_pNet, this, static_cast<u16>(nPort));
			LOGNOTE("HTTP web daemon initialized on port %d", nPort);
		}

		if (m_pConfig->NetworkWebServer && !m_pWebSocketDaemon)
		{
			m_pWebSocketDaemon = new CWebSocketDaemon(m_pNet, this, 8765);
			LOGNOTE("WebSocket daemon initialized on port 8765");
		}
	}
	else if (m_bNetworkReady && !bNetIsRunning)
	{
		m_bNetworkReady = false;
		LOGNOTE("Network disconnected.");
		LCDLog(TLCDLogType::Notice, "%s disconnected!", GetNetworkDeviceShortName());

	}
}

// UpdateMIDI — called every iteration of the Core 0 main loop.
//
// MIDI ingestion order (each feed calls ParseMIDIBytes() → CMIDIRouter → synth):
//   1. Physical MIDI in (serial GPIO / USB serial / USB MIDI HCI)
//      → reads from hardware or m_MIDIRxBuffer (filled by IRQ handler)
//   2. FluidSequencer::Tick() advances the player and produces note events
//      → DrainMIDIBytes() feeds them into ParseMIDIBytes() one chunk at a time
//      Also updates m_bSeqFinished / m_bSeqIsPlaying when the song ends.
//   3. Web keyboard bytes injected via HTTP handler (also Core 0, cooperative)
//      → m_WebMIDIRxBuffer, drained last to respect physical MIDI priority
//
// m_eMidiSource is set per-batch so OnShortMessage() can tag active notes
// with their origin (Physical / Player / WebUI).
void CMT32Pi::UpdateMIDI()
{
	size_t nBytes;
	u8 Buffer[MIDIRxBufferSize];

	// Read MIDI messages from serial device or ring buffer
	if (m_bSerialMIDIEnabled)
		nBytes = ReceiveSerialMIDI(Buffer, sizeof(Buffer));
	else if (m_pUSBSerialDevice)
	{
		const int nResult = m_pUSBSerialDevice->Read(Buffer, sizeof(Buffer));
		nBytes = nResult > 0 ? static_cast<size_t>(nResult) : 0;
	}
	else
		nBytes = m_MIDIRxBuffer.Dequeue(Buffer, sizeof(Buffer));

	if (nBytes > 0)
	{
		m_eMidiSource = static_cast<u8>(EMidiSource::Physical);
		ParseMIDIBytes(Buffer, nBytes);
		s_pThis->m_nActiveSenseTime = s_pThis->m_pTimer->GetTicks();
	}

	// Drive FluidSequencer player from Core 0 and drain produced MIDI bytes
	if (m_pFluidSequencer)
	{
		m_pFluidSequencer->Tick();

		while ((nBytes = m_pFluidSequencer->DrainMIDIBytes(Buffer, sizeof(Buffer))) > 0)
		{
			m_eMidiSource = static_cast<u8>(EMidiSource::Player);
			ParseMIDIBytes(Buffer, nBytes);
			s_pThis->m_nActiveSenseTime = s_pThis->m_pTimer->GetTicks();
		}

		// Check if FluidSequencer finished playing (only fires when loop is off)
		if (m_pFluidSequencer->IsFinished() && !m_bSeqFinished)
		{
			m_bSeqFinished  = true;
			m_bSeqIsPlaying = false;
		}
	}

	// Drain web keyboard bytes (injected from web handler context)
	while ((nBytes = m_WebMIDIRxBuffer.Dequeue(Buffer, sizeof(Buffer))) > 0)
	{
		m_eMidiSource = static_cast<u8>(EMidiSource::WebUI);
		ParseMIDIBytes(Buffer, nBytes);
		s_pThis->m_nActiveSenseTime = s_pThis->m_pTimer->GetTicks();
	}
}

void CMT32Pi::PurgeMIDIBuffers()
{
	size_t nBytes;
	u8 Buffer[MIDIRxBufferSize];

	// Process MIDI messages from all devices/ring buffers, but ignore note-ons
	while (m_bSerialMIDIEnabled && (nBytes = ReceiveSerialMIDI(Buffer, sizeof(Buffer))) > 0)
		ParseMIDIBytes(Buffer, nBytes, true);

	while (m_pUSBSerialDevice && (nBytes = m_pUSBSerialDevice->Read(Buffer, sizeof(Buffer))) > 0)
		ParseMIDIBytes(Buffer, nBytes, true);

	while ((nBytes = m_MIDIRxBuffer.Dequeue(Buffer, sizeof(Buffer))) > 0)
		ParseMIDIBytes(Buffer, nBytes, true);

	while ((nBytes = m_WebMIDIRxBuffer.Dequeue(Buffer, sizeof(Buffer))) > 0)
		ParseMIDIBytes(Buffer, nBytes, true);
}

size_t CMT32Pi::ReceiveSerialMIDI(u8* pOutData, size_t nSize)
{
	// Read serial MIDI data
	int nResult = m_pSerial->Read(pOutData, nSize);

	// No data
	if (nResult == 0)
		return 0;

	// Error
	if (nResult < 0)
	{
		if (m_pConfig->SystemVerbose)
		{
			const char* pErrorString;
			switch (nResult)
			{
				case -SERIAL_ERROR_BREAK:
					pErrorString = "UART break error!";
					break;

				case -SERIAL_ERROR_OVERRUN:
					pErrorString = "UART overrun error!";
					break;

				case -SERIAL_ERROR_FRAMING:
					pErrorString = "UART framing error!";
					break;

				default:
					pErrorString = "Unknown UART error!";
					break;
			}

			LOGWARN(pErrorString);
			LCDLog(TLCDLogType::Warning, pErrorString);
		}

		return 0;
	}

	// Replay received MIDI data out via the serial port ('software thru')
	if (m_pConfig->MIDIGPIOThru)
	{
		int nSendResult = m_pSerial->Write(pOutData, nResult);
		if (nSendResult != nResult)
		{
			LOGERR("received %d bytes, but only sent %d bytes", nResult, nSendResult);
			LCDLog(TLCDLogType::Error, "UART TX error!");
		}
	}

	return static_cast<size_t>(nResult);
}

void CMT32Pi::ProcessEventQueue()
{
	TEvent Buffer[EventQueueSize];
	const size_t nEvents = m_EventQueue.Dequeue(Buffer, sizeof(Buffer));

	// We got some events, wake up
	if (nEvents > 0)
		Awaken();

	for (size_t i = 0; i < nEvents; ++i)
	{
		const TEvent& Event = Buffer[i];

		switch (Event.Type)
		{
			case TEventType::Button:
				ProcessButtonEvent(Event.Button);
				break;

			case TEventType::SwitchSynth:
				SwitchSynth(Event.SwitchSynth.Synth);
				break;

			case TEventType::SwitchMT32ROMSet:
				SwitchMT32ROMSet(Event.SwitchMT32ROMSet.ROMSet);
				break;

			case TEventType::SwitchSoundFont:
				DeferSwitchSoundFont(Event.SwitchSoundFont.Index);
				break;

			case TEventType::AllSoundOff:
				if (m_pMT32Synth)
					m_pMT32Synth->AllSoundOff();
				if (m_pSoundFontSynth)
					m_pSoundFontSynth->AllSoundOff();
				break;

			case TEventType::DisplayImage:
				m_UserInterface.DisplayImage(Event.DisplayImage.Image);
				break;

			case TEventType::Encoder:
				if (m_UserInterface.IsInMenu())
					m_UserInterface.MenuEncoderEvent(Event.Encoder.nDelta);
				else
					SetMasterVolume(m_nMasterVolume + Event.Encoder.nDelta);
				break;
		}
	}
}

void CMT32Pi::ProcessButtonEvent(const TButtonEvent& Event)
{
	if (Event.Button == TButton::EncoderButton)
	{
		if (Event.bPressed && Event.bRepeat && !m_bMenuLongPressConsumed)
		{
			// Long press: enter or exit menu
			m_bMenuLongPressConsumed = true;
			if (m_UserInterface.IsInMenu())
				m_UserInterface.MenuBackEvent();
			else
				m_UserInterface.EnterMenu(m_pSoundFontSynth, m_pMT32Synth, m_pCurrentSynth, this);
		}
		else if (!Event.bPressed)
		{
			if (!m_bMenuLongPressConsumed && m_UserInterface.IsInMenu())
				m_UserInterface.MenuSelectEvent();
			m_bMenuLongPressConsumed = false;
		}
		return;
	}

	if (!Event.bPressed)
		return;

	if (Event.Button == TButton::Button1 && !Event.bRepeat)
	{
		if (m_bMixerEnabled)
		{
			// Cycle through mixer presets
			static const char* PresetNames[] = {
				"All MT-32",
				"All FluidSynth",
				"Split GM",
				"Custom"
			};
			const int nCurrent = static_cast<int>(m_MIDIRouter.GetPreset());
			const int nNext = (nCurrent + 1) % 4;
			SetMixerPreset(nNext);
			LOGNOTE("Mixer preset: %s", PresetNames[nNext]);
			LCDLog(TLCDLogType::Notice, PresetNames[nNext]);
		}
		else
		{
			// Swap synths
			if (m_pCurrentSynth == m_pMT32Synth)
				SwitchSynth(TSynth::SoundFont);
			else
				SwitchSynth(TSynth::MT32);
		}
	}
	else if (Event.Button == TButton::Button2 && !Event.bRepeat)
	{
		if (m_pCurrentSynth == m_pMT32Synth)
			NextMT32ROMSet();
		else
		{
			// Next SoundFont
			const size_t nSoundFonts = m_pSoundFontSynth->GetSoundFontManager().GetSoundFontCount();

			if (!nSoundFonts)
				LCDLog(TLCDLogType::Error, "No SoundFonts!");
			else
			{
				size_t nNextSoundFont;
				if (m_bDeferredSoundFontSwitchFlag)
					nNextSoundFont = (m_nDeferredSoundFontSwitchIndex + 1) % nSoundFonts;
				else
				{
					// Current SoundFont was probably on a USB stick that has since been removed
					const size_t nCurrentSoundFont = m_pSoundFontSynth->GetSoundFontIndex();
					if (nCurrentSoundFont > nSoundFonts)
						nNextSoundFont = 0;
					else
						nNextSoundFont = (nCurrentSoundFont + 1) % nSoundFonts;
				}

				DeferSwitchSoundFont(nNextSoundFont);
			}
		}
	}
	else if (Event.Button == TButton::Button3)
	{
		SetMasterVolume(m_nMasterVolume - 1);
	}
	else if (Event.Button == TButton::Button4)
	{
		SetMasterVolume(m_nMasterVolume + 1);
	}
}

void CMT32Pi::SetupMixerRouting()
{
	// Register available engines in router and mixer
	if (m_pMT32Synth)
	{
		m_MIDIRouter.SetMT32Engine(m_pMT32Synth);
		m_AudioMixer.AddEngine(m_pMT32Synth);
	}
	if (m_pSoundFontSynth)
	{
		m_MIDIRouter.SetFluidSynthEngine(m_pSoundFontSynth);
		m_AudioMixer.AddEngine(m_pSoundFontSynth);
	}

	m_bMixerEnabled = m_pConfig->MixerEnabled;

	if (m_bMixerEnabled)
	{
		// Apply routing preset from config
		// TMixerMode enum values match TRouterPreset order
		const TRouterPreset Preset = static_cast<TRouterPreset>(m_pConfig->MixerMode);
		m_MIDIRouter.ApplyPreset(Preset);

		// Apply per-engine volume (config is 0-100 int → 0.0-1.0 float)
		if (m_pMT32Synth)
		{
			m_AudioMixer.SetEngineVolume(m_pMT32Synth, m_pConfig->MixerMT32Volume / 100.0f);
			m_AudioMixer.SetEnginePan(m_pMT32Synth, m_pConfig->MixerMT32Pan / 100.0f);
		}
		if (m_pSoundFontSynth)
		{
			m_AudioMixer.SetEngineVolume(m_pSoundFontSynth, m_pConfig->MixerFluidSynthVolume / 100.0f);
			m_AudioMixer.SetEnginePan(m_pSoundFontSynth, m_pConfig->MixerFluidSynthPan / 100.0f);
		}

		// If only one engine routes, optimize with solo bypass
		if (!m_MIDIRouter.IsDualMode())
			m_AudioMixer.SetSoloEngine(m_pCurrentSynth);
		else
			ApplyDualModeLimits(true);

		LOGNOTE("Mixer enabled (mode: %d)", static_cast<int>(m_pConfig->MixerMode));
	}
	else
	{
		// Classic single-engine mode
		if (m_pCurrentSynth == m_pMT32Synth)
			m_MIDIRouter.ApplyPreset(TRouterPreset::SingleMT32);
		else
			m_MIDIRouter.ApplyPreset(TRouterPreset::SingleFluid);

		m_AudioMixer.SetSoloEngine(m_pCurrentSynth);
		LOGNOTE("Mixer disabled (classic single-synth mode)");
	}
}

void CMT32Pi::ApplyDualModeLimits(bool bDual)
{
	if (!m_pSoundFontSynth)
		return;

	if (bDual)
	{
		LOGNOTE("Dual mode active — applying performance limits");
		// Disable FluidSynth chorus to save CPU
		m_pSoundFontSynth->SetChorusActive(false);
		// TODO: reduce polyphony when CSoundFontSynth exposes SetPolyphony()
	}
	else
	{
		LOGNOTE("Single mode — restoring default performance");
		// Restore chorus to config value
		m_pSoundFontSynth->SetChorusActive(m_pConfig->FluidSynthDefaultChorusActive);
	}
}

CMT32Pi::TMixerStatus CMT32Pi::GetMixerStatus() const
{
	TMixerStatus s;
	s.bEnabled    = m_bMixerEnabled;
	s.nPreset     = static_cast<int>(m_MIDIRouter.GetPreset());
	s.bDualMode   = m_MIDIRouter.IsDualMode();
	s.fMasterVolume = m_AudioMixer.GetMasterVolume();

	s.fMT32Volume  = m_pMT32Synth      ? m_AudioMixer.GetEngineVolume(m_pMT32Synth)      : 0.0f;
	s.fFluidVolume = m_pSoundFontSynth  ? m_AudioMixer.GetEngineVolume(m_pSoundFontSynth) : 0.0f;
	s.fMT32Pan     = m_pMT32Synth      ? m_AudioMixer.GetEnginePan(m_pMT32Synth)         : 0.0f;
	s.fFluidPan    = m_pSoundFontSynth  ? m_AudioMixer.GetEnginePan(m_pSoundFontSynth)    : 0.0f;

	for (unsigned i = 0; i < 16; ++i)
	{
		s.pChannelEngine[i] = m_MIDIRouter.GetChannelEngineName(static_cast<u8>(i));
		s.nChannelRemap[i]  = m_MIDIRouter.GetChannelRemap(static_cast<u8>(i));
		s.bLayered[i]       = m_MIDIRouter.GetLayering(static_cast<u8>(i));

		CSynthBase* pEng = m_MIDIRouter.GetChannelEngine(static_cast<u8>(i));
		const u8 nRemapped = m_MIDIRouter.GetChannelRemap(static_cast<u8>(i));
		s.pChannelInstrument[i] = pEng ? pEng->GetChannelInstrumentName(nRemapped) : nullptr;
	}

	// Audio render performance
	s.nRenderUs      = m_nRenderUs;
	s.nRenderAvgUs   = m_nRenderAvgUs;
	s.nRenderPeakUs  = m_nRenderPeakUs;
	s.nDeadlineUs    = m_nDeadlineUs;
	s.nCpuLoadPercent = m_nDeadlineUs > 0
		? static_cast<unsigned>(m_nRenderAvgUs * 100U / m_nDeadlineUs) : 0;

	return s;
}

bool CMT32Pi::SetMixerEnabled(bool bEnabled)
{
	m_bMixerEnabled = bEnabled;

	if (bEnabled)
	{
		// Check dual mode and apply limits
		if (m_MIDIRouter.IsDualMode())
		{
			m_AudioMixer.ClearSoloEngine();
			ApplyDualModeLimits(true);
		}
		else
		{
			m_AudioMixer.SetSoloEngine(m_pCurrentSynth);
		}
	}
	else
	{
		// Revert to classic single mode
		if (m_pCurrentSynth == m_pMT32Synth)
			m_MIDIRouter.ApplyPreset(TRouterPreset::SingleMT32);
		else
			m_MIDIRouter.ApplyPreset(TRouterPreset::SingleFluid);

		m_AudioMixer.SetSoloEngine(m_pCurrentSynth);
		ApplyDualModeLimits(false);
	}

	return true;
}

bool CMT32Pi::SetMixerPreset(int nPreset)
{
	if (nPreset < 0 || nPreset > static_cast<int>(TRouterPreset::Custom))
		return false;

	const TRouterPreset Preset = static_cast<TRouterPreset>(nPreset);

	// Silence both engines before applying new routing to prevent stuck notes
	if (m_pMT32Synth)
		m_pMT32Synth->AllSoundOff();
	if (m_pSoundFontSynth)
		m_pSoundFontSynth->AllSoundOff();

	m_MIDIRouter.ApplyPreset(Preset);
	m_MIDIRouter.ResetChannelRemap();

	// Sync m_pCurrentSynth for single-engine presets so Sound page stays in sync
	if (Preset == TRouterPreset::SingleMT32 && m_pMT32Synth)
		m_pCurrentSynth = m_pMT32Synth;
	else if (Preset == TRouterPreset::SingleFluid && m_pSoundFontSynth)
		m_pCurrentSynth = m_pSoundFontSynth;

	// Update dual mode state
	const bool bDual = m_MIDIRouter.IsDualMode();
	if (bDual)
		m_AudioMixer.ClearSoloEngine();
	else
		m_AudioMixer.SetSoloEngine(m_MIDIRouter.GetPrimaryEngine());

	ApplyDualModeLimits(bDual);
	return true;
}

bool CMT32Pi::SetMixerChannelEngine(u8 nChannel, const char* pEngineName)
{
	if (nChannel >= 16 || !pEngineName)
		return false;

	CSynthBase* pEngine = nullptr;
	if (strcmp(pEngineName, "mt32") == 0 || strcmp(pEngineName, "MT-32") == 0)
		pEngine = m_pMT32Synth;
	else if (strcmp(pEngineName, "fluidsynth") == 0 || strcmp(pEngineName, "FluidSynth") == 0)
		pEngine = m_pSoundFontSynth;
	else
		return false;

	if (!pEngine)
		return false;

	// Send All Sound Off to the old engine for this channel to prevent stuck notes
	CSynthBase* pOldEngine = m_MIDIRouter.GetChannelEngine(nChannel);
	if (pOldEngine && pOldEngine != pEngine)
	{
		const u8 nRemap = m_MIDIRouter.GetChannelRemap(nChannel);
		// CC 120 (All Sound Off) on the remapped channel
		const u32 nAllSoundOff = 0x007800B0u | nRemap;
		pOldEngine->HandleMIDIShortMessage(nAllSoundOff);
		// CC 123 (All Notes Off) as well
		const u32 nAllNotesOff = 0x007B00B0u | nRemap;
		pOldEngine->HandleMIDIShortMessage(nAllNotesOff);
	}

	m_MIDIRouter.SetChannelEngine(nChannel, pEngine);

	// Update dual mode state
	const bool bDual = m_MIDIRouter.IsDualMode();
	if (bDual)
		m_AudioMixer.ClearSoloEngine();
	else
		m_AudioMixer.SetSoloEngine(m_MIDIRouter.GetPrimaryEngine());

	ApplyDualModeLimits(bDual);
	return true;
}

bool CMT32Pi::SetMixerChannelRemap(u8 nChannel, u8 nTargetChannel)
{
	if (nChannel >= 16 || nTargetChannel >= 16)
		return false;

	m_MIDIRouter.SetChannelRemap(nChannel, nTargetChannel);
	return true;
}

bool CMT32Pi::SetMixerEngineVolume(const char* pEngineName, int nVolumePercent)
{
	if (!pEngineName || nVolumePercent < 0 || nVolumePercent > 100)
		return false;

	CSynthBase* pEngine = nullptr;
	if (strcmp(pEngineName, "mt32") == 0)
		pEngine = m_pMT32Synth;
	else if (strcmp(pEngineName, "fluidsynth") == 0)
		pEngine = m_pSoundFontSynth;
	else
		return false;

	if (!pEngine)
		return false;

	m_AudioMixer.SetEngineVolume(pEngine, nVolumePercent / 100.0f);
	return true;
}

bool CMT32Pi::SetMixerEnginePan(const char* pEngineName, int nPanPercent)
{
	if (!pEngineName || nPanPercent < -100 || nPanPercent > 100)
		return false;

	CSynthBase* pEngine = nullptr;
	if (strcmp(pEngineName, "mt32") == 0)
		pEngine = m_pMT32Synth;
	else if (strcmp(pEngineName, "fluidsynth") == 0)
		pEngine = m_pSoundFontSynth;
	else
		return false;

	if (!pEngine)
		return false;

	m_AudioMixer.SetEnginePan(pEngine, nPanPercent / 100.0f);
	return true;
}

bool CMT32Pi::SetMixerCCFilter(unsigned nEngine, u8 nCC, bool bAllow)
{
	if (nEngine >= CMIDIRouter::MaxEngines || nCC >= CMIDIRouter::NumCCs)
		return false;
	m_MIDIRouter.SetCCFilter(nEngine, nCC, bAllow);
	return true;
}

bool CMT32Pi::GetMixerCCFilter(unsigned nEngine, u8 nCC) const
{
	return m_MIDIRouter.GetCCFilter(nEngine, nCC);
}

void CMT32Pi::ResetMixerCCFilters()
{
	m_MIDIRouter.ResetCCFilters();
}

bool CMT32Pi::SetMixerLayering(u8 nChannel, bool bLayered)
{
	if (nChannel >= 16)
		return false;
	m_MIDIRouter.SetLayering(nChannel, bLayered);
	return true;
}

bool CMT32Pi::SetMixerAllLayering(bool bLayered)
{
	m_MIDIRouter.SetAllLayering(bLayered);
	return true;
}

void CMT32Pi::SwitchSynth(TSynth NewSynth)
{
	CSynthBase* pNewSynth = nullptr;

	if (NewSynth == TSynth::MT32)
		pNewSynth = m_pMT32Synth;
	else if (NewSynth == TSynth::SoundFont)
		pNewSynth = m_pSoundFontSynth;

	if (pNewSynth == nullptr)
	{
		LCDLog(TLCDLogType::Warning, "Synth unavailable!");
		return;
	}

	if (pNewSynth == m_pCurrentSynth)
	{
		LCDLog(TLCDLogType::Warning, "Already active!");
		return;
	}

	m_pCurrentSynth->AllSoundOff();
	m_pCurrentSynth = pNewSynth;
	const char* pMode = NewSynth == TSynth::MT32 ? "MT-32 mode" : "SoundFont mode";
	LOGNOTE("Switching to %s", pMode);
	LCDLog(TLCDLogType::Notice, pMode);

	// Update router and mixer to single-engine mode
	if (m_bMixerEnabled)
	{
		if (NewSynth == TSynth::MT32)
			m_MIDIRouter.ApplyPreset(TRouterPreset::SingleMT32);
		else
			m_MIDIRouter.ApplyPreset(TRouterPreset::SingleFluid);

		m_AudioMixer.SetSoloEngine(pNewSynth);
		ApplyDualModeLimits(false);
	}
}

void CMT32Pi::SwitchMT32ROMSet(TMT32ROMSet ROMSet)
{
	if (m_pMT32Synth == nullptr)
		return;

	LOGNOTE("Switching to ROM set %d", static_cast<u8>(ROMSet));
	if (m_pMT32Synth->SwitchROMSet(ROMSet) && m_pCurrentSynth == m_pMT32Synth)
		m_pMT32Synth->ReportStatus();
}

void CMT32Pi::NextMT32ROMSet()
{
	if (m_pMT32Synth == nullptr)
		return;

	LOGNOTE("Switching to next ROM set");

	if (m_pMT32Synth->NextROMSet() && m_pCurrentSynth == m_pMT32Synth)
		m_pMT32Synth->ReportStatus();
}

void CMT32Pi::SwitchSoundFont(size_t nIndex)
{
	if (m_pSoundFontSynth == nullptr)
		return;

	LOGNOTE("Switching to SoundFont %d", nIndex);
	if (m_pSoundFontSynth->SwitchSoundFont(nIndex))
	{
		// Handle any MIDI data that has been queued up while busy
		PurgeMIDIBuffers();

		if (m_pCurrentSynth == m_pSoundFontSynth)
			m_pSoundFontSynth->ReportStatus();
	}
}

void CMT32Pi::DeferSwitchSoundFont(size_t nIndex)
{
	if (m_pSoundFontSynth == nullptr)
		return;

	const char* pName = m_pSoundFontSynth->GetSoundFontManager().GetSoundFontName(nIndex);
	LCDLog(TLCDLogType::Notice, "SF %ld: %s", nIndex, pName ? pName : "- N/A -");
	m_nDeferredSoundFontSwitchIndex = nIndex;
	m_nDeferredSoundFontSwitchTime  = CTimer::Get()->GetTicks();
	m_bDeferredSoundFontSwitchFlag  = true;
}

void CMT32Pi::SetMasterVolume(s32 nVolume)
{
	m_nMasterVolume = Utility::Clamp(nVolume, 0, 100);

	if (m_pMT32Synth)
		m_pMT32Synth->SetMasterVolume(m_nMasterVolume);
	if (m_pSoundFontSynth)
		m_pSoundFontSynth->SetMasterVolume(m_nMasterVolume);

	if (m_pCurrentSynth == m_pSoundFontSynth)
		LCDLog(TLCDLogType::Notice, "Volume: %d", m_nMasterVolume);
}

void CMT32Pi::LEDOn()
{
	m_pActLED->On();
	m_nLEDOnTime = m_pTimer->GetTicks();
	m_bLEDOn = true;
}

void CMT32Pi::LCDLog(TLCDLogType Type, const char* pFormat...)
{
	if (!m_pLCD)
		return;

	char Buffer[256];

	va_list Args;
	va_start(Args, pFormat);
	vsnprintf(Buffer, sizeof(Buffer), pFormat, Args);
	va_end(Args);

	// LCD task hasn't started yet; print directly
	if (Type == TLCDLogType::Startup)
	{
		if (m_pLCD->GetType() == CLCD::TType::Graphical && !m_pConfig->SystemVerbose)
			return;

		u8 nOffsetX = CUserInterface::CenterMessageOffset(*m_pLCD, Buffer);
		m_pLCD->Print(Buffer, nOffsetX, 1, true, true);
	}

	// Let LCD task pick up the message in its next update
	else
		m_UserInterface.ShowSystemMessage(Buffer, Type == TLCDLogType::Spinner);
}

const char* CMT32Pi::GetNetworkDeviceShortName() const
{
	switch (m_pConfig->NetworkMode)
	{
		case CConfig::TNetworkMode::Ethernet:	return "Ether";
		case CConfig::TNetworkMode::WiFi:	return "WiFi";
		default:				return "None";
	}
}

void CMT32Pi::EventHandler(const TEvent& Event)
{
	assert(s_pThis != nullptr);

	// Enqueue event
	s_pThis->m_EventQueue.Enqueue(Event);
}

void CMT32Pi::USBMIDIDeviceRemovedHandler(CDevice* pDevice, void* pContext)
{
	assert(s_pThis != nullptr);

	void** pDevicePointer = reinterpret_cast<void**>(pContext);
	*pDevicePointer = nullptr;

	// Re-enable serial MIDI if not in-use by logger and no other MIDI devices available
	if (s_pThis->m_bSerialMIDIAvailable && !(s_pThis->m_pUSBMIDIDevice || s_pThis->m_pUSBSerialDevice || s_pThis->m_pPisound))
	{
		LOGNOTE("Using serial MIDI interface");
		s_pThis->m_bSerialMIDIEnabled = true;
	}
}

// The following handlers are called from interrupt context, enqueue into ring buffer for main thread
void CMT32Pi::USBMIDIPacketHandler(unsigned nCable, u8* pPacket, unsigned nLength)
{
	IRQMIDIReceiveHandler(pPacket, nLength);
}

void CMT32Pi::IRQMIDIReceiveHandler(const u8* pData, size_t nSize)
{
	assert(s_pThis != nullptr);

	// Enqueue data into ring buffer
	if (s_pThis->m_MIDIRxBuffer.Enqueue(pData, nSize) != nSize)
	{
		static const char* pErrorString = "MIDI overrun error!";
		LOGWARN(pErrorString);
		s_pThis->LCDLog(TLCDLogType::Error, pErrorString);
	}
}

void CMT32Pi::PanicHandler()
{
	if (!s_pThis || !s_pThis->m_pLCD)
		return;

	// Kill UI task
	s_pThis->m_bRunning = false;
	while (!s_pThis->m_bUITaskDone)
		;

	const char* pGuru = "Guru Meditation:";
	u8 nOffsetX = CUserInterface::CenterMessageOffset(*s_pThis->m_pLCD, pGuru);
	s_pThis->m_pLCD->Clear(true);
	s_pThis->m_pLCD->Print(pGuru, nOffsetX, 0, true, true);

	char Buffer[LOGGER_BUFSIZE];
	CLogger::Get()->Read(Buffer, sizeof(Buffer), false);

	// Find last newline
	char* pMessageStart = strrchr(Buffer, '\n');
	if (!pMessageStart)
		return;
	*pMessageStart = '\0';

	// Find second-last newline
	pMessageStart = strrchr(Buffer, '\n');
	if (!pMessageStart)
		return;

	// Skip past timestamp and log source, kill color control characters
	pMessageStart = strstr(pMessageStart, ": ") + 2;
	char* pMessageEnd = strstr(pMessageStart, "\x1B[0m");
	*pMessageEnd = '\0';

	const size_t nMessageLength = strlen(pMessageStart);
	size_t nCurrentScrollOffset = 0;
	const char* pGuruFlash = pGuru;
	bool bFlash = false;

	unsigned int nTicks = CTimer::GetClockTicks();
	unsigned int nPanicStart = nTicks;
	unsigned int nFlashTime = nTicks;
	unsigned int nScrollTime = nTicks;

	const u8 nWidth = s_pThis->m_pLCD->Width();
	const u8 nHeight = s_pThis->m_pLCD->Height();

	// TODO: API for getting width in pixels/characters for a string
	const bool bGraphical = s_pThis->m_pLCD->GetType() == CLCD::TType::Graphical;
	const size_t nCharWidth = bGraphical ? 20 : nWidth;

	while (true)
	{
		s_pThis->m_pLCD->Clear(false);
		nTicks = CTimer::GetClockTicks();

		if (Utility::TicksToMillis(nTicks - nFlashTime) > 1000)
		{
			bFlash = !bFlash;
			nFlashTime = nTicks;
		}

		if (nMessageLength > nCharWidth)
		{
			if (nMessageLength - nCurrentScrollOffset > nCharWidth)
			{
				const unsigned int nTimeout = nCurrentScrollOffset == 0 ? 1500 : 175;
				if (Utility::TicksToMillis(nTicks - nScrollTime) >= nTimeout)
				{
					++nCurrentScrollOffset;
					nScrollTime = nTicks;
				}
			}
			else if (Utility::TicksToMillis(nTicks - nScrollTime) >= 3000)
			{
				nCurrentScrollOffset = 0;
				nScrollTime = nTicks;
			}
		}

		if (Utility::TicksToMillis(nTicks - nPanicStart) > 2 * 60 * 1000)
			break;

		if (!bGraphical)
			pGuruFlash = bFlash ? "" : pGuru;

		u8 nOffsetX = CUserInterface::CenterMessageOffset(*s_pThis->m_pLCD, pGuruFlash);
		s_pThis->m_pLCD->Print(pGuruFlash, nOffsetX, 0, true, false);
		s_pThis->m_pLCD->Print(pMessageStart + nCurrentScrollOffset, 0, 1, true, false);

		if (bGraphical && bFlash)
		{
			s_pThis->m_pLCD->DrawFilledRect(0, 0, nWidth - 1, 1);
			s_pThis->m_pLCD->DrawFilledRect(0, nHeight - 1, nWidth - 1, nHeight - 2);
			s_pThis->m_pLCD->DrawFilledRect(0, 0, 1, nHeight - 1);
			s_pThis->m_pLCD->DrawFilledRect(nWidth - 1, 0, nWidth - 2, nHeight - 1);
		}

		s_pThis->m_pLCD->Flip();
	}

	s_pThis->m_pLCD->Clear(true);
	const char* pMessage = "System halted";
	nOffsetX = CUserInterface::CenterMessageOffset(*s_pThis->m_pLCD, pMessage);
	s_pThis->m_pLCD->Print(pMessage, nOffsetX, 0, true, true);
	pMessage = "Please reboot";
	nOffsetX = CUserInterface::CenterMessageOffset(*s_pThis->m_pLCD, pMessage);
	s_pThis->m_pLCD->Print(pMessage, nOffsetX, 1, true, true);
}
