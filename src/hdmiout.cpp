// hdmiout.cpp — HDMI display implementation for mt32-pi-rt
// Proof of concept for Core 3 visual output.

#include "hdmiout.h"

#include <circle/serial.h>
#include <circle/usb/usbhcidevice.h>
#include "mt32pi.h"

#include <circle/timer.h>
#include <circle/font.h>
#include <cstring>
#include <cstdio>

// Peak hold duration in ticks (Circle ticks = µs).  2 seconds.
static constexpr u32 PeakHoldTicks = 2000000u;

CHdmiOutput::CHdmiOutput(CMT32Pi* pKernel)
        : m_Graphics(ScreenW, ScreenH, /*bVSync=*/FALSE, /*nDisplay=*/0),
          m_pKernel(pKernel)
{
        memset(&m_State, 0, sizeof(m_State));
        for (unsigned i = 0; i < Channels; ++i)
        {
                m_fPeakHold[i] = 0.0f;
                m_nPeakTime[i] = 0;
        }
}

bool CHdmiOutput::Initialize()
{
        return m_Graphics.Initialize();
}

// ---------------------------------------------------------------------------
// DrawFrame — main entry point called each tick from VideoTask()
// ---------------------------------------------------------------------------
void CHdmiOutput::DrawFrame(float levels[Channels], float peaks[Channels])
{
        // Capture system state into the local snapshot (fast copy, no allocation)
        CMT32Pi::TSystemState s = m_pKernel->GetSystemState();

        strncpy(m_State.SynthName, s.pActiveSynthName ? s.pActiveSynthName : "mt32-pi-rt", 31);
        m_State.SynthName[31] = '\0';

        // Choose the most relevant subtitle: SoundFont name or MT-32 ROM name
        const char* pSub = nullptr;
        if (s.pSoundFontName && s.pSoundFontName[0])
                pSub = s.pSoundFontName;
        else if (s.pMT32ROMName && s.pMT32ROMName[0])
                pSub = s.pMT32ROMName;
        else
                pSub = "";
        strncpy(m_State.RomOrSF, pSub, 63);
        m_State.RomOrSF[63] = '\0';

        m_State.nMasterVolume = s.nMasterVolume;
        m_State.bPlaying      = s.Sequencer.bPlaying;
        m_State.bPaused       = s.Sequencer.bPaused;
        m_State.nElapsedMs    = s.Sequencer.nElapsedMs;
        m_State.nDurationMs   = s.Sequencer.nDurationMs;
        m_State.nBPM          = s.Sequencer.nBPM;

        const char* pFile = s.Sequencer.pFile ? s.Sequencer.pFile : "";
        // Strip path — keep only the filename portion
        const char* pSlash = pFile;
        for (const char* p = pFile; *p; ++p)
                if (*p == '/' || *p == '\\') pSlash = p + 1;
        strncpy(m_State.FileName, pSlash, 63);
        m_State.FileName[63] = '\0';

        // Update peak hold
        u32 nNow = CTimer::GetClockTicks();
        for (unsigned i = 0; i < Channels; ++i)
        {
                if (peaks[i] >= m_fPeakHold[i])
                {
                        m_fPeakHold[i] = peaks[i];
                        m_nPeakTime[i] = nNow;
                }
                else if ((nNow - m_nPeakTime[i]) > PeakHoldTicks)
                {
                        // Decay slowly
                        m_fPeakHold[i] -= 0.01f;
                        if (m_fPeakHold[i] < 0.0f) m_fPeakHold[i] = 0.0f;
                }
        }

        // ---- Render ----
        m_Graphics.ClearScreen(ColBg);
        DrawTitleBar(m_State.SynthName, m_State.RomOrSF, m_State.nMasterVolume);

        for (unsigned ch = 0; ch < Channels; ++ch)
                DrawVuBar(ch, levels[ch], m_fPeakHold[ch]);

        DrawChannelLabels();
        DrawInfoBar();

        m_Graphics.UpdateDisplay();
}

// ---------------------------------------------------------------------------
// DrawTitleBar
// ---------------------------------------------------------------------------
void CHdmiOutput::DrawTitleBar(const char* pSynthName, const char* pRomOrSF, int nMasterVol)
{
        // Background strip
        m_Graphics.DrawRect(0, 0, ScreenW, TitleH, ColTitle);

        // Synth name — large font
        m_Graphics.DrawText(16, 8, ColText, pSynthName,
                            C2DGraphics::AlignLeft, Font12x22);

        // Subtitle (ROM/SoundFont)
        if (pRomOrSF && pRomOrSF[0])
                m_Graphics.DrawText(16, TitleH - 20, ColDim, pRomOrSF,
                                    C2DGraphics::AlignLeft, Font8x16);

        // Master volume bar — right side
        static constexpr unsigned VolBarW = 200;
        static constexpr unsigned VolBarH = 14;
        unsigned vx = ScreenW - VolBarW - 20;
        unsigned vy = (TitleH - VolBarH) / 2;

        m_Graphics.DrawText(vx - 60, vy, ColDim, "VOL",
                            C2DGraphics::AlignLeft, Font8x16);

        m_Graphics.DrawRect(vx, vy, VolBarW, VolBarH, ColEmpty);
        unsigned fill = static_cast<unsigned>(VolBarW * nMasterVol / 127);
        if (fill > VolBarW) fill = VolBarW;
        if (fill > 0)
                m_Graphics.DrawRect(vx, vy, fill, VolBarH, ColVolume);
        m_Graphics.DrawRectOutline(vx, vy, VolBarW, VolBarH, ColDim);
}

