//
// test_audiomixer.cpp
//
// Unit tests for CAudioMixer
//

#include "doctest/doctest.h"
#include "audiomixer.h"
#include "stubs/synthbase_stub.h"

#include <cmath>
#include <cstring>

static constexpr float kEpsilon = 1e-5f;

// ---------------------------------------------------------------
// Construction
// ---------------------------------------------------------------

TEST_CASE("Mixer: default state")
{
	CAudioMixer mixer;
	CHECK(mixer.GetEngineCount() == 0);
	CHECK(mixer.GetMasterVolume() == doctest::Approx(1.0f));
	CHECK(mixer.GetSoloEngine() == nullptr);
}

// ---------------------------------------------------------------
// AddEngine
// ---------------------------------------------------------------

TEST_CASE("Mixer: add engines")
{
	CSynthBaseStub a("A", TSynth::MT32);
	CSynthBaseStub b("B", TSynth::SoundFont);
	CAudioMixer mixer;

	CHECK(mixer.AddEngine(&a));
	CHECK(mixer.GetEngineCount() == 1);
	CHECK(mixer.AddEngine(&b));
	CHECK(mixer.GetEngineCount() == 2);
}

TEST_CASE("Mixer: reject null engine")
{
	CAudioMixer mixer;
	CHECK_FALSE(mixer.AddEngine(nullptr));
	CHECK(mixer.GetEngineCount() == 0);
}

TEST_CASE("Mixer: reject duplicate engine")
{
	CSynthBaseStub a("A", TSynth::MT32);
	CAudioMixer mixer;
	CHECK(mixer.AddEngine(&a));
	CHECK_FALSE(mixer.AddEngine(&a));
	CHECK(mixer.GetEngineCount() == 1);
}

TEST_CASE("Mixer: reject beyond MaxEngines")
{
	CSynthBaseStub engines[CAudioMixer::MaxEngines + 1] = {
		{"E0", TSynth::MT32}, {"E1", TSynth::SoundFont},
		{"E2", TSynth::MT32}, {"E3", TSynth::SoundFont},
		{"Extra", TSynth::MT32}
	};
	CAudioMixer mixer;
	for (unsigned i = 0; i < CAudioMixer::MaxEngines; ++i)
		CHECK(mixer.AddEngine(&engines[i]));
	CHECK_FALSE(mixer.AddEngine(&engines[CAudioMixer::MaxEngines]));
}

// ---------------------------------------------------------------
// Volume and Pan
// ---------------------------------------------------------------

TEST_CASE("Mixer: default engine volume is 0.8")
{
	CSynthBaseStub a("A", TSynth::MT32);
	CAudioMixer mixer;
	mixer.AddEngine(&a);
	CHECK(mixer.GetEngineVolume(&a) == doctest::Approx(0.8f));
}

TEST_CASE("Mixer: set/get engine volume")
{
	CSynthBaseStub a("A", TSynth::MT32);
	CAudioMixer mixer;
	mixer.AddEngine(&a);
	mixer.SetEngineVolume(&a, 0.5f);
	CHECK(mixer.GetEngineVolume(&a) == doctest::Approx(0.5f));
}

TEST_CASE("Mixer: volume is clamped")
{
	CSynthBaseStub a("A", TSynth::MT32);
	CAudioMixer mixer;
	mixer.AddEngine(&a);

	mixer.SetEngineVolume(&a, 1.5f);
	CHECK(mixer.GetEngineVolume(&a) == doctest::Approx(1.0f));

	mixer.SetEngineVolume(&a, -0.5f);
	CHECK(mixer.GetEngineVolume(&a) == doctest::Approx(0.0f));
}

TEST_CASE("Mixer: default pan is center")
{
	CSynthBaseStub a("A", TSynth::MT32);
	CAudioMixer mixer;
	mixer.AddEngine(&a);
	CHECK(mixer.GetEnginePan(&a) == doctest::Approx(0.0f));
}

