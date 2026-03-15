//
// mt32pi.h
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

#ifndef _mt32pi_h
#define _mt32pi_h

#include <circle/actled.h>
#include <circle/bcm54213.h>
#include <circle/bcmrandom.h>
#include <circle/cputhrottle.h>
#include <circle/devicenameservice.h>
#include <circle/gpiomanager.h>
#include <circle/i2cmaster.h>
#include <circle/interrupt.h>
#include <circle/logger.h>
#include <circle/multicore.h>
#include <circle/net/netsubsystem.h>
#include <circle/sched/scheduler.h>
#include <circle/sound/soundbasedevice.h>
#include <circle/spimaster.h>
#include <circle/timer.h>
#include <circle/types.h>
#include <circle/usb/usbhcidevice.h>
#include <circle/usb/usbmassdevice.h>
#include <circle/usb/usbmidi.h>
#include <circle/usb/usbserial.h>
#include <fatfs/ff.h>
#include <wlan/bcm4343.h>

#include "midisequencer.h"
#include <wlan/hostap/wpa_supplicant/wpasupplicant.h>

#include "config.h"
#include "control/control.h"
#include "control/mister.h"
#include "event.h"
#include "lcd/ui.h"
#include "midiparser.h"
#include "net/applemidi.h"
#include "net/ftpdaemon.h"
#include "net/udpmidi.h"
#include "net/webdaemon.h"
#include "net/websocketdaemon.h"
#include "pisound.h"
#include "power.h"
#include "ringbuffer.h"
#include "audiomixer.h"
#include "midirouter.h"
#include "synth/mt32romset.h"
#include "synth/mt32synth.h"
#include "synth/soundfontsynth.h"
#include "synth/synth.h"

//#define MONITOR_TEMPERATURE

class CMT32Pi : CMultiCoreSupport, CPower, CMIDIParser, CAppleMIDIHandler, CUDPMIDIHandler
{
public:
	CMT32Pi(CI2CMaster* pI2CMaster, CSPIMaster* pSPIMaster, CInterruptSystem* pInterrupt, CGPIOManager* pGPIOManager, CSerialDevice* pSerialDevice, CUSBHCIDevice* pUSBHCI);
	virtual ~CMT32Pi() override;

	bool Initialize(bool bSerialMIDIAvailable = true);

	virtual void Run(unsigned nCore) override;

	const CConfig* GetConfig() const { return m_pConfig; }
	bool IsNetworkReady() const { return m_bNetworkReady; }
	const char* GetNetworkInterfaceName() const { return GetNetworkDeviceShortName(); }
	void FormatIPAddress(CString& Out) const;
	const char* GetActiveSynthName() const;
	const char* GetCurrentMT32ROMName() const;
	const char* GetCurrentSoundFontName() const;
	const char* GetCurrentSoundFontPath() const;
	size_t GetCurrentSoundFontIndex() const;
	const char* GetSoundFontName(size_t nIndex) const;
	size_t GetSoundFontCount() const;
	int GetMasterVolume() const { return static_cast<int>(m_nMasterVolume); }
	int GetMT32ROMSetIndex() const;
	bool GetSoundFontFXState(bool& bReverbActive, float& nReverbRoomSize, float& nReverbLevel,
	                         float& nReverbDamping, float& nReverbWidth,
	                         bool& bChorusActive, float& nChorusDepth, float& nChorusLevel,
	                         int& nChorusVoices, float& nChorusSpeed, float& nGain) const;
	void GetMIDIChannelLevels(float* pOutLevels, float* pOutPeaks) const;
	bool SetActiveSynth(TSynth Synth);
	bool SetMT32ROMSet(TMT32ROMSet ROMSet);
	bool SetSoundFontIndex(size_t nIndex);
	bool SetMasterVolumePercent(int nVolume);
	bool SetSoundFontReverbActive(bool bActive);
	bool SetSoundFontReverbRoomSize(float nRoomSize);
	bool SetSoundFontReverbLevel(float nLevel);
	bool SetSoundFontReverbDamping(float nDamping);
	bool SetSoundFontReverbWidth(float nWidth);
	bool SetSoundFontChorusActive(bool bActive);
	bool SetSoundFontChorusDepth(float nDepth);
	bool SetSoundFontChorusLevel(float nLevel);
	bool SetSoundFontChorusVoices(int nVoices);
	bool SetSoundFontChorusSpeed(float nSpeed);
	bool SetSoundFontGain(float nGain);

