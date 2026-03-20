//
// test_fluidsequencer.cpp
//
// Unit tests for CFluidSequencer.
//
// Strategy: compile the real fluidsequencer.cpp with stub implementations of
// FluidSynth (fluidsynth_stub.cpp) and FatFS (fatfs_stub.cpp).  Stub globals
// let each test inject failures or preset query results.
//

#include "doctest/doctest.h"
#include "stubs/fluidsynth.h"   // FluidStub_Reset, FluidStub_InvokeCallback, g_fluid_*
#include "stubs/fatfs/ff.h"     // g_fatfs_*

#include "fluidsequencer.h"     // class under test

#include <cstring>

// ---------------------------------------------------------------------------
// Minimal valid MIDI file (format 0, 1 track, 480 ppqn, End-of-Track only)
// ---------------------------------------------------------------------------
static const unsigned char kMinimalMIDI[] = {
	// MThd — header chunk
	'M','T','h','d',  0,0,0,6,  // chunk type + length
	0,0,                        // format 0
	0,1,                        // 1 track
	0x01,0xE0,                  // 480 PPQN
	// MTrk — single track chunk
	'M','T','r','k',  0,0,0,4,  // chunk type + length (4 bytes)
	0x00, 0xFF, 0x2F, 0x00      // delta=0, meta End-of-Track
};

// ---------------------------------------------------------------------------
// Reset all stub state to safe defaults.  Call at the start of each test.
// ---------------------------------------------------------------------------
static void FullReset()
{
	FluidStub_Reset();
	g_fatfs_open_fail = false;
	g_fatfs_read_fail = false;
	g_fatfs_data      = kMinimalMIDI;
	g_fatfs_data_size = sizeof(kMinimalMIDI);
}

// Convenience: initialize + play with all stubs happy.
static bool InitAndPlay(CFluidSequencer& seq)
{
	static fluid_synth_t fake_synth{};
	return seq.Initialize(&fake_synth) && seq.Play("SD:test.mid");
}

// ===========================================================================
// Group 1: Default state (before Initialize)
// ===========================================================================

TEST_CASE("FluidSeq: default state — no player")
{
	CFluidSequencer seq;
	CHECK_FALSE(seq.IsPlaying());
	CHECK_FALSE(seq.IsFinished());
	CHECK(seq.GetCurrentTick() == 0);
	CHECK(seq.GetTotalTicks()  == 0);
	CHECK(seq.GetBPM()         == 120);
	CHECK(seq.GetDivision()    == 0);
	CHECK(seq.GetMidiTempo()   == 500000);
	CHECK(seq.GetDiag() != nullptr);
}

TEST_CASE("FluidSeq: DrainMIDIBytes returns 0 on empty buffer")
{
	CFluidSequencer seq;
	u8 buf[64];
	CHECK(seq.DrainMIDIBytes(buf, sizeof(buf)) == 0u);
}

TEST_CASE("FluidSeq: Seek returns false without player")
{
	CFluidSequencer seq;
	CHECK_FALSE(seq.Seek(100));
}

TEST_CASE("FluidSeq: SetTempoMultiplier returns false without player")
{
	CFluidSequencer seq;
	CHECK_FALSE(seq.SetTempoMultiplier(2.0));
}

TEST_CASE("FluidSeq: SetTempoBPM returns false without player")
{
	CFluidSequencer seq;
	CHECK_FALSE(seq.SetTempoBPM(120.0));
}

// ===========================================================================
// Group 2: Initialize
// ===========================================================================

TEST_CASE("FluidSeq: Initialize with null synth returns false")
{
	CFluidSequencer seq;
	CHECK_FALSE(seq.Initialize(nullptr));
}

TEST_CASE("FluidSeq: Initialize with valid synth returns true")
{
	fluid_synth_t fake_synth{};
	CFluidSequencer seq;
	CHECK(seq.Initialize(&fake_synth));
}

// ===========================================================================
// Group 3: Play — guard checks
// ===========================================================================

TEST_CASE("FluidSeq: Play with null path returns false")
{
	FullReset();
	fluid_synth_t fake_synth{};
	CFluidSequencer seq;
	seq.Initialize(&fake_synth);
	CHECK_FALSE(seq.Play(nullptr));
}

