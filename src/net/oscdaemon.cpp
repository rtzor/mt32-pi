//
// oscdaemon.cpp
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
#include <circle/net/netsubsystem.h>
#include <circle/sched/scheduler.h>
#include <circle/util.h>

#include "net/oscdaemon.h"

LOGMODULE("osc");

// ──────────────────────────────────────────────────────────────────────────────
// Helpers
// ──────────────────────────────────────────────────────────────────────────────

// Read big-endian u32 from pointer (no alignment required)
static inline u32 ReadBE32(const u8* p)
{
	return (static_cast<u32>(p[0]) << 24)
	     | (static_cast<u32>(p[1]) << 16)
	     | (static_cast<u32>(p[2]) << 8)
	     |  static_cast<u32>(p[3]);
}

// Read big-endian s32
static inline s32 ReadBES32(const u8* p)
{
	return static_cast<s32>(ReadBE32(p));
}

// Read big-endian float32 via memcpy (avoids strict-aliasing UB)
static inline float ReadBEFloat(const u8* p)
{
	u32 raw = ReadBE32(p);
	float f;
	__builtin_memcpy(&f, &raw, 4);
	return f;
}

// Advance p to the next 4-byte boundary; return nullptr if that exceeds pEnd
const u8* COSCParser::AlignTo4(const u8* p, const u8* pEnd)
{
	const uintptr_t rem = reinterpret_cast<uintptr_t>(p) & 3u;
	if (rem)
		p += (4u - rem);
	return (p <= pEnd) ? p : nullptr;
}

// ──────────────────────────────────────────────────────────────────────────────
// COSCParser::ParseMessage
// ──────────────────────────────────────────────────────────────────────────────
bool COSCParser::ParseMessage(const u8* pData, size_t nSize, TOSCMessage& Msg)
{
	if (!pData || nSize < 4)
		return false;

	const u8* const pEnd = pData + nSize;
	const u8* p = pData;

	// ── Address pattern ──────────────────────────────────────────────────────
	if (*p != '/')
		return false;

	Msg.pAddress = reinterpret_cast<const char*>(p);

	// Advance past the null-terminated address string, then align to 4 bytes
	while (p < pEnd && *p)
		++p;
	if (p >= pEnd)
		return false;
	++p; // consume the NUL

	p = AlignTo4(p, pEnd);
	if (!p)
		return false;

	// ── Type tag string ──────────────────────────────────────────────────────
	if (p >= pEnd || *p != ',')
		return false;

	const char* pTypeTags = reinterpret_cast<const char*>(p) + 1; // skip ','
	const u8* pTypeEnd = p;
	while (pTypeEnd < pEnd && *pTypeEnd)
		++pTypeEnd;
	if (pTypeEnd >= pEnd)
		return false;
	++pTypeEnd; // consume NUL

	p = AlignTo4(pTypeEnd, pEnd);
	if (!p)
		return false;

	// ── Arguments ────────────────────────────────────────────────────────────
	Msg.nArgs = 0;
	for (const char* pTag = pTypeTags; *pTag && Msg.nArgs < OSCMaxArgs; ++pTag)
	{
		TOSCArg& Arg = Msg.Args[Msg.nArgs];
		Arg.Type = static_cast<TOSCArgType>(*pTag);

		switch (*pTag)
		{
			case 'i':
				if (p + 4 > pEnd) return false;
				Arg.i = ReadBES32(p);
				p += 4;
				break;

			case 'f':
				if (p + 4 > pEnd) return false;
				Arg.f = ReadBEFloat(p);
				p += 4;
				break;

			case 's':
			{
				if (p >= pEnd) return false;
				Arg.s = reinterpret_cast<const char*>(p);
				while (p < pEnd && *p)
					++p;
				if (p >= pEnd) return false;
				++p;
				p = AlignTo4(p, pEnd);
				if (!p) return false;
				break;
			}

			case 'b':
			{
				if (p + 4 > pEnd) return false;
				const u32 nBlobSize = ReadBE32(p);
				p += 4;
				if (p + nBlobSize > pEnd) return false;
				Arg.b.pData = p;
				Arg.b.nSize = nBlobSize;
				p += nBlobSize;
				p = AlignTo4(p, pEnd);
				if (!p) return false;
				break;
			}

			case 'T': case 'F': case 'N': case 'I':
				// No data bytes for these types
				break;

			default:
				// Unknown type — skip 4 bytes as per OSC spec
				if (p + 4 > pEnd) return false;
				p += 4;
				break;
		}
		++Msg.nArgs;
	}

	return true;
}

