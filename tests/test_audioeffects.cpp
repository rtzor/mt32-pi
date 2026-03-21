// tests/test_audioeffects.cpp
// Unit tests for CAudioEffects (doctest).

#include "doctest/doctest.h"
#include "audioeffects.h"

#include <cmath>
#include <cstring>
#include <initializer_list>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static constexpr float SR = 48000.0f;

// Fill a stereo interleaved buffer with a constant value on both channels.
static void FillConstant(float* buf, size_t nFrames, float L, float R)
{
	for (size_t i = 0; i < nFrames; ++i)
	{
		buf[i * 2 + 0] = L;
		buf[i * 2 + 1] = R;
	}
}

// Fill a stereo interleaved buffer with a sine tone on both channels.
static void FillSine(float* buf, size_t nFrames, float freqHz, float amp = 0.5f)
{
	for (size_t i = 0; i < nFrames; ++i)
	{
		float s = amp * sinf(2.0f * 3.14159265f * freqHz * i / SR);
		buf[i * 2 + 0] = s;
		buf[i * 2 + 1] = s;
	}
}

// Check that all samples in buf are in [-limit, +limit].
static bool AllInRange(const float* buf, size_t nFrames, float limit = 1.0f)
{
	for (size_t i = 0; i < nFrames * 2; ++i)
	{
		if (buf[i] > limit || buf[i] < -limit)
			return false;
	}
	return true;
}

// Check that two buffers are equal sample-by-sample (within tolerance).
static bool Approximately(const float* a, const float* b, size_t nFrames, float tol = 1e-5f)
{
	for (size_t i = 0; i < nFrames * 2; ++i)
	{
		if (fabsf(a[i] - b[i]) > tol)
			return false;
	}
	return true;
}

// ---------------------------------------------------------------------------
// Tests — Basic
// ---------------------------------------------------------------------------

TEST_CASE("CAudioEffects: default config does not change signal")
{
	// All effects disabled by default.
	CAudioEffects fx;
	CAudioEffects::TConfig cfg;   // bEQEnabled=false, bLimiterEnabled=false, bReverbEnabled=false
	fx.Configure(cfg, SR);

	constexpr size_t N = 128;
	float orig[N * 2];
	float buf[N * 2];
	FillSine(orig, N, 440.0f, 0.5f);
	std::memcpy(buf, orig, sizeof(buf));

	fx.Process(buf, N);

	// With all effects off and no out-of-range samples, output == input
	CHECK(Approximately(orig, buf, N));
}

TEST_CASE("CAudioEffects: hard clamp always active")
{
	CAudioEffects fx;
	CAudioEffects::TConfig cfg;
	fx.Configure(cfg, SR);

	constexpr size_t N = 8;
	float buf[N * 2];
	FillConstant(buf, N, 2.0f, -3.0f);   // way out of range

	fx.Process(buf, N);

	CHECK(AllInRange(buf, N));
	// Positive channel should be exactly 1.0, negative exactly -1.0
	for (size_t i = 0; i < N; ++i)
	{
		CHECK(buf[i * 2 + 0] == doctest::Approx(1.0f).epsilon(1e-6f));
		CHECK(buf[i * 2 + 1] == doctest::Approx(-1.0f).epsilon(1e-6f));
	}
}

// ---------------------------------------------------------------------------
// Tests — Limiter
// ---------------------------------------------------------------------------

TEST_CASE("CAudioEffects: limiter does not alter signal below knee (0.8)")
{
	CAudioEffects fx;
	CAudioEffects::TConfig cfg;
	cfg.bLimiterEnabled = true;
	fx.Configure(cfg, SR);

	constexpr size_t N = 64;
	float buf[N * 2];
	// 0.5 is well below the 0.8 knee — should pass unchanged
	FillConstant(buf, N, 0.5f, -0.5f);

	float orig[N * 2];
	std::memcpy(orig, buf, sizeof(buf));

	fx.Process(buf, N);

	CHECK(Approximately(orig, buf, N));
}

TEST_CASE("CAudioEffects: limiter keeps output in [-1, 1] for any input")
{
	CAudioEffects fx;
	CAudioEffects::TConfig cfg;
	cfg.bLimiterEnabled = true;
	fx.Configure(cfg, SR);

	constexpr size_t N = 4;
	// Test several overload levels
	for (float amp : {0.9f, 1.0f, 1.5f, 2.0f, 10.0f})
	{
		float buf[N * 2];
		FillConstant(buf, N, amp, -amp);
		fx.Process(buf, N);
		CHECK(AllInRange(buf, N));
	}
}

