//
// fluidsynth_stub.cpp
//
// Controllable stub implementations for the FluidSynth player API and CTimer.
// Compiled into the host-side test binary alongside the real sequencer code.
//

#include <cstring>
#include "fluidsynth.h"

// ---------------------------------------------------------------------------
// StubTimer — shared clock for CTimer::GetClockTicks()
// ---------------------------------------------------------------------------
namespace StubTimer { unsigned s_clock_ticks = 0; }

// ---------------------------------------------------------------------------
// Stub player instance
// ---------------------------------------------------------------------------
static fluid_player_t s_player;

// Stored playback callback (registered by fluid_player_set_playback_callback)
static int (*s_cb)(void*, fluid_midi_event_t*) = nullptr;
static void* s_cb_data = nullptr;

// ---------------------------------------------------------------------------
// Controllable state globals
// ---------------------------------------------------------------------------
bool g_fluid_new_player_fail = false;
bool g_fluid_add_mem_fail    = false;
bool g_fluid_play_fail       = false;
bool g_fluid_seek_fail       = false;
int  g_fluid_player_status   = FLUID_PLAYER_READY;
int  g_fluid_current_tick    = 0;
int  g_fluid_total_ticks     = 1000;
int  g_fluid_bpm             = 120;
int  g_fluid_division        = 480;
int  g_fluid_midi_tempo      = 500000;

// ---------------------------------------------------------------------------
// Test helpers
// ---------------------------------------------------------------------------
void FluidStub_Reset()
{
	s_cb      = nullptr;
	s_cb_data = nullptr;

	g_fluid_new_player_fail = false;
	g_fluid_add_mem_fail    = false;
	g_fluid_play_fail       = false;
	g_fluid_seek_fail       = false;
	g_fluid_player_status   = FLUID_PLAYER_READY;
	g_fluid_current_tick    = 0;
	g_fluid_total_ticks     = 1000;
	g_fluid_bpm             = 120;
	g_fluid_division        = 480;
	g_fluid_midi_tempo      = 500000;
	StubTimer::s_clock_ticks = 0;
}

int FluidStub_InvokeCallback(fluid_midi_event_t* pEvent)
{
	if (!s_cb || !pEvent)
		return FLUID_OK;
	return s_cb(s_cb_data, pEvent);
}

// ---------------------------------------------------------------------------
// Player API stub implementations
// ---------------------------------------------------------------------------
extern "C" {

fluid_player_t* new_fluid_player(fluid_synth_t* /*synth*/)
{
	if (g_fluid_new_player_fail)
		return nullptr;
	std::memset(&s_player, 0, sizeof(s_player));
	return &s_player;
}

int delete_fluid_player(fluid_player_t* /*player*/)
{
	s_cb      = nullptr;
	s_cb_data = nullptr;
	return FLUID_OK;
}

int fluid_player_set_playback_callback(fluid_player_t* /*player*/,
	int (*handler)(void*, fluid_midi_event_t*), void* handler_data)
{
	s_cb      = handler;
	s_cb_data = handler_data;
	return FLUID_OK;
}

int fluid_player_add_mem(fluid_player_t* /*player*/,
	const void* /*buf*/, size_t /*len*/)
{
	return g_fluid_add_mem_fail ? FLUID_FAILED : FLUID_OK;
}

int fluid_player_set_loop(fluid_player_t* /*player*/, int /*loop*/)
{
	return FLUID_OK;
}

int fluid_player_play(fluid_player_t* /*player*/)
{
	if (g_fluid_play_fail)
		return FLUID_FAILED;
	g_fluid_player_status = FLUID_PLAYER_PLAYING;
	return FLUID_OK;
}

int fluid_player_stop(fluid_player_t* /*player*/)
{
	g_fluid_player_status = FLUID_PLAYER_READY;
	return FLUID_OK;
}

int fluid_player_seek(fluid_player_t* /*player*/, int ticks)
{
	if (g_fluid_seek_fail)
		return FLUID_FAILED;
	g_fluid_current_tick = ticks;
	return FLUID_OK;
}

int fluid_player_set_tempo(fluid_player_t* /*player*/,
	int /*tempo_type*/, double /*tempo*/)
{
	return FLUID_OK;
}

int fluid_player_get_status      (fluid_player_t* /*p*/) { return g_fluid_player_status; }
int fluid_player_get_current_tick(fluid_player_t* /*p*/) { return g_fluid_current_tick;  }
int fluid_player_get_total_ticks (fluid_player_t* /*p*/) { return g_fluid_total_ticks;   }
int fluid_player_get_bpm         (fluid_player_t* /*p*/) { return g_fluid_bpm;           }
int fluid_player_get_midi_tempo  (fluid_player_t* /*p*/) { return g_fluid_midi_tempo;    }
int fluid_player_get_division    (fluid_player_t* /*p*/) { return g_fluid_division;      }

void fluid_player_remove_timer(fluid_player_t* /*p*/, fluid_synth_t* /*s*/) {}
int  fluid_player_tick(fluid_player_t* /*p*/, unsigned int /*msec*/) { return FLUID_OK; }

// MIDI event field accessors
int fluid_midi_event_get_type    (fluid_midi_event_t* e) { return e->type;      }
int fluid_midi_event_get_channel (fluid_midi_event_t* e) { return e->channel;   }
int fluid_midi_event_get_key     (fluid_midi_event_t* e) { return e->key;       }
int fluid_midi_event_get_velocity(fluid_midi_event_t* e) { return e->velocity;  }
int fluid_midi_event_get_control (fluid_midi_event_t* e) { return e->control;   }
int fluid_midi_event_get_value   (fluid_midi_event_t* e) { return e->value;     }
int fluid_midi_event_get_program (fluid_midi_event_t* e) { return e->program;   }
int fluid_midi_event_get_pitch   (fluid_midi_event_t* e) { return e->pitch;     }

int fluid_midi_event_get_text(fluid_midi_event_t* e, void** data, int* size)
{
	if (data) *data = e->sysex_data;
	if (size) *size = e->sysex_size;
	return FLUID_OK;
}

} // extern "C"
