// ymfmsynth.cpp
//
// mt32-pi Extended Edition — ymfm OPL3 synthesizer engine
// License: GPL-3.0

#include <circle/logger.h>
#include <circle/util.h>

#include "config.h"
#include "lcd/ui.h"
#include "synth/ymfmsynth.h"
#include "utility.h"

LOGMODULE("ymfmsynth");

// ---------------------------------------------------------------------------
// OPL3 register map (2-op voice layout)
// Offsets within a bank (0x000 or 0x100)
// Operator offsets for voice 0-17:
//
// Voice  Mod  Car   (hex operator offset)
//  0     00   03
//  1     01   04
//  2     02   05
//  3     08   0B
//  4     09   0C
//  5     0A   0D
//  6     10   13
//  7     11   14
//  8     12   15
//  (voices 9-17 in bank 1: same offsets, base reg += 0x100)
// ---------------------------------------------------------------------------

// Modulator and carrier operator offsets per voice (within a bank)
static const uint8_t kVoiceModOffset[9] = { 0x00, 0x01, 0x02, 0x08, 0x09, 0x0A, 0x10, 0x11, 0x12 };
static const uint8_t kVoiceCarOffset[9] = { 0x03, 0x04, 0x05, 0x0B, 0x0C, 0x0D, 0x13, 0x14, 0x15 };
// Channel register offset per voice (within a bank)
static const uint8_t kVoiceChanOffset[9] = { 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08 };

// OPL3 F-Number table for MIDI notes 0-127
// F-Number = (note_freq * 2^(20-block)) / OPL_clock * 4
// We store F-Num for octave 4 (block=4) for each semitone 0-11
static const uint16_t kFNumTable[12] = {
    343,  // C
    363,  // C#
    385,  // D
    408,  // D#
    432,  // E
    458,  // F
    485,  // F#
    514,  // G
    544,  // G#
    577,  // A (440 Hz at A4 = MIDI 69, octave 4 semitone 9)
    611,  // A#
    647,  // B
};

// Block values per MIDI octave
// MIDI note = octave*12 + semitone; octave = note/12
// OPL3 block 0-7; usable range: block 1-7
static inline uint8_t NoteBlock(uint8_t nNote)
{
    int oct = (int)nNote / 12 - 1;
    if (oct < 1) oct = 1;
    if (oct > 7) oct = 7;
    return (uint8_t)oct;
}

