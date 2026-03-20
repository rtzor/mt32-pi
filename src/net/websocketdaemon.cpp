//
// websocketdaemon.cpp
//
// WebSocket server for mt32-pi.
// - Listens on port 8765 (separate from HTTP)
// - Handles the RFC 6455 upgrade handshake
// - Pushes sequencer status JSON every ~250ms
// - Accepts incoming note-on/note-off JSON from the browser
//

#include <circle/logger.h>
#include <circle/net/in.h>
#include <circle/net/netsubsystem.h>
#include <circle/net/socket.h>
#include <circle/sched/scheduler.h>
#include <circle/serial.h>
#include <circle/timer.h>

#include <cstring>
#include <cstdlib>
#include <cstdio>

#include "mt32pi.h"
#include "net/sha1.h"
#include "net/websocketdaemon.h"

LOGMODULE("wsd");

// ──────────────────────────────────────────────────────────────────────────
// Base64 encode — only what we need for the WS handshake accept key
// ──────────────────────────────────────────────────────────────────────────
static const char kB64[] =
	"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// out must hold at least ((nLen + 2) / 3) * 4 + 1 bytes
static void Base64Encode(const u8* pIn, size_t nLen, char* pOut)
{
	size_t i = 0, j = 0;
	while (i < nLen)
	{
		size_t read = 0;
		u32 b = (u32)pIn[i++] << 16; read++;
		if (i < nLen) { b |= (u32)pIn[i++] << 8; read++; }
		if (i < nLen) { b |= pIn[i++];            read++; }
		pOut[j++] = kB64[(b >> 18) & 0x3F];
		pOut[j++] = kB64[(b >> 12) & 0x3F];
		pOut[j++] = (read < 2) ? '=' : kB64[(b >> 6) & 0x3F];
		pOut[j++] = (read < 3) ? '=' : kB64[b & 0x3F];
	}
	pOut[j] = '\0';
}

// ──────────────────────────────────────────────────────────────────────────
// WebSocket frame builder (server→client, unmasked)
// ──────────────────────────────────────────────────────────────────────────
static int BuildFrame(u8* pBuf, size_t nBufSize, u8 opcode, const u8* pPayload, size_t nPayLen)
{
	if (nPayLen <= 125)
	{
		if (nBufSize < 2 + nPayLen) return -1;
		pBuf[0] = 0x80 | opcode;
		pBuf[1] = (u8)nPayLen;
		memcpy(pBuf + 2, pPayload, nPayLen);
		return (int)(2 + nPayLen);
	}
	else if (nPayLen <= 65535)
	{
		if (nBufSize < 4 + nPayLen) return -1;
		pBuf[0] = 0x80 | opcode;
		pBuf[1] = 126;
		pBuf[2] = (nPayLen >> 8) & 0xFF;
		pBuf[3] = nPayLen & 0xFF;
		memcpy(pBuf + 4, pPayload, nPayLen);
		return (int)(4 + nPayLen);
	}
	return -1;
}

// ──────────────────────────────────────────────────────────────────────────
// Parse one WebSocket client frame (masked, from browser).
// Returns payload length, or -1 on error / incomplete.
// Sets *pOpcode. Demasked payload written into pPayload[0..payloadLen-1].
// ──────────────────────────────────────────────────────────────────────────
static int ParseClientFrame(const u8* pBuf, size_t nLen, u8* pOpcode, u8* pPayload, size_t nPayBufSize)
{
	if (nLen < 2) return -1;
	*pOpcode = pBuf[0] & 0x0F;
	bool masked = (pBuf[1] & 0x80) != 0;
	size_t payLen = pBuf[1] & 0x7F;
	size_t hdrLen = 2;

	if (payLen == 126)
	{
		if (nLen < 4) return -1;
		payLen = ((size_t)pBuf[2] << 8) | pBuf[3];
		hdrLen = 4;
	}
	else if (payLen == 127)
	{
		return -1; // too large, not supported
	}

	if (masked) hdrLen += 4;
	if (nLen < hdrLen + payLen) return -1;
	if (payLen > nPayBufSize) return -1;

	if (masked)
	{
		const u8* mask = pBuf + hdrLen - 4;
		for (size_t i = 0; i < payLen; i++)
			pPayload[i] = pBuf[hdrLen + i] ^ mask[i & 3];
	}
	else
	{
		memcpy(pPayload, pBuf + hdrLen, payLen);
	}
	return (int)payLen;
}

