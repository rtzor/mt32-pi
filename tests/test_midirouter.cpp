//
// test_midirouter.cpp
//
// Unit tests for CMIDIRouter
//

#include "doctest/doctest.h"
#include "midirouter.h"
#include "stubs/synthbase_stub.h"

// Helper: build a MIDI short message from status, data1, data2
static u32 MakeShortMsg(u8 status, u8 data1, u8 data2)
{
	return static_cast<u32>(status) | (static_cast<u32>(data1) << 8) | (static_cast<u32>(data2) << 16);
}

// Helper: NoteOn on channel (0-based), note, velocity
static u32 NoteOn(u8 ch, u8 note, u8 vel)
{
	return MakeShortMsg(0x90 | ch, note, vel);
}

// Helper: NoteOff on channel (0-based), note
static u32 NoteOff(u8 ch, u8 note)
{
	return MakeShortMsg(0x80 | ch, note, 0);
}

// Helper: CC on channel (0-based), control, value
static u32 CC(u8 ch, u8 cc, u8 val)
{
	return MakeShortMsg(0xB0 | ch, cc, val);
}

// ---------------------------------------------------------------
// Construction
// ---------------------------------------------------------------

TEST_CASE("Router: default state")
{
	CMIDIRouter router;
	CHECK(router.GetPreset() == TRouterPreset::SingleMT32);
	CHECK_FALSE(router.IsEnabled());
	CHECK_FALSE(router.IsDualMode());
	CHECK(router.GetChannelEngine(0) == nullptr);
}

// ---------------------------------------------------------------
// Channel mapping
// ---------------------------------------------------------------

TEST_CASE("Router: SetAllChannels assigns every channel")
{
	CSynthBaseStub mt32("MT-32", TSynth::MT32);
	CMIDIRouter router;
	router.SetMT32Engine(&mt32);
	router.SetAllChannels(&mt32);

	for (u8 ch = 0; ch < 16; ++ch)
		CHECK(router.GetChannelEngine(ch) == &mt32);
}

TEST_CASE("Router: SetChannelEngine sets individual channel and marks Custom")
{
	CSynthBaseStub mt32("MT-32", TSynth::MT32);
	CSynthBaseStub fluid("FluidSynth", TSynth::SoundFont);
	CMIDIRouter router;
	router.SetMT32Engine(&mt32);
	router.SetFluidSynthEngine(&fluid);
	router.ApplyPreset(TRouterPreset::SingleMT32);

	router.SetChannelEngine(9, &fluid);
	CHECK(router.GetChannelEngine(9) == &fluid);
	CHECK(router.GetChannelEngine(0) == &mt32);
	CHECK(router.GetPreset() == TRouterPreset::Custom);
}

TEST_CASE("Router: out-of-range channel is ignored")
{
	CSynthBaseStub mt32("MT-32", TSynth::MT32);
	CMIDIRouter router;
	router.SetChannelEngine(16, &mt32);   // should be no-op
	router.SetChannelEngine(255, &mt32);  // should be no-op
	CHECK(router.GetChannelEngine(16) == nullptr);
}

// ---------------------------------------------------------------
// Presets
// ---------------------------------------------------------------

TEST_CASE("Router: SingleMT32 preset")
{
	CSynthBaseStub mt32("MT-32", TSynth::MT32);
	CSynthBaseStub fluid("FluidSynth", TSynth::SoundFont);
	CMIDIRouter router;
	router.SetMT32Engine(&mt32);
	router.SetFluidSynthEngine(&fluid);
	router.ApplyPreset(TRouterPreset::SingleMT32);

	for (u8 ch = 0; ch < 16; ++ch)
		CHECK(router.GetChannelEngine(ch) == &mt32);

	CHECK_FALSE(router.IsDualMode());
}

TEST_CASE("Router: SingleFluid preset")
{
	CSynthBaseStub mt32("MT-32", TSynth::MT32);
	CSynthBaseStub fluid("FluidSynth", TSynth::SoundFont);
	CMIDIRouter router;
	router.SetMT32Engine(&mt32);
	router.SetFluidSynthEngine(&fluid);
	router.ApplyPreset(TRouterPreset::SingleFluid);

	for (u8 ch = 0; ch < 16; ++ch)
		CHECK(router.GetChannelEngine(ch) == &fluid);

	CHECK_FALSE(router.IsDualMode());
}