TEST_CASE("FluidSeq: Play with empty path returns false")
{
	FullReset();
	fluid_synth_t fake_synth{};
	CFluidSequencer seq;
	seq.Initialize(&fake_synth);
	CHECK_FALSE(seq.Play(""));
}

TEST_CASE("FluidSeq: Play without Initialize returns false (null synth guard)")
{
	FullReset();
	CFluidSequencer seq;
	CHECK_FALSE(seq.Play("SD:test.mid"));
}

// ===========================================================================
// Group 3: Play — failure injection
// ===========================================================================

TEST_CASE("FluidSeq: Play fails when new_fluid_player returns null")
{
	FullReset();
	g_fluid_new_player_fail = true;
	fluid_synth_t fake_synth{};
	CFluidSequencer seq;
	seq.Initialize(&fake_synth);
	CHECK_FALSE(seq.Play("SD:test.mid"));
	CHECK_FALSE(seq.IsPlaying());
}

TEST_CASE("FluidSeq: Play fails when f_open fails")
{
	FullReset();
	g_fatfs_open_fail = true;
	fluid_synth_t fake_synth{};
	CFluidSequencer seq;
	seq.Initialize(&fake_synth);
	CHECK_FALSE(seq.Play("SD:test.mid"));
}

TEST_CASE("FluidSeq: Play fails when file size is zero")
{
	FullReset();
	g_fatfs_data_size = 0;
	fluid_synth_t fake_synth{};
	CFluidSequencer seq;
	seq.Initialize(&fake_synth);
	CHECK_FALSE(seq.Play("SD:test.mid"));
}

TEST_CASE("FluidSeq: Play fails when fluid_player_add_mem fails")
{
	FullReset();
	g_fluid_add_mem_fail = true;
	fluid_synth_t fake_synth{};
	CFluidSequencer seq;
	seq.Initialize(&fake_synth);
	CHECK_FALSE(seq.Play("SD:test.mid"));
}

TEST_CASE("FluidSeq: Play fails when fluid_player_play fails")
{
	FullReset();
	g_fluid_play_fail = true;
	fluid_synth_t fake_synth{};
	CFluidSequencer seq;
	seq.Initialize(&fake_synth);
	CHECK_FALSE(seq.Play("SD:test.mid"));
	CHECK_FALSE(seq.IsPlaying());
}

// ===========================================================================
// Group 3: Play — success path
// ===========================================================================

TEST_CASE("FluidSeq: Play succeeds — IsPlaying becomes true")
{
	FullReset();
	CFluidSequencer seq;
	REQUIRE(InitAndPlay(seq));
	CHECK(seq.IsPlaying());
	CHECK_FALSE(seq.IsFinished());
}

TEST_CASE("FluidSeq: status queries delegate to stub after play")
{
	FullReset();
	g_fluid_current_tick = 42;
	g_fluid_total_ticks  = 999;
	g_fluid_bpm          = 140;
	g_fluid_division     = 960;
	g_fluid_midi_tempo   = 428571;
	CFluidSequencer seq;
	REQUIRE(InitAndPlay(seq));
	CHECK(seq.GetCurrentTick() == 42);
	CHECK(seq.GetTotalTicks()  == 999);
	CHECK(seq.GetBPM()         == 140);
	CHECK(seq.GetDivision()    == 960);
	CHECK(seq.GetMidiTempo()   == 428571);
}

TEST_CASE("FluidSeq: IsFinished true when stub reports DONE")
{
	FullReset();
	CFluidSequencer seq;
	REQUIRE(InitAndPlay(seq));
	g_fluid_player_status = FLUID_PLAYER_DONE;
	CHECK_FALSE(seq.IsPlaying());
	CHECK(seq.IsFinished());
}

// ===========================================================================
// Group 4: Stop
// ===========================================================================

