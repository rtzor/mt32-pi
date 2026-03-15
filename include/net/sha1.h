//
// sha1.h
//
// Minimal SHA-1 implementation for WebSocket handshake (RFC 3174 / RFC 6455)
//

#ifndef _net_sha1_h
#define _net_sha1_h

#include <circle/types.h>
#include <stddef.h>

// Compute SHA-1 digest of pData[0..nLen-1].
// digest must be at least 20 bytes.
void SHA1Digest(const u8* pData, size_t nLen, u8 digest[20]);

#endif