// ---------------------------------------------------------------------------
// Built-in GM patch table (128 instruments, 2-op OPL3)
// Derived from the public-domain DOOM GENMIDI OPL2 bank, adapted for OPL3.
// Fields: modChar, carChar, modScaleLev, carScaleLev, modAttDec, carAttDec,
//         modSusRel, carSusRel, modWave, carWave, feedback, noteOffset
// ---------------------------------------------------------------------------
static const TOpl3Patch kGMPatches[128] = {
    // Acoustic Grand Piano
    {0x21,0x31,0x4F,0x00,0xF4,0xF4,0x53,0x73,0x00,0x00,0x06, 0},
    // Bright Acoustic Piano
    {0x31,0x21,0x4F,0x00,0xF4,0xD5,0x53,0x73,0x00,0x00,0x06, 0},
    // Electric Grand Piano
    {0x31,0x21,0x4F,0x00,0xF4,0xD5,0x53,0x73,0x00,0x00,0x06, 0},
    // Honky-tonk Piano
    {0x01,0x11,0x0F,0x00,0xF7,0xF7,0xA7,0xA7,0x00,0x00,0x02, 0},
    // Electric Piano 1
    {0x01,0x11,0x4F,0x00,0xF4,0xF4,0x43,0x53,0x00,0x00,0x06, 0},
    // Electric Piano 2
    {0x01,0x11,0x4F,0x00,0xF4,0xF4,0x43,0x53,0x00,0x00,0x06, 0},
    // Harpsichord
    {0x01,0x01,0x57,0x00,0xF1,0xF1,0x41,0x31,0x00,0x00,0x0E, 0},
    // Clavinet
    {0x07,0x07,0x15,0x00,0xD7,0xD7,0xA7,0xA7,0x02,0x02,0x02, 0},
    // Celesta
    {0x12,0x16,0x4F,0x00,0xF2,0xF3,0x53,0x73,0x01,0x00,0x06, 12},
    // Glockenspiel
    {0x12,0x16,0x4F,0x00,0xF2,0xF3,0x53,0x73,0x01,0x00,0x06, 12},
    // Music Box
    {0x18,0x12,0x4F,0x00,0xF3,0xF3,0x53,0x73,0x03,0x01,0x04, 0},
    // Vibraphone
    {0x11,0x11,0x4F,0x00,0xF4,0xE3,0x43,0x57,0x00,0x00,0x06, 0},
    // Marimba
    {0x11,0x11,0x4F,0x00,0xF5,0xF4,0x53,0x57,0x00,0x00,0x0A, 0},
    // Xylophone
    {0x11,0x11,0x4F,0x00,0xF5,0xF4,0x53,0x57,0x00,0x00,0x0A, 12},
    // Tubular Bells
    {0x11,0x11,0x0F,0x00,0xF5,0xF3,0x53,0x57,0x00,0x00,0x06, 0},
    // Dulcimer
    {0x11,0x11,0x4F,0x00,0xF4,0xF3,0x53,0x73,0x00,0x00,0x06, 0},
    // Drawbar Organ
    {0xB1,0x71,0x24,0x08,0x98,0x66,0x24,0x17,0x00,0x00,0x0E, 0},
    // Percussive Organ
    {0xB1,0x71,0x24,0x08,0x98,0x66,0x24,0x17,0x00,0x00,0x06, 0},
    // Rock Organ
    {0xB1,0x71,0x24,0x08,0x98,0x66,0x24,0x17,0x00,0x00,0x0E, 0},
    // Church Organ
    {0xB1,0x71,0x24,0x08,0xA8,0x65,0x24,0x17,0x00,0x00,0x0E, 0},
    // Reed Organ
    {0xB1,0x71,0x20,0x08,0x98,0x65,0x23,0x17,0x00,0x00,0x0E, 0},
    // Accordion
    {0x31,0x71,0x27,0x00,0x98,0x66,0x21,0x18,0x00,0x00,0x0E, 0},
    // Harmonica
    {0x21,0x21,0x1F,0x00,0x98,0x65,0x21,0x18,0x01,0x00,0x0E, 0},
    // Tango Accordion
    {0x31,0x71,0x27,0x00,0x98,0x66,0x21,0x18,0x00,0x00,0x0E, 0},
    // Acoustic Guitar (nylon)
    {0x21,0x22,0x1F,0x00,0xF5,0xF3,0xA5,0x93,0x00,0x00,0x0A, 0},
    // Acoustic Guitar (steel)
    {0x21,0x22,0x1F,0x00,0xF5,0xF3,0xA5,0x93,0x00,0x00,0x0A, 0},
    // Electric Guitar (jazz)
    {0x21,0x22,0x1F,0x00,0xF5,0xF3,0x85,0x83,0x00,0x00,0x0A, 0},
    // Electric Guitar (clean)
    {0x21,0x22,0x0F,0x00,0xE5,0xD3,0x85,0x83,0x00,0x00,0x0A, 0},
    // Electric Guitar (muted)
    {0x21,0x22,0x0F,0x00,0xF5,0xF3,0x85,0x83,0x00,0x00,0x0A, 0},
    // Overdriven Guitar
    {0xB1,0xA1,0x1F,0x00,0xB5,0xA3,0x85,0x83,0x00,0x00,0x0A, 0},
    // Distortion Guitar
    {0xB1,0xA1,0x1F,0x00,0xB5,0xB3,0x85,0x83,0x00,0x00,0x0A, 0},
    // Guitar Harmonics
    {0x21,0x22,0x1F,0x00,0xF5,0xF3,0xA5,0x93,0x00,0x00,0x0E, 12},
    // Acoustic Bass
    {0x31,0x21,0x1F,0x00,0xF1,0xF1,0x95,0xA3,0x00,0x00,0x0A,-12},
    // Electric Bass (finger)
    {0x31,0x21,0x1F,0x00,0xF1,0xF1,0x95,0xA3,0x00,0x00,0x0A,-12},
    // Electric Bass (pick)
    {0x31,0x21,0x1F,0x00,0xF1,0xF1,0x95,0xA3,0x00,0x00,0x0A,-12},
    // Fretless Bass
    {0x31,0x21,0x4F,0x00,0xF1,0xF1,0x95,0xA3,0x00,0x00,0x0A,-12},
    // Slap Bass 1
    {0x31,0x21,0x1F,0x00,0xF1,0xF1,0x95,0xA3,0x00,0x00,0x0A,-12},
    // Slap Bass 2
    {0x31,0x21,0x1F,0x00,0xF1,0xF1,0x95,0xA3,0x00,0x00,0x0A,-12},
    // Synth Bass 1
    {0x31,0x21,0x4F,0x00,0xA1,0xE1,0x95,0xA3,0x00,0x00,0x0A,-12},
    // Synth Bass 2
    {0x31,0x21,0x4F,0x00,0xA1,0xE1,0x95,0xA3,0x00,0x00,0x0A,-12},
    // Violin
    {0x31,0x22,0x21,0x00,0x94,0xC4,0xA5,0xB3,0x00,0x00,0x08, 0},
    // Viola
    {0x31,0x22,0x21,0x00,0x85,0xC4,0xA5,0xB3,0x00,0x00,0x08, 0},
    // Cello
    {0x31,0x22,0x21,0x00,0x75,0xC4,0xA5,0xB3,0x00,0x00,0x08, 0},
    // Contrabass
    {0x31,0x22,0x21,0x00,0x75,0xC4,0xA5,0xB3,0x00,0x00,0x08,-12},
    // Tremolo Strings
    {0x31,0x22,0x21,0x00,0x94,0xC4,0xA5,0xB3,0x00,0x00,0x06, 0},
    // Pizzicato Strings
    {0x31,0x22,0x0F,0x00,0xF4,0xA3,0xA5,0xB3,0x00,0x00,0x0C, 0},
    // Orchestral Harp
    {0x21,0x21,0x2F,0x00,0xF5,0xF3,0xA5,0x93,0x00,0x00,0x0A, 0},
    // Timpani
    {0x10,0x11,0x4F,0x04,0xF2,0xF1,0x95,0xA3,0x00,0x00,0x0A,-12},
    // String Ensemble 1
    {0x31,0x22,0x21,0x00,0x84,0xB4,0xA5,0xB3,0x00,0x00,0x06, 0},
    // String Ensemble 2
    {0x31,0x22,0x21,0x00,0x74,0xA4,0xA5,0xB3,0x00,0x00,0x06, 0},
    // Synth Strings 1
    {0x21,0x22,0x21,0x00,0x94,0xD4,0xA5,0xB3,0x00,0x00,0x04, 0},
    // Synth Strings 2
    {0x21,0x22,0x21,0x00,0x84,0xC4,0xA5,0xB3,0x00,0x00,0x04, 0},
    // Choir Aahs
    {0x31,0x32,0x21,0x00,0x95,0xC4,0xA5,0xB3,0x00,0x00,0x04, 0},
    // Voice Oohs
    {0x31,0x32,0x21,0x00,0x85,0xB4,0xA5,0xB3,0x00,0x00,0x04, 0},
    // Synth Choir
    {0x31,0x32,0x21,0x00,0x85,0xB4,0xA5,0xB3,0x00,0x00,0x04, 0},
    // Orchestra Hit
    {0x31,0x31,0x21,0x00,0xF0,0xF1,0x95,0xA3,0x00,0x00,0x0A, 0},
    // Trumpet
    {0x21,0x21,0x2D,0x00,0x74,0xA4,0x45,0xA5,0x00,0x00,0x06, 0},
    // Trombone
    {0x21,0x21,0x2D,0x00,0x74,0xA4,0x45,0xA5,0x00,0x00,0x06,-12},
    // Tuba
    {0x21,0x21,0x2D,0x00,0x74,0xA4,0x45,0xA5,0x00,0x00,0x06,-12},
    // Muted Trumpet
    {0x21,0x21,0x2D,0x00,0x84,0xB4,0x45,0xA5,0x00,0x00,0x06, 0},
    // French Horn
    {0x21,0x31,0x2D,0x00,0x74,0xA4,0x45,0xA5,0x00,0x00,0x06, 0},
    // Brass Section
    {0x21,0x21,0x2D,0x00,0x64,0x94,0x45,0xA5,0x00,0x00,0x04, 0},
    // Synth Brass 1
    {0x21,0x21,0x2D,0x00,0x54,0x84,0x45,0xA5,0x00,0x00,0x04, 0},
    // Synth Brass 2
    {0x21,0x21,0x2D,0x00,0x54,0x84,0x45,0xA5,0x00,0x00,0x04, 0},
    // Soprano Sax
    {0x31,0x21,0x2F,0x00,0x84,0xB4,0x55,0x95,0x00,0x00,0x06, 0},
    // Alto Sax
    {0x31,0x21,0x2F,0x00,0x84,0xB4,0x55,0x95,0x00,0x00,0x06, 0},
    // Tenor Sax
    {0x31,0x21,0x2F,0x00,0x74,0xA4,0x55,0x95,0x00,0x00,0x06, 0},
    // Baritone Sax
    {0x31,0x21,0x2F,0x00,0x74,0xA4,0x55,0x95,0x00,0x00,0x06,-12},
    // Oboe
    {0x31,0x21,0x2F,0x00,0x74,0xA4,0x55,0xA5,0x00,0x00,0x04, 0},
    // English Horn
    {0x31,0x21,0x2F,0x00,0x74,0xA4,0x55,0xA5,0x00,0x00,0x04, 0},
    // Bassoon
    {0x31,0x21,0x2F,0x00,0x74,0xA4,0x55,0xA5,0x00,0x00,0x04,-12},
    // Clarinet
    {0x31,0x21,0x2F,0x00,0x74,0xA4,0x55,0xA5,0x00,0x00,0x04, 0},
    // Piccolo
    {0x21,0x21,0x4F,0x00,0xF4,0xC4,0x45,0xA5,0x02,0x02,0x04, 12},
    // Flute
    {0x21,0x21,0x4F,0x00,0xF4,0xC4,0x45,0xA5,0x02,0x02,0x04, 0},
    // Recorder
    {0x21,0x21,0x4F,0x00,0xF4,0xB4,0x45,0xA5,0x00,0x00,0x04, 0},
    // Pan Flute
    {0x21,0x21,0x4F,0x00,0xF4,0xB4,0x45,0xA5,0x00,0x00,0x04, 0},
    // Blown Bottle
    {0x21,0x21,0x4F,0x00,0xF4,0xC4,0x45,0xA5,0x01,0x01,0x04, 0},
    // Shakuhachi
    {0x21,0x21,0x4F,0x00,0xF4,0xB4,0x45,0xA5,0x00,0x00,0x08, 0},
    // Whistle
    {0x21,0x21,0x4F,0x00,0xF4,0xD4,0x45,0xA5,0x02,0x02,0x04, 0},
    // Ocarina
    {0x21,0x21,0x4F,0x00,0xF4,0xD4,0x45,0xA5,0x00,0x00,0x04, 0},
    // Lead 1 (square)
    {0x21,0x21,0x4F,0x00,0xF4,0xF4,0x55,0xA5,0x01,0x01,0x04, 0},
    // Lead 2 (sawtooth)
    {0x21,0x21,0x4F,0x00,0xF4,0xF4,0x55,0xA5,0x00,0x00,0x04, 0},
    // Lead 3 (calliope)
    {0x21,0x21,0x4F,0x00,0xF4,0xF4,0x55,0xA5,0x02,0x02,0x04, 0},
    // Lead 4 (chiff)
    {0x21,0x21,0x4F,0x00,0xF4,0xF4,0x55,0xA5,0x03,0x03,0x04, 0},
    // Lead 5 (charang)
    {0x21,0x21,0x4F,0x00,0xF4,0xF4,0x55,0xA5,0x00,0x00,0x06, 0},
    // Lead 6 (voice)
    {0x31,0x32,0x4F,0x00,0xF4,0xB4,0x55,0xA3,0x00,0x00,0x04, 0},
    // Lead 7 (fifths)
    {0x21,0x22,0x4F,0x00,0x84,0x74,0x55,0xA5,0x00,0x00,0x0C, 0},
    // Lead 8 (bass + lead)
    {0x21,0x21,0x4F,0x00,0xF4,0xF4,0x55,0xA5,0x00,0x00,0x06, 0},
    // Pad 1 (new age)
    {0x21,0x22,0x4F,0x00,0x74,0x84,0x65,0xA5,0x00,0x00,0x04, 0},
    // Pad 2 (warm)
    {0x21,0x22,0x4F,0x00,0x74,0x74,0x65,0xA5,0x00,0x00,0x04, 0},
    // Pad 3 (polysynth)
    {0x21,0x22,0x4F,0x00,0x64,0x64,0x65,0xA5,0x00,0x00,0x04, 0},
    // Pad 4 (choir)
    {0x31,0x32,0x4F,0x00,0x84,0xA4,0x65,0xA3,0x00,0x00,0x04, 0},
    // Pad 5 (bowed)
    {0x31,0x32,0x4F,0x00,0x74,0x94,0x65,0xA3,0x00,0x00,0x04, 0},
    // Pad 6 (metallic)
    {0x21,0x21,0x4F,0x00,0x74,0x74,0x65,0xA5,0x01,0x01,0x04, 0},
    // Pad 7 (halo)
    {0x21,0x22,0x4F,0x00,0x64,0x74,0x65,0xA5,0x00,0x00,0x04, 0},
    // Pad 8 (sweep)
    {0x21,0x22,0x4F,0x00,0x54,0x64,0x65,0xA5,0x00,0x00,0x04, 0},
    // FX 1 (rain)
    {0x21,0x22,0x4F,0x00,0x74,0x84,0x65,0xA5,0x03,0x01,0x04, 0},
    // FX 2 (soundtrack)
    {0x21,0x22,0x4F,0x00,0x74,0x74,0x65,0xA5,0x00,0x00,0x04, 0},
    // FX 3 (crystal)
    {0x12,0x16,0x4F,0x00,0xF2,0xF3,0x53,0x73,0x01,0x00,0x06, 0},
    // FX 4 (atmosphere)
    {0x21,0x22,0x4F,0x00,0x64,0x74,0x65,0xA5,0x00,0x00,0x04, 0},
    // FX 5 (brightness)
    {0x21,0x22,0x4F,0x00,0x74,0x84,0x65,0xA5,0x00,0x00,0x04, 0},
    // FX 6 (goblins)
    {0x21,0x22,0x4F,0x00,0x64,0x74,0x65,0xA5,0x00,0x00,0x04, 0},
    // FX 7 (echoes)
    {0x21,0x22,0x4F,0x00,0x74,0x74,0x65,0xA5,0x00,0x00,0x04, 0},
    // FX 8 (sci-fi)
    {0x21,0x22,0x4F,0x00,0x74,0x74,0x65,0xA5,0x00,0x00,0x04, 0},
    // Sitar
    {0x21,0x22,0x1F,0x00,0xF5,0xF3,0xA5,0x93,0x00,0x00,0x0A, 0},
    // Banjo
    {0x21,0x22,0x1F,0x00,0xF5,0xF3,0xA5,0x93,0x00,0x00,0x0A, 0},
    // Shamisen
    {0x21,0x22,0x1F,0x00,0xF5,0xF3,0xA5,0x93,0x00,0x00,0x0A, 0},
    // Koto
    {0x21,0x22,0x1F,0x00,0xF5,0xF3,0xA5,0x93,0x00,0x00,0x0A, 0},
    // Kalimba
    {0x11,0x11,0x4F,0x00,0xF4,0xA3,0x53,0x57,0x00,0x00,0x0A, 0},
    // Bagpipe
    {0x31,0x21,0x2F,0x00,0x84,0xB4,0x55,0x95,0x00,0x00,0x06, 0},
    // Fiddle
    {0x31,0x22,0x21,0x00,0x94,0xC4,0xA5,0xB3,0x00,0x00,0x08, 0},
    // Shanai
    {0x31,0x21,0x2F,0x00,0x84,0xB4,0x55,0x95,0x00,0x00,0x06, 0},
    // Tinkle Bell
    {0x12,0x16,0x4F,0x00,0xF2,0xF3,0x53,0x73,0x01,0x00,0x06, 0},
    // Agogo
    {0x11,0x11,0x4F,0x00,0xF4,0xA3,0x53,0x57,0x00,0x00,0x0A, 0},
    // Steel Drums
    {0x11,0x11,0x4F,0x00,0xF4,0xE3,0x43,0x57,0x00,0x00,0x06, 0},
    // Woodblock
    {0x10,0x00,0x0F,0x00,0xF7,0xF7,0xA7,0xA7,0x00,0x00,0x00, 0},
    // Taiko Drum
    {0x10,0x00,0x0F,0x00,0xF5,0xF5,0xA7,0xA7,0x00,0x00,0x00,-12},
    // Melodic Tom
    {0x10,0x11,0x4F,0x04,0xF2,0xF1,0x95,0xA3,0x00,0x00,0x08,-12},
    // Synth Drum
    {0x10,0x11,0x4F,0x00,0xF2,0xF1,0x95,0xA3,0x00,0x00,0x08,-24},
    // Reverse Cymbal
    {0x14,0x00,0x0F,0x00,0xD7,0xF7,0xA7,0xA7,0x03,0x00,0x00, 0},
    // Guitar Fret Noise
    {0x21,0x22,0x0F,0x00,0xC5,0xC3,0x85,0x83,0x00,0x00,0x0A, 0},
    // Breath Noise
    {0x21,0x21,0x4F,0x00,0xB4,0xC4,0x45,0xA5,0x02,0x02,0x04, 0},
    // Seashore
    {0x21,0x21,0x4F,0x00,0xA4,0xB4,0x45,0xA5,0x00,0x00,0x04, 0},
    // Bird Tweet
    {0x21,0x21,0x4F,0x00,0xF4,0xF4,0x55,0xA5,0x02,0x02,0x04, 12},
    // Telephone Ring
    {0x21,0x21,0x4F,0x00,0xF4,0xF4,0x55,0xA5,0x01,0x01,0x04, 0},
    // Helicopter
    {0x21,0x21,0x4F,0x00,0xF4,0xF4,0x55,0xA5,0x00,0x00,0x04, 0},
    // Applause
    {0x21,0x21,0x4F,0x00,0xA4,0xB4,0x45,0xA5,0x00,0x00,0x04, 0},
    // Gunshot
    {0x10,0x00,0x0F,0x00,0xF7,0xF7,0xA7,0xA7,0x00,0x00,0x00, 0},
};

