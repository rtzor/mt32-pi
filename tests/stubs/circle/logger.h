//
// circle/logger.h
//
// Minimal stub for Circle's logging macros, for host-side unit tests.
// All macros expand to nothing.
//

#ifndef _circle_logger_stub_h
#define _circle_logger_stub_h

#include <cassert>  // midiparser.cpp uses assert()

#define LOGMODULE(n)   static_assert(true, "")
#define LOGDBG(...)    do {} while (0)
#define LOGNOTE(...)   do {} while (0)
#define LOGWARN(...)   do {} while (0)
#define LOGERR(...)    do {} while (0)
#define LOGPANIC(...)  do {} while (0)

#endif
