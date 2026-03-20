//
// fatfs_stub.cpp
//
// Controllable stub implementations for FatFS functions.
//

#include <cstring>
#include <algorithm>
#include "fatfs/ff.h"

// ---------------------------------------------------------------------------
// Test control globals
// ---------------------------------------------------------------------------
bool                 g_fatfs_open_fail  = false;
bool                 g_fatfs_read_fail  = false;
const unsigned char* g_fatfs_data       = nullptr;
size_t               g_fatfs_data_size  = 0;

// ---------------------------------------------------------------------------
// FatFS stub implementations
// ---------------------------------------------------------------------------
FRESULT f_open(FIL* /*fp*/, const char* /*path*/, int /*mode*/)
{
	return g_fatfs_open_fail ? 1 : FR_OK;
}

FRESULT f_close(FIL* /*fp*/)
{
	return FR_OK;
}

FSIZE_t f_size(FIL* /*fp*/)
{
	return static_cast<FSIZE_t>(g_fatfs_data_size);
}

FRESULT f_read(FIL* /*fp*/, void* buf, UINT btr, UINT* br)
{
	if (g_fatfs_read_fail) { *br = 0; return 1; }
	UINT n = static_cast<UINT>(std::min(static_cast<size_t>(btr), g_fatfs_data_size));
	if (g_fatfs_data && n > 0)
		std::memcpy(buf, g_fatfs_data, n);
	*br = n;
	return FR_OK;
}

FRESULT f_stat(const char* /*path*/, FILINFO* fno)
{
	if (fno)
		fno->fsize = static_cast<FSIZE_t>(g_fatfs_data_size);
	return FR_OK;
}
