//
// test_config_parse.cpp
//
// Unit tests for the config parser primitive converters and INI dispatch.
// Tests the ParseOption logic in isolation — no Circle / FatFS dependencies.
// The same conversion algorithms are used verbatim; the only difference is
// that they are declared as free functions here rather than as CConfig static
// member functions, avoiding the heavy include chain of config.h.
//

#include "doctest/doctest.h"

#include <cstdlib>
#include <cstring>
#include <strings.h>   // strcasecmp (POSIX)

// inih — real library, available at ../external/inih/
#include "ini.h"

// ---------------------------------------------------------------
// Replicate config.cpp ParseOption logic as standalone helpers
// (same code as production, only relocated for host-side testing)
// ---------------------------------------------------------------

static const char* const s_TrueStrings[]  = {"true", "on",  "1"};
static const char* const s_FalseStrings[] = {"false", "off", "0"};

static bool ParseBool(const char* pString, bool* pOut)
{
        for (auto s : s_TrueStrings)
                if (!strcasecmp(pString, s)) { *pOut = true;  return true; }
        for (auto s : s_FalseStrings)
                if (!strcasecmp(pString, s)) { *pOut = false; return true; }
        return false;
}

static bool ParseInt(const char* pString, int* pOut, bool bHex = false)
{
        *pOut = static_cast<int>(strtol(pString, nullptr, bHex ? 16 : 10));
        return true;
}

static bool ParseFloat(const char* pString, float* pOut)
{
        *pOut = strtof(pString, nullptr);
        return true;
}

// Generic enum parser — mirrors the templated ParseEnum<> in config.cpp
static bool ParseEnum(const char* pString, int* pOut,
                      const char* const* pStrings, size_t nCount)
{
        for (size_t i = 0; i < nCount; ++i)
                if (!strcasecmp(pString, pStrings[i]))
                {
                        *pOut = static_cast<int>(i);
                        return true;
                }
        return false;
}

// ---------------------------------------------------------------
// Bool parsing
// ---------------------------------------------------------------

TEST_CASE("Config/Bool: true strings")
{
        bool v = false;
        CHECK(ParseBool("true",  &v));  CHECK(v == true);
        CHECK(ParseBool("on",    &v));  CHECK(v == true);
        CHECK(ParseBool("1",     &v));  CHECK(v == true);
}

TEST_CASE("Config/Bool: false strings")
{
        bool v = true;
        CHECK(ParseBool("false", &v));  CHECK(v == false);
        CHECK(ParseBool("off",   &v));  CHECK(v == false);
        CHECK(ParseBool("0",     &v));  CHECK(v == false);
}

TEST_CASE("Config/Bool: case-insensitive")
{
        bool v = false;
        CHECK(ParseBool("TRUE",  &v));  CHECK(v == true);
        CHECK(ParseBool("True",  &v));  CHECK(v == true);
        CHECK(ParseBool("ON",    &v));  CHECK(v == true);
        CHECK(ParseBool("FALSE", &v));  CHECK(v == false);
        CHECK(ParseBool("OFF",   &v));  CHECK(v == false);
}

TEST_CASE("Config/Bool: invalid strings return false, value unchanged")
{
        bool v = true;
        CHECK_FALSE(ParseBool("yes",   &v));  CHECK(v == true);
        CHECK_FALSE(ParseBool("",      &v));  CHECK(v == true);
        CHECK_FALSE(ParseBool("2",     &v));  CHECK(v == true);
        CHECK_FALSE(ParseBool("maybe", &v));  CHECK(v == true);
}

TEST_CASE("Config/Bool: boundary — '0' vs '00'")
{
        // inih trims surrounding whitespace; '00' is not a recognised boolean.
        bool v = true;
        CHECK(ParseBool("0",  &v));  CHECK(v == false);
        CHECK_FALSE(ParseBool("00", &v));
}

// ---------------------------------------------------------------
// Int parsing
// ---------------------------------------------------------------

