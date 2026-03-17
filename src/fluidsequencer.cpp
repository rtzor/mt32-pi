//
// fluidsequencer.cpp
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

#include <circle/logger.h>
#include <circle/timer.h>
#include <fatfs/ff.h>

#include "fluidsequencer.h"

// From fluid_midi.h — manual tick function for baremetal
extern "C" int fluid_player_tick(fluid_player_t *player, unsigned int msec);
// From fluid_midi.c — to detach the sample timer from the synth
extern "C" void fluid_player_remove_timer(fluid_player_t *player, fluid_synth_t *synth);

LOGMODULE("fluidseq");

CFluidSequencer::CFluidSequencer()
:	m_pSynth(nullptr),
	m_pPlayer(nullptr),
	m_bFinished(false),
	m_nLoopCount(1),
	m_nStartTicks(0)
{
	m_szDiag[0] = '\0';
}

CFluidSequencer::~CFluidSequencer()
{
	if (m_pPlayer)
	{
		fluid_player_stop(m_pPlayer);
		delete_fluid_player(m_pPlayer);
	}
}

bool CFluidSequencer::Initialize(fluid_synth_t* pSynth)
{
	if (!pSynth)
		return false;

	m_pSynth = pSynth;
	return true;
}

bool CFluidSequencer::Play(const char* pPath)
{
	if (!m_pSynth || !pPath || !*pPath)
		return false;

	// Stop and destroy any existing player
	if (m_pPlayer)
	{
		fluid_player_stop(m_pPlayer);
		delete_fluid_player(m_pPlayer);
		m_pPlayer = nullptr;
	}

	// Create a fresh player for each file
	snprintf(m_szDiag, sizeof(m_szDiag), "Step1: creating player for %s", pPath);
	m_pPlayer = new_fluid_player(m_pSynth);
	if (!m_pPlayer)
	{
		snprintf(m_szDiag, sizeof(m_szDiag), "FAIL: new_fluid_player returned NULL");
		LOGERR("Failed to create fluid_player");
		return false;
	}

	// Redirect events to our callback instead of directly to the synth.
	// This allows events to flow through the MIDI Router.
	fluid_player_set_playback_callback(m_pPlayer, PlaybackCallback, this);

	// Load the MIDI file into memory on Core 0 (FatFS/EMMC is only safe here),
	// then pass the buffer to fluid_player_add_mem().
	snprintf(m_szDiag, sizeof(m_szDiag), "Step2: f_open(%s)", pPath);
	FIL File;
	FRESULT fr = f_open(&File, pPath, FA_READ);
	if (fr != FR_OK)
	{
		snprintf(m_szDiag, sizeof(m_szDiag), "FAIL: f_open(%s) err=%d", pPath, fr);
		LOGWARN("Failed to open MIDI file: %s (FatFS error %d)", pPath, fr);
		delete_fluid_player(m_pPlayer);
		m_pPlayer = nullptr;
		return false;
	}

	const FSIZE_t nFileSize = f_size(&File);
	snprintf(m_szDiag, sizeof(m_szDiag), "Step3: filesize=%lu", static_cast<unsigned long>(nFileSize));
	if (nFileSize == 0 || nFileSize > 4 * 1024 * 1024) // sanity limit 4 MB
	{
		snprintf(m_szDiag, sizeof(m_szDiag), "FAIL: bad size %lu", static_cast<unsigned long>(nFileSize));
		LOGWARN("MIDI file invalid size: %lu bytes", static_cast<unsigned long>(nFileSize));
		f_close(&File);
		delete_fluid_player(m_pPlayer);
		m_pPlayer = nullptr;
		return false;
	}

	u8* pFileData = new u8[nFileSize];
	if (!pFileData)
	{
		f_close(&File);
		delete_fluid_player(m_pPlayer);
		m_pPlayer = nullptr;
		return false;
	}

	UINT nBytesRead = 0;
	fr = f_read(&File, pFileData, static_cast<UINT>(nFileSize), &nBytesRead);
	f_close(&File);

	if (fr != FR_OK || nBytesRead != nFileSize)
	{
		snprintf(m_szDiag, sizeof(m_szDiag), "FAIL: f_read err=%d read=%u expected=%lu", fr, nBytesRead, static_cast<unsigned long>(nFileSize));
		LOGWARN("Failed to read MIDI file: %s", pPath);
		delete[] pFileData;
		delete_fluid_player(m_pPlayer);
		m_pPlayer = nullptr;
		return false;
	}

	// Verify MIDI header
	snprintf(m_szDiag, sizeof(m_szDiag), "Step4: hdr=%02X%02X%02X%02X sz=%u",
		pFileData[0], pFileData[1], pFileData[2], pFileData[3], nBytesRead);
	LOGNOTE("FluidSeq: file %s, size=%u, hdr=%c%c%c%c",
		pPath, nBytesRead, pFileData[0], pFileData[1], pFileData[2], pFileData[3]);

	if (fluid_player_add_mem(m_pPlayer, pFileData, nBytesRead) != FLUID_OK)
	{
		snprintf(m_szDiag, sizeof(m_szDiag), "FAIL: fluid_player_add_mem");
		LOGWARN("Failed to load MIDI data: %s", pPath);
		delete[] pFileData;
		delete_fluid_player(m_pPlayer);
		m_pPlayer = nullptr;
		return false;
	}

	delete[] pFileData; // fluid_player_add_mem takes a copy

	m_bFinished = false;

	// Set loop mode before starting playback
	// FluidSynth API: -1 = infinite, 1 = play once, 0 = stop immediately
	fluid_player_set_loop(m_pPlayer, m_nLoopCount);

	snprintf(m_szDiag, sizeof(m_szDiag), "Step5: add_mem OK, calling play()");

	// Remove the sample timer that new_fluid_player() registered on the synth.
	// The sample timer runs inside fluid_synth_write_float() on Core 2, but the
	// player was created on Core 0. The linked list insertion is not thread-safe
	// and Core 2 never sees the new timer node (cross-core visibility issue).
	// Instead, we drive the player manually from Core 0 via Tick().
	fluid_player_remove_timer(m_pPlayer, m_pSynth);

	// Start playback — we will drive the player callback manually from Tick()
	if (fluid_player_play(m_pPlayer) != FLUID_OK)
	{
		snprintf(m_szDiag, sizeof(m_szDiag), "FAIL: fluid_player_play");
		LOGERR("Failed to start fluid_player");
		delete_fluid_player(m_pPlayer);
		m_pPlayer = nullptr;
		return false;
	}

	// Record start time for Tick() millisecond computation
	m_nStartTicks = CTimer::GetClockTicks();

	int status = fluid_player_get_status(m_pPlayer);
	snprintf(m_szDiag, sizeof(m_szDiag), "OK: play() status=%d loop=%d sz=%u timer=manual",
		status, m_nLoopCount, nBytesRead);
	LOGNOTE("FluidSequencer: playing %s (status=%d, manual tick)", pPath, status);
	return true;
}

