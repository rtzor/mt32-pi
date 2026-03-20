//
// playlist.h
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

#ifndef _playlist_h
#define _playlist_h

//
// CPlaylist — ordered queue of MIDI files with shuffle and repeat modes.
//
// Designed for Core 0 / single-thread access only.
// Uses fixed-size arrays; no heap allocations.
//
class CPlaylist
{
public:
	static constexpr unsigned MaxEntries = 256;
	static constexpr unsigned MaxPathLen = 256;

	CPlaylist();

	// --- Entry management ---

	// Returns false if full or if path is already in the queue.
	bool Add(const char* pPath);
	void Remove(unsigned nIndex);
	void Clear();

	// Swap with the previous entry; returns false if nIndex == 0.
	bool MoveUp(unsigned nIndex);
	// Swap with the next entry; returns false if nIndex is the last entry.
	bool MoveDown(unsigned nIndex);

	// --- Accessors ---
	unsigned    GetCount()        const { return m_nCount; }
	bool        IsEmpty()         const { return m_nCount == 0; }
	const char* GetEntry(unsigned nIndex) const;
	const char* GetCurrent()      const;
	unsigned    GetCurrentIndex() const { return m_nCurrentIndex; }

	// --- Navigation ---
	// Advance forward one step.  Returns true on success.
	// Returns false (and does NOT wrap) when at the last entry and repeat is off.
	bool AdvanceToNext();

	// Advance backward one step.  Returns true on success.
	// Returns false (and does NOT wrap) when at the first entry and repeat is off.
	bool AdvanceToPrev();

	// Set current index to the first entry whose path equals pPath.
	// No-op if not found.
	void SetCurrentByPath(const char* pPath);

	// --- Modes ---
	void SetRepeat(bool bRepeat)   { m_bRepeat = bRepeat; }
	bool GetRepeat()         const { return m_bRepeat; }

	// Enabling shuffle immediately re-randomises the order and resets
	// the current index to 0.  Disabling is a no-op on the order.
	void SetShuffle(bool bShuffle);
	bool GetShuffle()        const { return m_bShuffle; }

	// Fisher-Yates shuffle using CTimer::GetClockTicks() as seed.
	void Shuffle();

	// Serialize to a compact JSON object:
	//   {"count":N,"index":N,"repeat":bool,"shuffle":bool,"entries":["path",...]}
	// Returns bytes written (not including NUL) or -1 on buffer overflow.
	int BuildJSON(char* buf, unsigned nSize) const;

private:
	char     m_Entries[MaxEntries][MaxPathLen];
	unsigned m_nCount;
	unsigned m_nCurrentIndex;
	bool     m_bRepeat;
	bool     m_bShuffle;
};

#endif // _playlist_h