TEST_CASE("Router: SplitGM preset")
{
	CSynthBaseStub mt32("MT-32", TSynth::MT32);
	CSynthBaseStub fluid("FluidSynth", TSynth::SoundFont);
	CMIDIRouter router;
	router.SetMT32Engine(&mt32);
	router.SetFluidSynthEngine(&fluid);
	router.ApplyPreset(TRouterPreset::SplitGM);

	// Channels 1-9 (indices 0-8) → MT-32
	for (u8 ch = 0; ch < 9; ++ch)
		CHECK(router.GetChannelEngine(ch) == &mt32);

	// Channels 10-16 (indices 9-15) → FluidSynth
	for (u8 ch = 9; ch < 16; ++ch)
		CHECK(router.GetChannelEngine(ch) == &fluid);

	CHECK(router.IsDualMode());
}

TEST_CASE("Router: Custom preset does not change table")
{
	CSynthBaseStub mt32("MT-32", TSynth::MT32);
	CSynthBaseStub fluid("FluidSynth", TSynth::SoundFont);
	CMIDIRouter router;
	router.SetMT32Engine(&mt32);
	router.SetFluidSynthEngine(&fluid);
	router.ApplyPreset(TRouterPreset::SingleMT32);
	router.SetChannelEngine(5, &fluid);

	// Applying Custom should not reset
	router.ApplyPreset(TRouterPreset::Custom);
	CHECK(router.GetChannelEngine(5) == &fluid);
	CHECK(router.GetChannelEngine(0) == &mt32);
}

// ---------------------------------------------------------------
// IsDualMode
// ---------------------------------------------------------------

TEST_CASE("Router: IsDualMode detects mixed engines")
{
	CSynthBaseStub mt32("MT-32", TSynth::MT32);
	CSynthBaseStub fluid("FluidSynth", TSynth::SoundFont);
	CMIDIRouter router;
	router.SetMT32Engine(&mt32);
	router.SetFluidSynthEngine(&fluid);

	router.ApplyPreset(TRouterPreset::SingleMT32);
	CHECK_FALSE(router.IsDualMode());

	router.SetChannelEngine(15, &fluid);
	CHECK(router.IsDualMode());
}

TEST_CASE("Router: IsDualMode false when only one engine registered")
{
	CSynthBaseStub mt32("MT-32", TSynth::MT32);
	CMIDIRouter router;
	router.SetMT32Engine(&mt32);
	router.ApplyPreset(TRouterPreset::SingleMT32);
	CHECK_FALSE(router.IsDualMode());
}

// ---------------------------------------------------------------
// Short message routing
// ---------------------------------------------------------------

TEST_CASE("Router: routes NoteOn to correct engine per channel")
{
	CSynthBaseStub mt32("MT-32", TSynth::MT32);
	CSynthBaseStub fluid("FluidSynth", TSynth::SoundFont);
	CMIDIRouter router;
	router.SetMT32Engine(&mt32);
	router.SetFluidSynthEngine(&fluid);
	router.ApplyPreset(TRouterPreset::SplitGM);

	// NoteOn ch 0 → MT-32
	router.RouteShortMessage(NoteOn(0, 60, 100));
	CHECK(mt32.m_nShortMessageCount == 1);
	CHECK(fluid.m_nShortMessageCount == 0);

	// NoteOn ch 9 (percussion) → FluidSynth
	router.RouteShortMessage(NoteOn(9, 36, 80));
	CHECK(mt32.m_nShortMessageCount == 1);
	CHECK(fluid.m_nShortMessageCount == 1);
}

