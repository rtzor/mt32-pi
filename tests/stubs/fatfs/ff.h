//
// fatfs/ff.h (test stub)
//
// Minimal stub for FatFS <fatfs/ff.h>, for host-side unit tests.
//

#ifndef _fatfs_ff_stub_h
#define _fatfs_ff_stub_h

#include <stddef.h>

typedef int      FRESULT;
typedef unsigned UINT;
typedef unsigned long long FSIZE_t;

#define FR_OK     0
#define FR_NO_FILE 4

#define FA_READ          0x01
#define FA_WRITE         0x02
#define FA_CREATE_ALWAYS 0x08

typedef struct { char _dummy; } FIL;
typedef struct { char _dummy; } DIR;
typedef struct { FSIZE_t fsize; char fname[256]; } FILINFO;

#ifdef __cplusplus
extern "C" {
#endif

FRESULT f_open (FIL* fp, const char* path, int mode);
FRESULT f_close(FIL* fp);
FRESULT f_read (FIL* fp, void* buf, UINT btr, UINT* br);
FSIZE_t f_size (FIL* fp);
FRESULT f_stat (const char* path, FILINFO* fno);

#ifdef __cplusplus
}

// Test control globals (C++ only)
extern bool                 g_fatfs_open_fail;
extern bool                 g_fatfs_read_fail;
extern const unsigned char* g_fatfs_data;
extern size_t               g_fatfs_data_size;
#endif

#endif // _fatfs_ff_stub_h
