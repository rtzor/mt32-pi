//
// circle_types.h
//
// Minimal stubs for Circle OS types, allowing mt32-pi classes
// to compile natively on the host for unit testing.
//

#ifndef _circle_types_stub_h
#define _circle_types_stub_h

#include <cstdint>
#include <cstddef>

using u8  = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using s16 = int16_t;
using s32 = int32_t;

#define TASK_LEVEL 0

class CSpinLock
{
public:
	CSpinLock(unsigned = 0) {}
	void Acquire() {}
	void Release() {}
};

#endif