TEST_CASE("Router: routes CC to correct engine")
{
	CSynthBaseStub mt32("MT-32", TSynth::MT32);
	CSynthBaseStub fluid("FluidSynth", TSynth::SoundFont);
	CMIDIRouter router;
	router.SetMT32Engine(&mt32);
	router.SetFluidSynthEngine(&fluid);
	router.ApplyPreset(TRouterPreset::SplitGM);

	// CC7 (volume) on ch 2 → MT-32
	router.RouteShortMessage(CC(2, 7, 100));
	CHECK(mt32.m_nShortMessageCount == 1);
	CHECK(mt32.m_nLastShortMessage == CC(2, 7, 100));
	CHECK(fluid.m_nShortMessageCount == 0);
}

TEST_CASE("Router: system messages go to both engines")
{
	CSynthBaseStub mt32("MT-32", TSynth::MT32);
	CSynthBaseStub fluid("FluidSynth", TSynth::SoundFont);
	CMIDIRouter router;
	router.SetMT32Engine(&mt32);
	router.SetFluidSynthEngine(&fluid);
	router.ApplyPreset(TRouterPreset::SplitGM);

	// System reset (0xFF)
	router.RouteShortMessage(0xFF);
	CHECK(mt32.m_nShortMessageCount == 1);
	CHECK(fluid.m_nShortMessageCount == 1);
}

TEST_CASE("Router: system messages with only one engine")
{
	CSynthBaseStub mt32("MT-32", TSynth::MT32);
	CMIDIRouter router;
	router.SetMT32Engine(&mt32);
	router.ApplyPreset(TRouterPreset::SingleMT32);

	// Timing clock (0xF8) — should not crash
	router.RouteShortMessage(0xF8);
	CHECK(mt32.m_nShortMessageCount == 1);
}

TEST_CASE("Router: NoteOff routed same as NoteOn")
{
	CSynthBaseStub mt32("MT-32", TSynth::MT32);
	CSynthBaseStub fluid("FluidSynth", TSynth::SoundFont);
	CMIDIRouter router;
	router.SetMT32Engine(&mt32);
	router.SetFluidSynthEngine(&fluid);
	router.ApplyPreset(TRouterPreset::SplitGM);

	router.RouteShortMessage(NoteOff(0, 60));
	CHECK(mt32.m_nShortMessageCount == 1);
	CHECK(fluid.m_nShortMessageCount == 0);

	router.RouteShortMessage(NoteOff(10, 60));
	CHECK(fluid.m_nShortMessageCount == 1);
}

TEST_CASE("Router: null engine in channel map is safe")
{
	CMIDIRouter router;
	// No engines, no mappings — should not crash
	router.RouteShortMessage(NoteOn(0, 60, 100));
	router.RouteShortMessage(0xFF);
}

// ---------------------------------------------------------------
// SysEx routing
// ---------------------------------------------------------------

TEST_CASE("Router: Roland SysEx goes to MT-32")
{
	CSynthBaseStub mt32("MT-32", TSynth::MT32);
	CSynthBaseStub fluid("FluidSynth", TSynth::SoundFont);
	CMIDIRouter router;
	router.SetMT32Engine(&mt32);
	router.SetFluidSynthEngine(&fluid);

	// Roland SysEx: F0 41 10 16 12 ... F7
	const u8 rolandSysEx[] = { 0xF0, 0x41, 0x10, 0x16, 0x12, 0x20, 0x00, 0x00, 0x00, 0x60, 0xF7 };
	router.RouteSysEx(rolandSysEx, sizeof(rolandSysEx));

	CHECK(mt32.m_nSysExCount == 1);
	CHECK(fluid.m_nSysExCount == 0);
}

TEST_CASE("Router: non-Roland SysEx goes to FluidSynth")
{
	CSynthBaseStub mt32("MT-32", TSynth::MT32);
	CSynthBaseStub fluid("FluidSynth", TSynth::SoundFont);
	CMIDIRouter router;
	router.SetMT32Engine(&mt32);
	router.SetFluidSynthEngine(&fluid);

	// Yamaha SysEx: F0 43 ...
	const u8 yamahaSysEx[] = { 0xF0, 0x43, 0x10, 0x4C, 0x00, 0x00, 0x7E, 0x00, 0xF7 };
	router.RouteSysEx(yamahaSysEx, sizeof(yamahaSysEx));

	CHECK(mt32.m_nSysExCount == 0);
	CHECK(fluid.m_nSysExCount == 1);
}

