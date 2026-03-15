//
// midisequencer.cpp
//

#include <circle/logger.h>
#include <fatfs/ff.h>

#include "midisequencer.h"
#include "utility.h"

LOGMODULE("midisequencer");

// ---- Low-level helpers ----

static inline u16 ReadBE16(const u8* p)
{
	return (static_cast<u16>(p[0]) << 8) | p[1];
}

static inline u32 ReadBE32(const u8* p)
{
	return (static_cast<u32>(p[0]) << 24) |
	       (static_cast<u32>(p[1]) << 16) |
	       (static_cast<u32>(p[2]) <<  8) |
	        static_cast<u32>(p[3]);
}

static u32 ReadVarLen(const u8* pData, size_t nEnd, size_t& nPos)
{
	u32 nValue = 0;
	for (int i = 0; i < 4 && nPos < nEnd; ++i)
	{
		const u8 b = pData[nPos++];
		nValue = (nValue << 7) | (b & 0x7F);
		if (!(b & 0x80))
			break;
	}
	return nValue;
}

// ---- Tempo map (stack-safe: 256 * 8 = 2 KB) ----

struct TTempoEntry
{
	u32 nTick;
	u32 nTempoUs;
};

static constexpr size_t MaxTempoEntries = 256;

// Convert absolute tick to microseconds using the accumulated tempo map.
static u32 TickToUs(u32 nTick, const TTempoEntry* pMap, size_t nEntries, u16 nTPB)
{
	u32 nUs        = 0;
	u32 nTickBase  = 0;
	u32 nTempo     = 500000; // 120 BPM default

	for (size_t i = 0; i < nEntries; ++i)
	{
		if (pMap[i].nTick >= nTick)
			break;
		nUs      += static_cast<u32>((static_cast<u64>(pMap[i].nTick - nTickBase) * nTempo) / nTPB);
		nTickBase = pMap[i].nTick;
		nTempo    = pMap[i].nTempoUs;
	}
	nUs += static_cast<u32>((static_cast<u64>(nTick - nTickBase) * nTempo) / nTPB);
	return nUs;
}

// ---- Parse a single MTrk chunk ----
// Appends MIDI voice events into pEvents (nTimeUs = abs tick temporarily).
// Appends tempo changes into pTempoMap.
// Returns number of events appended.

static size_t ParseTrack(const u8* pBuf, size_t nTrackStart, size_t nTrackEnd,
                         TMIDIEvent* pEvents, size_t nMaxEvents, size_t nEventOffset,
                         TTempoEntry* pTempoMap, size_t nMaxTempo, size_t& nTempoEntries)
{
	size_t nPos       = nTrackStart;
	u32    nAbsTick   = 0;
	u8     nLastStatus = 0;
	size_t nAdded     = 0;

	while (nPos < nTrackEnd)
	{
		const u32 nDelta = ReadVarLen(pBuf, nTrackEnd, nPos);
		nAbsTick += nDelta;

		if (nPos >= nTrackEnd)
			break;

		const u8 nByte = pBuf[nPos];

		// Meta event
		if (nByte == 0xFF)
		{
			nPos++;
			if (nPos >= nTrackEnd) break;
			const u8 nType = pBuf[nPos++];
			const u32 nMetaLen = ReadVarLen(pBuf, nTrackEnd, nPos);

			if (nType == 0x51 && nMetaLen == 3 && nPos + 3 <= nTrackEnd && nTempoEntries < nMaxTempo)
			{
				// Insert sorted by tick
				const u32 nTempo = (static_cast<u32>(pBuf[nPos]) << 16) |
				                   (static_cast<u32>(pBuf[nPos + 1]) << 8) |
				                    static_cast<u32>(pBuf[nPos + 2]);
				size_t nIns = nTempoEntries;
				while (nIns > 0 && pTempoMap[nIns - 1].nTick > nAbsTick)
				{
					pTempoMap[nIns] = pTempoMap[nIns - 1];
					--nIns;
				}
				pTempoMap[nIns] = {nAbsTick, nTempo};
				++nTempoEntries;
			}
			else if (nType == 0x2F)
			{
				nPos += nMetaLen;
				break; // End of Track
			}
			nPos += nMetaLen;
			continue;
		}

		// SysEx — skip
		if (nByte == 0xF0 || nByte == 0xF7)
		{
			nPos++;
			const u32 nLen = ReadVarLen(pBuf, nTrackEnd, nPos);
			nPos += nLen;
			nLastStatus = 0;
			continue;
		}

		// Voice message (with running status)
		u8 nStatus;
		if (nByte & 0x80)
		{
			nStatus = nByte;
			nLastStatus = nByte;
			nPos++;
		}
		else
		{
			nStatus = nLastStatus;
		}

		if (nStatus < 0x80) { nPos++; continue; }

		const u8 nStatusType = nStatus & 0xF0;
		const u8 nDataBytes  = (nStatusType == 0xC0 || nStatusType == 0xD0) ? 1 : 2;

		if (nPos + nDataBytes > nTrackEnd)
			break;

		const size_t nIndex = nEventOffset + nAdded;
		if (nIndex < nMaxEvents)
		{
			TMIDIEvent& ev = pEvents[nIndex];
			ev.nTimeUs = nAbsTick; // temporarily stores ticks; converted later
			ev.Data[0] = nStatus;
			ev.Data[1] = pBuf[nPos];
			ev.Data[2] = (nDataBytes == 2) ? pBuf[nPos + 1] : 0;
			ev.nLength = 1 + nDataBytes;
			++nAdded;
		}
		nPos += nDataBytes;
	}

	return nAdded;
}