void CFluidSequencer::Stop()
{
	if (m_pPlayer)
	{
		fluid_player_stop(m_pPlayer);

		// Send MIDI panic: All Notes Off + Reset All Controllers on all channels
		u8 panic[6];
		for (unsigned ch = 0; ch < 16; ++ch)
		{
			panic[0] = 0xB0u | ch; panic[1] = 0x7Bu; panic[2] = 0x00u; // All Notes Off
			panic[3] = 0xB0u | ch; panic[4] = 0x79u; panic[5] = 0x00u; // Reset All Controllers
			m_MIDIOutBuffer.Enqueue(panic, sizeof(panic));
		}
	}
	m_bFinished = false;
}

bool CFluidSequencer::Seek(int nTicks)
{
	if (!m_pPlayer)
		return false;

	// Send MIDI panic before seeking to kill any stuck notes
	u8 panic[6];
	for (unsigned ch = 0; ch < 16; ++ch)
	{
		panic[0] = 0xB0u | ch; panic[1] = 0x7Bu; panic[2] = 0x00u; // All Notes Off
		panic[3] = 0xB0u | ch; panic[4] = 0x79u; panic[5] = 0x00u; // Reset All Controllers
		m_MIDIOutBuffer.Enqueue(panic, sizeof(panic));
	}

	return fluid_player_seek(m_pPlayer, nTicks) == FLUID_OK;
}

void CFluidSequencer::SetLoop(int nLoop)
{
	m_nLoopCount = nLoop;
	if (m_pPlayer)
		fluid_player_set_loop(m_pPlayer, nLoop);
}

bool CFluidSequencer::SetTempoMultiplier(double nMultiplier)
{
	if (!m_pPlayer)
		return false;
	return fluid_player_set_tempo(m_pPlayer, FLUID_PLAYER_TEMPO_INTERNAL, nMultiplier) == FLUID_OK;
}

bool CFluidSequencer::SetTempoBPM(double nBPM)
{
	if (!m_pPlayer)
		return false;
	return fluid_player_set_tempo(m_pPlayer, FLUID_PLAYER_TEMPO_EXTERNAL_BPM, nBPM) == FLUID_OK;
}

bool CFluidSequencer::IsPlaying() const
{
	if (!m_pPlayer)
		return false;
	return fluid_player_get_status(m_pPlayer) == FLUID_PLAYER_PLAYING;
}

