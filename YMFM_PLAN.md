# ymfm Integration Plan

## Objective

Add a third synth engine backed by [ymfm](https://github.com/aaronsgiles/ymfm) (BSD-3-Clause) to emulate classic Yamaha FM chips (OPL3, OPN2, OPM, etc.) and expose it alongside MT-32 (Munt) and FluidSynth in the existing MIDI router/mixer architecture.

---

## Phase 0 — Prep & README restoration

- [ ] **0.1** Restore the License section removed from the README (GPL-3.0 + logo terms)
- [ ] **0.2** Add an Acknowledgments section crediting: Munt, FluidSynth, Circle, circle-stdlib, inih, GeneralUser GS, stb_vorbis (fork addition)
- [ ] **0.3** Add a Dependencies / Third-party section listing all `external/` submodules with their licenses

## Phase 1 — Submodule & build plumbing

- [ ] **1.1** Add ymfm as `external/ymfm` submodule (`git submodule add https://github.com/aaronsgiles/ymfm external/ymfm`)
- [ ] **1.2** Copy or symlink ymfm's `LICENSE` into the repo root or a `LICENSES/` directory
- [ ] **1.3** Create `Makefile` / cmake fragment to compile the needed `.cpp` files (`ymfm_opl.cpp`, `ymfm_opm.cpp`, `ymfm_opn.cpp`, `ymfm_adpcm.cpp`, `ymfm_pcm.cpp`, `ymfm_ssg.cpp`) into a static library or object files
- [ ] **1.4** Verify it compiles with the AArch64 cross-toolchain (C++14 minimum; we already use C++17)
- [ ] **1.5** Add ymfm to `Kernel.mk` link step

## Phase 2 — Synth wrapper class

- [ ] **2.1** Create `include/synth/ymfmsynth.h` and `src/synth/ymfmsynth.cpp`
- [ ] **2.2** Implement `ymfm::ymfm_interface` callback class (timer, IRQ — mostly no-ops for us)
- [ ] **2.3** Instantiate the target chip (start with `ymfm::ymf262` / OPL3 — widest GM MIDI compatibility)
- [ ] **2.4** Implement `CYmfmSynth` inheriting from the same `CSynthBase` interface as `CMT32Synth` / `CSoundFontSynth`
- [ ] **2.5** Implement `Render(s16* buf, unsigned nFrames)` — call `chip.generate()` in a loop, resample from chip native rate (~49.7 kHz for OPL3) to 48 kHz

## Phase 3 — MIDI-to-register driver (the hard part)

This is the core challenge: ymfm is register-level only, so we must write a complete MIDI driver.

- [ ] **3.1** Frequency table: MIDI note → OPL3 F-Number + Block (or OPN2 equivalent)
- [ ] **3.2** Voice allocator: 18 2-op voices (or 6 4-op + 6 2-op in OPL3 mode). Track channel → voice mapping with priority/stealing
- [ ] **3.3** Instrument bank: load OPL patch data from `.OP2` / `.WOPL` / `.OPL` bank files (standard DOS-era formats) or embed a default GM bank (e.g. GENMIDI from Doom, DMXGUS-style banks)
- [ ] **3.4** Note On / Note Off: program operator registers, key-on bit, velocity → total level scaling
- [ ] **3.5** Program Change: swap instrument patch → re-program operator params
- [ ] **3.6** Pitch Bend: recalculate F-Number in real time
- [ ] **3.7** CC support: CC1 (vibrato/LFO), CC7 (volume), CC10 (pan via OPL3 stereo bits), CC11 (expression), CC64 (sustain pedal), CC121 (reset all), CC123 (all notes off)
- [ ] **3.8** Channel 10 percussion: use separate rhythm-mode patches, map GM percussion keys

### Reference implementations to study

| Project | Notes |
|---------|-------|
| **ADLMIDI** (libADLMIDI) | Full OPL3 MIDI driver, GPL-3.0 — voice allocation, multi-bank, .WOPL format. Best reference. |
| **OPNMIDI** (libOPNMIDI) | Same architecture for OPN2. |
| **Doom / DMX** | Classic OPL2 MIDI driver, GENMIDI lump format. |
| **Miles Sound System / AIL** | Documented register programming sequences. |
| **Nuked-OPL3** | Not a MIDI driver, but useful for accuracy comparison. |

## Phase 4 — Integration into mt32-pi architecture

- [ ] **4.1** Register `CYmfmSynth` as a third engine in `CMT32Pi` (alongside MT-32 and FluidSynth)
- [ ] **4.2** Extend `CMIDIRouter` to route channels to ymfm (route enum: MT-32 | FluidSynth | ymfm | Both | Off)
- [ ] **4.3** Feed ymfm output through `CAudioMixer` (third stereo pair)
- [ ] **4.4** Add ymfm configuration to `config.def`: chip type, clock, instrument bank path
- [ ] **4.5** Extend web UI Sound page to show ymfm status, patch name, voice allocation

## Phase 5 — Instrument bank management

- [ ] **5.1** Support loading `.WOPL` bank files from SD card (libADLMIDI format — well documented, open)
- [ ] **5.2** Embed a default GM bank so it works out of the box (e.g. the free WOPL banks from libADLMIDI)
- [ ] **5.3** Bank switching via SysEx or web UI
- [ ] **5.4** Optional: support `.OP2` (Doom GENMIDI) and `.OPL` (Reality AdLib Tracker) formats

## Phase 6 — Advanced / Optional

- [ ] **6.1** OPN2 (YM2612 / Mega Drive) mode as an alternative chip type
- [ ] **6.2** OPM (YM2151) mode for Sharp X68000 / arcade sound
- [ ] **6.3** Dual-chip mode (two OPL3 = 36 voices) for richer GM playback
- [ ] **6.4** Per-voice VU meters in the web UI
- [ ] **6.5** Resampler optimization (NEON-accelerated linear or sinc interpolation)

---

## Licensing checklist

- [ ] ymfm: BSD-3-Clause — include `external/ymfm/LICENSE`, credit in README
- [ ] If using libADLMIDI code as reference: GPL-3.0 compatible, but do NOT copy code — rewrite from documentation
- [ ] If embedding WOPL instrument banks: verify individual bank licenses (many are public domain / CC0)

---

## README restoration checklist

The following sections from the original upstream README were removed during the fork rewrite and should be restored (adapted for the fork):

### Missing: License section
```
## ⚖️ License

This project's source code is licensed under the [GNU General Public License v3.0][license].
```
The original also included logo usage terms (© Dale Whinham).

### Missing: Acknowledgments section
```
## 🙌 Acknowledgments

- [Munt] — MT-32 / CM-32L emulation
- [FluidSynth] — SoundFont synthesizer
- [S. Christian Collins / GeneralUser GS] — bundled GM SoundFont
- [Circle] and [circle-stdlib] — baremetal C++ framework for Raspberry Pi
- [inih] — lightweight INI config parser
- [stb_vorbis] — Ogg Vorbis decoder (added by this fork for SF3 support)
```

### Missing: link reference definitions
The original README used `[Munt]: https://...` style reference links at the bottom. These should be restored.

---

## Estimated complexity

| Phase | Effort | Risk |
|-------|--------|------|
| 0 — README fix | Trivial | None |
| 1 — Submodule/build | Low | Low — straightforward, similar to Munt integration |
| 2 — Synth wrapper | Medium | Low — clean interface design |
| 3 — MIDI driver | **High** | **High** — this is 80% of the work. Voice allocation, bank loading, frequency math, CC handling |
| 4 — Architecture integration | Medium | Low — well-trodden path (copy pattern from FluidSynth engine) |
| 5 — Bank management | Medium | Medium — format parsing, SD card I/O |
| 6 — Advanced | Optional | Low — incremental additions |
