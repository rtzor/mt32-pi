//
// synthbase_stub.h
//
// Stub of CSynthBase and a concrete test double (CSynthBaseStub)
// for host-side unit tests.  No Circle dependencies.
//

#ifndef _synthbase_stub_h
#define _synthbase_stub_h

#include "circle_types.h"
#include <cstring>

// Minimal CLCD stub
class CLCD
{
public:
	void Clear() {}
};

// Minimal CMIDIMonitor stub
class CMIDIMonitor
{
public:
	void OnShortMessage(u32) {}
	void AllNotesOff() {}
	void GetChannelLevels(unsigned, float*, float*) const {}
};

// Minimal CUserInterface stub
class CUserInterface {};

// Synth type enum — matches include/synth/synth.h
enum class TSynth { MT32, SoundFont };

// -------------------------------------------------------------------
// CSynthBase — mirrors the real interface in include/synth/synthbase.h
// -------------------------------------------------------------------
class CSynthBase
{
public:
	CSynthBase(unsigned nSampleRate = 48000)
		: m_Lock(TASK_LEVEL),
		  m_nSampleRate(nSampleRate),
		  m_pUI(nullptr)
	{
	}

	virtual ~CSynthBase() = default;

	virtual bool Initialize() { return true; }
	virtual void HandleMIDIShortMessage(u32 nMessage) { m_MIDIMonitor.OnShortMessage(nMessage); }
	virtual void HandleMIDISysExMessage(const u8* pData, size_t nSize) { (void)pData; (void)nSize; }
	virtual bool IsActive() { return true; }
	virtual void AllSoundOff() { m_MIDIMonitor.AllNotesOff(); }
	virtual void SetMasterVolume(u8 nVolume) { (void)nVolume; }
	virtual size_t Render(s16* pOutBuffer, size_t nFrames) { (void)pOutBuffer; return nFrames; }
	virtual size_t Render(float* pOutBuffer, size_t nFrames) { (void)pOutBuffer; return nFrames; }
	virtual void ReportStatus() const {}
	virtual void UpdateLCD(CLCD& LCD, unsigned int nTicks) { (void)LCD; (void)nTicks; }

	virtual const char* GetName() const = 0;
	virtual TSynth GetType() const = 0;

	CSpinLock m_Lock;
	unsigned int m_nSampleRate;
	CMIDIMonitor m_MIDIMonitor;
	CUserInterface* m_pUI;
};

// -------------------------------------------------------------------
// CSynthBaseStub — concrete test double that records calls
// -------------------------------------------------------------------
class CSynthBaseStub : public CSynthBase
{
public:
	CSynthBaseStub(const char* pName, TSynth type, float fRenderValue = 0.0f)
		: CSynthBase(48000),
		  m_pName(pName),
		  m_Type(type),
		  m_fRenderValue(fRenderValue),
		  m_nShortMessageCount(0),
		  m_nLastShortMessage(0),
		  m_nSysExCount(0),
		  m_nLastSysExSize(0),
		  m_nRenderCount(0)
	{
		memset(m_LastSysEx, 0, sizeof(m_LastSysEx));
	}

	const char* GetName() const override { return m_pName; }
	TSynth GetType() const override { return m_Type; }

	void HandleMIDIShortMessage(u32 nMessage) override
	{
		CSynthBase::HandleMIDIShortMessage(nMessage);
		m_nLastShortMessage = nMessage;
		++m_nShortMessageCount;
	}

	void HandleMIDISysExMessage(const u8* pData, size_t nSize) override
	{
		++m_nSysExCount;
		if (nSize > 0 && nSize <= sizeof(m_LastSysEx))
		{
			memcpy(m_LastSysEx, pData, nSize);
			m_nLastSysExSize = nSize;
		}
	}

	size_t Render(float* pOutBuffer, size_t nFrames) override
	{
		++m_nRenderCount;
		for (size_t i = 0; i < nFrames * 2; ++i)
			pOutBuffer[i] = m_fRenderValue;
		return nFrames;
	}

	void Reset()
	{
		m_nShortMessageCount = 0;
		m_nLastShortMessage = 0;
		m_nSysExCount = 0;
		m_nLastSysExSize = 0;
		m_nRenderCount = 0;
		memset(m_LastSysEx, 0, sizeof(m_LastSysEx));
	}

	// --- observable state ---
	const char* m_pName;
	TSynth      m_Type;
	float       m_fRenderValue;

	unsigned    m_nShortMessageCount;
	u32         m_nLastShortMessage;

	unsigned    m_nSysExCount;
	u8          m_LastSysEx[256];
	size_t      m_nLastSysExSize;

	unsigned    m_nRenderCount;
};

#endif