// ---------------------------------------------------------------------------
// DrawVuBar — one channel (0-based)
// ---------------------------------------------------------------------------
void CHdmiOutput::DrawVuBar(unsigned nChannel, float fLevel, float fPeak)
{
        unsigned x = VuLeft + nChannel * (VuBarW + VuGap);

        // Empty background
        m_Graphics.DrawRect(x, VuTop, VuBarW, VuH, ColEmpty);

        // Filled portion (grows from bottom)
        if (fLevel > 1.0f) fLevel = 1.0f;
        if (fLevel > 0.0f)
        {
                unsigned fillH = static_cast<unsigned>(VuH * fLevel);
                unsigned fillY = VuTop + VuH - fillH;

                // Colour changes with level
                T2DColor col;
                if (fLevel > 0.90f)     col = ColVuHigh;
                else if (fLevel > 0.70f) col = ColVuMid;
                else                    col = ColVuLow;

                m_Graphics.DrawRect(x, fillY, VuBarW, fillH, col);
        }

        // Peak dot
        if (fPeak > 0.01f)
        {
                unsigned peakY = VuTop + VuH - static_cast<unsigned>(VuH * fPeak);
                if (peakY + PeakDotH > VuTop + VuH) peakY = VuTop + VuH - PeakDotH;
                m_Graphics.DrawRect(x, peakY, VuBarW, PeakDotH, ColPeak);
        }

        // Outline
        m_Graphics.DrawRectOutline(x, VuTop, VuBarW, VuH, ColDim);
}

// ---------------------------------------------------------------------------
// DrawChannelLabels — channel numbers under each VU bar
// ---------------------------------------------------------------------------
void CHdmiOutput::DrawChannelLabels()
{
        char buf[4];
        for (unsigned ch = 0; ch < Channels; ++ch)
        {
                unsigned x = VuLeft + ch * (VuBarW + VuGap);
                snprintf(buf, sizeof(buf), "%u", ch + 1);
                // Centre text inside the bar width
                m_Graphics.DrawText(x + VuBarW / 2, VuBottom + 4,
                                    ColDim, buf,
                                    C2DGraphics::AlignCenter, Font8x16);
        }
}

// ---------------------------------------------------------------------------
// DrawInfoBar — sequencer info at the bottom
// ---------------------------------------------------------------------------
void CHdmiOutput::DrawInfoBar()
{
        m_Graphics.DrawRect(0, InfoTop - 4, ScreenW, ScreenH - (InfoTop - 4), ColSeqBg);

        if (!m_State.bPlaying && !m_State.bPaused)
        {
                m_Graphics.DrawText(ScreenW / 2, InfoTop + InfoH / 2 - 8,
                                    ColDim, "-- idle --",
                                    C2DGraphics::AlignCenter, Font8x16);
                return;
        }

        // File name
        m_Graphics.DrawText(16, InfoTop + 4, ColText, m_State.FileName,
                            C2DGraphics::AlignLeft, Font8x16);

        // BPM + pause indicator
        char bpmBuf[32];
        snprintf(bpmBuf, sizeof(bpmBuf), "%s  %d BPM",
                 m_State.bPaused ? "[PAUSED]" : "", m_State.nBPM);
        m_Graphics.DrawText(ScreenW - 16, InfoTop + 4, ColDim, bpmBuf,
                            C2DGraphics::AlignRight, Font8x16);

        // Progress bar
        static constexpr unsigned PrgY = InfoTop + 30;
        static constexpr unsigned PrgH = 12;
        m_Graphics.DrawRect(16, PrgY, ScreenW - 32, PrgH, ColEmpty);

        if (m_State.nDurationMs > 0)
        {
                unsigned fill = static_cast<unsigned>(
                        (u64)(ScreenW - 32) * m_State.nElapsedMs / m_State.nDurationMs);
                if (fill > ScreenW - 32) fill = ScreenW - 32;
                if (fill > 0)
                        m_Graphics.DrawRect(16, PrgY, fill, PrgH, ColProgress);
        }
        m_Graphics.DrawRectOutline(16, PrgY, ScreenW - 32, PrgH, ColDim);

        // Elapsed / total time
        auto fmtMs = [](char* buf, size_t n, u32 ms) {
                unsigned s = ms / 1000;
                snprintf(buf, n, "%u:%02u", s / 60, s % 60);
        };
        char elBuf[16], totBuf[16];
        fmtMs(elBuf,  sizeof(elBuf),  m_State.nElapsedMs);
        fmtMs(totBuf, sizeof(totBuf), m_State.nDurationMs);

        m_Graphics.DrawText(16, PrgY + PrgH + 4, ColDim, elBuf,
                            C2DGraphics::AlignLeft, Font8x10);
        m_Graphics.DrawText(ScreenW - 16, PrgY + PrgH + 4, ColDim, totBuf,
                            C2DGraphics::AlignRight, Font8x10);
}