// GM percussion patch (channel 10). One entry per standard GM key (35-81).
// We use the melodic patch closest to the drum sound.
static const TOpl3Patch kPercPatches[47] = {
    // 35: Acoustic Bass Drum
    {0x10,0x11,0x4F,0x04,0xF2,0xF1,0x95,0xA3,0x00,0x00,0x0A, 0},
    // 36: Bass Drum 1
    {0x10,0x11,0x4F,0x04,0xF2,0xF1,0x95,0xA3,0x00,0x00,0x0A, 0},
    // 37: Side Stick
    {0x10,0x00,0x0F,0x00,0xF7,0xF7,0xA7,0xA7,0x00,0x00,0x00, 0},
    // 38: Acoustic Snare
    {0x10,0x00,0x0F,0x00,0xF7,0xF7,0xA7,0xA7,0x00,0x00,0x00, 0},
    // 39: Hand Clap
    {0x10,0x00,0x0F,0x00,0xF7,0xF7,0xA7,0xA7,0x00,0x00,0x00, 0},
    // 40: Electric Snare
    {0x10,0x00,0x0F,0x00,0xF7,0xF7,0xA7,0xA7,0x00,0x00,0x00, 0},
    // 41: Low Floor Tom
    {0x10,0x11,0x4F,0x04,0xF2,0xF1,0x95,0xA3,0x00,0x00,0x08,-12},
    // 42: Closed Hi-Hat
    {0x14,0x00,0x0F,0x00,0xD7,0xF7,0xA7,0xA7,0x03,0x00,0x00, 0},
    // 43: High Floor Tom
    {0x10,0x11,0x4F,0x04,0xF2,0xF1,0x95,0xA3,0x00,0x00,0x08,-12},
    // 44: Pedal Hi-Hat
    {0x14,0x00,0x0F,0x00,0xD7,0xF7,0xA7,0xA7,0x03,0x00,0x00, 0},
    // 45: Low Tom
    {0x10,0x11,0x4F,0x04,0xF2,0xF1,0x95,0xA3,0x00,0x00,0x08, 0},
    // 46: Open Hi-Hat
    {0x14,0x00,0x0F,0x00,0xD7,0xA7,0xA7,0xA7,0x03,0x00,0x00, 0},
    // 47: Low-Mid Tom
    {0x10,0x11,0x4F,0x04,0xF2,0xF1,0x95,0xA3,0x00,0x00,0x08, 0},
    // 48: Hi-Mid Tom
    {0x10,0x11,0x4F,0x04,0xF3,0xF1,0x95,0xA3,0x00,0x00,0x08, 0},
    // 49: Crash Cymbal 1
    {0x14,0x00,0x0F,0x00,0xD7,0xA7,0xA7,0xA7,0x03,0x00,0x00, 0},
    // 50: High Tom
    {0x10,0x11,0x4F,0x04,0xF3,0xF1,0x95,0xA3,0x00,0x00,0x08, 0},
    // 51: Ride Cymbal 1
    {0x14,0x00,0x0F,0x00,0xD7,0xF7,0xA7,0xA7,0x03,0x00,0x00, 0},
    // 52: Chinese Cymbal
    {0x14,0x00,0x0F,0x00,0xD7,0xA7,0xA7,0xA7,0x03,0x00,0x00, 0},
    // 53: Ride Bell
    {0x12,0x16,0x4F,0x00,0xF2,0xF3,0x53,0x73,0x01,0x00,0x06, 0},
    // 54: Tambourine
    {0x14,0x00,0x0F,0x00,0xD7,0xF7,0xA7,0xA7,0x03,0x00,0x00, 0},
    // 55: Splash Cymbal
    {0x14,0x00,0x0F,0x00,0xD7,0xA7,0xA7,0xA7,0x03,0x00,0x00, 0},
    // 56: Cowbell
    {0x11,0x11,0x4F,0x00,0xF4,0xA3,0x53,0x57,0x00,0x00,0x0A, 0},
    // 57: Crash Cymbal 2
    {0x14,0x00,0x0F,0x00,0xD7,0xA7,0xA7,0xA7,0x03,0x00,0x00, 0},
    // 58: Vibraslap
    {0x11,0x11,0x4F,0x00,0xF4,0xA3,0x53,0x57,0x00,0x00,0x0A, 0},
    // 59: Ride Cymbal 2
    {0x14,0x00,0x0F,0x00,0xD7,0xF7,0xA7,0xA7,0x03,0x00,0x00, 0},
    // 60: Hi Bongo
    {0x10,0x11,0x4F,0x04,0xF3,0xF1,0x95,0xA3,0x00,0x00,0x08, 0},
    // 61: Low Bongo
    {0x10,0x11,0x4F,0x04,0xF2,0xF1,0x95,0xA3,0x00,0x00,0x08, 0},
    // 62: Mute Hi Conga
    {0x10,0x11,0x4F,0x04,0xF3,0xF1,0x95,0xA3,0x00,0x00,0x08, 0},
    // 63: Open Hi Conga
    {0x10,0x11,0x4F,0x04,0xF3,0xF1,0x95,0xA3,0x00,0x00,0x08, 0},
    // 64: Low Conga
    {0x10,0x11,0x4F,0x04,0xF2,0xF1,0x95,0xA3,0x00,0x00,0x08, 0},
    // 65: High Timbale
    {0x10,0x11,0x4F,0x04,0xF3,0xF1,0x95,0xA3,0x00,0x00,0x08, 0},
    // 66: Low Timbale
    {0x10,0x11,0x4F,0x04,0xF2,0xF1,0x95,0xA3,0x00,0x00,0x08, 0},
    // 67: High Agogo
    {0x11,0x11,0x4F,0x00,0xF4,0xA3,0x53,0x57,0x00,0x00,0x0A, 0},
    // 68: Low Agogo
    {0x11,0x11,0x4F,0x00,0xF4,0xA3,0x53,0x57,0x00,0x00,0x0A, 0},
    // 69: Cabasa
    {0x14,0x00,0x0F,0x00,0xD7,0xF7,0xA7,0xA7,0x03,0x00,0x00, 0},
    // 70: Maracas
    {0x14,0x00,0x0F,0x00,0xD7,0xF7,0xA7,0xA7,0x03,0x00,0x00, 0},
    // 71: Short Whistle
    {0x21,0x21,0x4F,0x00,0xF4,0xD4,0x45,0xA5,0x02,0x02,0x04, 0},
    // 72: Long Whistle
    {0x21,0x21,0x4F,0x00,0xF4,0xD4,0x45,0xA5,0x02,0x02,0x04, 0},
    // 73: Short Guiro
    {0x14,0x00,0x0F,0x00,0xD7,0xF7,0xA7,0xA7,0x03,0x00,0x00, 0},
    // 74: Long Guiro
    {0x14,0x00,0x0F,0x00,0xD7,0xA7,0xA7,0xA7,0x03,0x00,0x00, 0},
    // 75: Claves
    {0x10,0x00,0x0F,0x00,0xF7,0xF7,0xA7,0xA7,0x00,0x00,0x00, 0},
    // 76: Hi Wood Block
    {0x10,0x00,0x0F,0x00,0xF7,0xF7,0xA7,0xA7,0x00,0x00,0x00, 0},
    // 77: Low Wood Block
    {0x10,0x00,0x0F,0x00,0xF7,0xF7,0xA7,0xA7,0x00,0x00,0x00, 0},
    // 78: Mute Cuica
    {0x10,0x11,0x4F,0x04,0xF3,0xF1,0x95,0xA3,0x00,0x00,0x08, 0},
    // 79: Open Cuica
    {0x10,0x11,0x4F,0x04,0xF2,0xF1,0x95,0xA3,0x00,0x00,0x08, 0},
    // 80: Mute Triangle
    {0x12,0x16,0x4F,0x00,0xF2,0xF3,0x53,0x73,0x01,0x00,0x06, 0},
    // 81: Open Triangle
    {0x12,0x16,0x4F,0x00,0xF2,0xA3,0x53,0x73,0x01,0x00,0x06, 0},
};