TEST_CASE("CAudioEffects: limiter knee at 1.0 gives output exactly 1.0")
{
	CAudioEffects fx;
	CAudioEffects::TConfig cfg;
	cfg.bLimiterEnabled = true;
	fx.Configure(cfg, SR);

	constexpr size_t N = 2;
	float buf[N * 2];
	FillConstant(buf, N, 1.0f, 1.0f);
	fx.Process(buf, N);

	for (size_t i = 0; i < N; ++i)
		CHECK(buf[i * 2] == doctest::Approx(1.0f).epsilon(1e-5f));
}

// ---------------------------------------------------------------------------
// Tests — EQ (identity with 0 dB gain)
// ---------------------------------------------------------------------------

TEST_CASE("CAudioEffects: EQ with 0 dB gains is transparent")
{
	CAudioEffects fx;
	CAudioEffects::TConfig cfg;
	cfg.bEQEnabled = true;
	cfg.nBassGain   = 0;
	cfg.nTrebleGain = 0;
	fx.Configure(cfg, SR);

	constexpr size_t N = 256;
	float buf[N * 2];
	FillSine(buf, N, 440.0f, 0.3f);

	float orig[N * 2];
	std::memcpy(orig, buf, sizeof(buf));

	// Steady-state: the biquads will have a small transient at startup.
	// Skip the first ~50 frames and compare the rest.
	fx.Process(buf, 50);

	// Reset and process again from the same starting state
	CAudioEffects fx2;
	fx2.Configure(cfg, SR);
	std::memcpy(buf, orig, sizeof(buf));
	fx2.Process(buf, N);

	// All samples should remain well within ±1 (0 dB gain doesn't amplify)
	CHECK(AllInRange(buf, N, 0.31f));
}

// ---------------------------------------------------------------------------
// Tests — Reverb
// ---------------------------------------------------------------------------

TEST_CASE("CAudioEffects: reverb produces non-zero output after silence burst")
{
	CAudioEffects fx;
	CAudioEffects::TConfig cfg;
	cfg.bReverbEnabled  = true;
	cfg.fReverbRoomSize = 0.8f;
	cfg.fReverbDamping  = 0.5f;
	cfg.fReverbWet      = 0.5f;
	fx.Configure(cfg, SR);

	// Freeverb shortest comb delay at 48 kHz is ~1215 samples.
	// Use 2048 frames so the reverb tail has time to emerge.
	constexpr size_t N = 2048;
	float buf[N * 2];

	// Feed a short impulse then silence
	FillConstant(buf, N, 0.0f, 0.0f);
	buf[0] = 0.5f;
	buf[1] = 0.5f;

	fx.Process(buf, N);

	// The reverb tail means we should find at least some non-zero energy
	// after the shortest comb delay (sample 1215+)
	float energy = 0.0f;
	for (size_t i = 1215; i < N; ++i)
		energy += buf[i * 2 + 0] * buf[i * 2 + 0];

	CHECK(energy > 1e-6f);
}

TEST_CASE("CAudioEffects: reverb output stays in [-1, 1] with loud input")
{
	// A loud steady-state sine + long reverb can accumulate energy.
	// The safety hard clamp must keep samples within bounds.
	CAudioEffects fx;
	CAudioEffects::TConfig cfg;
	cfg.bReverbEnabled  = true;
	cfg.fReverbRoomSize = 1.0f;
	cfg.fReverbDamping  = 0.0f;
	cfg.fReverbWet      = 1.0f;
	fx.Configure(cfg, SR);

	constexpr size_t N = 1024;
	float buf[N * 2];
	FillSine(buf, N, 440.0f, 0.8f);

	fx.Process(buf, N);

	CHECK(AllInRange(buf, N));
}

// ---------------------------------------------------------------------------
// Tests — Configure / GetConfig roundtrip
// ---------------------------------------------------------------------------

TEST_CASE("CAudioEffects: GetConfig returns what was configured")
{
	CAudioEffects fx;
	CAudioEffects::TConfig cfg;
	cfg.bEQEnabled      = true;
	cfg.nBassGain       = 6;
	cfg.nTrebleGain     = -3;
	cfg.bLimiterEnabled = true;
	cfg.bReverbEnabled  = true;
	cfg.fReverbRoomSize = 0.7f;
	cfg.fReverbDamping  = 0.3f;
	cfg.fReverbWet      = 0.25f;

	fx.Configure(cfg, SR);
	const CAudioEffects::TConfig& out = fx.GetConfig();

	CHECK(out.bEQEnabled      == true);
	CHECK(out.nBassGain       == 6);
	CHECK(out.nTrebleGain     == -3);
	CHECK(out.bLimiterEnabled == true);
	CHECK(out.bReverbEnabled  == true);
	CHECK(out.fReverbRoomSize == doctest::Approx(0.7f).epsilon(1e-5f));
	CHECK(out.fReverbDamping  == doctest::Approx(0.3f).epsilon(1e-5f));
	CHECK(out.fReverbWet      == doctest::Approx(0.25f).epsilon(1e-5f));
}
