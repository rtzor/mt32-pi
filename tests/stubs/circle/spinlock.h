//
// circle/spinlock.h (test stub)
//
// Defines IRQ_LEVEL and re-exports CSpinLock from circle_types.h.
//

#ifndef _circle_spinlock_stub_h
#define _circle_spinlock_stub_h

#include "../circle_types.h"

// IRQ_LEVEL is the level passed to CSpinLock when interrupts must be gated.
// In tests CSpinLock is a no-op, so the value doesn't matter.
#define IRQ_LEVEL 1

#endif