TEST_CASE("Router: universal SysEx goes to both engines")
{
	CSynthBaseStub mt32("MT-32", TSynth::MT32);
	CSynthBaseStub fluid("FluidSynth", TSynth::SoundFont);
	CMIDIRouter router;
	router.SetMT32Engine(&mt32);
	router.SetFluidSynthEngine(&fluid);

	// GM System On: F0 7E 7F 09 01 F7
	const u8 gmOnSysEx[] = { 0xF0, 0x7E, 0x7F, 0x09, 0x01, 0xF7 };
	router.RouteSysEx(gmOnSysEx, sizeof(gmOnSysEx));

	CHECK(mt32.m_nSysExCount == 1);
	CHECK(fluid.m_nSysExCount == 1);
}

TEST_CASE("Router: SysEx with null or too-short data is safe")
{
	CMIDIRouter router;
	router.RouteSysEx(nullptr, 0);

	const u8 tooShort[] = { 0xF0, 0xF7 };
	router.RouteSysEx(tooShort, 2);
}

// ---------------------------------------------------------------
// GetPrimaryEngine
// ---------------------------------------------------------------

TEST_CASE("Router: GetPrimaryEngine returns engine with most channels")
{
	CSynthBaseStub mt32("MT-32", TSynth::MT32);
	CSynthBaseStub fluid("FluidSynth", TSynth::SoundFont);
	CMIDIRouter router;
	router.SetMT32Engine(&mt32);
	router.SetFluidSynthEngine(&fluid);

	// SplitGM: 9 MT-32 + 7 Fluid → primary = MT-32
	router.ApplyPreset(TRouterPreset::SplitGM);
	CHECK(router.GetPrimaryEngine() == &mt32);

	// All Fluid → primary = Fluid
	router.ApplyPreset(TRouterPreset::SingleFluid);
	CHECK(router.GetPrimaryEngine() == &fluid);
}

// ---------------------------------------------------------------
// Enable/disable
// ---------------------------------------------------------------

TEST_CASE("Router: enable/disable flag")
{
	CMIDIRouter router;
	CHECK_FALSE(router.IsEnabled());
	router.SetEnabled(true);
	CHECK(router.IsEnabled());
	router.SetEnabled(false);
	CHECK_FALSE(router.IsEnabled());
}

// ---------------------------------------------------------------
// Channel remapping
// ---------------------------------------------------------------

TEST_CASE("Router: default remap is identity")
{
	CMIDIRouter router;
	for (u8 i = 0; i < 16; ++i)
		CHECK(router.GetChannelRemap(i) == i);
}

TEST_CASE("Router: set/get channel remap")
{
	CMIDIRouter router;
	router.SetChannelRemap(4, 0);  // ch5 → ch1
	CHECK(router.GetChannelRemap(4) == 0);
	CHECK(router.GetChannelRemap(0) == 0);  // unchanged
}

TEST_CASE("Router: remap out of range is ignored")
{
	CMIDIRouter router;
	router.SetChannelRemap(16, 0);   // invalid source
	router.SetChannelRemap(0, 16);   // invalid destination
	CHECK(router.GetChannelRemap(0) == 0);  // unchanged
	CHECK(router.GetChannelRemap(16) == 16);  // out of range returns input
}

TEST_CASE("Router: remap changes preset to Custom")
{
	CMIDIRouter router;
	CSynthBaseStub mt32("MT-32", TSynth::MT32);
	router.SetMT32Engine(&mt32);
	router.ApplyPreset(TRouterPreset::SingleMT32);
	CHECK(router.GetPreset() == TRouterPreset::SingleMT32);

	router.SetChannelRemap(5, 3);
	CHECK(router.GetPreset() == TRouterPreset::Custom);
}