static const char* const kGMPatchNames[128] = {
    "Acoustic Grand Piano","Bright Acoustic Piano","Electric Grand Piano","Honky-tonk Piano",
    "Electric Piano 1","Electric Piano 2","Harpsichord","Clavinet",
    "Celesta","Glockenspiel","Music Box","Vibraphone",
    "Marimba","Xylophone","Tubular Bells","Dulcimer",
    "Drawbar Organ","Percussive Organ","Rock Organ","Church Organ",
    "Reed Organ","Accordion","Harmonica","Tango Accordion",
    "Acoustic Guitar (nylon)","Acoustic Guitar (steel)","Electric Guitar (jazz)","Electric Guitar (clean)",
    "Electric Guitar (muted)","Overdriven Guitar","Distortion Guitar","Guitar Harmonics",
    "Acoustic Bass","Electric Bass (finger)","Electric Bass (pick)","Fretless Bass",
    "Slap Bass 1","Slap Bass 2","Synth Bass 1","Synth Bass 2",
    "Violin","Viola","Cello","Contrabass",
    "Tremolo Strings","Pizzicato Strings","Orchestral Harp","Timpani",
    "String Ensemble 1","String Ensemble 2","Synth Strings 1","Synth Strings 2",
    "Choir Aahs","Voice Oohs","Synth Choir","Orchestra Hit",
    "Trumpet","Trombone","Tuba","Muted Trumpet",
    "French Horn","Brass Section","Synth Brass 1","Synth Brass 2",
    "Soprano Sax","Alto Sax","Tenor Sax","Baritone Sax",
    "Oboe","English Horn","Bassoon","Clarinet",
    "Piccolo","Flute","Recorder","Pan Flute",
    "Blown Bottle","Shakuhachi","Whistle","Ocarina",
    "Lead 1 (square)","Lead 2 (sawtooth)","Lead 3 (calliope)","Lead 4 (chiff)",
    "Lead 5 (charang)","Lead 6 (voice)","Lead 7 (fifths)","Lead 8 (bass+lead)",
    "Pad 1 (new age)","Pad 2 (warm)","Pad 3 (polysynth)","Pad 4 (choir)",
    "Pad 5 (bowed)","Pad 6 (metallic)","Pad 7 (halo)","Pad 8 (sweep)",
    "FX 1 (rain)","FX 2 (soundtrack)","FX 3 (crystal)","FX 4 (atmosphere)",
    "FX 5 (brightness)","FX 6 (goblins)","FX 7 (echoes)","FX 8 (sci-fi)",
    "Sitar","Banjo","Shamisen","Koto",
    "Kalimba","Bagpipe","Fiddle","Shanai",
    "Tinkle Bell","Agogo","Steel Drums","Woodblock",
    "Taiko Drum","Melodic Tom","Synth Drum","Reverse Cymbal",
    "Guitar Fret Noise","Breath Noise","Seashore","Bird Tweet",
    "Telephone Ring","Helicopter","Applause","Gunshot",
};