TEST_CASE("Config/Int: basic decimal values")
{
        int v = -99;
        CHECK(ParseInt("0",   &v));  CHECK(v == 0);
        CHECK(ParseInt("42",  &v));  CHECK(v == 42);
        CHECK(ParseInt("-1",  &v));  CHECK(v == -1);
        CHECK(ParseInt("100", &v));  CHECK(v == 100);
}

TEST_CASE("Config/Int: typical config constants")
{
        int v = 0;
        CHECK(ParseInt("31250",  &v));  CHECK(v == 31250);   // MIDI baud rate
        CHECK(ParseInt("48000",  &v));  CHECK(v == 48000);   // sample rate
        CHECK(ParseInt("256",    &v));  CHECK(v == 256);     // chunk size
        CHECK(ParseInt("400000", &v));  CHECK(v == 400000);  // I2C baud rate
        CHECK(ParseInt("8765",   &v));  CHECK(v == 8765);    // WebSocket port
        CHECK(ParseInt("250",    &v));  CHECK(v == 250);     // WS interval ms
}

TEST_CASE("Config/Int: hex mode")
{
        int v = 0;
        CHECK(ParseInt("1F",    &v, true));  CHECK(v == 0x1F);
        CHECK(ParseInt("FF",    &v, true));  CHECK(v == 0xFF);
        CHECK(ParseInt("0",     &v, true));  CHECK(v == 0);
        CHECK(ParseInt("400000",&v, true));  CHECK(v == 0x400000);
}

TEST_CASE("Config/Int: negative values")
{
        int v = 0;
        CHECK(ParseInt("-1",    &v));  CHECK(v == -1);
        CHECK(ParseInt("-300",  &v));  CHECK(v == -300);
}

// ---------------------------------------------------------------
// Float parsing
// ---------------------------------------------------------------

TEST_CASE("Config/Float: basic values")
{
        float v = 0.f;
        CHECK(ParseFloat("0.0",  &v));  CHECK(v == doctest::Approx(0.0f));
        CHECK(ParseFloat("1.0",  &v));  CHECK(v == doctest::Approx(1.0f));
        CHECK(ParseFloat("0.5",  &v));  CHECK(v == doctest::Approx(0.5f));
        CHECK(ParseFloat("-2.5", &v));  CHECK(v == doctest::Approx(-2.5f));
}

TEST_CASE("Config/Float: integer strings")
{
        float v = 0.f;
        CHECK(ParseFloat("1", &v));  CHECK(v == doctest::Approx(1.0f));
        CHECK(ParseFloat("0", &v));  CHECK(v == doctest::Approx(0.0f));
}

TEST_CASE("Config/Float: gain / reverb values")
{
        float v = 0.f;
        CHECK(ParseFloat("0.8",  &v));  CHECK(v == doctest::Approx(0.8f));
        CHECK(ParseFloat("0.25", &v));  CHECK(v == doctest::Approx(0.25f));
        CHECK(ParseFloat("2.0",  &v));  CHECK(v == doctest::Approx(2.0f));
}

// ---------------------------------------------------------------
// Enum parsing
// ---------------------------------------------------------------

// System default synth: mt32, soundfont
static const char* const s_SynthNames[] = {"mt32", "soundfont"};

TEST_CASE("Config/Enum: basic match")
{
        int v = -1;
        CHECK(ParseEnum("mt32",      &v, s_SynthNames, 2));  CHECK(v == 0);
        CHECK(ParseEnum("soundfont", &v, s_SynthNames, 2));  CHECK(v == 1);
}

TEST_CASE("Config/Enum: case-insensitive")
{
        int v = -1;
        CHECK(ParseEnum("MT32",      &v, s_SynthNames, 2));  CHECK(v == 0);
        CHECK(ParseEnum("SOUNDFONT", &v, s_SynthNames, 2));  CHECK(v == 1);
        CHECK(ParseEnum("SoundFont", &v, s_SynthNames, 2));  CHECK(v == 1);
        CHECK(ParseEnum("Mt32",      &v, s_SynthNames, 2));  CHECK(v == 0);
}

