//
// test_osc.cpp
//
// Unit tests for COSCParser
//

#include "doctest/doctest.h"
#include "net/oscdaemon.h"

#include <cstring>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

// Collector used with ParsePacket
static std::vector<TOSCMessage> g_Messages;

static bool CollectMessage(const TOSCMessage& Msg, void* /*pUserData*/)
{
	g_Messages.push_back(Msg);
	return true;
}

// Build a 4-byte-aligned block from a string (including NUL, padded to 4B)
static void AppendPadded(std::vector<u8>& buf, const char* str)
{
	size_t n = strlen(str) + 1; // include NUL
	for (size_t i = 0; i < n; ++i)
		buf.push_back(static_cast<u8>(str[i]));
	while (buf.size() % 4 != 0)
		buf.push_back(0);
}

// Append big-endian u32
static void AppendBE32(std::vector<u8>& buf, u32 val)
{
	buf.push_back(static_cast<u8>((val >> 24) & 0xFF));
	buf.push_back(static_cast<u8>((val >> 16) & 0xFF));
	buf.push_back(static_cast<u8>((val >>  8) & 0xFF));
	buf.push_back(static_cast<u8>((val      ) & 0xFF));
}

// Append big-endian s32 (reuse BE32)
static void AppendBES32(std::vector<u8>& buf, s32 val)
{
	AppendBE32(buf, static_cast<u32>(val));
}

// ─────────────────────────────────────────────────────────────────────────────
// ParseMessage tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("ParseMessage: basic note_on (iii)")
{
	// Packet:
	//   "/midi/note_on\0\0\0" (16 bytes, 4-aligned)
	//   ",iii\0\0\0\0"        (8 bytes)
	//   channel=0, note=60, velocity=100
	std::vector<u8> pkt;
	AppendPadded(pkt, "/midi/note_on");
	AppendPadded(pkt, ",iii");
	AppendBES32(pkt, 0);   // channel
	AppendBES32(pkt, 60);  // note
	AppendBES32(pkt, 100); // velocity

	TOSCMessage msg;
	CHECK(COSCParser::ParseMessage(pkt.data(), pkt.size(), msg));
	CHECK(strcmp(msg.pAddress, "/midi/note_on") == 0);
	CHECK(msg.nArgs == 3);
	CHECK(msg.Args[0].Type == TOSCArgType::Int32);
	CHECK(msg.Args[0].i == 0);
	CHECK(msg.Args[1].Type == TOSCArgType::Int32);
	CHECK(msg.Args[1].i == 60);
	CHECK(msg.Args[2].Type == TOSCArgType::Int32);
	CHECK(msg.Args[2].i == 100);
}

TEST_CASE("ParseMessage: float argument")
{
	std::vector<u8> pkt;
	AppendPadded(pkt, "/test/float");
	AppendPadded(pkt, ",f");
	// 1.0f big-endian = 0x3F800000
	AppendBE32(pkt, 0x3F800000u);

	TOSCMessage msg;
	CHECK(COSCParser::ParseMessage(pkt.data(), pkt.size(), msg));
	CHECK(msg.nArgs == 1);
	CHECK(msg.Args[0].Type == TOSCArgType::Float32);
	CHECK(msg.Args[0].f == doctest::Approx(1.0f));
}

TEST_CASE("ParseMessage: string argument")
{
	std::vector<u8> pkt;
	AppendPadded(pkt, "/mt32pi/synth");
	AppendPadded(pkt, ",s");
	AppendPadded(pkt, "soundfont");

	TOSCMessage msg;
	CHECK(COSCParser::ParseMessage(pkt.data(), pkt.size(), msg));
	CHECK(msg.nArgs == 1);
	CHECK(msg.Args[0].Type == TOSCArgType::String);
	CHECK(strcmp(msg.Args[0].s, "soundfont") == 0);
}

