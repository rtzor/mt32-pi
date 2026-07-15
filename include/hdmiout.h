// hdmiout.h — HDMI display output for mt32-pi-rt
//
// Runs on Core 3. Draws a synth status HUD using Circle's C2DGraphics:
//   - 16 channel VU bars with peak hold
//   - Synth name / SoundFont / ROM info
//   - Sequencer progress bar + BPM + file name
//   - Master volume bar
//
// Enable with:  [video]  hdmi_display = true

#ifndef _hdmiout_h
#define _hdmiout_h

#include <circle/2dgraphics.h>
#include <circle/types.h>

// Forward declarations — avoids pulling in the full kernel headers
class CMT32Pi;

class CHdmiOutput
{
public:
        static constexpr unsigned Channels = 16;

        // Screen geometry — exposed so callers can log/report them
        static constexpr unsigned ScreenW   = 1280;
        static constexpr unsigned ScreenH   = 720;

        explicit CHdmiOutput(CMT32Pi* pKernel);

        // Called once from VideoTask() before entering the render loop.
        // Returns false if the framebuffer failed to initialize (HDMI cable absent, etc.).
        bool Initialize();

        // Draw one frame. Call at ~30 fps.
        // levels[16] and peaks[16] are 0.0–1.0 floats already computed by the caller.
        void DrawFrame(float levels[Channels], float peaks[Channels]);

private:
        // ---- Layout constants (1280×720 logical grid) ----

        static constexpr unsigned TitleH    = 56;

        // VU section — 16 bars
        static constexpr unsigned VuTop     = TitleH + 12;
        static constexpr unsigned VuBottom  = ScreenH - 120;
        static constexpr unsigned VuH       = VuBottom - VuTop;
        static constexpr unsigned VuBarW    = 52;
        static constexpr unsigned VuGap     = 26;
        static constexpr unsigned VuAreaW   = Channels * VuBarW + (Channels - 1) * VuGap;
        static constexpr unsigned VuLeft    = (ScreenW - VuAreaW) / 2;
        static constexpr unsigned PeakDotH  = 4;

        // Info bar at the bottom
        static constexpr unsigned InfoTop   = VuBottom + 16;
        static constexpr unsigned InfoH     = ScreenH - InfoTop;

        // ---- Colours ----
        // COLOR2D(r,g,b) — values 0-255
        static constexpr T2DColor ColBg       = COLOR2D(  8,  14,  28);
        static constexpr T2DColor ColTitle    = COLOR2D( 30,  60, 100);
        static constexpr T2DColor ColText     = COLOR2D(200, 220, 255);
        static constexpr T2DColor ColDim      = COLOR2D( 80,  90, 120);
        static constexpr T2DColor ColVuLow    = COLOR2D( 30, 180,  80);   // 0–70 %
        static constexpr T2DColor ColVuMid    = COLOR2D(220, 200,  40);   // 70–90 %
        static constexpr T2DColor ColVuHigh   = COLOR2D(220,  50,  40);   // 90–100 %
        static constexpr T2DColor ColPeak     = COLOR2D(255, 255, 255);
        static constexpr T2DColor ColEmpty    = COLOR2D( 20,  30,  50);
        static constexpr T2DColor ColProgress = COLOR2D( 50, 130, 220);
        static constexpr T2DColor ColVolume   = COLOR2D(100, 160, 255);
        static constexpr T2DColor ColSeqBg    = COLOR2D( 15,  25,  45);

        // ---- Helpers ----
        void DrawTitleBar(const char* pSynthName, const char* pRomOrSF, int nMasterVol);
        void DrawVuBar(unsigned nChannel, float fLevel, float fPeak);
        void DrawChannelLabels();
        void DrawInfoBar();

        C2DGraphics m_Graphics;
        CMT32Pi*    m_pKernel;

        // Snapshot of system state captured each frame
        struct TFrameState
        {
                char     SynthName[32];
                char     RomOrSF[64];
                int      nMasterVolume;
                bool     bPlaying;
                bool     bPaused;
                char     FileName[64];
                u32      nElapsedMs;
                u32      nDurationMs;
                int      nBPM;
        };

        TFrameState m_State;

        // Peak hold decay: stored as 0.0–1.0 float per channel
        float m_fPeakHold[Channels];
        u32   m_nPeakTime[Channels];   // tick when peak was last updated
};

#endif