TEST_CASE("Mixer: set/get engine pan")
{
	CSynthBaseStub a("A", TSynth::MT32);
	CAudioMixer mixer;
	mixer.AddEngine(&a);
	mixer.SetEnginePan(&a, -0.7f);
	CHECK(mixer.GetEnginePan(&a) == doctest::Approx(-0.7f));
}

TEST_CASE("Mixer: pan is clamped")
{
	CSynthBaseStub a("A", TSynth::MT32);
	CAudioMixer mixer;
	mixer.AddEngine(&a);

	mixer.SetEnginePan(&a, 2.0f);
	CHECK(mixer.GetEnginePan(&a) == doctest::Approx(1.0f));

	mixer.SetEnginePan(&a, -3.0f);
	CHECK(mixer.GetEnginePan(&a) == doctest::Approx(-1.0f));
}

TEST_CASE("Mixer: set/get master volume")
{
	CAudioMixer mixer;
	mixer.SetMasterVolume(0.6f);
	CHECK(mixer.GetMasterVolume() == doctest::Approx(0.6f));

	mixer.SetMasterVolume(5.0f);
	CHECK(mixer.GetMasterVolume() == doctest::Approx(1.0f));
}

TEST_CASE("Mixer: volume/pan of unregistered engine returns 0")
{
	CSynthBaseStub a("A", TSynth::MT32);
	CSynthBaseStub b("B", TSynth::SoundFont);
	CAudioMixer mixer;
	mixer.AddEngine(&a);
	CHECK(mixer.GetEngineVolume(&b) == doctest::Approx(0.0f));
	CHECK(mixer.GetEnginePan(&b) == doctest::Approx(0.0f));
}

// ---------------------------------------------------------------
// Render — single engine
// ---------------------------------------------------------------

TEST_CASE("Mixer: render with one engine, center pan, full volume")
{
	CSynthBaseStub a("A", TSynth::MT32, 0.5f);
	CAudioMixer mixer;
	mixer.AddEngine(&a);
	mixer.SetEngineVolume(&a, 1.0f);
	mixer.SetMasterVolume(1.0f);

	constexpr size_t nFrames = 64;
	float output[nFrames * 2];
	memset(output, 0, sizeof(output));

	mixer.Render(output, nFrames);

	// With vol=1.0 and pan=0 (center): gainL = 1.0, gainR = 1.0
	// Engine outputs 0.5 on all samples
	for (size_t i = 0; i < nFrames * 2; ++i)
		CHECK(output[i] == doctest::Approx(0.5f));
}

TEST_CASE("Mixer: render with volume scaling")
{
	CSynthBaseStub a("A", TSynth::MT32, 1.0f);
	CAudioMixer mixer;
	mixer.AddEngine(&a);
	mixer.SetEngineVolume(&a, 0.5f);
	mixer.SetMasterVolume(1.0f);

	constexpr size_t nFrames = 32;
	float output[nFrames * 2];

	mixer.Render(output, nFrames);

	// Engine outputs 1.0, volume 0.5, pan center → 0.5
	for (size_t i = 0; i < nFrames * 2; ++i)
		CHECK(output[i] == doctest::Approx(0.5f));
}

TEST_CASE("Mixer: master volume scales everything")
{
	CSynthBaseStub a("A", TSynth::MT32, 1.0f);
	CAudioMixer mixer;
	mixer.AddEngine(&a);
	mixer.SetEngineVolume(&a, 1.0f);
	mixer.SetMasterVolume(0.25f);

	constexpr size_t nFrames = 16;
	float output[nFrames * 2];

	mixer.Render(output, nFrames);

	for (size_t i = 0; i < nFrames * 2; ++i)
		CHECK(output[i] == doctest::Approx(0.25f));
}

// ---------------------------------------------------------------
// Render — pan
// ---------------------------------------------------------------

