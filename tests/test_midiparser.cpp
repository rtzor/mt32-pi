//
// test_midiparser.cpp
//
// Unit tests for CMIDIParser
//

#include "doctest/doctest.h"
#include "midiparser.h"

#include <cassert>
#include <cstring>
#include <vector>

// ---------------------------------------------------------------
// Concrete test double
// ---------------------------------------------------------------

struct TEvent
{
	bool bSysEx;
	u32  nMsg;               // valid only when bSysEx == false
	std::vector<u8> sysex;  // valid only when bSysEx == true
};

class CTestParser : public CMIDIParser
{
public:
	std::vector<TEvent> Events;
	int nUnexpected = 0;
	int nOverflow   = 0;

	void Reset()
	{
		Events.clear();
		nUnexpected = 0;
		nOverflow   = 0;
	}

protected:
	void OnShortMessage(u32 nMsg) override
	{
		Events.push_back({false, nMsg, {}});
	}

	void OnSysExMessage(const u8* pData, size_t nSize) override
	{
		Events.push_back({true, 0, std::vector<u8>(pData, pData + nSize)});
	}

	void OnUnexpectedStatus() override { ++nUnexpected; }
	void OnSysExOverflow()    override { ++nOverflow; }
};

// Build a 3-byte MIDI short message word
static u32 Msg3(u8 s, u8 d1, u8 d2)
{
	return static_cast<u32>(s) | (static_cast<u32>(d1) << 8) | (static_cast<u32>(d2) << 16);
}

// ---------------------------------------------------------------
// Basic short messages
// ---------------------------------------------------------------

TEST_CASE("Parser: Note On 3 bytes")
{
	CTestParser p;
	const u8 buf[] = {0x90, 0x3C, 0x7F};
	p.ParseMIDIBytes(buf, sizeof(buf));
	REQUIRE(p.Events.size() == 1);
	CHECK_FALSE(p.Events[0].bSysEx);
	CHECK(p.Events[0].nMsg == Msg3(0x90, 0x3C, 0x7F));
}

TEST_CASE("Parser: Note Off 3 bytes")
{
	CTestParser p;
	const u8 buf[] = {0x80, 0x3C, 0x00};
	p.ParseMIDIBytes(buf, sizeof(buf));
	REQUIRE(p.Events.size() == 1);
	CHECK(p.Events[0].nMsg == Msg3(0x80, 0x3C, 0x00));
}

TEST_CASE("Parser: CC 3 bytes")
{
	CTestParser p;
	const u8 buf[] = {0xB0, 0x07, 0x64};
	p.ParseMIDIBytes(buf, sizeof(buf));
	REQUIRE(p.Events.size() == 1);
	CHECK(p.Events[0].nMsg == Msg3(0xB0, 0x07, 0x64));
}

TEST_CASE("Parser: Program Change is 2 bytes")
{
	CTestParser p;
	const u8 buf[] = {0xC0, 0x05};
	p.ParseMIDIBytes(buf, sizeof(buf));
	REQUIRE(p.Events.size() == 1);
	CHECK(p.Events[0].nMsg == Msg3(0xC0, 0x05, 0x00));
}

TEST_CASE("Parser: Channel Pressure is 2 bytes")
{
	CTestParser p;
	const u8 buf[] = {0xD0, 0x40};
	p.ParseMIDIBytes(buf, sizeof(buf));
	REQUIRE(p.Events.size() == 1);
	CHECK(p.Events[0].nMsg == Msg3(0xD0, 0x40, 0x00));
}

TEST_CASE("Parser: Pitch Bend 3 bytes")
{
	CTestParser p;
	const u8 buf[] = {0xE0, 0x00, 0x40};
	p.ParseMIDIBytes(buf, sizeof(buf));
	REQUIRE(p.Events.size() == 1);
	CHECK(p.Events[0].nMsg == Msg3(0xE0, 0x00, 0x40));
}

// ---------------------------------------------------------------
// Multiple messages in one buffer
// ---------------------------------------------------------------