bool CFluidSequencer::IsFinished() const
{
	if (!m_pPlayer)
		return false;
	// Check directly from player status — more reliable than callback flag
	return fluid_player_get_status(m_pPlayer) == FLUID_PLAYER_DONE;
}

int CFluidSequencer::GetCurrentTick() const
{
	return m_pPlayer ? fluid_player_get_current_tick(m_pPlayer) : 0;
}

int CFluidSequencer::GetTotalTicks() const
{
	return m_pPlayer ? fluid_player_get_total_ticks(m_pPlayer) : 0;
}

int CFluidSequencer::GetBPM() const
{
	return m_pPlayer ? fluid_player_get_bpm(m_pPlayer) : 120;
}

int CFluidSequencer::GetDivision() const
{
	return m_pPlayer ? fluid_player_get_division(m_pPlayer) : 0;
}

int CFluidSequencer::GetMidiTempo() const
{
	return m_pPlayer ? fluid_player_get_midi_tempo(m_pPlayer) : 500000;
}

size_t CFluidSequencer::DrainMIDIBytes(u8* pOutBuffer, size_t nMaxBytes)
{
	return m_MIDIOutBuffer.Dequeue(pOutBuffer, nMaxBytes);
}

void CFluidSequencer::Tick()
{
	if (!m_pPlayer || fluid_player_get_status(m_pPlayer) != FLUID_PLAYER_PLAYING)
		return;

	// Compute elapsed milliseconds since playback started
	const unsigned nNow = CTimer::GetClockTicks();
	const unsigned nElapsed = nNow - m_nStartTicks;
	const unsigned nMsec = nElapsed / (CLOCKHZ / 1000);

	// Drive the player callback manually from Core 0
	fluid_player_tick(m_pPlayer, nMsec);
}

// Static callback invoked by fluid_player_tick() on Core 0.
// Converts fluid_midi_event_t to raw MIDI bytes and enqueues into the ring buffer.
int CFluidSequencer::PlaybackCallback(void* pData, fluid_midi_event_t* pEvent)
{
	auto* pSelf = static_cast<CFluidSequencer*>(pData);
	if (!pSelf || !pEvent)
		return FLUID_OK;

	const int nType    = fluid_midi_event_get_type(pEvent);
	const int nChannel = fluid_midi_event_get_channel(pEvent);

	u8 msg[3];
	size_t nLen = 0;

	switch (nType)
	{
		case 0x80: // Note Off
		case 0x90: // Note On
		case 0xA0: // Poly Aftertouch
			msg[0] = static_cast<u8>(nType | nChannel);
			msg[1] = static_cast<u8>(fluid_midi_event_get_key(pEvent));
			msg[2] = static_cast<u8>(fluid_midi_event_get_velocity(pEvent));
			nLen = 3;
			break;

		case 0xB0: // Control Change
			msg[0] = static_cast<u8>(nType | nChannel);
			msg[1] = static_cast<u8>(fluid_midi_event_get_control(pEvent));
			msg[2] = static_cast<u8>(fluid_midi_event_get_value(pEvent));
			nLen = 3;
			break;

		case 0xC0: // Program Change
			msg[0] = static_cast<u8>(nType | nChannel);
			msg[1] = static_cast<u8>(fluid_midi_event_get_program(pEvent));
			nLen = 2;
			break;

		case 0xD0: // Channel Pressure
			msg[0] = static_cast<u8>(nType | nChannel);
			msg[1] = static_cast<u8>(fluid_midi_event_get_value(pEvent));
			nLen = 2;
			break;

		case 0xE0: // Pitch Bend
			{
				const int nPitch = fluid_midi_event_get_pitch(pEvent);
				msg[0] = static_cast<u8>(nType | nChannel);
				msg[1] = static_cast<u8>(nPitch & 0x7F);
				msg[2] = static_cast<u8>((nPitch >> 7) & 0x7F);
				nLen = 3;
			}
			break;

		case 0xF0: // SysEx
			{
				void* pSysExData = nullptr;
				int nSysExSize = 0;
				// fluid_midi_event_get_text is the generic getter for extended data
				// For SysEx events, the data starts with F0 and ends with F7
				fluid_midi_event_get_text(pEvent, &pSysExData, &nSysExSize);
				if (pSysExData && nSysExSize > 0)
				{
					// Enqueue the SysEx data directly (it includes F0...F7)
					pSelf->m_MIDIOutBuffer.Enqueue(static_cast<const u8*>(pSysExData),
					                               static_cast<size_t>(nSysExSize));
				}
				return FLUID_OK; // already enqueued
			}

		default:
			// Meta events, etc. — ignore
			return FLUID_OK;
	}

	if (nLen > 0)
		pSelf->m_MIDIOutBuffer.Enqueue(msg, nLen);

	return FLUID_OK;
}