// ──────────────────────────────────────────────────────────────────────────
// Build combined status JSON (sequencer + MIDI levels)
// ──────────────────────────────────────────────────────────────────────────
static int BuildStatusJSON(char* buf, size_t bufSize, CMT32Pi* pPi)
{
	const CMT32Pi::TSystemState st = pPi->GetSystemState();

	u8 activeNotes[16][128];
	pPi->GetActiveNotes(activeNotes);

	int n = snprintf(buf, bufSize,
		"{\"playing\":%s,\"loading\":%s,\"finished\":%s,\"loop_enabled\":%s,"
		"\"file\":\"%s\",\"duration_ms\":%u,\"elapsed_ms\":%u,"
		"\"bpm\":%d,\"current_tick\":%d,\"total_ticks\":%d,"
		"\"division\":%d,\"tempo_multiplier\":%.3f,\"file_size_kb\":%u,"
		"\"synth\":\"%s\",\"mixer\":%s,\"preset\":%d,\"channels\":[",
		st.Sequencer.bPlaying     ? "true" : "false",
		st.Sequencer.bLoading     ? "true" : "false",
		st.Sequencer.bFinished    ? "true" : "false",
		st.Sequencer.bLoopEnabled ? "true" : "false",
		st.Sequencer.pFile ? st.Sequencer.pFile : "",
		st.Sequencer.nDurationMs, st.Sequencer.nElapsedMs,
		st.Sequencer.nBPM, st.Sequencer.nCurrentTick, st.Sequencer.nTotalTicks,
		st.Sequencer.nDivision, (float)st.Sequencer.nTempoMultiplier, st.Sequencer.nFileSizeKB,
		st.pActiveSynthName,
		st.Mixer.bEnabled ? "true" : "false",
		st.Mixer.nPreset);

	if (n <= 0 || (size_t)n >= bufSize) return -1;

	// eng: 0=MT-32, 1=FluidSynth (derived from engine name string)
	for (int i = 0; i < 16; i++)
	{
		const char* pEng = st.Mixer.pChannelEngine[i];
		int nEng = (pEng && pEng[0] == 'F') ? 1 : 0;  // "FluidSynth" vs "MT-32"
		int added = snprintf(buf + n, bufSize - n,
			"%s{\"ch\":%d,\"lv\":%.3f,\"pk\":%.3f,\"eng\":%d}",
			i > 0 ? "," : "", i,
			(double)st.MIDILevels[i], (double)st.MIDIPeaks[i], nEng);
		if (added <= 0 || (size_t)(n + added) >= bufSize) return -1;
		n += added;
	}

	// notes: array of 16 subarrays, each holds active note numbers for that channel
	// src:   dominant source per channel (0=none,1=physical,2=player,3=webui)
	{
		int added = snprintf(buf + n, bufSize - n, "],\"notes\":[");
		if (added <= 0 || (size_t)(n + added) >= bufSize) return -1;
		n += added;

		for (int ch = 0; ch < 16; ch++)
		{
			added = snprintf(buf + n, bufSize - n, "%s[", ch > 0 ? "," : "");
			if (added <= 0 || (size_t)(n + added) >= bufSize) return -1;
			n += added;

			bool first = true;
			for (int note = 0; note < 128; note++)
			{
				if (activeNotes[ch][note] != 0)
				{
					added = snprintf(buf + n, bufSize - n, "%s%d", first ? "" : ",", note);
					if (added <= 0 || (size_t)(n + added) >= bufSize) return -1;
					n += added;
					first = false;
				}
			}

			if ((size_t)n + 1 >= bufSize) return -1;
			buf[n++] = ']';
		}
	}

	// src: dominant source per channel (highest-priority non-zero)
	{
		int added = snprintf(buf + n, bufSize - n, "],\"src\":[");
		if (added <= 0 || (size_t)(n + added) >= bufSize) return -1;
		n += added;

		for (int ch = 0; ch < 16; ch++)
		{
			u8 src = 0;
			for (int note = 0; note < 128 && src == 0; note++)
				src = activeNotes[ch][note];

			added = snprintf(buf + n, bufSize - n, "%s%d", ch > 0 ? "," : "", (int)src);
			if (added <= 0 || (size_t)(n + added) >= bufSize) return -1;
			n += added;
		}
	}

	// Audio render performance + recorder state
	{
		int added = snprintf(buf + n, bufSize - n,
			"],\"render_us\":%u,\"render_avg_us\":%u,\"deadline_us\":%u,\"cpu_load\":%u"
			",\"mt32_render_us\":%u,\"fluid_render_us\":%u,\"mixer_render_us\":%u"
			",\"mt32_cpu\":%u,\"fluid_cpu\":%u,\"mixer_cpu\":%u"
			",\"recording\":%s}",
			st.Mixer.nRenderUs, st.Mixer.nRenderAvgUs, st.Mixer.nDeadlineUs, st.Mixer.nCpuLoadPercent,
			st.Mixer.nMT32RenderUs, st.Mixer.nFluidRenderUs, st.Mixer.nMixerRenderUs,
			st.Mixer.nMT32LoadPercent, st.Mixer.nFluidLoadPercent, st.Mixer.nMixerLoadPercent,
			st.bMidiRecording ? "true" : "false");
		if (added <= 0) return -1;
		return n + added;
	}
}