TEST_CASE("Router: reset remap restores identity")
{
	CMIDIRouter router;
	router.SetChannelRemap(0, 9);
	router.SetChannelRemap(5, 3);
	router.ResetChannelRemap();
	for (u8 i = 0; i < 16; ++i)
		CHECK(router.GetChannelRemap(i) == i);
}

TEST_CASE("Router: remap rewrites channel in routed message")
{
	CSynthBaseStub mt32("MT-32", TSynth::MT32);
	CSynthBaseStub fluid("FluidSynth", TSynth::SoundFont);
	CMIDIRouter router;
	router.SetMT32Engine(&mt32);
	router.SetFluidSynthEngine(&fluid);
	router.ApplyPreset(TRouterPreset::SingleMT32);

	// Remap channel 4 (0-based) → channel 0
	router.SetChannelRemap(4, 0);

	// Send NoteOn on channel 4: status = 0x94
	u32 noteOn = NoteOn(4, 60, 100);
	router.RouteShortMessage(noteOn);

	CHECK(mt32.m_nShortMessageCount == 1);
	// Check the received message has channel 0: status should be 0x90
	u8 receivedStatus = mt32.m_nLastShortMessage & 0xFF;
	CHECK(receivedStatus == 0x90);
	// Data bytes preserved
	CHECK(((mt32.m_nLastShortMessage >> 8) & 0x7F) == 60);
	CHECK(((mt32.m_nLastShortMessage >> 16) & 0x7F) == 100);
}

TEST_CASE("Router: identity remap does not alter message")
{
	CSynthBaseStub mt32("MT-32", TSynth::MT32);
	CMIDIRouter router;
	router.SetMT32Engine(&mt32);
	router.ApplyPreset(TRouterPreset::SingleMT32);
	// No remap set (identity)

	u32 noteOn = NoteOn(7, 64, 80);
	router.RouteShortMessage(noteOn);

	CHECK(mt32.m_nShortMessageCount == 1);
	CHECK(mt32.m_nLastShortMessage == noteOn);
}

TEST_CASE("Router: remap with split preset routes to correct engine")
{
	CSynthBaseStub mt32("MT-32", TSynth::MT32);
	CSynthBaseStub fluid("FluidSynth", TSynth::SoundFont);
	CMIDIRouter router;
	router.SetMT32Engine(&mt32);
	router.SetFluidSynthEngine(&fluid);
	router.ApplyPreset(TRouterPreset::SplitGM);

	// Channel 10 (index 9) goes to FluidSynth
	// Remap channel 9 → channel 0 (so FluidSynth receives it as ch1)
	router.SetChannelRemap(9, 0);

	u32 noteOn = NoteOn(9, 50, 90);
	router.RouteShortMessage(noteOn);

	CHECK(mt32.m_nShortMessageCount == 0);
	CHECK(fluid.m_nShortMessageCount == 1);
	// Received as channel 0
	CHECK((fluid.m_nLastShortMessage & 0x0F) == 0);
	CHECK(((fluid.m_nLastShortMessage >> 8) & 0x7F) == 50);
}

TEST_CASE("Router: GetChannelEngineName returns engine name")
{
	CSynthBaseStub mt32("MT-32", TSynth::MT32);
	CSynthBaseStub fluid("FluidSynth", TSynth::SoundFont);
	CMIDIRouter router;
	router.SetMT32Engine(&mt32);
	router.SetFluidSynthEngine(&fluid);
	router.ApplyPreset(TRouterPreset::SplitGM);

	CHECK(strcmp(router.GetChannelEngineName(0), "MT-32") == 0);
	CHECK(strcmp(router.GetChannelEngineName(9), "FluidSynth") == 0);
	CHECK(strcmp(router.GetChannelEngineName(16), "none") == 0);
}

// ---------------------------------------------------------------
// CC Filters
// ---------------------------------------------------------------

TEST_CASE("Router: CC filters default to all allowed")
{
	CMIDIRouter router;
	for (u8 cc = 0; cc < 128; ++cc)
	{
		CHECK(router.GetCCFilter(CMIDIRouter::EngMT32, cc) == true);
		CHECK(router.GetCCFilter(CMIDIRouter::EngFluid, cc) == true);
	}
}

