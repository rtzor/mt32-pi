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

#include "fluidsequencer.h"
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
#include "audioeffects.h"
#include "midirouter.h"
#include "midimonitor.h"
#include "midirecorder.h"
#include "playlist.h"
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
	const char* GetSoundFontPath(size_t nIndex) const;
	size_t GetSoundFontCount() const;
	int GetMasterVolume() const { return static_cast<int>(m_nMasterVolume); }
	int GetMT32ROMSetIndex() const;
	bool GetSoundFontFXState(bool& bReverbActive, float& nReverbRoomSize, float& nReverbLevel,
	                         float& nReverbDamping, float& nReverbWidth,
	                         bool& bChorusActive, float& nChorusDepth, float& nChorusLevel,
	                         int& nChorusVoices, float& nChorusSpeed, float& nGain) const;
	void GetMIDIChannelLevels(float* pOutLevels, float* pOutPeaks) const;
	unsigned GetMIDIEventLog(CMIDIMonitor::TEventEntry* pOut, unsigned nMax) const;
	void ClearMIDIEventLog();
	unsigned GetSysExLog(CMIDIMonitor::TSysExEntry* pOut, unsigned nMax) const;
	void     ClearSysExLog();
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
	bool SetSoundFontTuning(int nPreset);
	int GetSoundFontTuning() const;
	const char* GetSoundFontTuningName() const;
	bool SetSoundFontPolyphony(int nPolyphony);
	int GetSoundFontPolyphony() const;
	bool SetSoundFontChannelType(int nChannel, int nType);
	u16 GetSoundFontPercussionMask() const;

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
		// Per-channel: engine name string, remap target, layering flag, and volume
		const char* pChannelEngine[16];
		const char* pChannelInstrument[16];
		u8          nChannelRemap[16];
		bool        bLayered[16];
		int         nChannelVolume[16];  // 0–100, per-channel CC7 scaling
		// Audio render performance
		unsigned nRenderUs;      // last render time in µs
		unsigned nRenderAvgUs;   // rolling average render time
		unsigned nRenderPeakUs;  // peak render time since last read
		unsigned nDeadlineUs;    // max allowed render time for current chunk
		unsigned nCpuLoadPercent;// render_avg / deadline * 100
		unsigned nMT32RenderUs;  // MT-32 render time for the last chunk
		unsigned nFluidRenderUs; // FluidSynth render time for the last chunk
		unsigned nMixerRenderUs; // mixer/gain/clamp overhead for the last chunk
		unsigned nMT32LoadPercent;
		unsigned nFluidLoadPercent;
		unsigned nMixerLoadPercent;
		// Post-mix audio effects state
		bool bEffectsEQEnabled;
		bool bEffectsLimiterEnabled;
		bool bEffectsReverbEnabled;
		int  nEffectsEQBass;        // -12..+12 dB
		int  nEffectsEQTreble;      // -12..+12 dB
		int  nEffectsReverbRoom;    // 0-100 (maps to 0.0-1.0)
		int  nEffectsReverbDamp;    // 0-100
		int  nEffectsReverbWet;     // 0-100
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

	// Per-channel volume
	bool SetMixerChannelVolume(u8 nChannel, int nVolumePercent);  // 0–100
	int  GetMixerChannelVolume(u8 nChannel) const;
	void ResetMixerChannelVolumes();

	// Post-mix audio effects (called from Core 0 / web handler)
	bool SetEffectEQEnabled(bool bEnabled);
	bool SetEffectEQBass(int nDb);        // -12..+12
	bool SetEffectEQTreble(int nDb);      // -12..+12
	bool SetEffectLimiterEnabled(bool bEnabled);
	bool SetEffectReverbEnabled(bool bEnabled);
	bool SetEffectReverbRoom(int nPercent);  // 0-100 → 0.0-1.0
	bool SetEffectReverbDamp(int nPercent);  // 0-100 → 0.0-1.0
	bool SetEffectReverbWet(int nPercent);   // 0-100 → 0.0-1.0

	// Save / load custom router preset to SD card
	bool SaveRouterPreset() const;
	bool LoadRouterPreset();

	// ---- MIDI recorder ----
	bool StartMidiRecording();
	void StopMidiRecording();  // no-op if not recording

	// ---- Sequencer control (called from Core 0 / web handler) ----
	struct TSequencerStatus
	{
		bool        bPlaying;
		bool        bPaused;     // true if paused (stopped at a saved tick)
		bool        bLoopEnabled;
		bool        bAutoNext;   // true if auto-advance to next file is enabled
		bool        bFinished;   // true when song ended naturally (loop=off)
		bool        bLoading;    // true while SequencerPlayFile is reading from SD
		const char* pFile;       // points to internal buffer; valid until next call
		u32         nEventCount;
		u32         nDurationMs;
		u32         nElapsedMs;
		int         nCurrentTick;
		int         nTotalTicks;
		int         nBPM;
		double      nTempoMultiplier;
		int         nDivision;
		u32         nFileSizeKB; // file size in KB (0 if unknown)
		const char* pDiag;       // diagnostic string from FluidSequencer
	};

	void             SequencerPlayFile(const char* pPath);
	void             SequencerStop();
	bool             SequencerPause();         // save position and stop
	bool             SequencerResume();        // replay from saved pause position
	void             SequencerNext();          // advance to next MIDI file alphabetically
	void             SequencerPrev();          // go to previous MIDI file
	void             SetSequencerLoop(bool bLoop);
	void             SetSequencerAutoNext(bool bEnabled);
	TSequencerStatus GetSequencerStatus() const;
	void             GetMIDIFileListJSON(CString& outJSON) const;

	// Playlist queue management
	void PlaylistAdd(const char* pPath);
	void PlaylistRemove(unsigned nIndex);
	bool PlaylistMoveUp(unsigned nIndex);
	bool PlaylistMoveDown(unsigned nIndex);
	void PlaylistClear();
	void PlaylistSetShuffle(bool bShuffle);
	void PlaylistSetRepeat(bool bRepeat);
	void PlaylistPlay(unsigned nIndex);
	void PlaylistAddAll();
	void GetPlaylistJSON(CString& outJSON) const;
	void             SendRawMIDI(const u8* pData, size_t nSize);

	// Advanced sequencer controls (FluidSequencer mode only)
	bool             SequencerSeek(int nTicks);
	bool             SequencerSetTempoMultiplier(double nMultiplier);
	bool             SequencerSetTempoBPM(double nBPM);

	// Active note snapshot — indexed [channel][note], value = EMidiSource (0=off)
	enum class EMidiSource : u8 { None = 0, Physical = 1, Player = 2, WebUI = 3 };
	void GetActiveNotes(u8 out[16][128]) const;

	// ---- Aggregated system state snapshot ----
	// Captures all runtime state in one call; pointers are valid until the next
	// mutation on Core 0.  ActiveNotes are NOT included (2 KB, websocket-only).
	struct TSystemState
	{
		TSequencerStatus Sequencer;
		TMixerStatus     Mixer;

		// Synth identity
		const char* pActiveSynthName;
		const char* pMT32ROMName;
		int         nMT32ROMSetIndex;
		const char* pSoundFontName;
		const char* pSoundFontPath;
		size_t      nSoundFontIndex;
		size_t      nSoundFontCount;
		int         nMasterVolume;

		// SoundFont FX
		bool  bSFFXAvailable;
		float fSFGain;
		bool  bSFReverbActive;
		float fSFReverbRoom;
		float fSFReverbLevel;
		float fSFReverbDamping;
		float fSFReverbWidth;
		bool  bSFChorusActive;
		float fSFChorusDepth;
		float fSFChorusLevel;
		int   nSFChorusVoices;
		float fSFChorusSpeed;
		int         nSFTuning;
		const char* pSFTuningName;
		int   nSFPolyphony;
		u16   nSFPercussionMask;

		// MT-32 parameters
		float fMT32ReverbGain;
		bool  bMT32ReverbActive;
		bool  bMT32NiceAmpRamp;
		bool  bMT32NicePanning;
		bool  bMT32NicePartialMixing;
		int   nMT32DACMode;
		int   nMT32MIDIDelayMode;
		int   nMT32AnalogMode;
		int   nMT32RendererType;
		int   nMT32PartialCount;

		// Network
		bool        bNetworkReady;
		const char* pNetworkInterfaceName;
		char        IPAddress[32];

		// MIDI activity levels
		float MIDILevels[16];
		float MIDIPeaks[16];

		// Recorder state
		bool bMidiRecording;

		// Playlist state
		unsigned nPlaylistCount;
		unsigned nPlaylistIndex;
		bool     bPlaylistRepeat;
		bool     bPlaylistShuffle;
	};

	TSystemState GetSystemState() const;

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

	// MIDI Router + Audio Mixer + Effects
	CMIDIRouter   m_MIDIRouter;
	CAudioMixer   m_AudioMixer;
	CAudioEffects m_AudioEffects;
	bool m_bMixerEnabled;

	// Audio render performance monitor (Core 2 writes, Core 0 reads)
	volatile unsigned m_nRenderUs;           // last chunk render time in µs
	volatile unsigned m_nRenderAvgUs;        // exponential moving average
	volatile unsigned m_nRenderPeakUs;       // peak since last API read
	volatile unsigned m_nDeadlineUs;         // deadline for current chunk size
	volatile unsigned m_nRenderMT32Us;       // MT-32 render time for last chunk
	volatile unsigned m_nRenderFluidUs;      // FluidSynth render time for last chunk
	volatile unsigned m_nRenderMixerUs;      // mixer overhead for last chunk
	bool m_bAutoReducePartials;              // auto-reduce MT-32 partials if overloaded

	// Menu long-press tracking
	bool m_bMenuLongPressConsumed;

	// MIDI receive buffers
	CRingBuffer<u8, MIDIRxBufferSize> m_MIDIRxBuffer;
	CRingBuffer<u8, MIDIRxBufferSize> m_WebMIDIRxBuffer;  // Web keyboard → Core 0

	// ---- Sequencer (FluidSequencer, runs entirely on Core 0) ----
	static constexpr size_t SeqPathMax = 260;

	CFluidSequencer* m_pFluidSequencer;  // heap, allocated on first play
	double           m_nTempoMultiplier; // current tempo multiplier (for status reporting)
	bool    m_bSeqLoopEnabled;           // repeat when song finishes
	volatile bool m_bSeqLoading;         // true while SequencerPlayFile is reading the file from SD
	bool    m_bSeqIsPlaying;             // playback active
	bool    m_bSeqFinished;              // song ended naturally (loop=off)
	u32     m_nSeqElapsedUs;             // elapsed µs (updated each Tick)
	u32     m_nSeqDurationUs;            // total song duration, set after LoadFromFile
	u32     m_nSeqEventCount;            // event count, set after LoadFromFile
	u32     m_nSeqFileSizeKB;            // file size in KB, set after LoadFromFile
	char             m_szSeqCurrentFile[SeqPathMax]; // Core 0 writes after LoadFromFile

	// Pause state
	bool             m_bSeqPaused;              // currently paused at m_nSeqPausedTick
	int              m_nSeqPausedTick;          // tick position at pause
	char             m_szSeqPausedFile[SeqPathMax]; // file path at pause

	// Auto-next
	bool             m_bSeqAutoNext;            // advance to next file when song ends

	// Per-file seek memory: remember the last tick when leaving a file so the
	// user can return to roughly the same position.  Bounded fixed-size table.
	static constexpr size_t SeekHistoryMax = 32;
	struct TSeekEntry { char szPath[SeqPathMax]; int nTick; };
	TSeekEntry       m_SeekHistory[SeekHistoryMax];
	size_t           m_nSeekHistoryCount;

	int  SeekHistoryGet(const char* pPath) const;   // -1 if not found
	void SeekHistorySet(const char* pPath, int nTick);

	bool GetAdjacentMIDIFile(const char* pCurrentPath, int nDirection,
	                         char* pOutPath, size_t nMaxLen) const;

	// Active note snapshot (written by OnShortMessage on Core 0 task context)
	u8 m_activeNotes[16][128];   // value = EMidiSource (0 = off)
	u8 m_eMidiSource;            // source tag for the current ParseMIDIBytes batch

	// MIDI recorder (Core 0 only)
	CMidiRecorder m_MidiRecorder;

	// Playlist queue (Core 0 only)
	CPlaylist m_Playlist;

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