// ──────────────────────────────────────────────────────────────────────────
// Handle one accepted WebSocket connection (blocking, runs as task)
// ──────────────────────────────────────────────────────────────────────────
static void HandleConnection(CSocket* pSock, CMT32Pi* pMT32Pi, unsigned nIntervalMs)
{
	static const size_t kRxBuf = 2048;
	static const size_t kTxBuf = 3072;

	u8* rxBuf = new u8[kRxBuf];
	u8* txBuf = new u8[kTxBuf];

	if (!rxBuf || !txBuf)
	{
		delete[] rxBuf;
		delete[] txBuf;
		delete pSock;
		return;
	}

	// ── 1. Read HTTP Upgrade request ──────────────────────────────────
	// We read until we find \r\n\r\n (end of HTTP headers)
	size_t nRxTotal = 0;
	bool bGotHeaders = false;
	for (int attempt = 0; attempt < 200 && !bGotHeaders; attempt++)
	{
		int n = pSock->Receive(rxBuf + nRxTotal, kRxBuf - 1 - nRxTotal, MSG_DONTWAIT);
		if (n > 0)
		{
			nRxTotal += n;
			rxBuf[nRxTotal] = 0;
			// Search for end-of-headers marker
			for (size_t si = 0; si + 3 < nRxTotal && !bGotHeaders; si++)
				if (rxBuf[si]=='\r' && rxBuf[si+1]=='\n' && rxBuf[si+2]=='\r' && rxBuf[si+3]=='\n')
					bGotHeaders = true;
		}
		else if (n < 0)
			break;
		else
			CScheduler::Get()->MsSleep(5);
	}

	if (!bGotHeaders)
		goto cleanup;

	// ── 2. Extract Sec-WebSocket-Key ─────────────────────────────────
	{
		const char* pKey = strstr((char*)rxBuf, "Sec-WebSocket-Key:");
		if (!pKey) goto cleanup;
		pKey += 18; // skip header name
		while (*pKey == ' ') pKey++;
		const char* pEnd = strstr(pKey, "\r\n");
		if (!pEnd) goto cleanup;

		size_t keyLen = (size_t)(pEnd - pKey);
		if (keyLen == 0 || keyLen > 64) goto cleanup;

		// Concatenate key + WS GUID
		static const char kGUID[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
		char concat[128];
		if (keyLen + sizeof(kGUID) > sizeof(concat)) goto cleanup;
		memcpy(concat, pKey, keyLen);
		memcpy(concat + keyLen, kGUID, sizeof(kGUID) - 1);
		size_t concatLen = keyLen + sizeof(kGUID) - 1;

		// SHA-1 → Base64
		u8 digest[20];
		SHA1Digest((const u8*)concat, concatLen, digest);
		char acceptKey[32]; // 28 chars + NUL
		Base64Encode(digest, 20, acceptKey);

		// ── 3. Send 101 Switching Protocols ──────────────────────────
		char response[256];
		int rLen = snprintf(response, sizeof(response),
			"HTTP/1.1 101 Switching Protocols\r\n"
			"Upgrade: websocket\r\n"
			"Connection: Upgrade\r\n"
			"Sec-WebSocket-Accept: %s\r\n\r\n",
			acceptKey);
		pSock->Send(response, rLen, 0);
	}

	// ── 4. Main loop: push status every 250ms, read incoming frames ───
	{
		unsigned nLastPush = 0;
		u8 framePay[512];
		char jsonBuf[2560];  // seq status + 16 channels + notes[] + engine routing
		char prevJSON[2560]; // last JSON sent; used to skip duplicate frames
		prevJSON[0] = '\0';

		// HZ=100 → 1 tick = 10ms; clamp to [50,5000] ms
		const unsigned nIntervalTicks = (nIntervalMs < 50u ? 50u : nIntervalMs > 5000u ? 5000u : nIntervalMs) / 10u;
		while (true)
		{
			unsigned nNow = CTimer::Get()->GetTicks();

			// Push status at configured interval, but only if state changed
			if (nNow - nLastPush >= nIntervalTicks)
			{
				nLastPush = nNow;
				int jLen = BuildStatusJSON(jsonBuf, sizeof(jsonBuf), pMT32Pi);
				if (jLen > 0 && strcmp(jsonBuf, prevJSON) != 0)
				{
					int fLen = BuildFrame(txBuf, kTxBuf, 0x01, (u8*)jsonBuf, (size_t)jLen);
					if (fLen > 0)
					{
						if (pSock->Send(txBuf, (unsigned)fLen, MSG_DONTWAIT) < 0)
							break; // connection closed
						memcpy(prevJSON, jsonBuf, (size_t)jLen + 1);
					}
				}
			}

			int n = pSock->Receive(rxBuf, kRxBuf, MSG_DONTWAIT);
			if (n < 0) break; // connection error/closed
			if (n > 0)
			{
				u8 opcode = 0;
				int pLen = ParseClientFrame(rxBuf, (size_t)n, &opcode, framePay, sizeof(framePay));

				if (opcode == 0x8) // Close
					break;

				if (opcode == 0x9) // Ping → Pong
				{
					int fLen = BuildFrame(txBuf, kTxBuf, 0x0A, framePay, pLen > 0 ? (size_t)pLen : 0);
					if (fLen > 0) pSock->Send(txBuf, (unsigned)fLen, MSG_DONTWAIT);
				}

				if (opcode == 0x1 && pLen > 0) // Text frame (MIDI command)
				{
					// Expect JSON: {"type":"on","ch":0,"note":60,"vel":100}
					// Simple string parsing to extract values
					framePay[pLen] = 0;
					char* p = (char*)framePay;

					auto getInt = [](const char* json, const char* key) -> int {
						const char* pos = strstr(json, key);
						if (!pos) return -1;
						pos += strlen(key);
						while (*pos == ':' || *pos == ' ') pos++;
						return atoi(pos);
					};
					auto getStr = [](const char* json, const char* key, char* out, size_t outLen) -> bool {
						const char* pos = strstr(json, key);
						if (!pos) return false;
						pos += strlen(key);
						while (*pos == ':' || *pos == ' ' || *pos == '"') pos++;
						size_t i = 0;
						while (*pos && *pos != '"' && i + 1 < outLen)
							out[i++] = *pos++;
						out[i] = 0;
						return i > 0;
					};

					char typeStr[8];
					if (getStr(p, "\"type\"", typeStr, sizeof(typeStr)))
					{
						int ch  = getInt(p, "\"ch\"");
						int note = getInt(p, "\"note\"");
						int vel  = getInt(p, "\"vel\"");
						if (ch >= 0 && ch < 16 && note >= 0 && note < 128 && vel >= 0 && vel < 128)
						{
							u8 status = (strcmp(typeStr, "on") == 0 && vel > 0)
								? (0x90 | (u8)ch) : (0x80 | (u8)ch);
							u8 msg[3] = { status, (u8)note, (u8)vel };
							pMT32Pi->SendRawMIDI(msg, 3);
						}
					}
				}
			}

			CScheduler::Get()->MsSleep(10);
		}

		// Send close frame
		u8 closePayload[2] = { 0x03, 0xE8 };
		int fLen = BuildFrame(txBuf, kTxBuf, 0x8, closePayload, 2);
		if (fLen > 0) pSock->Send(txBuf, (unsigned)fLen, MSG_DONTWAIT);
	}

cleanup:
	delete[] rxBuf;
	delete[] txBuf;
	delete pSock;
}

// ──────────────────────────────────────────────────────────────────────────
// Worker task for each WebSocket client
// ──────────────────────────────────────────────────────────────────────────
class CWebSocketWorker : public CTask
{
public:
	CWebSocketWorker(CSocket* pSock, CMT32Pi* pPi, unsigned nIntervalMs)
		: CTask(8192), m_pSock(pSock), m_pPi(pPi), m_nIntervalMs(nIntervalMs) {}

	void Run() override
	{
		HandleConnection(m_pSock, m_pPi, m_nIntervalMs);
	}

private:
	CSocket*  m_pSock;
	CMT32Pi*  m_pPi;
	unsigned  m_nIntervalMs;
};

// ──────────────────────────────────────────────────────────────────────────
// CWebSocketDaemon — listener task
// ──────────────────────────────────────────────────────────────────────────
CWebSocketDaemon::CWebSocketDaemon(CNetSubSystem* pNet, CMT32Pi* pPi, u16 nPort, unsigned nIntervalMs)
	: CTask(8192),
	  m_pNetSubSystem(pNet),
	  m_pMT32Pi(pPi),
	  m_nPort(nPort),
	  m_nIntervalMs(nIntervalMs)
{
}

CWebSocketDaemon::~CWebSocketDaemon()
{
}

void CWebSocketDaemon::Run()
{
	CSocket* pServer = new CSocket(m_pNetSubSystem, IPPROTO_TCP);
	if (!pServer)
	{
		LOGERR("Could not create server socket");
		return;
	}

	if (pServer->Bind(m_nPort) < 0)
	{
		LOGERR("Bind failed on port %u", (unsigned)m_nPort);
		delete pServer;
		return;
	}

	if (pServer->Listen(4) < 0)
	{
		LOGERR("Listen failed");
		delete pServer;
		return;
	}

	LOGNOTE("WebSocket daemon listening on port %u", (unsigned)m_nPort);

	while (true)
	{
		CIPAddress clientIP;
		u16 clientPort;
		CSocket* pClient = pServer->Accept(&clientIP, &clientPort);
		if (pClient)
		{
			new CWebSocketWorker(pClient, m_pMT32Pi, m_nIntervalMs);
		}
		else
		{
			CScheduler::Get()->MsSleep(10);
		}
	}
}