TEST_CASE("Router: blocking a CC prevents delivery")
{
	CSynthBaseStub mt32("MT-32", TSynth::MT32);
	CSynthBaseStub fluid("FluidSynth", TSynth::SoundFont);
	CMIDIRouter router;
	router.SetMT32Engine(&mt32);
	router.SetFluidSynthEngine(&fluid);
	router.ApplyPreset(TRouterPreset::SingleMT32);

	// Block CC7 (volume) for MT-32
	router.SetCCFilter(CMIDIRouter::EngMT32, 7, false);

	router.RouteShortMessage(CC(0, 7, 100));
	CHECK(mt32.m_nShortMessageCount == 0);  // blocked

	// CC1 (mod wheel) still allowed
	router.RouteShortMessage(CC(0, 1, 64));
	CHECK(mt32.m_nShortMessageCount == 1);  // allowed
}

TEST_CASE("Router: CC filter per engine is independent")
{
	CSynthBaseStub mt32("MT-32", TSynth::MT32);
	CSynthBaseStub fluid("FluidSynth", TSynth::SoundFont);
	CMIDIRouter router;
	router.SetMT32Engine(&mt32);
	router.SetFluidSynthEngine(&fluid);
	router.ApplyPreset(TRouterPreset::SplitGM);

	// Block CC10 (pan) for FluidSynth only
	router.SetCCFilter(CMIDIRouter::EngFluid, 10, false);

	// CC10 on ch0 (MT-32): allowed
	router.RouteShortMessage(CC(0, 10, 64));
	CHECK(mt32.m_nShortMessageCount == 1);

	// CC10 on ch9 (FluidSynth): blocked
	router.RouteShortMessage(CC(9, 10, 64));
	CHECK(fluid.m_nShortMessageCount == 0);

	// CC7 on ch9 (FluidSynth): allowed
	router.RouteShortMessage(CC(9, 7, 100));
	CHECK(fluid.m_nShortMessageCount == 1);
}

TEST_CASE("Router: ResetCCFilters allows all again")
{
	CMIDIRouter router;
	router.SetCCFilter(CMIDIRouter::EngMT32, 7, false);
	router.SetCCFilter(CMIDIRouter::EngFluid, 10, false);
	router.ResetCCFilters();

	CHECK(router.GetCCFilter(CMIDIRouter::EngMT32, 7) == true);
	CHECK(router.GetCCFilter(CMIDIRouter::EngFluid, 10) == true);
}

TEST_CASE("Router: CC filter out-of-range is safe")
{
	CMIDIRouter router;
	router.SetCCFilter(5, 0, false);   // invalid engine
	router.SetCCFilter(0, 128, false); // invalid CC (u8 wraps, but we check < NumCCs)
	CHECK(router.GetCCFilter(5, 0) == true);   // returns default
}

// ---------------------------------------------------------------
// Layering
// ---------------------------------------------------------------

TEST_CASE("Router: layering default is off for all channels")
{
	CMIDIRouter router;
	for (u8 i = 0; i < 16; ++i)
		CHECK_FALSE(router.GetLayering(i));
	CHECK_FALSE(router.HasAnyLayering());
}

TEST_CASE("Router: layering duplicates NoteOn to both engines")
{
	CSynthBaseStub mt32("MT-32", TSynth::MT32);
	CSynthBaseStub fluid("FluidSynth", TSynth::SoundFont);
	CMIDIRouter router;
	router.SetMT32Engine(&mt32);
	router.SetFluidSynthEngine(&fluid);
	router.ApplyPreset(TRouterPreset::SingleMT32);  // all → MT-32

	router.SetLayering(0, true);
	CHECK(router.HasAnyLayering());

	router.RouteShortMessage(NoteOn(0, 60, 100));
	CHECK(mt32.m_nShortMessageCount == 1);
	CHECK(fluid.m_nShortMessageCount == 1);  // duplicated
}