TEST_CASE("Mixer: hard pan left")
{
	CSynthBaseStub a("A", TSynth::MT32, 1.0f);
	CAudioMixer mixer;
	mixer.AddEngine(&a);
	mixer.SetEngineVolume(&a, 1.0f);
	mixer.SetEnginePan(&a, -1.0f);
	mixer.SetMasterVolume(1.0f);

	constexpr size_t nFrames = 16;
	float output[nFrames * 2];

	mixer.Render(output, nFrames);

	// Pan -1.0: gainL = vol * 1.0 = 1.0, gainR = vol * (1 + (-1)) = 0.0
	for (size_t i = 0; i < nFrames * 2; i += 2)
	{
		CHECK(output[i]     == doctest::Approx(1.0f));   // left
		CHECK(output[i + 1] == doctest::Approx(0.0f));   // right
	}
}

TEST_CASE("Mixer: hard pan right")
{
	CSynthBaseStub a("A", TSynth::MT32, 1.0f);
	CAudioMixer mixer;
	mixer.AddEngine(&a);
	mixer.SetEngineVolume(&a, 1.0f);
	mixer.SetEnginePan(&a, 1.0f);
	mixer.SetMasterVolume(1.0f);

	constexpr size_t nFrames = 16;
	float output[nFrames * 2];

	mixer.Render(output, nFrames);

	// Pan +1.0: gainL = vol * (1 - 1) = 0.0, gainR = vol * 1.0 = 1.0
	for (size_t i = 0; i < nFrames * 2; i += 2)
	{
		CHECK(output[i]     == doctest::Approx(0.0f));   // left
		CHECK(output[i + 1] == doctest::Approx(1.0f));   // right
	}
}

// ---------------------------------------------------------------
// Render — two engines
// ---------------------------------------------------------------

TEST_CASE("Mixer: two engines sum their output")
{
	CSynthBaseStub a("A", TSynth::MT32, 0.3f);
	CSynthBaseStub b("B", TSynth::SoundFont, 0.4f);
	CAudioMixer mixer;
	mixer.AddEngine(&a);
	mixer.AddEngine(&b);
	mixer.SetEngineVolume(&a, 1.0f);
	mixer.SetEngineVolume(&b, 1.0f);
	mixer.SetMasterVolume(1.0f);

	constexpr size_t nFrames = 16;
	float output[nFrames * 2];

	mixer.Render(output, nFrames);

	// 0.3 * 1.0 + 0.4 * 1.0 = 0.7
	for (size_t i = 0; i < nFrames * 2; ++i)
		CHECK(output[i] == doctest::Approx(0.7f));
}

TEST_CASE("Mixer: output is clamped to [-1, 1]")
{
	CSynthBaseStub a("A", TSynth::MT32, 0.8f);
	CSynthBaseStub b("B", TSynth::SoundFont, 0.8f);
	CAudioMixer mixer;
	mixer.AddEngine(&a);
	mixer.AddEngine(&b);
	mixer.SetEngineVolume(&a, 1.0f);
	mixer.SetEngineVolume(&b, 1.0f);
	mixer.SetMasterVolume(1.0f);

	constexpr size_t nFrames = 8;
	float output[nFrames * 2];

	mixer.Render(output, nFrames);

	// 0.8 + 0.8 = 1.6 → clamped to 1.0
	for (size_t i = 0; i < nFrames * 2; ++i)
		CHECK(output[i] == doctest::Approx(1.0f));
}

TEST_CASE("Mixer: two engines with different pans")
{
	CSynthBaseStub a("A", TSynth::MT32, 1.0f);
	CSynthBaseStub b("B", TSynth::SoundFont, 1.0f);
	CAudioMixer mixer;
	mixer.AddEngine(&a);
	mixer.AddEngine(&b);
	mixer.SetEngineVolume(&a, 0.5f);
	mixer.SetEngineVolume(&b, 0.5f);
	mixer.SetEnginePan(&a, -1.0f);  // full left
	mixer.SetEnginePan(&b, 1.0f);   // full right
	mixer.SetMasterVolume(1.0f);

	constexpr size_t nFrames = 8;
	float output[nFrames * 2];

	mixer.Render(output, nFrames);

	// A: gainL=0.5, gainR=0
	// B: gainL=0,   gainR=0.5
	for (size_t i = 0; i < nFrames * 2; i += 2)
	{
		CHECK(output[i]     == doctest::Approx(0.5f));   // left = A only
		CHECK(output[i + 1] == doctest::Approx(0.5f));   // right = B only
	}
}