	// MT-32 Sound Parameters
	float GetMT32ReverbOutputGain() const;
	bool SetMT32ReverbOutputGain(float nGain);
	bool IsMT32ReverbActive() const;
	bool SetMT32ReverbActive(bool bActive);
	bool IsMT32NiceAmpRamp() const;
	bool SetMT32NiceAmpRamp(bool bEnabled);
	bool IsMT32NicePanning() const;
	bool SetMT32NicePanning(bool bEnabled);
	bool IsMT32NicePartialMixing() const;
	bool SetMT32NicePartialMixing(bool bEnabled);
	int GetMT32DACMode() const;
	bool SetMT32DACMode(int nMode);
	int GetMT32MIDIDelayMode() const;
	bool SetMT32MIDIDelayMode(int nMode);
	int GetMT32AnalogMode() const;
	bool SetMT32AnalogMode(int nMode);
	int GetMT32RendererType() const;
	bool SetMT32RendererType(int nType);
	int GetMT32PartialCount() const;
	bool SetMT32PartialCount(int nCount);
	
	void RequestReboot() { m_bRunning = false; }
	bool HasMT32Synth() const { return m_pMT32Synth != nullptr; }
	bool HasSoundFontSynth() const { return m_pSoundFontSynth != nullptr; }

	// ---- Mixer control (called from web handler on Core 0) ----
	struct TMixerStatus
	{
		bool  bEnabled;
		int   nPreset;           // TRouterPreset as int
		bool  bDualMode;
		float fMT32Volume;
		float fFluidVolume;
		float fMT32Pan;
		float fFluidPan;
		float fMasterVolume;
		// Per-channel: engine name string, remap target, and layering flag
		const char* pChannelEngine[16];
		const char* pChannelInstrument[16];
		u8          nChannelRemap[16];
		bool        bLayered[16];
		// Audio render performance
		unsigned nRenderUs;      // last render time in µs
		unsigned nRenderAvgUs;   // rolling average render time
		unsigned nRenderPeakUs;  // peak render time since last read
		unsigned nDeadlineUs;    // max allowed render time for current chunk
		unsigned nCpuLoadPercent;// render_avg / deadline * 100
	};

	TMixerStatus GetMixerStatus() const;
	bool SetMixerEnabled(bool bEnabled);
	bool SetMixerPreset(int nPreset);
	bool SetMixerChannelEngine(u8 nChannel, const char* pEngineName);
	bool SetMixerChannelRemap(u8 nChannel, u8 nTargetChannel);
	bool SetMixerEngineVolume(const char* pEngineName, int nVolumePercent);
	bool SetMixerEnginePan(const char* pEngineName, int nPanPercent);

	// CC filter and layering
	bool SetMixerCCFilter(unsigned nEngine, u8 nCC, bool bAllow);
	bool GetMixerCCFilter(unsigned nEngine, u8 nCC) const;
	void ResetMixerCCFilters();
	bool SetMixerLayering(u8 nChannel, bool bLayered);
	bool SetMixerAllLayering(bool bLayered);

	// ---- Sequencer control (called from Core 0 / web handler) ----
	struct TSequencerStatus
	{
		bool        bPlaying;
		bool        bLoopEnabled;
		bool        bFinished;   // true when song ended naturally (loop=off)
		const char* pFile;       // points to internal buffer; valid until next call
		u32         nEventCount;
		u32         nDurationMs;
		u32         nElapsedMs;
	};

	void             SequencerPlayFile(const char* pPath);
	void             SequencerStop();
	void             SetSequencerLoop(bool bLoop);
	TSequencerStatus GetSequencerStatus() const;
	void             GetMIDIFileListJSON(CString& outJSON) const;
	void             SendRawMIDI(const u8* pData, size_t nSize);