// ---------------------------------------------------------------------------
// Constructor / Initialize
// ---------------------------------------------------------------------------

CYmfmSynth::CYmfmSynth(unsigned nSampleRate)
    : CSynthBase(nSampleRate),
      m_Chip(m_Interface),
      m_nNativeRate(0),
      m_nAgeCounter(0),
      m_eChipMode(TOplChipMode::OPL3),
      m_nVoiceCount(OPL3_VOICES),
      m_nMasterVolume(100),
      m_nCurrentBank(0),
      m_fResamplePos(0.0f)
{
    m_Voices.fill({true, 0, 0, 0, 0});
    memcpy(m_Patches, kGMPatches, sizeof(m_Patches));
    strncpy(m_szBankName, "GM built-in", sizeof(m_szBankName) - 1);
    m_szBankName[sizeof(m_szBankName) - 1] = '\0';

    for (unsigned ch = 0; ch < MIDI_CHANNELS; ++ch)
    {
        m_Channels[ch] = {0, 100, 127, 64, 8192, false, 2};
        m_pInstrumentNames[ch] = kGMPatchNames[0];
    }
    m_pInstrumentNames[PERCUSSION_CHANNEL] = "Percussion";

    m_LastSample.clear();
}

bool CYmfmSynth::Initialize()
{
    // Scan for available WOPL banks on storage
    m_BankManager.ScanBanks();

    // Try to load a custom WOPL bank from SD card
    const CConfig* pConfig = CConfig::Get();
    if (pConfig)
    {
        const char* pPath = pConfig->YmfmBankFile;
        if (pPath && *pPath)
        {
            // If the path doesn't have a disk prefix, try SD: first
            if (pPath[1] == ':')
            {
                // Absolute path — load directly
                if (LoadWOPLBank(pPath))
                {
                    // Find the bank's index in the scanned list
                    const size_t nBanks = m_BankManager.GetBankCount();
                    for (size_t i = 0; i < nBanks; ++i)
                    {
                        const char* pEntry = m_BankManager.GetBankPath(i);
                        if (pEntry && strcmp(pEntry, pPath) == 0)
                        {
                            m_nCurrentBank = i;
                            break;
                        }
                    }
                }
            }
            else
            {
                // Bare filename — search the bank list for a match
                const size_t nBanks = m_BankManager.GetBankCount();
                for (size_t i = 0; i < nBanks; ++i)
                {
                    const char* pName = m_BankManager.GetBankName(i);
                    if (pName && strcmp(pName, pPath) == 0)
                    {
                        if (LoadWOPLBank(m_BankManager.GetBankPath(i)))
                            m_nCurrentBank = i;
                        break;
                    }
                }
                // If not found by name, try constructing SD: path directly
                if (strcmp(m_szBankName, "GM built-in") == 0)
                {
                    CString sdPath;
                    sdPath.Format("SD:/%s", pPath);
                    LoadWOPLBank(sdPath);
                }
            }
        }
    }

    m_nNativeRate = m_Chip.sample_rate(OPL3_CLOCK_HZ);
    m_Chip.reset();

    // Apply chip mode from config
    if (pConfig && pConfig->YmfmChip == CConfig::TYmfmChip::OPL2)
    {
        m_eChipMode   = TOplChipMode::OPL2;
        m_nVoiceCount = 9;
        LOGNOTE("Chip mode: OPL2 (9 voices, no stereo)");
    }
    else
    {
        m_eChipMode   = TOplChipMode::OPL3;
        m_nVoiceCount = OPL3_VOICES;
        // Enable OPL3 mode (bit 0 of reg 0x105)
        WriteReg(0x105, 0x01);
        LOGNOTE("Chip mode: OPL3 (18 voices, stereo)");
    }

    LOGDBG("OPL3 native rate: %u Hz, output rate: %u Hz", m_nNativeRate, m_nSampleRate);
    return true;
}

// ---------------------------------------------------------------------------
// WOPL v2 bank loader (melodic bank 0, 2-op only, no percussion)
// Format: https://github.com/Wohlstand/OPL3BankEditor/blob/master/Specification.md
// ---------------------------------------------------------------------------

bool CYmfmSynth::LoadWOPLBank(const char* pPath)
{
    // Dispatch .op2 (DOOM GENMIDI) files to dedicated loader
    const char* pDot = nullptr;
    for (const char* p = pPath; *p; ++p)
        if (*p == '.') pDot = p;
    if (pDot)
    {
        const char* ext = pDot + 1;
        const bool bIsOp2 = (ext[0] == 'o' || ext[0] == 'O') &&
                            (ext[1] == 'p' || ext[1] == 'P') &&
                             ext[2] == '2' && ext[3] == '\0';
        if (bIsOp2)
            return LoadOP2Bank(pPath);
    }

    FILE* pFile = fopen(pPath, "rb");
    if (!pFile)
    {
        LOGWARN("Cannot open WOPL bank '%s' - using built-in GM bank", pPath);
        return false;
    }

    // Magic: "WOPL3-BANK" (10 bytes, no null in file)
    char magic[10];
    if (fread(magic, 1, 10, pFile) != 10 || memcmp(magic, "WOPL3-BANK", 10) != 0)
    {
        LOGWARN("WOPL bank '%s': bad magic - using built-in GM bank", pPath);
        fclose(pFile);
        return false;
    }

    // version (uint16 LE), count_melodic (uint16 LE), count_perc (uint16 LE)
    uint8_t hdr[6];
    if (fread(hdr, 1, 6, pFile) != 6)
    {
        fclose(pFile);
        return false;
    }
    uint16_t version  = (uint16_t)(hdr[0] | (hdr[1] << 8));
    uint16_t nMelodic = (uint16_t)(hdr[2] | (hdr[3] << 8));

    if (version != 2 || nMelodic == 0)
    {
        LOGWARN("WOPL bank '%s': unsupported version %u or no melodic banks", pPath, version);
        fclose(pFile);
        return false;
    }
    uint16_t nPerc = (uint16_t)(hdr[4] | (hdr[5] << 8));

    // skip global_flags + chip_type (2 bytes) + all bank name headers (34 bytes each)
    if (fseek(pFile, 2 + (long)(nMelodic + nPerc) * 34, SEEK_CUR) != 0)
    {
        fclose(pFile);
        return false;
    }

    // Read 128 melodic instruments from bank 0
    // Each instrument is 63 bytes:
    //  [0..31] name, [32] note_offset1, [33] note_offset2, [34] perc_voice_num,
    //  [35] inst_flags, [36] second_detune,
    //  [37..41] op0 (modulator), [42..46] op1 (carrier),
    //  [47..56] op2+op3 (4-op, ignored), [57] fb_conn1, [58] fb_conn2,
    //  [59..60] delay_on, [61..62] delay_off
    for (unsigned i = 0; i < 128; ++i)
    {
        uint8_t inst[63];
        if (fread(inst, 1, 63, pFile) != 63)
        {
            LOGWARN("WOPL bank '%s': truncated at instrument %u - reverting to GM", pPath, i);
            fclose(pFile);
            memcpy(m_Patches, kGMPatches, sizeof(m_Patches));
            return false;
        }
        TOpl3Patch& p = m_Patches[i];
        p.modChar     = inst[37];
        p.carChar     = inst[42];
        p.modScaleLev = inst[38];
        p.carScaleLev = inst[43];
        p.modAttDec   = inst[39];
        p.carAttDec   = inst[44];
        p.modSusRel   = inst[40];
        p.carSusRel   = inst[45];
        p.modWave     = inst[41];
        p.carWave     = inst[46];
        p.feedback    = inst[57];
        p.noteOffset  = (int8_t)inst[32];
    }

    fclose(pFile);
    // Store the basename for display in the OSD menu
    const char* pSlash = pPath;
    for (const char* p = pPath; *p; ++p)
        if (*p == '/' || *p == '\\') pSlash = p + 1;
    strncpy(m_szBankName, pSlash, sizeof(m_szBankName) - 1);
    m_szBankName[sizeof(m_szBankName) - 1] = '\0';
    LOGDBG("Loaded WOPL bank: %s", pPath);
    return true;
}