// ---------------------------------------------------------------
// Solo mode
// ---------------------------------------------------------------

TEST_CASE("Mixer: solo engine bypasses mixing")
{
	CSynthBaseStub a("A", TSynth::MT32, 0.3f);
	CSynthBaseStub b("B", TSynth::SoundFont, 0.7f);
	CAudioMixer mixer;
	mixer.AddEngine(&a);
	mixer.AddEngine(&b);
	mixer.SetSoloEngine(&a);
	mixer.SetMasterVolume(1.0f);

	constexpr size_t nFrames = 8;
	float output[nFrames * 2];

	mixer.Render(output, nFrames);

	// Only A renders (0.3), B is skipped
	for (size_t i = 0; i < nFrames * 2; ++i)
		CHECK(output[i] == doctest::Approx(0.3f));

	CHECK(a.m_nRenderCount == 1);
	CHECK(b.m_nRenderCount == 0);
}

TEST_CASE("Mixer: solo mode applies master volume")
{
	CSynthBaseStub a("A", TSynth::MT32, 1.0f);
	CAudioMixer mixer;
	mixer.AddEngine(&a);
	mixer.SetSoloEngine(&a);
	mixer.SetMasterVolume(0.4f);

	constexpr size_t nFrames = 8;
	float output[nFrames * 2];

	mixer.Render(output, nFrames);

	for (size_t i = 0; i < nFrames * 2; ++i)
		CHECK(output[i] == doctest::Approx(0.4f));
}

TEST_CASE("Mixer: clear solo returns to normal mixing")
{
	CSynthBaseStub a("A", TSynth::MT32, 0.3f);
	CSynthBaseStub b("B", TSynth::SoundFont, 0.4f);
	CAudioMixer mixer;
	mixer.AddEngine(&a);
	mixer.AddEngine(&b);
	mixer.SetEngineVolume(&a, 1.0f);
	mixer.SetEngineVolume(&b, 1.0f);
	mixer.SetMasterVolume(1.0f);

	mixer.SetSoloEngine(&a);
	mixer.ClearSoloEngine();

	constexpr size_t nFrames = 8;
	float output[nFrames * 2];
	mixer.Render(output, nFrames);

	// Back to summing: 0.3 + 0.4 = 0.7
	for (size_t i = 0; i < nFrames * 2; ++i)
		CHECK(output[i] == doctest::Approx(0.7f));
}

TEST_CASE("Mixer: solo with unregistered engine is rejected")
{
	CSynthBaseStub a("A", TSynth::MT32);
	CSynthBaseStub b("B", TSynth::SoundFont);
	CAudioMixer mixer;
	mixer.AddEngine(&a);
	mixer.SetSoloEngine(&b);  // not registered
	CHECK(mixer.GetSoloEngine() == nullptr);
}

// ---------------------------------------------------------------
// Edge cases
// ---------------------------------------------------------------

TEST_CASE("Mixer: render with no engines produces silence")
{
	CAudioMixer mixer;
	constexpr size_t nFrames = 16;
	float output[nFrames * 2];
	for (auto& s : output) s = 99.0f;

	mixer.Render(output, nFrames);

	for (size_t i = 0; i < nFrames * 2; ++i)
		CHECK(output[i] == doctest::Approx(0.0f));
}

TEST_CASE("Mixer: render with zero volume produces silence")
{
	CSynthBaseStub a("A", TSynth::MT32, 1.0f);
	CAudioMixer mixer;
	mixer.AddEngine(&a);
	mixer.SetEngineVolume(&a, 0.0f);
	mixer.SetMasterVolume(1.0f);

	constexpr size_t nFrames = 8;
	float output[nFrames * 2];

	mixer.Render(output, nFrames);

	for (size_t i = 0; i < nFrames * 2; ++i)
		CHECK(output[i] == doctest::Approx(0.0f));
}