	// Active note snapshot — indexed [channel][note], value = EMidiSource (0=off)
	enum class EMidiSource : u8 { None = 0, Physical = 1, Player = 2, WebUI = 3 };
	void GetActiveNotes(u8 out[16][128]) const;

private:
	enum class TLCDLogType
	{
		Startup,
		Error,
		Warning,
		Notice,
		Spinner,
	};

	static constexpr size_t MIDIRxBufferSize = 2048;

	// CPower
	virtual void OnEnterPowerSavingMode() override;
	virtual void OnExitPowerSavingMode() override;
	virtual void OnThrottleDetected() override;
	virtual void OnUnderVoltageDetected() override;

	// CMIDIParser
	virtual void OnShortMessage(u32 nMessage) override;
	virtual void OnSysExMessage(const u8* pData, size_t nSize) override;
	virtual void OnUnexpectedStatus() override;
	virtual void OnSysExOverflow() override;

	// CAppleMIDIHandler
	virtual void OnAppleMIDIDataReceived(const u8* pData, size_t nSize) override { ParseMIDIBytes(pData, nSize); };
	virtual void OnAppleMIDIConnect(const CIPAddress* pIPAddress, const char* pName) override;
	virtual void OnAppleMIDIDisconnect(const CIPAddress* pIPAddress, const char* pName) override;

	// CUDPMIDIHandler
	virtual void OnUDPMIDIDataReceived(const u8* pData, size_t nSize) override { ParseMIDIBytes(pData, nSize); };

	// Initialization
	bool InitNetwork();
	bool InitMT32Synth();
	bool InitSoundFontSynth();

	// Tasks for specific CPU cores
	void MainTask();
	void UITask();
	void AudioTask();
	void Core3SequencerTask();

	void UpdateUSB(bool bStartup = false);
	void UpdateNetwork();
	void UpdateMIDI();
	void PurgeMIDIBuffers();
	size_t ReceiveSerialMIDI(u8* pOutData, size_t nSize);
	bool ParseCustomSysEx(const u8* pData, size_t nSize);

	void ProcessEventQueue();
	void ProcessButtonEvent(const TButtonEvent& Event);

	// Actions that can be triggered via events
	void SwitchSynth(TSynth Synth);
	void SetupMixerRouting();
	void ApplyDualModeLimits(bool bDual);
	void SwitchMT32ROMSet(TMT32ROMSet ROMSet);
	void NextMT32ROMSet();
	void SwitchSoundFont(size_t nIndex);
	void DeferSwitchSoundFont(size_t nIndex);
	void SetMasterVolume(s32 nVolume);

	const char* GetNetworkDeviceShortName() const;
	void LEDOn();
	void LCDLog(TLCDLogType Type, const char* pFormat...);

	CConfig* volatile m_pConfig;

	CTimer* m_pTimer;
	CActLED* m_pActLED;

	CI2CMaster* m_pI2CMaster;
	CSPIMaster* m_pSPIMaster;
	CInterruptSystem* m_pInterrupt;
	CGPIOManager* m_pGPIOManager;
	CSerialDevice* m_pSerial;
	CUSBHCIDevice* m_pUSBHCI;
	FATFS m_USBFileSystem;
	bool m_bUSBAvailable;

	// Networking
	CNetSubSystem* m_pNet;
	CNetDevice* m_pNetDevice;
	CBcm4343Device m_WLAN;
	CWPASupplicant m_WPASupplicant;
	bool m_bNetworkReady;
	CAppleMIDIParticipant* m_pAppleMIDIParticipant;
	CUDPMIDIReceiver* m_pUDPMIDIReceiver;
	CFTPDaemon* m_pFTPDaemon;
	CWebDaemon* m_pWebDaemon;
	CWebSocketDaemon* m_pWebSocketDaemon;

	CBcmRandomNumberGenerator m_Random;

	CLCD* m_pLCD;
	unsigned m_nLCDUpdateTime;
	CUserInterface m_UserInterface;
#ifdef MONITOR_TEMPERATURE
	unsigned m_nTempUpdateTime;
#endif

	CControl* m_pControl;

	// MiSTer control interface
	CMisterControl m_MisterControl;
	unsigned m_nMisterUpdateTime;