// ---- Constructor / Destructor ----

CMIDISequencer::CMIDISequencer()
	: m_bLoaded(false),
	  m_bRunning(false),
	  m_nEventCount(0),
	  m_nNextEvent(0),
	  m_nDurationUs(0),
	  m_nStartTicksUs(0),
	  m_pEvents(nullptr)
{
}

CMIDISequencer::~CMIDISequencer()
{
	delete[] m_pEvents;
}

bool CMIDISequencer::LoadFromFile(const char* pPath)
{
	if (!pPath)
		return false;

	FIL File;
	if (f_open(&File, pPath, FA_READ) != FR_OK)
		return false;

	const FSIZE_t nFileSize = f_size(&File);
	if (nFileSize < 14 || nFileSize > 1024 * 1024)
	{
		f_close(&File);
		return false;
	}

	u8* const pBuf = new u8[nFileSize];
	if (!pBuf)
	{
		f_close(&File);
		return false;
	}

	UINT nBytesRead = 0;
	const FRESULT res = f_read(&File, pBuf, static_cast<UINT>(nFileSize), &nBytesRead);
	f_close(&File);

	if (res != FR_OK || nBytesRead != static_cast<UINT>(nFileSize))
	{
		delete[] pBuf;
		return false;
	}

	// ---- Parse MThd ----
	size_t nPos = 0;
	if (nFileSize < 14 ||
	    pBuf[0] != 'M' || pBuf[1] != 'T' || pBuf[2] != 'h' || pBuf[3] != 'd')
	{
		delete[] pBuf;
		return false;
	}
	nPos += 4;

	const u32 nHdrLen   = ReadBE32(pBuf + nPos); nPos += 4;
	const u16 nFormat   = ReadBE16(pBuf + nPos); nPos += 2;
	const u16 nTracks   = ReadBE16(pBuf + nPos); nPos += 2;
	const u16 nDivision = ReadBE16(pBuf + nPos); nPos += 2;

	// SMPTE timecode division not supported
	if (nDivision & 0x8000)
	{
		delete[] pBuf;
		return false;
	}

	// Formats 0 and 1 only
	if (nFormat > 1 || nTracks == 0)
	{
		delete[] pBuf;
		return false;
	}

	const u16 nTicksPerBeat = nDivision;

	// Skip any extra header bytes
	if (nHdrLen > 6)
		nPos += nHdrLen - 6;

	// ---- Allocate event array once ----
	if (!m_pEvents)
	{
		m_pEvents = new TMIDIEvent[MaxEvents];
		if (!m_pEvents)
		{
			delete[] pBuf;
			return false;
		}
	}

	// ---- Tempo map (stack, 2 KB) ----
	TTempoEntry TempoMap[MaxTempoEntries];
	size_t nTempoEntries = 0;

	// ---- Parse all tracks ----
	m_nEventCount = 0;

	for (u16 nTrack = 0; nTrack < nTracks; ++nTrack)
	{
		// Locate MTrk header
		while (nPos + 8 <= static_cast<size_t>(nFileSize))
		{
			if (pBuf[nPos] == 'M' && pBuf[nPos+1] == 'T' &&
			    pBuf[nPos+2] == 'r' && pBuf[nPos+3] == 'k')
				break;
			++nPos; // skip unknown chunk bytes
		}

		if (nPos + 8 > static_cast<size_t>(nFileSize))
			break;

		nPos += 4;
		const u32 nTrackLen = ReadBE32(pBuf + nPos); nPos += 4;
		const size_t nTrackEnd = nPos + nTrackLen;

		if (nTrackEnd > static_cast<size_t>(nFileSize))
			break;

		const size_t nAdded = ParseTrack(pBuf, nPos, nTrackEnd,
		                                 m_pEvents, MaxEvents, m_nEventCount,
		                                 TempoMap, MaxTempoEntries, nTempoEntries);
		m_nEventCount += nAdded;
		nPos = nTrackEnd;
	}

	delete[] pBuf;

	if (m_nEventCount == 0)
	{
		m_bLoaded = false;
		return false;
	}

	// ---- Sort all events by tick (nTimeUs currently holds ticks) ----
	Utility::QSort(m_pEvents,
	               Utility::Comparator::LessThan<TMIDIEvent>,
	               0, m_nEventCount - 1);

	// ---- Convert ticks → microseconds in-place ----
	u32 nMaxUs = 0;
	for (size_t i = 0; i < m_nEventCount; ++i)
	{
		const u32 nUs    = TickToUs(m_pEvents[i].nTimeUs, TempoMap, nTempoEntries, nTicksPerBeat);
		m_pEvents[i].nTimeUs = nUs;
		if (nUs > nMaxUs)
			nMaxUs = nUs;
	}

	m_nDurationUs = nMaxUs;
	m_nNextEvent  = 0;
	m_bLoaded     = true;
	m_bRunning    = false;

	LOGNOTE("fmt%u %u tracks, %u events, %u ms",
	         static_cast<unsigned>(nFormat),
	         static_cast<unsigned>(nTracks),
	         static_cast<unsigned>(m_nEventCount),
	         m_nDurationUs / 1000);
	return true;
}

void CMIDISequencer::Start(unsigned nStartTicksUs)
{
	if (!m_bLoaded)
		return;

	m_nNextEvent    = 0;
	m_nStartTicksUs = nStartTicksUs;
	m_bRunning      = true;
}

void CMIDISequencer::Stop()
{
	m_bRunning = false;
}

size_t CMIDISequencer::PopDueBytes(unsigned nNowTicksUs, u8* pOutBuffer, size_t nMaxBytes)
{
	if (!m_bRunning || !m_pEvents)
		return 0;

	const u32 nElapsedUs = nNowTicksUs - m_nStartTicksUs;

	size_t nWritten = 0;
	while (m_nNextEvent < m_nEventCount)
	{
		const TMIDIEvent& ev = m_pEvents[m_nNextEvent];
		if (ev.nTimeUs > nElapsedUs)
			break;
		if (nWritten + ev.nLength > nMaxBytes)
			break;
		for (u8 i = 0; i < ev.nLength; ++i)
			pOutBuffer[nWritten++] = ev.Data[i];
		++m_nNextEvent;
	}

	// Auto-stop at end of song
	if (m_nNextEvent >= m_nEventCount)
		m_bRunning = false;

	return nWritten;
}
