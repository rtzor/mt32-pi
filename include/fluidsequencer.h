//
// fluidsequencer.h
//
// mt32-pi - A baremetal MIDI synthesizer for Raspberry Pi
// Copyright (C) 2020-2023 Dale Whinham <daleyo@gmail.com>
//
// Advanced MIDI file player using FluidSynth's fluid_player API.
// Provides seek, tempo control, SysEx support, and unlimited events.
// Events are redirected via playback callback into a ring buffer
// so they flow through the MIDI Router (reaching MT-32, FluidSynth, or both).
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

#ifndef _fluidsequencer_h
#define _fluidsequencer_h

#include <circle/types.h>
#include <fluidsynth.h>

#include "ringbuffer.h"

class CFluidSequencer
{
public:
	CFluidSequencer();
	~CFluidSequencer();

	// Must be called once with a valid fluid_synth_t* (from CSoundFontSynth).
	// Returns false if player creation fails.
	bool Initialize(fluid_synth_t* pSynth);

	// Load and start a MIDI file. Stops any previous playback first.
	bool Play(const char* pPath);

	// Stop playback and send MIDI panic.
	void Stop();

	// Seek to a position in ticks.
	bool Seek(int nTicks);

	// Set loop mode: -1 = infinite, 0 = no loop, N = play N times.
	void SetLoop(int nLoop);

	// Tempo control.
	// nMultiplier: 1.0 = normal, 2.0 = double speed, 0.5 = half speed.
	bool SetTempoMultiplier(double nMultiplier);
	// nBPM: override tempo to specific BPM.
	bool SetTempoBPM(double nBPM);

	// Query state.
	bool IsPlaying() const;
	bool IsFinished() const;
	int  GetCurrentTick() const;
	int  GetTotalTicks() const;
	int  GetBPM() const;
	int  GetDivision() const;
	int  GetMidiTempo() const;

	// Drain MIDI bytes produced by the playback callback.
	// Called from Core 0's UpdateMIDI() to collect events for the MIDI Router.
	// Returns number of bytes written to pOutBuffer.
	size_t DrainMIDIBytes(u8* pOutBuffer, size_t nMaxBytes);

	// Advance the player by calling its callback with current system time.
	// Must be called frequently from Core 0 (e.g., from UpdateMIDI).
	void Tick();

	// Diagnostic string for web-visible debugging
	const char* GetDiag() const { return m_szDiag; }

private:
	// Static callback for fluid_player_set_playback_callback.
	// Converts fluid_midi_event_t into raw MIDI bytes and enqueues them
	// into the ring buffer for Core 0 consumption.
	static int PlaybackCallback(void* pData, fluid_midi_event_t* pEvent);

	fluid_synth_t*  m_pSynth;    // borrowed, not owned
	fluid_player_t* m_pPlayer;
	bool            m_bFinished;
	int             m_nLoopCount; // -1 = infinite, 1 = play once (default)
	unsigned        m_nStartTicks; // system clock ticks when play started

	// Diagnostic buffer for web-visible debugging
	char m_szDiag[256];

	// Ring buffer: written from PlaybackCallback (called from Core 0 Tick()),
	// read from Core 0 (UpdateMIDI drain loop).
	CRingBuffer<u8, 4096> m_MIDIOutBuffer;
};

#endif