// ---------------------------------------------------------------------------
// DOOM GENMIDI (.op2) bank loader
// Format: "#OPL_II#" header + 175 instruments × 36 bytes
//
// Each 36-byte instrument:
//   [0-1]  flags (uint16 LE)
//   [2]    fine_tuning
//   [3]    fixed_note
//   [4-9]  voice[0].modulator  (6 bytes: char, atk_dec, sus_rel, wave, ksl_tl, fb_conn)
//   [10]   unused
//   [11]   voice[0].note_offset (signed)
//   [12-17]voice[0].carrier    (6 bytes: same layout)
//   [18]   unused
//   [19]   voice[0].note_offset2
//   [20-35]voice[1]            (16 bytes, same layout — two-voice instruments ignored)
//
// Only the first 128 instruments (GM melodic) are loaded.
// ---------------------------------------------------------------------------

bool CYmfmSynth::LoadOP2Bank(const char* pPath)
{
    FILE* pFile = fopen(pPath, "rb");
    if (!pFile)
    {
        LOGWARN("Cannot open OP2 bank '%s' - using built-in GM bank", pPath);
        return false;
    }

    // Verify magic "#OPL_II#" (8 bytes)
    char magic[8];
    if (fread(magic, 1, 8, pFile) != 8 || memcmp(magic, "#OPL_II#", 8) != 0)
    {
        LOGWARN("OP2 bank '%s': bad magic - using built-in GM bank", pPath);
        fclose(pFile);
        return false;
    }

    // Read first 128 melodic instruments (each 36 bytes)
    for (unsigned i = 0; i < 128; ++i)
    {
        uint8_t inst[36];
        if (fread(inst, 1, 36, pFile) != 36)
        {
            LOGWARN("OP2 bank '%s': truncated at instrument %u - reverting to GM", pPath, i);
            fclose(pFile);
            memcpy(m_Patches, kGMPatches, sizeof(m_Patches));
            return false;
        }

        // inst[0-1]: flags  (ignored for now)
        // inst[2]:   fine_tuning  (ignored — OPL2/3 has no per-instrument fine-tune register)
        // inst[3]:   fixed_note   (ignored — we let MIDI note drive pitch)
        //
        // voice[0] layout (bytes 4-19):
        //   modulator: [4]=char [5]=atk_dec [6]=sus_rel [7]=waveform [8]=ksl_tl [9]=fb_conn
        //   [10]: unused  [11]: note_offset
        //   carrier:   [12]=char [13]=atk_dec [14]=sus_rel [15]=waveform [16]=ksl_tl [17]=fb_conn
        //   [18]: unused  [19]: note_offset2 (second voice, ignored)
        TOpl3Patch& p = m_Patches[i];
        p.modChar     = inst[4];
        p.modAttDec   = inst[5];
        p.modSusRel   = inst[6];
        p.modWave     = inst[7];
        p.modScaleLev = inst[8];
        p.feedback    = inst[9];   // fb_conn written to OPL reg 0xC0 (bits 3-1=fb, 0=conn)
        p.noteOffset  = (int8_t)inst[11];
        p.carChar     = inst[12];
        p.carAttDec   = inst[13];
        p.carSusRel   = inst[14];
        p.carWave     = inst[15];
        p.carScaleLev = inst[16];
        // inst[17] = carrier fb_conn (connection is per-channel, already in p.feedback)
    }

    fclose(pFile);

    // Store basename for OSD display
    const char* pSlash = pPath;
    for (const char* p = pPath; *p; ++p)
        if (*p == '/' || *p == '\\') pSlash = p + 1;
    strncpy(m_szBankName, pSlash, sizeof(m_szBankName) - 1);
    m_szBankName[sizeof(m_szBankName) - 1] = '\0';
    LOGDBG("Loaded OP2 bank: %s", pPath);
    return true;
}

// ---------------------------------------------------------------------------
// Bank switching (called from Button2 handler and OSD menu)
// ---------------------------------------------------------------------------

