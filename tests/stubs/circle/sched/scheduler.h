//
// circle/sched/scheduler.h (test stub)
//
// Minimal stub for CScheduler, for host-side unit tests.
//

#ifndef _circle_sched_scheduler_stub_h
#define _circle_sched_scheduler_stub_h

class CScheduler
{
public:
	static CScheduler* Get() { return nullptr; }
	void Yield() {}
};

#endif