TEST_CASE("FluidSeq: Stop sends 96 MIDI panic bytes (16 channels x 6)")
{
	FullReset();
	CFluidSequencer seq;
	REQUIRE(InitAndPlay(seq));

	seq.Stop();
	CHECK_FALSE(seq.IsPlaying());

	u8 buf[256];
	const size_t n = seq.DrainMIDIBytes(buf, sizeof(buf));

	// 16 channels × 6 bytes (AllNotesOff B0 7B 00 + ResetAllCtrl B0 79 00)
	REQUIRE(n == 96u);

	// First channel (0): B0 7B 00 B0 79 00
	CHECK(buf[0]  == 0xB0u);
	CHECK(buf[1]  == 0x7Bu);
	CHECK(buf[2]  == 0x00u);
	CHECK(buf[3]  == 0xB0u);
	CHECK(buf[4]  == 0x79u);
	CHECK(buf[5]  == 0x00u);

	// Last channel (15): BF 7B 00 BF 79 00
	CHECK(buf[90] == 0xBFu);
	CHECK(buf[91] == 0x7Bu);
	CHECK(buf[92] == 0x00u);
	CHECK(buf[93] == 0xBFu);
	CHECK(buf[94] == 0x79u);
	CHECK(buf[95] == 0x00u);
}

// ===========================================================================
// Group 5: Seek
// ===========================================================================

TEST_CASE("FluidSeq: Seek sends panic and updates current tick")
{
	FullReset();
	CFluidSequencer seq;
	REQUIRE(InitAndPlay(seq));

	CHECK(seq.Seek(500));

	// Seek unconditionally sends panic before seeking
	u8 buf[256];
	const size_t n = seq.DrainMIDIBytes(buf, sizeof(buf));
	CHECK(n == 96u);

	// The stub records the tick passed to fluid_player_seek
	CHECK(seq.GetCurrentTick() == 500);
}

TEST_CASE("FluidSeq: Seek returns false when stub signals failure")
{
	FullReset();
	g_fluid_seek_fail = true;
	CFluidSequencer seq;
	REQUIRE(InitAndPlay(seq));
	CHECK_FALSE(seq.Seek(100));
}

TEST_CASE("FluidSeq: Seek returns false without player")
{
	CFluidSequencer seq;
	CHECK_FALSE(seq.Seek(100));
}

// ===========================================================================
// Group 6: Loop and tempo
// ===========================================================================

TEST_CASE("FluidSeq: SetLoop before Initialize is stored and applied on Play")
{
	FullReset();
	CFluidSequencer seq;
	seq.SetLoop(-1);  // infinite — should not crash
	fluid_synth_t fake_synth{};
	REQUIRE(seq.Initialize(&fake_synth));
	REQUIRE(seq.Play("SD:test.mid"));
	CHECK(seq.IsPlaying());
}

TEST_CASE("FluidSeq: SetTempoMultiplier returns true with active player")
{
	FullReset();
	CFluidSequencer seq;
	REQUIRE(InitAndPlay(seq));
	CHECK(seq.SetTempoMultiplier(2.0));
	CHECK(seq.SetTempoMultiplier(0.5));
}

TEST_CASE("FluidSeq: SetTempoBPM returns true with active player")
{
	FullReset();
	CFluidSequencer seq;
	REQUIRE(InitAndPlay(seq));
	CHECK(seq.SetTempoBPM(120.0));
}

// ===========================================================================
// Group 7: PlaybackCallback (via FluidStub_InvokeCallback)
// ===========================================================================
// The static PlaybackCallback is registered during Play().  We invoke it
// through the stub helper and verify the correct MIDI bytes are enqueued.

TEST_CASE("FluidSeq: callback NoteOn produces 3 correct bytes")
{
	FullReset();
	CFluidSequencer seq;
	REQUIRE(InitAndPlay(seq));

	fluid_midi_event_t ev{};
	ev.type     = 0x90;
	ev.channel  = 0;
	ev.key      = 60;
	ev.velocity = 100;
	FluidStub_InvokeCallback(&ev);

	u8 buf[8];
	REQUIRE(seq.DrainMIDIBytes(buf, sizeof(buf)) == 3u);
	CHECK(buf[0] == 0x90u);
	CHECK(buf[1] == 60u);
	CHECK(buf[2] == 100u);
}

TEST_CASE("FluidSeq: callback NoteOff produces 3 correct bytes")
{
	FullReset();
	CFluidSequencer seq;
	REQUIRE(InitAndPlay(seq));

	fluid_midi_event_t ev{};
	ev.type     = 0x80;
	ev.channel  = 2;
	ev.key      = 64;
	ev.velocity = 0;
	FluidStub_InvokeCallback(&ev);

	u8 buf[8];
	REQUIRE(seq.DrainMIDIBytes(buf, sizeof(buf)) == 3u);
	CHECK(buf[0] == 0x82u);  // 0x80 | ch=2
	CHECK(buf[1] == 64u);
	CHECK(buf[2] == 0u);
}