// ──────────────────────────────────────────────────────────────────────────────
// COSCParser::ParseBundle
// ──────────────────────────────────────────────────────────────────────────────
// Bundle format:
//   "#bundle\0"    (8 bytes)
//   timetag        (8 bytes, u64 big-endian — we ignore it)
//   [ u32 element_size, element_data ... ] repeated
//
bool COSCParser::ParseBundle(const u8* pData, size_t nSize,
                             bool (*pHandler)(const TOSCMessage&, void*),
                             void* pUserData)
{
	if (!pData || nSize < 16)
		return false;

	// Verify "#bundle\0" header
	static const char kBundlePrefix[] = "#bundle";
	if (__builtin_memcmp(pData, kBundlePrefix, 8) != 0)
		return false;

	const u8* const pEnd = pData + nSize;
	const u8* p = pData + 16; // skip header (8) + timetag (8)

	while (p + 4 <= pEnd)
	{
		const u32 nElemSize = ReadBE32(p);
		p += 4;

		if (p + nElemSize > pEnd)
			return false;

		// Recurse: each element can itself be a message or a bundle
		ParsePacket(p, nElemSize, pHandler, pUserData);

		p += nElemSize;
	}

	return true;
}

// ──────────────────────────────────────────────────────────────────────────────
// COSCParser::ParsePacket  (top-level entry point)
// ──────────────────────────────────────────────────────────────────────────────
bool COSCParser::ParsePacket(const u8* pData, size_t nSize,
                             bool (*pHandler)(const TOSCMessage&, void*),
                             void* pUserData)
{
	if (!pData || nSize == 0 || !pHandler)
		return false;

	if (pData[0] == '#')
		return ParseBundle(pData, nSize, pHandler, pUserData);

	TOSCMessage Msg;
	if (!ParseMessage(pData, nSize, Msg))
		return false;

	return pHandler(Msg, pUserData);
}

// ──────────────────────────────────────────────────────────────────────────────
// COSCReceiver
// ──────────────────────────────────────────────────────────────────────────────
COSCReceiver::COSCReceiver(COSCHandler* pHandler, u16 nPort)
	: CTask(TASK_STACK_SIZE, true),
	  m_pHandler(pHandler),
	  m_nPort(nPort),
	  m_pSocket(nullptr),
	  m_Buffer{0}
{
}

COSCReceiver::~COSCReceiver()
{
	delete m_pSocket;
}

bool COSCReceiver::Initialize()
{
	assert(m_pSocket == nullptr);

	CNetSubSystem* const pNet = CNetSubSystem::Get();

	if ((m_pSocket = new CSocket(pNet, IPPROTO_UDP)) == nullptr)
		return false;

	if (m_pSocket->Bind(m_nPort) != 0)
	{
		LOGERR("Couldn't bind OSC to port %u", static_cast<unsigned>(m_nPort));
		return false;
	}

	Start();
	return true;
}

bool COSCReceiver::OnMessage(const TOSCMessage& Msg, void* pUserData)
{
	auto* pThis = static_cast<COSCReceiver*>(pUserData);
	pThis->m_pHandler->OnOSCMessage(Msg);
	return true;
}

void COSCReceiver::Run()
{
	assert(m_pHandler != nullptr);
	assert(m_pSocket  != nullptr);

	CScheduler* const pScheduler = CScheduler::Get();

	while (true)
	{
		const int nResult = m_pSocket->Receive(m_Buffer, sizeof(m_Buffer), 0);

		if (nResult < 0)
			LOGERR("OSC socket receive error: %d", nResult);
		else if (nResult > 0)
			COSCParser::ParsePacket(m_Buffer, static_cast<size_t>(nResult),
			                        OnMessage, this);

		pScheduler->Yield();
	}
}