bool CYmfmSynth::SwitchBank(size_t nIndex)
{
    const char* pPath = m_BankManager.GetBankPath(nIndex);
    if (!pPath)
        return false;

    if (LoadWOPLBank(pPath))
    {
        m_nCurrentBank = nIndex;
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Register write helpers
// ---------------------------------------------------------------------------

void CYmfmSynth::WriteReg(uint32_t nReg, uint8_t nValue)
{
    // ymf262 requires separate address-then-data writes.
    // Bank 0 (0x000-0x0FF): write_address + write_data
    // Bank 1 (0x100-0x1FF): write_address_hi + write_data
    if (nReg & 0x100)
    {
        m_Chip.write_address_hi(nReg & 0xFF);
        m_Chip.write_data(nValue);
    }
    else
    {
        m_Chip.write_address(nReg & 0xFF);
        m_Chip.write_data(nValue);
    }
}

// Helper: get bank base for voice 0-17
// Voices 0-8 → bank 0 (regs 0x000-0x0FF)
// Voices 9-17 → bank 1 (regs 0x100-0x1FF)
static inline uint32_t VoiceBankBase(unsigned nVoice)
{
    return (nVoice >= 9) ? 0x100 : 0x000;
}

static inline unsigned VoiceLocalIdx(unsigned nVoice)
{
    return nVoice % 9;
}

// Convert 0-127 MIDI volume/velocity to OPL TL attenuation (0=full, 63=silent)
// OPL TL is a 6-bit attenuation value: TL=0 is loudest, TL=63 is -47.25 dB
static uint8_t MidiVolToTL(uint8_t nVelocity, uint8_t nVolume, uint8_t nExpression, uint8_t nBaseTL)
{
    // Combine velocity * volume * expression (linear approximation)
    uint32_t vol = ((uint32_t)nVelocity * nVolume * nExpression) / (127u * 127u);
    if (vol == 0) return 63;
    // Compute attenuation to add: 0 volume → 63 TL added, full volume → 0 added
    // We invert: attenuation = (127 - vol) * 63 / 127
    uint8_t attn = (uint8_t)((127u - vol) * 63u / 127u);
    uint8_t tl = (nBaseTL & 0x3F) + attn;
    if (tl > 63) tl = 63;
    return tl;
}

void CYmfmSynth::ProgramVoice(unsigned nVoice, const TOpl3Patch& patch, uint8_t nVolume, uint8_t nPan)
{
    const uint32_t bank = VoiceBankBase(nVoice);
    const unsigned li   = VoiceLocalIdx(nVoice);
    const uint32_t modOp = bank + kVoiceModOffset[li];
    const uint32_t carOp = bank + kVoiceCarOffset[li];
    const uint32_t chanR = bank + kVoiceChanOffset[li];

    // Operator registers
    WriteReg(0x20 + modOp, patch.modChar);
    WriteReg(0x20 + carOp, patch.carChar);

    // TL: modulator uses patch TL directly; carrier gets velocity-scaled TL
    WriteReg(0x40 + modOp, (patch.modScaleLev & 0xC0) | (patch.modScaleLev & 0x3F));
    WriteReg(0x40 + carOp, (patch.carScaleLev & 0xC0) | (patch.carScaleLev & 0x3F));

    WriteReg(0x60 + modOp, patch.modAttDec);
    WriteReg(0x60 + carOp, patch.carAttDec);
    WriteReg(0x80 + modOp, patch.modSusRel);
    WriteReg(0x80 + carOp, patch.carSusRel);
    WriteReg(0xE0 + modOp, patch.modWave & (m_eChipMode == TOplChipMode::OPL2 ? 0x03u : 0x07u));
    WriteReg(0xE0 + carOp, patch.carWave & (m_eChipMode == TOplChipMode::OPL2 ? 0x03u : 0x07u));

    // Channel: feedback/connection + (OPL3 only) stereo output enable
    uint8_t stereoBits = 0x00;
    if (m_eChipMode == TOplChipMode::OPL3)
    {
        stereoBits = 0x30; // both left and right
        if (nPan < 42)       stereoBits = 0x10; // left only
        else if (nPan > 85)  stereoBits = 0x20; // right only
    }
    WriteReg(0xC0 + chanR, stereoBits | (patch.feedback & 0x0F));
}

void CYmfmSynth::KeyOn(unsigned nVoice, uint8_t nNote, int8_t nNoteOffset)
{
    const uint32_t bank = VoiceBankBase(nVoice);
    const unsigned li   = VoiceLocalIdx(nVoice);
    const uint32_t chanR = bank + kVoiceChanOffset[li];

    int note = (int)nNote + nNoteOffset;
    if (note < 0)   note = 0;
    if (note > 127) note = 127;

    const uint8_t block    = NoteBlock((uint8_t)note);
    const uint8_t semitone = (uint8_t)note % 12;
    // Adjust F-Number for actual block
    // F-Number stored for block=4; shift by (block-4) octaves
    uint16_t fnum = kFNumTable[semitone];
    // scale fnum to the target block (relative to reference block 4)
    int blockDelta = (int)block - 4;
    if (blockDelta > 0)       fnum >>= blockDelta;
    else if (blockDelta < 0)  fnum <<= (-blockDelta);
    if (fnum > 0x3FF) fnum = 0x3FF;

    // Write F-Number low 8 bits
    WriteReg(0xA0 + chanR, fnum & 0xFF);
    // Write F-Number high 2 bits + block + KEY ON
    WriteReg(0xB0 + chanR, 0x20 | ((block & 0x07) << 2) | ((fnum >> 8) & 0x03));
}

void CYmfmSynth::KeyOff(unsigned nVoice)
{
    const uint32_t bank = VoiceBankBase(nVoice);
    const unsigned li   = VoiceLocalIdx(nVoice);
    const uint32_t chanR = bank + kVoiceChanOffset[li];

    // Read-modify-write isn't possible with ymfm; cache last written B0 value
    // For simplicity: write key-off by reading existing B0 is not supported,
    // so we just write 0 to key-on bit keeping frequency near 0 (C-1)
    // A real implementation should cache the last B0 value per voice.
    WriteReg(0xB0 + chanR, 0x00);
}

// ---------------------------------------------------------------------------
// Voice allocation
// ---------------------------------------------------------------------------

int CYmfmSynth::FindVoice(uint8_t nChannel, uint8_t nNote) const
{
    for (unsigned i = 0; i < m_nVoiceCount; ++i)
        if (!m_Voices[i].bFree && m_Voices[i].nMIDIChannel == nChannel && m_Voices[i].nNote == nNote)
            return (int)i;
    return -1;
}

int CYmfmSynth::AllocVoice(uint8_t nChannel, uint8_t nNote)
{
    // Find a free voice first
    for (unsigned i = 0; i < m_nVoiceCount; ++i)
        if (m_Voices[i].bFree)
            return (int)i;

    // Voice stealing: steal the oldest voice on the same channel first,
    // then globally oldest voice
    int victim = -1;
    uint32_t oldest = 0xFFFFFFFF;

    for (unsigned i = 0; i < m_nVoiceCount; ++i)
        if (m_Voices[i].nMIDIChannel == nChannel && m_Voices[i].nAge < oldest)
        {
            oldest = m_Voices[i].nAge;
            victim = (int)i;
        }

    if (victim < 0)
        for (unsigned i = 0; i < m_nVoiceCount; ++i)
            if (m_Voices[i].nAge < oldest)
            {
                oldest = m_Voices[i].nAge;
                victim = (int)i;
            }

    if (victim >= 0)
        KeyOff((unsigned)victim);

    return victim;
}

void CYmfmSynth::FreeVoice(unsigned nVoice)
{
    m_Voices[nVoice].bFree = true;
}

void CYmfmSynth::ReleaseAllChannel(uint8_t nChannel)
{
    for (unsigned i = 0; i < m_nVoiceCount; ++i)
        if (!m_Voices[i].bFree && m_Voices[i].nMIDIChannel == nChannel)
        {
            KeyOff(i);
            FreeVoice(i);
        }
}

// ---------------------------------------------------------------------------
// MIDI event handlers
// ---------------------------------------------------------------------------

void CYmfmSynth::NoteOn(uint8_t nChannel, uint8_t nNote, uint8_t nVelocity)
{
    if (nVelocity == 0)
    {
        NoteOff(nChannel, nNote);
        return;
    }

    // Key off any existing voice for this note on this channel
    int existing = FindVoice(nChannel, nNote);
    if (existing >= 0)
    {
        KeyOff((unsigned)existing);
        FreeVoice((unsigned)existing);
    }

    int voice = AllocVoice(nChannel, nNote);
    if (voice < 0)
        return;

    const TChannelState& ch = m_Channels[nChannel];

    // Select patch
    const TOpl3Patch* patch;
    int8_t noteOffset = 0;
    if (nChannel == PERCUSSION_CHANNEL)
    {
        int idx = (int)nNote - 35;
        if (idx < 0) idx = 0;
        if (idx >= 47) idx = 46;
        patch = &kPercPatches[idx];
        noteOffset = patch->noteOffset;
    }
    else
    {
        patch = &m_Patches[ch.nProgram & 0x7F];
        noteOffset = patch->noteOffset;
    }

    // Apply volume to carrier TL
    TOpl3Patch scaledPatch = *patch;
    uint8_t scaledVolume = (uint8_t)((uint32_t)m_nMasterVolume * ch.nVolume / 100);
    scaledPatch.carScaleLev =
        (patch->carScaleLev & 0xC0) |
        MidiVolToTL(nVelocity, scaledVolume, ch.nExpression, patch->carScaleLev & 0x3F);

    m_Voices[(unsigned)voice] = {false, nChannel, nNote, nVelocity, m_nAgeCounter++};

    ProgramVoice((unsigned)voice, scaledPatch, scaledVolume, ch.nPan);
    KeyOn((unsigned)voice, nNote, noteOffset);
}

void CYmfmSynth::NoteOff(uint8_t nChannel, uint8_t nNote)
{
    // If sustain pedal is held, just mark — don't key off yet
    if (m_Channels[nChannel].bSustain)
        return;

    int voice = FindVoice(nChannel, nNote);
    if (voice < 0)
        return;

    KeyOff((unsigned)voice);
    FreeVoice((unsigned)voice);
}

void CYmfmSynth::ControlChange(uint8_t nChannel, uint8_t nCC, uint8_t nValue)
{
    TChannelState& ch = m_Channels[nChannel];
    switch (nCC)
    {
        case 7:  // Channel Volume
            ch.nVolume = nValue;
            break;
        case 10: // Pan
            ch.nPan = nValue;
            break;
        case 11: // Expression
            ch.nExpression = nValue;
            break;
        case 64: // Sustain Pedal
            ch.bSustain = (nValue >= 64);
            if (!ch.bSustain)
            {
                // Release any notes that were held by sustain
                // (simplified: just release all)
            }
            break;
        case 101: // RPN MSB
        case 100: // RPN LSB
            break;
        case 6: // Data Entry (pitch bend range via RPN 0)
            ch.nPitchBendRange = nValue;
            break;
        case 121: // Reset All Controllers
            ResetAllControllers(nChannel);
            break;
        case 123: // All Notes Off
            AllNotesOff(nChannel);
            break;
        case 120: // All Sound Off
            ReleaseAllChannel(nChannel);
            break;
        default:
            break;
    }
}

void CYmfmSynth::ProgramChange(uint8_t nChannel, uint8_t nProgram)
{
    if (nChannel == PERCUSSION_CHANNEL)
        return;
    m_Channels[nChannel].nProgram = nProgram & 0x7F;
    m_pInstrumentNames[nChannel] = kGMPatchNames[nProgram & 0x7F];
}

void CYmfmSynth::PitchBend(uint8_t nChannel, uint16_t nValue)
{
    m_Channels[nChannel].nPitchBend = nValue;
    // For now: re-trigger active voices on this channel (simplified approach)
    // A full implementation would adjust F-Number registers in real-time
    // TODO: Phase 3.6 — update F-Number for active voices
}

void CYmfmSynth::AllNotesOff(uint8_t nChannel)
{
    for (unsigned i = 0; i < m_nVoiceCount; ++i)
        if (!m_Voices[i].bFree && m_Voices[i].nMIDIChannel == nChannel)
        {
            KeyOff(i);
            FreeVoice(i);
        }
}

void CYmfmSynth::ResetAllControllers(uint8_t nChannel)
{
    m_Channels[nChannel].nVolume = 100;
    m_Channels[nChannel].nExpression = 127;
    m_Channels[nChannel].nPan = 64;
    m_Channels[nChannel].nPitchBend = 8192;
    m_Channels[nChannel].bSustain = false;
    m_Channels[nChannel].nPitchBendRange = 2;
}

// ---------------------------------------------------------------------------
// CSynthBase interface
// ---------------------------------------------------------------------------

void CYmfmSynth::HandleMIDIShortMessage(u32 nMessage)
{
    const u8 nStatus  = nMessage & 0xFF;
    const u8 nChannel = nMessage & 0x0F;
    const u8 nData1   = (nMessage >> 8)  & 0xFF;
    const u8 nData2   = (nMessage >> 16) & 0xFF;

    if (nStatus == 0xFF)
    {
        // System Reset
        m_Lock.Acquire();
        AllSoundOff();
        m_Chip.reset();
        WriteReg(0x105, 0x01);
        m_Lock.Release();
        return;
    }

    m_Lock.Acquire();

    switch (nStatus & 0xF0)
    {
        case 0x80: NoteOff(nChannel, nData1);                          break;
        case 0x90: NoteOn(nChannel, nData1, nData2);                   break;
        case 0xB0: ControlChange(nChannel, nData1, nData2);            break;
        case 0xC0: ProgramChange(nChannel, nData1);                    break;
        case 0xE0: PitchBend(nChannel, (u16)((nData2 << 7) | nData1)); break;
        default:   break;
    }

    m_Lock.Release();

    CSynthBase::HandleMIDIShortMessage(nMessage);
}

void CYmfmSynth::HandleMIDISysExMessage(const u8* pData, size_t nSize)
{
    // GM reset (F0 7E ?? 09 01 F7)
    if (nSize >= 6 && pData[1] == 0x7E && pData[3] == 0x09 && pData[4] == 0x01)
    {
        m_Lock.Acquire();
        for (unsigned ch = 0; ch < MIDI_CHANNELS; ++ch)
            ResetAllControllers((uint8_t)ch);
        AllSoundOff();
        m_Lock.Release();
    }
}

bool CYmfmSynth::IsActive()
{
    for (unsigned i = 0; i < m_nVoiceCount; ++i)
        if (!m_Voices[i].bFree)
            return true;
    return false;
}

void CYmfmSynth::AllSoundOff()
{
    for (unsigned i = 0; i < m_nVoiceCount; ++i)
    {
        KeyOff(i);
        m_Voices[i].bFree = true;
    }
    CSynthBase::AllSoundOff();
}

const char* CYmfmSynth::GetName() const
{
    return m_eChipMode == TOplChipMode::OPL3 ? "ymfm OPL3" : "ymfm OPL2";
}

void CYmfmSynth::SetChipMode(TOplChipMode eMode)
{
    if (m_eChipMode == eMode)
        return;

    AllSoundOff();
    m_eChipMode   = eMode;
    m_nVoiceCount = (eMode == TOplChipMode::OPL2) ? 9u : OPL3_VOICES;

    m_Chip.reset();
    if (eMode == TOplChipMode::OPL3)
        WriteReg(0x105, 0x01);

    LOGNOTE("Chip mode switched to %s", eMode == TOplChipMode::OPL3 ? "OPL3" : "OPL2");
}

void CYmfmSynth::SetMasterVolume(u8 nVolume)
{
    m_nMasterVolume = nVolume;
}

// ---------------------------------------------------------------------------
// Audio rendering with resampling (chip native ~49.7kHz → 48kHz)
// ---------------------------------------------------------------------------

size_t CYmfmSynth::Render(float* pOutBuffer, size_t nFrames)
{
    m_Lock.Acquire();

    const float fRatio = (float)m_nNativeRate / (float)m_nSampleRate;
    const float fScale = 1.0f / 32768.0f;

    ymfm::ymf262::output_data sample;
    sample.clear();

    for (size_t i = 0; i < nFrames; ++i)
    {
        // Advance resampler position
        m_fResamplePos += fRatio;
        while (m_fResamplePos >= 1.0f)
        {
            m_Chip.generate(&m_LastSample);
            m_fResamplePos -= 1.0f;
        }

        sample = m_LastSample;
        sample.clamp16();

        // OPL3 outputs 4 channels (L0, R0, L1, R1); mix down to stereo
        float fL = ((float)sample.data[0] + (float)sample.data[2]) * fScale * 0.5f;
        float fR = ((float)sample.data[1] + (float)sample.data[3]) * fScale * 0.5f;

        pOutBuffer[i * 2 + 0] = fL;
        pOutBuffer[i * 2 + 1] = fR;
    }

    m_Lock.Release();
    return nFrames;
}

size_t CYmfmSynth::Render(s16* pOutBuffer, size_t nFrames)
{
    m_Lock.Acquire();

    const float fRatio = (float)m_nNativeRate / (float)m_nSampleRate;

    for (size_t i = 0; i < nFrames; ++i)
    {
        m_fResamplePos += fRatio;
        while (m_fResamplePos >= 1.0f)
        {
            m_Chip.generate(&m_LastSample);
            m_fResamplePos -= 1.0f;
        }

        m_LastSample.clamp16();

        // Mix 4 OPL3 outputs to stereo s16
        int32_t nL = ((int32_t)m_LastSample.data[0] + (int32_t)m_LastSample.data[2]) / 2;
        int32_t nR = ((int32_t)m_LastSample.data[1] + (int32_t)m_LastSample.data[3]) / 2;

        pOutBuffer[i * 2 + 0] = (s16)nL;
        pOutBuffer[i * 2 + 1] = (s16)nR;
    }

    m_Lock.Release();
    return nFrames;
}

void CYmfmSynth::ReportStatus() const
{
    if (m_pUI)
        m_pUI->ShowSystemMessage("ymfm OPL3");
}

void CYmfmSynth::UpdateLCD(CLCD& LCD, unsigned int nTicks)
{
    const u8 nBarHeight = LCD.Height();
    float ChannelLevels[16], PeakLevels[16];
    m_MIDIMonitor.GetChannelLevels(nTicks, ChannelLevels, PeakLevels, 1 << PERCUSSION_CHANNEL);
    CUserInterface::DrawChannelLevels(LCD, nBarHeight, ChannelLevels, PeakLevels, 16, true);
}

const char* CYmfmSynth::GetChannelInstrumentName(u8 nChannel)
{
    if (nChannel >= MIDI_CHANNELS)
        return nullptr;
    return m_pInstrumentNames[nChannel];
}
