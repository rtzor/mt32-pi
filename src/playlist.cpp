//
// playlist.cpp
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

#include "playlist.h"

#include <circle/timer.h>
#include <stdio.h>
#include <string.h>

CPlaylist::CPlaylist()
	: m_nCount(0)
	, m_nCurrentIndex(0)
	, m_bRepeat(false)
	, m_bShuffle(false)
{
}

bool CPlaylist::Add(const char* pPath)
{
	if (!pPath || !pPath[0])    return false;
	if (m_nCount >= MaxEntries) return false;

	// Reject duplicates
	for (unsigned i = 0; i < m_nCount; ++i)
		if (strcmp(m_Entries[i], pPath) == 0) return false;

	strncpy(m_Entries[m_nCount], pPath, MaxPathLen - 1);
	m_Entries[m_nCount][MaxPathLen - 1] = '\0';
	++m_nCount;
	return true;
}

void CPlaylist::Remove(unsigned nIndex)
{
	if (nIndex >= m_nCount) return;

	// Shift entries down
	for (unsigned i = nIndex; i + 1 < m_nCount; ++i)
		memcpy(m_Entries[i], m_Entries[i + 1], MaxPathLen);

	--m_nCount;

	if (m_nCount == 0)
		m_nCurrentIndex = 0;
	else if (m_nCurrentIndex >= m_nCount)
		m_nCurrentIndex = m_nCount - 1;
	else if (nIndex < m_nCurrentIndex)
		--m_nCurrentIndex;
}

void CPlaylist::Clear()
{
	m_nCount        = 0;
	m_nCurrentIndex = 0;
}

bool CPlaylist::MoveUp(unsigned nIndex)
{
	if (nIndex == 0 || nIndex >= m_nCount) return false;

	char szTmp[MaxPathLen];
	memcpy(szTmp,                  m_Entries[nIndex - 1], MaxPathLen);
	memcpy(m_Entries[nIndex - 1],  m_Entries[nIndex],     MaxPathLen);
	memcpy(m_Entries[nIndex],      szTmp,                 MaxPathLen);

	if      (m_nCurrentIndex == nIndex)     m_nCurrentIndex = nIndex - 1;
	else if (m_nCurrentIndex == nIndex - 1) m_nCurrentIndex = nIndex;
	return true;
}

bool CPlaylist::MoveDown(unsigned nIndex)
{
	if (nIndex + 1 >= m_nCount) return false;

	char szTmp[MaxPathLen];
	memcpy(szTmp,                  m_Entries[nIndex + 1], MaxPathLen);
	memcpy(m_Entries[nIndex + 1],  m_Entries[nIndex],     MaxPathLen);
	memcpy(m_Entries[nIndex],      szTmp,                 MaxPathLen);

	if      (m_nCurrentIndex == nIndex)     m_nCurrentIndex = nIndex + 1;
	else if (m_nCurrentIndex == nIndex + 1) m_nCurrentIndex = nIndex;
	return true;
}

const char* CPlaylist::GetEntry(unsigned nIndex) const
{
	if (nIndex >= m_nCount) return nullptr;
	return m_Entries[nIndex];
}

const char* CPlaylist::GetCurrent() const
{
	if (m_nCount == 0) return nullptr;
	return m_Entries[m_nCurrentIndex];
}

bool CPlaylist::AdvanceToNext()
{
	if (m_nCount == 0) return false;
	if (m_nCurrentIndex + 1 < m_nCount) { ++m_nCurrentIndex; return true; }
	if (m_bRepeat)                       { m_nCurrentIndex = 0; return true; }
	return false;
}

bool CPlaylist::AdvanceToPrev()
{
	if (m_nCount == 0) return false;
	if (m_nCurrentIndex > 0)  { --m_nCurrentIndex; return true; }
	if (m_bRepeat)            { m_nCurrentIndex = m_nCount - 1; return true; }
	return false;
}

void CPlaylist::SetCurrentByPath(const char* pPath)
{
	if (!pPath) return;
	for (unsigned i = 0; i < m_nCount; ++i)
	{
		if (strcmp(m_Entries[i], pPath) == 0)
		{
			m_nCurrentIndex = i;
			return;
		}
	}
}

void CPlaylist::SetShuffle(bool bShuffle)
{
	m_bShuffle = bShuffle;
	if (bShuffle) Shuffle();
}

void CPlaylist::Shuffle()
{
	if (m_nCount < 2) return;

	// Fisher-Yates using a Numerical Recipes LCG seeded from the system clock
	unsigned nSeed = CTimer::GetClockTicks() ^ 0xDEADBEEFu;
	for (unsigned i = m_nCount - 1; i > 0; --i)
	{
		nSeed = nSeed * 1664525u + 1013904223u;
		unsigned j = nSeed % (i + 1);
		if (i != j)
		{
			char szTmp[MaxPathLen];
			memcpy(szTmp,        m_Entries[i], MaxPathLen);
			memcpy(m_Entries[i], m_Entries[j], MaxPathLen);
			memcpy(m_Entries[j], szTmp,        MaxPathLen);
		}
	}
	m_nCurrentIndex = 0;
}

int CPlaylist::BuildJSON(char* buf, unsigned nSize) const
{
	int n = snprintf(buf, nSize,
		"{\"count\":%u,\"index\":%u,\"repeat\":%s,\"shuffle\":%s,\"entries\":[",
		m_nCount, m_nCurrentIndex,
		m_bRepeat  ? "true" : "false",
		m_bShuffle ? "true" : "false");
	if (n <= 0 || (unsigned)n >= nSize) return -1;

	for (unsigned i = 0; i < m_nCount; ++i)
	{
		// Paths are SD:/file.mid or USB:/file.mid — no chars that need escaping
		int added = snprintf(buf + n, nSize - n,
			"%s\"%s\"", i > 0 ? "," : "", m_Entries[i]);
		if (added <= 0 || (unsigned)(n + added) >= nSize) return -1;
		n += added;
	}

	if ((unsigned)n + 2 >= nSize) return -1;
	buf[n++] = ']';
	buf[n++] = '}';
	buf[n]   = '\0';
	return n;
}