TEST_CASE("Parser: two consecutive NoteOns")
{
	CTestParser p;
	const u8 buf[] = {0x91, 0x3C, 0x7F, 0x91, 0x40, 0x60};
	p.ParseMIDIBytes(buf, sizeof(buf));
	REQUIRE(p.Events.size() == 2);
	CHECK(p.Events[0].nMsg == Msg3(0x91, 0x3C, 0x7F));
	CHECK(p.Events[1].nMsg == Msg3(0x91, 0x40, 0x60));
}

// ---------------------------------------------------------------
// Running Status
// ---------------------------------------------------------------

TEST_CASE("Parser: running status for Note On")
{
	CTestParser p;
	// Send status once, then two sets of data bytes
	const u8 buf[] = {0x90, 0x3C, 0x7F, 0x40, 0x60};
	p.ParseMIDIBytes(buf, sizeof(buf));
	REQUIRE(p.Events.size() == 2);
	CHECK(p.Events[0].nMsg == Msg3(0x90, 0x3C, 0x7F));
	CHECK(p.Events[1].nMsg == Msg3(0x90, 0x40, 0x60));
}

TEST_CASE("Parser: running status for CC")
{
	CTestParser p;
	const u8 buf[] = {0xB0, 0x07, 0x64, 0x0A, 0x40, 0x0B, 0x7F};
	p.ParseMIDIBytes(buf, sizeof(buf));
	REQUIRE(p.Events.size() == 3);
	CHECK(p.Events[0].nMsg == Msg3(0xB0, 0x07, 0x64));
	CHECK(p.Events[1].nMsg == Msg3(0xB0, 0x0A, 0x40));
	CHECK(p.Events[2].nMsg == Msg3(0xB0, 0x0B, 0x7F));
}

TEST_CASE("Parser: new status byte resets running status")
{
	CTestParser p;
	const u8 buf[] = {0x90, 0x3C, 0x7F, 0xB0, 0x07, 0x64};
	p.ParseMIDIBytes(buf, sizeof(buf));
	REQUIRE(p.Events.size() == 2);
	CHECK(p.Events[0].nMsg == Msg3(0x90, 0x3C, 0x7F));
	CHECK(p.Events[1].nMsg == Msg3(0xB0, 0x07, 0x64));
}

// ---------------------------------------------------------------
// SysEx
// ---------------------------------------------------------------

TEST_CASE("Parser: complete SysEx")
{
	CTestParser p;
	const u8 buf[] = {0xF0, 0x41, 0x10, 0x42, 0xF7};
	p.ParseMIDIBytes(buf, sizeof(buf));
	REQUIRE(p.Events.size() == 1);
	CHECK(p.Events[0].bSysEx);
	REQUIRE(p.Events[0].sysex.size() == 5);
	CHECK(p.Events[0].sysex[0] == 0xF0);
	CHECK(p.Events[0].sysex[4] == 0xF7);
}

TEST_CASE("Parser: SysEx split across two calls")
{
	CTestParser p;
	const u8 part1[] = {0xF0, 0x41, 0x10};
	const u8 part2[] = {0x42, 0xF7};
	p.ParseMIDIBytes(part1, sizeof(part1));
	CHECK(p.Events.empty());  // not complete yet
	p.ParseMIDIBytes(part2, sizeof(part2));
	REQUIRE(p.Events.size() == 1);
	CHECK(p.Events[0].bSysEx);
	CHECK(p.Events[0].sysex.size() == 5);
}

TEST_CASE("Parser: two SysEx messages")
{
	CTestParser p;
	const u8 buf[] = {0xF0, 0x7E, 0x7F, 0x09, 0x01, 0xF7,
	                  0xF0, 0x41, 0x10, 0xF7};
	p.ParseMIDIBytes(buf, sizeof(buf));
	REQUIRE(p.Events.size() == 2);
	CHECK(p.Events[0].bSysEx);
	CHECK(p.Events[1].bSysEx);
	CHECK(p.Events[0].sysex.size() == 6);
	CHECK(p.Events[1].sysex.size() == 4);
}

TEST_CASE("Parser: SysEx overflow triggers callback and resets")
{
	CTestParser p;
	// SysEx with 1001 bytes of data (exceeds 1000-byte buffer)
	std::vector<u8> msg;
	msg.push_back(0xF0);
	for (int i = 0; i < 1000; ++i)
		msg.push_back(0x01);  // data (no MSB = valid SysEx data bytes)
	msg.push_back(0xF7);

	p.ParseMIDIBytes(msg.data(), msg.size());
	CHECK(p.nOverflow == 1);
	CHECK(p.Events.empty());  // overflow discards the message
}

