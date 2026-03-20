//
// circle/timer.h (test stub)
//
// Provides CTimer::GetClockTicks() and CLOCKHZ for host-side unit tests.
//

#ifndef _circle_timer_stub_h
#define _circle_timer_stub_h

// Circle uses a 1 MHz free-running counter.
#define CLOCKHZ 1000000

namespace StubTimer { extern unsigned s_clock_ticks; }

class CTimer
{
public:
	static unsigned GetClockTicks() { return StubTimer::s_clock_ticks; }
};

#endif
