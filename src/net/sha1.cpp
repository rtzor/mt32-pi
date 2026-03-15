//
// sha1.cpp
//
// Minimal SHA-1 for WebSocket handshake.
//

#include "net/sha1.h"
#include <cstring>

static inline u32 rol32(u32 v, int n)
{
	return (v << n) | (v >> (32 - n));
}

static inline u32 be32(const u8* p)
{
	return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

static inline void put32be(u8* p, u32 v)
{
	p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
}

static void SHA1Block(u32 h[5], const u8 block[64])
{
	u32 w[80];
	for (int i = 0; i < 16; i++)
		w[i] = be32(block + i * 4);
	for (int i = 16; i < 80; i++)
		w[i] = rol32(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);

	u32 a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];

	for (int i = 0; i < 80; i++)
	{
		u32 f, k;
		if      (i < 20) { f = (b & c) | (~b & d);           k = 0x5A827999u; }
		else if (i < 40) { f = b ^ c ^ d;                    k = 0x6ED9EBA1u; }
		else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCu; }
		else             { f = b ^ c ^ d;                    k = 0xCA62C1D6u; }

		u32 temp = rol32(a, 5) + f + e + k + w[i];
		e = d; d = c; c = rol32(b, 30); b = a; a = temp;
	}

	h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
}

void SHA1Digest(const u8* pData, size_t nLen, u8 digest[20])
{
	u32 h[5] = { 0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u };

	u8 block[64];
	size_t processed = 0;

	while (processed + 64 <= nLen)
	{
		SHA1Block(h, pData + processed);
		processed += 64;
	}

	size_t rem = nLen - processed;
	memcpy(block, pData + processed, rem);
	block[rem] = 0x80;
	memset(block + rem + 1, 0, 63 - rem);

	if (rem >= 56)
	{
		SHA1Block(h, block);
		memset(block, 0, 56);
	}

	// Append bit-length as 64-bit big-endian
	u64 bitLen = (u64)nLen * 8;
	block[56] = (u8)(bitLen >> 56); block[57] = (u8)(bitLen >> 48);
	block[58] = (u8)(bitLen >> 40); block[59] = (u8)(bitLen >> 32);
	block[60] = (u8)(bitLen >> 24); block[61] = (u8)(bitLen >> 16);
	block[62] = (u8)(bitLen >> 8);  block[63] = (u8)bitLen;
	SHA1Block(h, block);

	for (int i = 0; i < 5; i++)
		put32be(digest + i * 4, h[i]);
}