TEST_CASE("ParseMessage: blob argument")
{
	std::vector<u8> pkt;
	AppendPadded(pkt, "/midi/raw");
	AppendPadded(pkt, ",b");
	// blob: u32 size + data, padded to 4
	const u8 midiBytes[] = { 0x90, 0x3C, 0x64 }; // note_on ch0 C4 vel100
	AppendBE32(pkt, sizeof(midiBytes));
	for (u8 b : midiBytes)
		pkt.push_back(b);
	while (pkt.size() % 4 != 0)
		pkt.push_back(0);

	TOSCMessage msg;
	CHECK(COSCParser::ParseMessage(pkt.data(), pkt.size(), msg));
	CHECK(msg.nArgs == 1);
	CHECK(msg.Args[0].Type == TOSCArgType::Blob);
	CHECK(msg.Args[0].b.nSize == sizeof(midiBytes));
	CHECK(memcmp(msg.Args[0].b.pData, midiBytes, sizeof(midiBytes)) == 0);
}

TEST_CASE("ParseMessage: True/False/Nil sentinels (no data)")
{
	std::vector<u8> pkt;
	AppendPadded(pkt, "/test/flags");
	AppendPadded(pkt, ",TFN");
	// no data bytes for T, F, N

	TOSCMessage msg;
	CHECK(COSCParser::ParseMessage(pkt.data(), pkt.size(), msg));
	CHECK(msg.nArgs == 3);
	CHECK(msg.Args[0].Type == TOSCArgType::True);
	CHECK(msg.Args[1].Type == TOSCArgType::False);
	CHECK(msg.Args[2].Type == TOSCArgType::Nil);
}

TEST_CASE("ParseMessage: pitch bend (ii)")
{
	std::vector<u8> pkt;
	AppendPadded(pkt, "/midi/pitch_bend");
	AppendPadded(pkt, ",ii");
	AppendBES32(pkt, 0);     // channel
	AppendBES32(pkt, 8192);  // centre

	TOSCMessage msg;
	CHECK(COSCParser::ParseMessage(pkt.data(), pkt.size(), msg));
	CHECK(msg.nArgs == 2);
	CHECK(msg.Args[0].i == 0);
	CHECK(msg.Args[1].i == 8192);
}

TEST_CASE("ParseMessage: rejects empty packet")
{
	TOSCMessage msg;
	CHECK_FALSE(COSCParser::ParseMessage(nullptr, 0, msg));
	u8 empty[4] = {0};
	CHECK_FALSE(COSCParser::ParseMessage(empty, 1, msg)); // address must start with '/'
}

TEST_CASE("ParseMessage: rejects truncated packet")
{
	// valid address, but truncated before type tag
	std::vector<u8> pkt;
	AppendPadded(pkt, "/midi/cc");
	// Truncate: provide only half the type tag block
	pkt.push_back(',');
	pkt.push_back('i');
	// no NUL → parse should fail

	TOSCMessage msg;
	CHECK_FALSE(COSCParser::ParseMessage(pkt.data(), pkt.size(), msg));
}

TEST_CASE("ParseMessage: address must start with '/'")
{
	std::vector<u8> pkt;
	AppendPadded(pkt, "no_slash");
	AppendPadded(pkt, ",i");
	AppendBES32(pkt, 42);

	TOSCMessage msg;
	CHECK_FALSE(COSCParser::ParseMessage(pkt.data(), pkt.size(), msg));
}

// ─────────────────────────────────────────────────────────────────────────────
// ParseBundle tests
// ─────────────────────────────────────────────────────────────────────────────

// Build a minimal OSC message packet and return it as a vector
static std::vector<u8> MakeMessage(const char* addr, s32 val)
{
	std::vector<u8> pkt;
	AppendPadded(pkt, addr);
	AppendPadded(pkt, ",i");
	AppendBES32(pkt, val);
	return pkt;
}

