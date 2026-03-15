//
// midisequencer.h
//

#ifndef _midisequencer_h
#define _midisequencer_h

#include <circle/types.h>

struct TMIDIEvent
{
	u32 nTimeUs;  // absolute time from track start (microseconds)
	u8  Data[3];  // MIDI bytes
	u8  nLength;  // number of valid bytes (1-3)

	bool operator<(const TMIDIEvent& o) const { return nTimeUs < o.nTimeUs; }
};

class CMIDISequencer
{
public:
	static constexpr size_t MaxEvents = 8192;

	CMIDISequencer();
	~CMIDISequencer();

	bool LoadFromFile(const char* pPath);
	void Start(unsigned nStartTicksUs);
	void Stop();
	bool IsLoaded()  const { return m_bLoaded; }
	bool IsRunning() const { return m_bRunning; }

	// Returns MIDI bytes for all events due at nNowTicksUs into pOutBuffer.
	size_t PopDueBytes(unsigned nNowTicksUs, u8* pOutBuffer, size_t nMaxBytes);

	size_t GetEventCount()    const { return m_nEventCount; }
	u32    GetDurationMillis() const { return m_nDurationUs / 1000; }

private:
	bool        m_bLoaded;
	bool        m_bRunning;
	size_t      m_nEventCount;
	size_t      m_nNextEvent;   // playback cursor
	u32         m_nDurationUs;
	u32         m_nStartTicksUs;
	TMIDIEvent* m_pEvents;      // heap-allocated
};

#endif