TEST_CASE("Parser: SysEx interrupted by status restarts parser")
{
	CTestParser p;
	// F0 ... but then a NoteOn interrupts before F7
	const u8 buf[] = {0xF0, 0x41, 0x90, 0x3C, 0x7F};
	p.ParseMIDIBytes(buf, sizeof(buf));
	// The interrupted SysEx triggers OnUnexpectedStatus
	CHECK(p.nUnexpected == 1);
	// The NoteOn is then parsed normally
	REQUIRE(p.Events.size() == 1);
	CHECK_FALSE(p.Events[0].bSysEx);
	CHECK(p.Events[0].nMsg == Msg3(0x90, 0x3C, 0x7F));
}

// ---------------------------------------------------------------
// System Real-Time (can appear anywhere)
// ---------------------------------------------------------------

TEST_CASE("Parser: Real-Time byte inside NoteOn data")
{
	CTestParser p;
	// Timing Clock (0xF8) can appear mid-message
	const u8 buf[] = {0x90, 0xF8, 0x3C, 0x7F};
	p.ParseMIDIBytes(buf, sizeof(buf));
	// F8 fires as a standalone event, then NoteOn completes
	REQUIRE(p.Events.size() == 2);
	// RT message first
	CHECK(p.Events[0].nMsg == 0xF8);
	// Then the NoteOn
	CHECK(p.Events[1].nMsg == Msg3(0x90, 0x3C, 0x7F));
}

TEST_CASE("Parser: Real-Time byte inside SysEx")
{
	CTestParser p;
	const u8 buf[] = {0xF0, 0x41, 0xF8, 0x10, 0xF7};
	p.ParseMIDIBytes(buf, sizeof(buf));
	// F8 fires immediately; SysEx still completes (without the RT byte)
	REQUIRE(p.Events.size() == 2);
	CHECK(p.Events[0].nMsg == 0xF8);
	CHECK(p.Events[1].bSysEx);
	// SysEx body: F0 41 10 F7 (F8 not stored in SysEx buffer)
	CHECK(p.Events[1].sysex.size() == 4);
}

TEST_CASE("Parser: undefined Real-Time bytes 0xF9 and 0xFD are ignored")
{
	CTestParser p;
	const u8 buf[] = {0xF9, 0xFD};
	p.ParseMIDIBytes(buf, sizeof(buf));
	CHECK(p.Events.empty());
}

TEST_CASE("Parser: all valid Real-Time bytes fire one event each")
{
	CTestParser p;
	// 0xF8 Timing Clock, 0xFA Start, 0xFB Continue, 0xFC Stop, 0xFE Active Sense, 0xFF Reset
	const u8 buf[] = {0xF8, 0xFA, 0xFB, 0xFC, 0xFE, 0xFF};
	p.ParseMIDIBytes(buf, sizeof(buf));
	REQUIRE(p.Events.size() == 6);
	CHECK(p.Events[0].nMsg == 0xF8);
	CHECK(p.Events[1].nMsg == 0xFA);
	CHECK(p.Events[2].nMsg == 0xFB);
	CHECK(p.Events[3].nMsg == 0xFC);
	CHECK(p.Events[4].nMsg == 0xFE);
	CHECK(p.Events[5].nMsg == 0xFF);
}

// ---------------------------------------------------------------
// System Common
// ---------------------------------------------------------------

TEST_CASE("Parser: Tune Request (0xF6) fires single-byte event")
{
	CTestParser p;
	const u8 buf[] = {0xF6};
	p.ParseMIDIBytes(buf, sizeof(buf));
	REQUIRE(p.Events.size() == 1);
	CHECK(p.Events[0].nMsg == 0xF6);
}