TEST_CASE("ParseBundle: single message inside bundle")
{
	g_Messages.clear();

	std::vector<u8> msg = MakeMessage("/test/a", 10);

	std::vector<u8> bundle;
	// "#bundle\0"
	const char* bundleHdr = "#bundle";
	for (int i = 0; i < 8; ++i)
		bundle.push_back(static_cast<u8>(bundleHdr[i]));
	// timetag (8 bytes, value = 1 immediate)
	for (int i = 0; i < 8; ++i)
		bundle.push_back(0);
	// element: u32 size + data
	AppendBE32(bundle, static_cast<u32>(msg.size()));
	bundle.insert(bundle.end(), msg.begin(), msg.end());

	CHECK(COSCParser::ParseBundle(bundle.data(), bundle.size(), CollectMessage, nullptr));
	CHECK(g_Messages.size() == 1);
	CHECK(strcmp(g_Messages[0].pAddress, "/test/a") == 0);
	CHECK(g_Messages[0].Args[0].i == 10);
}

TEST_CASE("ParseBundle: two messages")
{
	g_Messages.clear();

	auto msg1 = MakeMessage("/test/x", 1);
	auto msg2 = MakeMessage("/test/y", 2);

	std::vector<u8> bundle;
	const char* bundleHdr = "#bundle";
	for (int i = 0; i < 8; ++i)
		bundle.push_back(static_cast<u8>(bundleHdr[i]));
	for (int i = 0; i < 8; ++i)
		bundle.push_back(0);
	AppendBE32(bundle, static_cast<u32>(msg1.size()));
	bundle.insert(bundle.end(), msg1.begin(), msg1.end());
	AppendBE32(bundle, static_cast<u32>(msg2.size()));
	bundle.insert(bundle.end(), msg2.begin(), msg2.end());

	CHECK(COSCParser::ParseBundle(bundle.data(), bundle.size(), CollectMessage, nullptr));
	CHECK(g_Messages.size() == 2);
	CHECK(g_Messages[0].Args[0].i == 1);
	CHECK(g_Messages[1].Args[0].i == 2);
}

TEST_CASE("ParseBundle: rejects wrong header")
{
	u8 bad[] = {
		'#','b','u','n','d','l','X','\0',  // bad header
		0,0,0,0,0,0,0,1                    // timetag
	};
	CHECK_FALSE(COSCParser::ParseBundle(bad, sizeof(bad), CollectMessage, nullptr));
}

// ─────────────────────────────────────────────────────────────────────────────
// ParsePacket top-level dispatcher
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("ParsePacket: dispatches message starting with '/'")
{
	g_Messages.clear();

	std::vector<u8> pkt;
	AppendPadded(pkt, "/mt32pi/volume");
	AppendPadded(pkt, ",i");
	AppendBES32(pkt, 80);

	CHECK(COSCParser::ParsePacket(pkt.data(), pkt.size(), CollectMessage, nullptr));
	CHECK(g_Messages.size() == 1);
	CHECK(g_Messages[0].Args[0].i == 80);
}

TEST_CASE("ParsePacket: dispatches bundle starting with '#'")
{
	g_Messages.clear();

	auto msg = MakeMessage("/test/z", 99);

	std::vector<u8> bundle;
	const char* bundleHdr = "#bundle";
	for (int i = 0; i < 8; ++i)
		bundle.push_back(static_cast<u8>(bundleHdr[i]));
	for (int i = 0; i < 8; ++i)
		bundle.push_back(0);
	AppendBE32(bundle, static_cast<u32>(msg.size()));
	bundle.insert(bundle.end(), msg.begin(), msg.end());

	CHECK(COSCParser::ParsePacket(bundle.data(), bundle.size(), CollectMessage, nullptr));
	CHECK(g_Messages.size() == 1);
	CHECK(g_Messages[0].Args[0].i == 99);
}

TEST_CASE("ParsePacket: rejects null handler")
{
	std::vector<u8> pkt;
	AppendPadded(pkt, "/test/x");
	AppendPadded(pkt, ",i");
	AppendBES32(pkt, 1);
	CHECK_FALSE(COSCParser::ParsePacket(pkt.data(), pkt.size(), nullptr, nullptr));
}
