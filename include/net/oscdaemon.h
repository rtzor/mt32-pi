//
// oscdaemon.h
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

#ifndef _oscdaemon_h
#define _oscdaemon_h

#include <circle/net/socket.h>
#include <circle/sched/task.h>
#include <circle/types.h>

// Maximum UDP datagram size we accept (standard Ethernet MTU minus IP+UDP headers)
static constexpr size_t OSCMaxPacketSize = 1472;
// Maximum number of arguments per OSC message
static constexpr unsigned OSCMaxArgs = 16;

// OSC argument types (subset we handle)
enum class TOSCArgType : char
{
	Int32   = 'i',
	Float32 = 'f',
	String  = 's',
	Blob    = 'b',
	True    = 'T',
	False   = 'F',
	Nil     = 'N',
};

// A single parsed OSC argument
struct TOSCArg
{
	TOSCArgType Type;
	union
	{
		s32         i;   // Int32
		float       f;   // Float32
		const char* s;   // String  (pointer into the receive buffer — not owned)
		struct { const u8* pData; u32 nSize; } b;  // Blob
	};
};

// A fully parsed OSC message
struct TOSCMessage
{
	const char* pAddress;           // OSC address pattern (e.g. "/midi/note_on")
	unsigned    nArgs;
	TOSCArg     Args[OSCMaxArgs];
};

// ──────────────────────────────────────────────────────────────────────────────
// OSC packet parser
//
// Parses a raw UDP payload in-place (no heap, no copies).  The caller must
// keep the buffer alive for as long as any string/blob pointer in the result
// is used.
// ──────────────────────────────────────────────────────────────────────────────
class COSCParser
{
public:
	// Parse one OSC message from [pData, nSize).
	// Returns true and fills *pMsg on success; false on malformed input.
	static bool ParseMessage(const u8* pData, size_t nSize, TOSCMessage& Msg);

	// Parse a bundle and call ParseMessage for every contained message.
	// pHandler is called once per message; stops early if pHandler returns false.
	// Returns false if the packet is not a valid bundle.
	//
	// Usage: call ParsePacket() — it dispatches to ParseMessage or ParseBundle
	// automatically based on the first byte.
	static bool ParseBundle(const u8* pData, size_t nSize,
	                        bool (*pHandler)(const TOSCMessage&, void*),
	                        void* pUserData);

	// Top-level entry point: routes to ParseMessage or ParseBundle.
	// Calls pHandler for every message found (including inside bundles).
	static bool ParsePacket(const u8* pData, size_t nSize,
	                        bool (*pHandler)(const TOSCMessage&, void*),
	                        void* pUserData);

private:
	static const u8* AlignTo4(const u8* p, const u8* pEnd);
};

// ──────────────────────────────────────────────────────────────────────────────
// Handler interface
// ──────────────────────────────────────────────────────────────────────────────
class COSCHandler
{
public:
	virtual void OnOSCMessage(const TOSCMessage& Msg) = 0;
};

// ──────────────────────────────────────────────────────────────────────────────
// UDP receiver task (mirrors CUDPMIDIReceiver pattern)
// ──────────────────────────────────────────────────────────────────────────────
class COSCReceiver : protected CTask
{
public:
	COSCReceiver(COSCHandler* pHandler, u16 nPort);
	virtual ~COSCReceiver() override;

	bool Initialize();
	virtual void Run() override;

private:
	static bool OnMessage(const TOSCMessage& Msg, void* pUserData);

	COSCHandler* m_pHandler;
	u16          m_nPort;
	CSocket*     m_pSocket;
	u8           m_Buffer[OSCMaxPacketSize];
};

#endif
