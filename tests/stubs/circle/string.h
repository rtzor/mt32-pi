//
// circle/string.h (test stub)
//
// Minimal CString stub.  The real CString is not used by the code under test;
// this file exists only to satisfy the include chain from utility.h.
//

#ifndef _circle_string_stub_h
#define _circle_string_stub_h

#include <cstdarg>
#include <cstdio>
#include <cstring>

class CString
{
public:
	CString()                 { m_buf[0] = '\0'; }
	CString(const char* psz)  { std::strncpy(m_buf, psz ? psz : "", sizeof(m_buf) - 1); m_buf[sizeof(m_buf)-1] = '\0'; }
	void Format(const char* pFmt, ...) { va_list va; va_start(va, pFmt); std::vsnprintf(m_buf, sizeof(m_buf), pFmt, va); va_end(va); }
	unsigned GetLength() const { return static_cast<unsigned>(std::strlen(m_buf)); }
	operator const char*() const { return m_buf; }
private:
	char m_buf[256];
};

#endif
