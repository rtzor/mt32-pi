//
// circle/sched/task.h (test stub)
//
// Minimal stub for CTask, for host-side unit tests.
// Only needs to compile — no runtime behaviour required.
//

#ifndef _circle_sched_task_stub_h
#define _circle_sched_task_stub_h

#ifndef TASK_STACK_SIZE
#define TASK_STACK_SIZE 8192
#endif

class CTask
{
public:
	CTask(unsigned /*nStackSize*/, bool /*bSuspended*/ = false) {}
	virtual ~CTask() {}
	void Start() {}
	virtual void Run() = 0;
};

#endif