TEST_CASE("Parser: Tune Request fires a single event")
{
	CTestParser p;
	// Establish running status, then Tune Request, then a lone data byte
	const u8 buf[] = {0x90, 0x3C, 0x7F, 0xF6, 0x40};
	p.ParseMIDIBytes(buf, sizeof(buf));
	// NoteOn fires; F6 fires; 0x40 alone is an incomplete message (no fire)
	REQUIRE(p.Events.size() == 2);
	CHECK(p.Events[0].nMsg == Msg3(0x90, 0x3C, 0x7F));
	CHECK(p.Events[1].nMsg == 0xF6);
}

TEST_CASE("Parser: F4/F5/F7 alone clear running status without firing event")
{
	CTestParser p;
	// Establish running status then send an orphan F7 (EOX without SysEx)
	const u8 buf[] = {0x90, 0x3C, 0x7F, 0xF7, 0x40, 0x60};
	p.ParseMIDIBytes(buf, sizeof(buf));
	// NoteOn fires; F7 clears running status; 0x40 0x60 are orphan data bytes
	REQUIRE(p.Events.size() == 1);
	CHECK(p.Events[0].nMsg == Msg3(0x90, 0x3C, 0x7F));
}

// ---------------------------------------------------------------
// Unexpected status
// ---------------------------------------------------------------

TEST_CASE("Parser: unexpected status mid 3-byte message fires callback")
{
	CTestParser p;
	// Start NoteOn, get only one data byte, then another status arrives
	const u8 buf[] = {0x90, 0x3C, 0xB0, 0x07, 0x64};
	p.ParseMIDIBytes(buf, sizeof(buf));
	CHECK(p.nUnexpected == 1);
	// The CC still parses correctly
	REQUIRE(p.Events.size() == 1);
	CHECK(p.Events[0].nMsg == Msg3(0xB0, 0x07, 0x64));
}

// ---------------------------------------------------------------
// bIgnoreNoteOns flag
// ---------------------------------------------------------------

TEST_CASE("Parser: bIgnoreNoteOns suppresses Note On events")
{
	CTestParser p;
	const u8 buf[] = {0x90, 0x3C, 0x7F,
	                  0x80, 0x3C, 0x00,
	                  0xB0, 0x07, 0x64};
	p.ParseMIDIBytes(buf, sizeof(buf), /*bIgnoreNoteOns=*/true);
	// Note On is suppressed; NoteOff and CC still fire
	REQUIRE(p.Events.size() == 2);
	CHECK(p.Events[0].nMsg == Msg3(0x80, 0x3C, 0x00));
	CHECK(p.Events[1].nMsg == Msg3(0xB0, 0x07, 0x64));
}

TEST_CASE("Parser: bIgnoreNoteOns suppresses all 0x90 status messages")
{
	CTestParser p;
	// velocity=0 is still a 0x90 status byte, so bIgnoreNoteOns suppresses it
	const u8 buf[] = {0x90, 0x3C, 0x00};
	p.ParseMIDIBytes(buf, sizeof(buf), /*bIgnoreNoteOns=*/true);
	// ALL 0x90-status messages are suppressed, including vel=0
	REQUIRE(p.Events.size() == 0);
}

// ---------------------------------------------------------------
// Empty / byte-at-a-time
// ---------------------------------------------------------------

TEST_CASE("Parser: empty buffer produces no events")
{
	CTestParser p;
	p.ParseMIDIBytes(nullptr, 0);
	CHECK(p.Events.empty());
}

TEST_CASE("Parser: byte-at-a-time NoteOn")
{
	CTestParser p;
	const u8 buf[] = {0x92, 0x3C, 0x7F};
	for (size_t i = 0; i < sizeof(buf); ++i)
		p.ParseMIDIBytes(&buf[i], 1);
	REQUIRE(p.Events.size() == 1);
	CHECK(p.Events[0].nMsg == Msg3(0x92, 0x3C, 0x7F));
}

TEST_CASE("Parser: byte-at-a-time SysEx")
{
	CTestParser p;
	const u8 buf[] = {0xF0, 0x41, 0x10, 0x42, 0xF7};
	for (size_t i = 0; i < sizeof(buf); ++i)
	{
		p.ParseMIDIBytes(&buf[i], 1);
		if (i < sizeof(buf) - 1)
			CHECK(p.Events.empty());
	}
	REQUIRE(p.Events.size() == 1);
	CHECK(p.Events[0].bSysEx);
}