	// Deferred SoundFont switch
	bool m_bDeferredSoundFontSwitchFlag;
	size_t m_nDeferredSoundFontSwitchIndex;
	unsigned m_nDeferredSoundFontSwitchTime;

	// Serial GPIO MIDI
	bool m_bSerialMIDIAvailable;
	bool m_bSerialMIDIEnabled;

	// USB devices
	CUSBMIDIDevice* m_pUSBMIDIDevice;
	CUSBSerialDevice* m_pUSBSerialDevice;
	CUSBBulkOnlyMassStorageDevice* volatile m_pUSBMassStorageDevice;

	bool m_bActiveSenseFlag;
	unsigned m_nActiveSenseTime;

	volatile bool m_bRunning;
	volatile bool m_bUITaskDone;
	bool m_bLEDOn;
	unsigned m_nLEDOnTime;

	// Audio output
	CSoundBaseDevice* m_pSound;

	// Extra devices
	CPisound* m_pPisound;

	// Synthesizers
	u8 m_nMasterVolume;
	CSynthBase* m_pCurrentSynth;
	CMT32Synth* m_pMT32Synth;
	CSoundFontSynth* m_pSoundFontSynth;

	// MIDI Router + Audio Mixer
	CMIDIRouter m_MIDIRouter;
	CAudioMixer m_AudioMixer;
	bool m_bMixerEnabled;

	// Audio render performance monitor (Core 2 writes, Core 0 reads)
	volatile unsigned m_nRenderUs;           // last chunk render time in µs
	volatile unsigned m_nRenderAvgUs;        // exponential moving average
	volatile unsigned m_nRenderPeakUs;       // peak since last API read
	volatile unsigned m_nDeadlineUs;         // deadline for current chunk size
	bool m_bAutoReducePartials;              // auto-reduce MT-32 partials if overloaded

	// Menu long-press tracking
	bool m_bMenuLongPressConsumed;

	// MIDI receive buffers
	CRingBuffer<u8, MIDIRxBufferSize> m_MIDIRxBuffer;
	CRingBuffer<u8, MIDIRxBufferSize> m_Core3MIDIRxBuffer;
	CRingBuffer<u8, MIDIRxBufferSize> m_WebMIDIRxBuffer;  // Web keyboard → Core 0

	// ---- Sequencer Core0↔Core3 communication ----
	// Core 0 loads files and writes command flags.
	// Core 3 only calls Start()/PopDueBytes() — no FatFS I/O on Core 3.
	static constexpr size_t SeqPathMax = 260;

	CMIDISequencer*  m_pSequencer;          // heap, allocated by Core 0 on first play
	volatile bool    m_bSeqReadyToPlay;     // Core 0 → Core 3: file loaded, start playing
	volatile bool    m_bSeqStopFlag;        // Core 0 → Core 3: stop playback
	volatile bool    m_bSeqLoopEnabled;     // Core 0 writes; Core 3 reads: repeat when done
	volatile bool    m_bSeqIsPlaying;       // Core 3 → Core 0: playback active
	volatile bool    m_bSeqFinished;        // Core 3 → Core 0: song ended naturally
	volatile u32     m_nSeqElapsedUs;       // Core 3 → Core 0: elapsed µs
	volatile u32     m_nSeqDurationUs;      // Core 0 writes after LoadFromFile
	volatile u32     m_nSeqEventCount;      // Core 0 writes after LoadFromFile
	char             m_szSeqCurrentFile[SeqPathMax]; // Core 0 writes after LoadFromFile

	// Active note snapshot (written by OnShortMessage on Core 0 task context)
	u8 m_activeNotes[16][128];   // value = EMidiSource (0 = off)
	u8 m_eMidiSource;            // source tag for the current ParseMIDIBytes batch

	// Event handling
	TEventQueue m_EventQueue;

	static void EventHandler(const TEvent& Event);
	static void USBMIDIDeviceRemovedHandler(CDevice* pDevice, void* pContext);
	static void USBMIDIPacketHandler(unsigned nCable, u8* pPacket, unsigned nLength);
	static void IRQMIDIReceiveHandler(const u8* pData, size_t nSize);

	static void PanicHandler();

	static CMT32Pi* s_pThis;
};

#endif