TEST_CASE("Config/Enum: invalid string returns false, value unchanged")
{
        int v = 42;
        CHECK_FALSE(ParseEnum("invalid",    &v, s_SynthNames, 2));  CHECK(v == 42);
        CHECK_FALSE(ParseEnum("",           &v, s_SynthNames, 2));  CHECK(v == 42);
        CHECK_FALSE(ParseEnum("fluidsynth", &v, s_SynthNames, 2));  CHECK(v == 42);
}

// Network mode: off, ethernet, wifi
static const char* const s_NetworkModeNames[] = {"off", "ethernet", "wifi"};

TEST_CASE("Config/Enum: network mode")
{
        int v = -1;
        CHECK(ParseEnum("off",      &v, s_NetworkModeNames, 3));  CHECK(v == 0);
        CHECK(ParseEnum("ethernet", &v, s_NetworkModeNames, 3));  CHECK(v == 1);
        CHECK(ParseEnum("wifi",     &v, s_NetworkModeNames, 3));  CHECK(v == 2);
        CHECK(ParseEnum("WiFi",     &v, s_NetworkModeNames, 3));  CHECK(v == 2);
}

// Audio output device: pwm, hdmi, i2s
static const char* const s_AudioDevNames[] = {"pwm", "hdmi", "i2s"};

TEST_CASE("Config/Enum: audio output device")
{
        int v = -1;
        CHECK(ParseEnum("pwm",  &v, s_AudioDevNames, 3));  CHECK(v == 0);
        CHECK(ParseEnum("hdmi", &v, s_AudioDevNames, 3));  CHECK(v == 1);
        CHECK(ParseEnum("i2s",  &v, s_AudioDevNames, 3));  CHECK(v == 2);
        CHECK(ParseEnum("PWM",  &v, s_AudioDevNames, 3));  CHECK(v == 0);
}

// MT-32 ROM set: old, new, cm32l, any, all
static const char* const s_ROMSetNames[] = {"old", "new", "cm32l", "any", "all"};

TEST_CASE("Config/Enum: MT-32 ROM set")
{
        int v = -1;
        CHECK(ParseEnum("old",   &v, s_ROMSetNames, 5));  CHECK(v == 0);
        CHECK(ParseEnum("new",   &v, s_ROMSetNames, 5));  CHECK(v == 1);
        CHECK(ParseEnum("cm32l", &v, s_ROMSetNames, 5));  CHECK(v == 2);
        CHECK(ParseEnum("any",   &v, s_ROMSetNames, 5));  CHECK(v == 3);
        CHECK(ParseEnum("all",   &v, s_ROMSetNames, 5));  CHECK(v == 4);
        CHECK(ParseEnum("CM32L", &v, s_ROMSetNames, 5));  CHECK(v == 2);
}

// ---------------------------------------------------------------
// INI parsing via inih — end-to-end dispatch tests
// ---------------------------------------------------------------

struct TTestConfig
{
        bool  bVerbose      = false;
        int   nBaudRate     = 0;
        float fGain         = 0.f;
        char  szSynth[32]   = {};
        int   nUnhandled    = 0;  // keys routed but not matched
};

static int TestINIHandler(void* pUser, const char* pSection,
                          const char* pName, const char* pValue)
{
        auto* cfg = static_cast<TTestConfig*>(pUser);

        if (!strcmp("system", pSection))
        {
                if (!strcmp("verbose",   pName)) return ParseBool (pValue, &cfg->bVerbose) ? 1 : 0;
                if (!strcmp("gain",      pName)) return ParseFloat(pValue, &cfg->fGain)    ? 1 : 0;
                if (!strcmp("synth",     pName))
                {
                        strncpy(cfg->szSynth, pValue, sizeof(cfg->szSynth) - 1);
                        cfg->szSynth[sizeof(cfg->szSynth) - 1] = '\0';
                        return 1;
                }
                ++cfg->nUnhandled;
                return 0;
        }

        if (!strcmp("midi", pSection))
        {
                if (!strcmp("baud_rate", pName)) return ParseInt(pValue, &cfg->nBaudRate) ? 1 : 0;
                ++cfg->nUnhandled;
                return 0;
        }

        ++cfg->nUnhandled;
        return 0;
}