TEST_CASE("Router: layering duplicates NoteOff to both engines")
{
	CSynthBaseStub mt32("MT-32", TSynth::MT32);
	CSynthBaseStub fluid("FluidSynth", TSynth::SoundFont);
	CMIDIRouter router;
	router.SetMT32Engine(&mt32);
	router.SetFluidSynthEngine(&fluid);
	router.ApplyPreset(TRouterPreset::SingleFluid);

	router.SetLayering(5, true);

	router.RouteShortMessage(NoteOff(5, 60));
	CHECK(mt32.m_nShortMessageCount == 1);   // duplicated
	CHECK(fluid.m_nShortMessageCount == 1);
}

TEST_CASE("Router: layering does not affect non-layered channels")
{
	CSynthBaseStub mt32("MT-32", TSynth::MT32);
	CSynthBaseStub fluid("FluidSynth", TSynth::SoundFont);
	CMIDIRouter router;
	router.SetMT32Engine(&mt32);
	router.SetFluidSynthEngine(&fluid);
	router.ApplyPreset(TRouterPreset::SingleMT32);

	router.SetLayering(0, true);

	// Ch1 is layered, ch2 is not
	router.RouteShortMessage(NoteOn(1, 60, 100));
	CHECK(mt32.m_nShortMessageCount == 1);
	CHECK(fluid.m_nShortMessageCount == 0);  // not layered
}

TEST_CASE("Router: layering applies CC filter to both engines")
{
	CSynthBaseStub mt32("MT-32", TSynth::MT32);
	CSynthBaseStub fluid("FluidSynth", TSynth::SoundFont);
	CMIDIRouter router;
	router.SetMT32Engine(&mt32);
	router.SetFluidSynthEngine(&fluid);
	router.ApplyPreset(TRouterPreset::SingleMT32);

	router.SetLayering(0, true);
	router.SetCCFilter(CMIDIRouter::EngFluid, 7, false);  // block CC7 for FluidSynth

	router.RouteShortMessage(CC(0, 7, 100));
	CHECK(mt32.m_nShortMessageCount == 1);   // primary target, CC allowed
	CHECK(fluid.m_nShortMessageCount == 0);  // blocked by CC filter
}

TEST_CASE("Router: SetAllLayering and disable")
{
	CMIDIRouter router;
	router.SetAllLayering(true);
	for (u8 i = 0; i < 16; ++i)
		CHECK(router.GetLayering(i));
	CHECK(router.HasAnyLayering());

	router.SetAllLayering(false);
	CHECK_FALSE(router.HasAnyLayering());
}

TEST_CASE("Router: layering with remap sends remapped message to both")
{
	CSynthBaseStub mt32("MT-32", TSynth::MT32);
	CSynthBaseStub fluid("FluidSynth", TSynth::SoundFont);
	CMIDIRouter router;
	router.SetMT32Engine(&mt32);
	router.SetFluidSynthEngine(&fluid);
	router.ApplyPreset(TRouterPreset::SingleMT32);

	router.SetLayering(3, true);
	router.SetChannelRemap(3, 0);  // ch4 → ch1

	router.RouteShortMessage(NoteOn(3, 64, 80));
	CHECK(mt32.m_nShortMessageCount == 1);
	CHECK(fluid.m_nShortMessageCount == 1);

	// Both should receive ch0
	CHECK((mt32.m_nLastShortMessage & 0x0F) == 0);
	CHECK((fluid.m_nLastShortMessage & 0x0F) == 0);
}

TEST_CASE("Router: non-note messages on layered channel go only to primary (except CC)")
{
	CSynthBaseStub mt32("MT-32", TSynth::MT32);
	CSynthBaseStub fluid("FluidSynth", TSynth::SoundFont);
	CMIDIRouter router;
	router.SetMT32Engine(&mt32);
	router.SetFluidSynthEngine(&fluid);
	router.ApplyPreset(TRouterPreset::SingleMT32);

	router.SetLayering(0, true);

	// Program change on ch0: should only go to primary (MT-32)
	u32 pc = MakeShortMsg(0xC0, 5, 0);
	router.RouteShortMessage(pc);
	CHECK(mt32.m_nShortMessageCount == 1);
	CHECK(fluid.m_nShortMessageCount == 0);
}
