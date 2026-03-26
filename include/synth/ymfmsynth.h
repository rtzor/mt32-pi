// ymfmsynth.h
//
// mt32-pi Extended Edition — ymfm OPL3 synthesizer engine
// License: GPL-3.0 (ymfm itself is BSD-3-Clause; compatible)

#ifndef _ymfmsynth_h
#define _ymfmsynth_h

#include <circle/spinlock.h>
#include <circle/types.h>

#include <array>
#include <cstdint>

#include <ymfm_opl.h>

#include "synth/synthbase.h"
#include "synth/synth.h"
#include "synth/woplmanager.h"

// Supported chip emulation modes
enum class TOplChipMode { OPL3, OPL2 };

// OPL3 has 18 two-operator voices (or 6 four-op + 6 two-op)
// We use all 18 in 2-op mode for maximum polyphony.
static constexpr unsigned OPL3_VOICES = 18;
static constexpr unsigned MIDI_CHANNELS = 16;
static constexpr unsigned PERCUSSION_CHANNEL = 9;

// OPL3 standard FM clock (14.31818 MHz)
static constexpr uint32_t OPL3_CLOCK_HZ = 14318180;

// One OPL3 instrument patch (2-op, as stored in OP2/WOPL format subset)
struct TOpl3Patch
{
    uint8_t modChar;        // AM/Vib/EG/KSR/Multi (modulator)
    uint8_t carChar;        // AM/Vib/EG/KSR/Multi (carrier)
    uint8_t modScaleLev;    // KSL/TL (modulator)
    uint8_t carScaleLev;    // KSL/TL (carrier)
    uint8_t modAttDec;      // AR/DR (modulator)
    uint8_t carAttDec;      // AR/DR (carrier)
    uint8_t modSusRel;      // SL/RR (modulator)
    uint8_t carSusRel;      // SL/RR (carrier)
    uint8_t modWave;        // Wave select (modulator)
    uint8_t carWave;        // Wave select (carrier)
    uint8_t feedback;       // Feedback/algorithm (connection)
    int8_t  noteOffset;     // Transpose (semitones)
};

struct TPercPatch
{
    uint8_t key;            // MIDI note number this maps to
    TOpl3Patch patch;
};

// Voice slot — tracks which MIDI note is playing
struct TVoice
{
    bool     bFree;
    uint8_t  nMIDIChannel;
    uint8_t  nNote;
    uint8_t  nVelocity;
    uint32_t nAge;          // frame counter at note-on (for voice stealing)
};

// Per-MIDI-channel runtime state
struct TChannelState
{
    uint8_t  nProgram;
    uint8_t  nVolume;
    uint8_t  nExpression;
    uint8_t  nPan;
    uint16_t nPitchBend;    // 0..16383, center = 8192
    bool     bSustain;
    uint8_t  nPitchBendRange; // semitones (default 2)
};

// Minimal ymfm interface for baremetal (timers/IRQ ignored)
class CYmfmInterface : public ymfm::ymfm_interface
{
public:
    // All timer/IRQ callbacks are no-ops — we drive timing via generate()
    virtual void ymfm_set_timer(uint32_t, int32_t) override {}
    virtual void ymfm_update_irq(bool) override {}
};

class CYmfmSynth : public CSynthBase
{
public:
    CYmfmSynth(unsigned nSampleRate);
    virtual ~CYmfmSynth() override = default;

    // CSynthBase interface
    virtual bool Initialize() override;
    virtual void HandleMIDIShortMessage(u32 nMessage) override;
    virtual void HandleMIDISysExMessage(const u8* pData, size_t nSize) override;
    virtual bool IsActive() override;
    virtual void AllSoundOff() override;
    virtual void SetMasterVolume(u8 nVolume) override;
    virtual size_t Render(s16* pOutBuffer, size_t nFrames) override;
    virtual size_t Render(float* pOutBuffer, size_t nFrames) override;
    virtual void ReportStatus() const override;
    virtual void UpdateLCD(CLCD& LCD, unsigned int nTicks) override;
    virtual const char* GetName() const override;
    virtual TSynth GetType() const override { return TSynth::Ymfm; }
    virtual const char* GetChannelInstrumentName(u8 nChannel) override;

    const char* GetBankName() const { return m_szBankName; }
    const CWoplBankManager& GetBankManager() const { return m_BankManager; }
    size_t GetCurrentBankIndex() const { return m_nCurrentBank; }
    bool SwitchBank(size_t nIndex);
    void RescanBanks() { m_BankManager.ScanBanks(); }
    TOplChipMode GetChipMode() const { return m_eChipMode; }
    void SetChipMode(TOplChipMode eMode);

private:
    // Register-level helpers
    void WriteReg(uint32_t nReg, uint8_t nValue);
    void ProgramVoice(unsigned nVoice, const TOpl3Patch& patch, uint8_t nVolume, uint8_t nPan);
    void KeyOn(unsigned nVoice, uint8_t nNote, int8_t nNoteOffset);
    void KeyOff(unsigned nVoice);

    // Bank loading — dispatches by file extension
    bool LoadWOPLBank(const char* pPath);   // WOPL v2 format
    bool LoadOP2Bank(const char* pPath);    // DOOM GENMIDI (.op2) format

    // Voice allocation
    int  AllocVoice(uint8_t nChannel, uint8_t nNote);
    int  FindVoice(uint8_t nChannel, uint8_t nNote) const;
    void FreeVoice(unsigned nVoice);
    void ReleaseAllChannel(uint8_t nChannel);

    void AllNotesOff(uint8_t nChannel);
    void ResetAllControllers(uint8_t nChannel);

    // MIDI event handlers
    void NoteOn(uint8_t nChannel, uint8_t nNote, uint8_t nVelocity);
    void NoteOff(uint8_t nChannel, uint8_t nNote);
    void ControlChange(uint8_t nChannel, uint8_t nCC, uint8_t nValue);
    void ProgramChange(uint8_t nChannel, uint8_t nProgram);
    void PitchBend(uint8_t nChannel, uint16_t nValue);

    CYmfmInterface          m_Interface;
    ymfm::ymf262            m_Chip;
    uint32_t                m_nNativeRate;      // chip native sample rate
    uint32_t                m_nAgeCounter;

    TOplChipMode            m_eChipMode;        // OPL2 or OPL3 mode
    unsigned                m_nVoiceCount;      // 9 (OPL2) or 18 (OPL3)

    uint8_t                 m_nMasterVolume;    // 0–100
    std::array<TVoice, OPL3_VOICES>       m_Voices;
    std::array<TChannelState, MIDI_CHANNELS> m_Channels;

    TOpl3Patch              m_Patches[128];     // runtime melodic bank (GM or custom WOPL)

    CWoplBankManager        m_BankManager;      // scanned bank list
    size_t                  m_nCurrentBank;     // index into m_BankManager

    char                    m_szBankName[64];   // displayed in OSD menu

    // Resampler state (naive linear for now)
    float                   m_fResamplePos;
    ymfm::ymf262::output_data m_LastSample;

    // Instrument name cache for web UI
    const char*             m_pInstrumentNames[MIDI_CHANNELS];
};

#endif