TEST_CASE("Config/INI: basic parse — known keys set correctly")
{
        const char* ini =
                "[system]\n"
                "verbose = true\n"
                "gain = 0.8\n"
                "synth = mt32\n"
                "[midi]\n"
                "baud_rate = 31250\n";

        TTestConfig cfg;
        CHECK(ini_parse_string(ini, TestINIHandler, &cfg) == 0);
        CHECK(cfg.bVerbose == true);
        CHECK(cfg.fGain    == doctest::Approx(0.8f));
        CHECK(strcmp(cfg.szSynth, "mt32") == 0);
        CHECK(cfg.nBaudRate == 31250);
        CHECK(cfg.nUnhandled == 0);
}

TEST_CASE("Config/INI: empty string is valid, fields stay at defaults")
{
        TTestConfig cfg;
        CHECK(ini_parse_string("", TestINIHandler, &cfg) == 0);
        CHECK(cfg.bVerbose  == false);
        CHECK(cfg.nBaudRate == 0);
        CHECK(cfg.nUnhandled == 0);
}

TEST_CASE("Config/INI: unknown section is silently skipped — inih returns line > 0")
{
        // When the handler returns 0 for an unknown key/section, inih records
        // the first such line number as the return value but CONTINUES parsing.
        // This mirrors CConfig::INIHandler which also returns 0 for unknown keys.
        const char* ini =
                "[system]\n"
                "verbose = true\n"
                "[unknown_section]\n"
                "foo = bar\n";

        TTestConfig cfg;
        const int r = ini_parse_string(ini, TestINIHandler, &cfg);
        CHECK(r > 0);              // line number where handler returned 0
        CHECK(cfg.bVerbose == true);
        CHECK(cfg.nUnhandled == 1);
}

TEST_CASE("Config/INI: unknown key in known section — inih returns line > 0")
{
        // Same as above: handler returns 0 for unknown key; inih records line
        // but continues. Known keys are still parsed correctly.
        const char* ini =
                "[system]\n"
                "verbose = on\n"
                "unknown_key = something\n";

        TTestConfig cfg;
        const int r = ini_parse_string(ini, TestINIHandler, &cfg);
        CHECK(r > 0);
        CHECK(cfg.bVerbose == true);
        CHECK(cfg.nUnhandled == 1);
}

TEST_CASE("Config/INI: comments and blank lines are ignored")
{
        const char* ini =
                "; this is a comment\n"
                "# another comment style\n"
                "\n"
                "[system]\n"
                "; comment after section header\n"
                "verbose = false\n"
                "\n"
                "gain = 1.0\n";

        TTestConfig cfg;
        CHECK(ini_parse_string(ini, TestINIHandler, &cfg) == 0);
        CHECK(cfg.bVerbose == false);
        CHECK(cfg.fGain    == doctest::Approx(1.0f));
        CHECK(cfg.nUnhandled == 0);
}

TEST_CASE("Config/INI: inih trims whitespace around values")
{
        const char* ini =
                "[system]\n"
                "verbose =  true  \n"
                "gain    =  0.5   \n";

        TTestConfig cfg;
        CHECK(ini_parse_string(ini, TestINIHandler, &cfg) == 0);
        CHECK(cfg.bVerbose == true);
        CHECK(cfg.fGain    == doctest::Approx(0.5f));
}

TEST_CASE("Config/INI: bool value '1' accepted via INI")
{
        const char* ini =
                "[system]\n"
                "verbose = 1\n";

        TTestConfig cfg;
        CHECK(ini_parse_string(ini, TestINIHandler, &cfg) == 0);
        CHECK(cfg.bVerbose == true);
}

TEST_CASE("Config/INI: multiple sections, keys interleaved")
{
        const char* ini =
                "[midi]\n"
                "baud_rate = 38400\n"
                "[system]\n"
                "verbose = off\n"
                "[midi]\n"
                "baud_rate = 31250\n";  // second occurrence overwrites

        TTestConfig cfg;
        CHECK(ini_parse_string(ini, TestINIHandler, &cfg) == 0);
        CHECK(cfg.nBaudRate == 31250);
        CHECK(cfg.bVerbose  == false);
}
