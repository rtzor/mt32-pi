//
// fluidsynth.h (test stub)
//
// Minimal stub for <fluidsynth.h>, for host-side unit tests of CFluidSequencer.
// Exposes concrete types, constants, and a test-control API (C++ only).
//

#ifndef _fluidsynth_stub_h
#define _fluidsynth_stub_h

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Concrete struct definitions (opaque in real FluidSynth, concrete here so
// tests can stack-allocate them).
// ---------------------------------------------------------------------------
typedef struct _fluid_synth_t  { int _dummy; } fluid_synth_t;
typedef struct _fluid_player_t { int _dummy; } fluid_player_t;

typedef struct _fluid_midi_event_t {
	int   type;
	int   channel;
	int   key;
	int   velocity;
	int   control;
	int   value;       // CC value or channel pressure
	int   program;
	int   pitch;
	void* sysex_data;
	int   sysex_size;
} fluid_midi_event_t;

// ---------------------------------------------------------------------------
// Return codes
// ---------------------------------------------------------------------------
#define FLUID_OK     0
#define FLUID_FAILED (-1)

// ---------------------------------------------------------------------------
// Player status values
// ---------------------------------------------------------------------------
#define FLUID_PLAYER_READY   0
#define FLUID_PLAYER_PLAYING 1
#define FLUID_PLAYER_DONE    2

// ---------------------------------------------------------------------------
// Tempo type selectors
// ---------------------------------------------------------------------------
#define FLUID_PLAYER_TEMPO_INTERNAL      0
#define FLUID_PLAYER_TEMPO_EXTERNAL_BPM  1
#define FLUID_PLAYER_TEMPO_EXTERNAL_MIDI 2

// ---------------------------------------------------------------------------
// Player API declarations
// ---------------------------------------------------------------------------
fluid_player_t* new_fluid_player(fluid_synth_t* synth);
int delete_fluid_player(fluid_player_t* player);

int fluid_player_set_playback_callback(fluid_player_t* player,
	int (*handler)(void*, fluid_midi_event_t*), void* handler_data);
int fluid_player_add_mem(fluid_player_t* player, const void* buffer, size_t len);
int fluid_player_set_loop(fluid_player_t* player, int loop);
int fluid_player_play(fluid_player_t* player);
int fluid_player_stop(fluid_player_t* player);
int fluid_player_seek(fluid_player_t* player, int ticks);
int fluid_player_set_tempo(fluid_player_t* player, int tempo_type, double tempo);
int fluid_player_get_status(fluid_player_t* player);
int fluid_player_get_current_tick(fluid_player_t* player);
int fluid_player_get_total_ticks(fluid_player_t* player);
int fluid_player_get_bpm(fluid_player_t* player);
int fluid_player_get_midi_tempo(fluid_player_t* player);
int fluid_player_get_division(fluid_player_t* player);

// Internal FluidSynth functions declared manually in fluidsequencer.cpp
void fluid_player_remove_timer(fluid_player_t* player, fluid_synth_t* synth);
int  fluid_player_tick(fluid_player_t* player, unsigned int msec);

// MIDI event field accessors
int fluid_midi_event_get_type    (fluid_midi_event_t* evt);
int fluid_midi_event_get_channel (fluid_midi_event_t* evt);
int fluid_midi_event_get_key     (fluid_midi_event_t* evt);
int fluid_midi_event_get_velocity(fluid_midi_event_t* evt);
int fluid_midi_event_get_control (fluid_midi_event_t* evt);
int fluid_midi_event_get_value   (fluid_midi_event_t* evt);
int fluid_midi_event_get_program (fluid_midi_event_t* evt);
int fluid_midi_event_get_pitch   (fluid_midi_event_t* evt);
int fluid_midi_event_get_text    (fluid_midi_event_t* evt, void** data, int* size);

#ifdef __cplusplus
} // extern "C"

// ---------------------------------------------------------------------------
// Test control API (C++ only)
// ---------------------------------------------------------------------------

// Reset all stub state to safe defaults.  Call at the start of each test.
void FluidStub_Reset();

// Invoke the playback callback that was registered via
// fluid_player_set_playback_callback().  Returns FLUID_OK if none registered.
int FluidStub_InvokeCallback(fluid_midi_event_t* pEvent);

// ---------------------------------------------------------------------------
// Controllable globals — tests set these to inject failures or specific values
// ---------------------------------------------------------------------------
extern bool g_fluid_new_player_fail;  // new_fluid_player returns nullptr
extern bool g_fluid_add_mem_fail;     // fluid_player_add_mem returns FLUID_FAILED
extern bool g_fluid_play_fail;        // fluid_player_play returns FLUID_FAILED
extern bool g_fluid_seek_fail;        // fluid_player_seek returns FLUID_FAILED
extern int  g_fluid_player_status;    // returned by fluid_player_get_status
extern int  g_fluid_current_tick;
extern int  g_fluid_total_ticks;
extern int  g_fluid_bpm;
extern int  g_fluid_division;
extern int  g_fluid_midi_tempo;

// Stub timer ticks (CTimer::GetClockTicks) — advance to simulate elapsed time
namespace StubTimer { extern unsigned s_clock_ticks; }

#endif // __cplusplus

#endif // _fluidsynth_stub_h