TEST_CASE("FluidSeq: callback CC produces 3 correct bytes")
{
	FullReset();
	CFluidSequencer seq;
	REQUIRE(InitAndPlay(seq));

	fluid_midi_event_t ev{};
	ev.type    = 0xB0;
	ev.channel = 1;
	ev.control = 7;
	ev.value   = 80;
	FluidStub_InvokeCallback(&ev);

	u8 buf[8];
	REQUIRE(seq.DrainMIDIBytes(buf, sizeof(buf)) == 3u);
	CHECK(buf[0] == 0xB1u);
	CHECK(buf[1] == 7u);
	CHECK(buf[2] == 80u);
}

TEST_CASE("FluidSeq: callback ProgramChange produces 2 correct bytes")
{
	FullReset();
	CFluidSequencer seq;
	REQUIRE(InitAndPlay(seq));

	fluid_midi_event_t ev{};
	ev.type    = 0xC0;
	ev.channel = 0;
	ev.program = 40;
	FluidStub_InvokeCallback(&ev);

	u8 buf[8];
	REQUIRE(seq.DrainMIDIBytes(buf, sizeof(buf)) == 2u);
	CHECK(buf[0] == 0xC0u);
	CHECK(buf[1] == 40u);
}

TEST_CASE("FluidSeq: callback ChannelPressure produces 2 correct bytes")
{
	FullReset();
	CFluidSequencer seq;
	REQUIRE(InitAndPlay(seq));

	fluid_midi_event_t ev{};
	ev.type    = 0xD0;
	ev.channel = 3;
	ev.value   = 50;
	FluidStub_InvokeCallback(&ev);

	u8 buf[8];
	REQUIRE(seq.DrainMIDIBytes(buf, sizeof(buf)) == 2u);
	CHECK(buf[0] == 0xD3u);  // 0xD0 | ch=3
	CHECK(buf[1] == 50u);
}

TEST_CASE("FluidSeq: callback PitchBend splits into 7-bit LSB + MSB")
{
	FullReset();
	CFluidSequencer seq;
	REQUIRE(InitAndPlay(seq));

	// pitch = 8192 (0x2000): LSB = 8192 & 0x7F = 0, MSB = (8192 >> 7) & 0x7F = 64
	fluid_midi_event_t ev{};
	ev.type    = 0xE0;
	ev.channel = 0;
	ev.pitch   = 8192;
	FluidStub_InvokeCallback(&ev);

	u8 buf[8];
	REQUIRE(seq.DrainMIDIBytes(buf, sizeof(buf)) == 3u);
	CHECK(buf[0] == 0xE0u);
	CHECK(buf[1] == 0u);    // LSB
	CHECK(buf[2] == 64u);   // MSB
}

TEST_CASE("FluidSeq: callback SysEx enqueues raw bytes verbatim")
{
	FullReset();
	CFluidSequencer seq;
	REQUIRE(InitAndPlay(seq));

	static const u8 sysex[] = { 0xF0, 0x41, 0x10, 0x16, 0x12, 0xF7 };
	fluid_midi_event_t ev{};
	ev.type       = 0xF0;
	ev.sysex_data = const_cast<u8*>(sysex);
	ev.sysex_size = static_cast<int>(sizeof(sysex));
	FluidStub_InvokeCallback(&ev);

	u8 buf[16];
	const size_t n = seq.DrainMIDIBytes(buf, sizeof(buf));
	REQUIRE(n == sizeof(sysex));
	CHECK(std::memcmp(buf, sysex, n) == 0);
}

TEST_CASE("FluidSeq: callback unknown/meta type produces no bytes")
{
	FullReset();
	CFluidSequencer seq;
	REQUIRE(InitAndPlay(seq));

	fluid_midi_event_t ev{};
	ev.type = 0xFF;  // meta event — must be silently ignored
	FluidStub_InvokeCallback(&ev);

	u8 buf[8];
	CHECK(seq.DrainMIDIBytes(buf, sizeof(buf)) == 0u);
}
