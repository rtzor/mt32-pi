## ⚠️ Experimental Fork

This repository is a personal, **highly experimental fork** of the original project.

- It may be unstable, incomplete, or broken.
- Features are prototypes and may contain bugs.
- **No releases are provided** — you must build from source.
- **Only tested on Raspberry Pi 3 (64-bit / AArch64)**. Other boards may or may not work.
- I do **not guarantee** that anything works as expected.
- I do **not provide support** for this fork.

If you are looking for a stable and fully functional version, please use the original project:
👉 https://github.com/dwhinham/mt32-pi

All credit goes to the original author, who did an outstanding job creating and maintaining this project.

### Purpose of this fork

This fork exists purely for experimentation, testing new ideas, and personal learning.
Expect things to break — that's part of the process.

---

## 🔀 Extended Edition Features

This fork adds a full web-based control interface, a dual-engine MIDI mixer/router system, a built-in MIDI file sequencer, and real-time audio/MIDI monitoring on top of the original mt32-pi.


### System architecture

#### Core assignment

| Core | Responsibility | Cadence |
|------|---------------|----------|
| 0 | MIDI polling, parsing, routing, sequencer tick, network, control | Continuous |
| 1 | LCD / MiSTer status display | ~16 ms |
| 2 | Audio render (hot path — synth + mixer) | ~11.6 ms (256 frames @ 48 kHz) |
| 3 | currently available | — |

No mutexes on the audio hot path. Inter-core communication uses `volatile` flags and lock-free ring buffers.

#### Component overview

```
┌─────────────────────────────────────────────────────────────────────────┐
│                              CORE 0  (main loop)                        │
│                                                                         │
│  ┌──────────────┐   ┌──────────────┐   ┌──────────────┐                │
│  │  MIDI Input  │   │  Sequencer   │   │  Web Keyboard│                │
│  │  Serial/USB/ │   │  (SMF Type   │   │  (REST API)  │                │
│  │  GPIO/Apple  │   │   0 & 1)     │   │              │                │
│  └──────┬───────┘   └──────┬───────┘   └──────┬───────┘                │
│         │                  │                  │                         │
│         └──────────────────┴──────────────────┘                        │
│                            │                                            │
│                    ┌───────▼────────┐                                   │
│                    │  CMIDIParser   │  running status, SysEx            │
│                    └───────┬────────┘                                   │
│                            │                                            │
│                    ┌───────▼────────┐                                   │
│                    │  CMIDIRouter   │  per-channel route, remap,        │
│                    │                │  CC filter, layering              │
│                    └──┬─────────┬──┘                                   │
│                       │         │                                       │
│             ┌─────────┘         └─────────┐                            │
│             │                             │                            │
│    ┌────────▼───────┐           ┌─────────▼──────┐                    │
│    │  CMT32Synth    │           │ CSoundFontSynth │                    │
│    │  (munt / MT-32)│           │  (FluidSynth)   │                    │
│    └────────┬───────┘           └─────────┬──────┘                    │
│             │                             │                            │
│             └─────────┐       ┌───────────┘                            │
│                       │       │                                        │
│               ┌───────▼───────▼───────┐                               │
│               │     CAudioMixer        │  volume, pan, solo,           │
│               │                        │  per-channel & per-engine     │
│               └───────────┬───────────┘                               │
└───────────────────────────│───────────────────────────────────────────┘
                            │  (shared buffer, written Core 0 / read Core 2)
┌───────────────────────────│───────────────────────────────────────────┐
│                    CORE 2 (audio render)                               │
│               ┌───────────▼───────────┐                               │
│               │   HAL audio output     │  PWM / I²S / HDMI @ 48 kHz  │
│               └───────────────────────┘                               │
└───────────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────────────┐
│                        NETWORK SERVICES  (Core 0)                       │
│                                                                         │
│  ┌────────────┐  ┌─────────────────┐  ┌────────────┐  ┌────────────┐  │
│  │ CWebDaemon │  │CWebSocketDaemon │  │ AppleMIDI  │  │  UDP MIDI  │  │
│  │ HTTP:80    │  │  WS:8765        │  │  RTP MIDI  │  │  (opt.)    │  │
│  │ 5 pages +  │  │  JSON status    │  │            │  │            │  │
│  │ REST API   │  │  ~250 ms push   │  │            │  │            │  │
│  └────────────┘  └─────────────────┘  └────────────┘  └────────────┘  │
└─────────────────────────────────────────────────────────────────────────┘
```

#### MIDI data flow (Core 0)

```
Serial / USB / GPIO IRQ
        │
        ▼
  Ring buffer (lock-free)
        │
        ▼
  CMIDIParser ──► OnSysExMessage ──► SysEx handler (synth params, custom)
        │
        ▼
  CMIDIRouter
    ├── channel route table  (MT-32 | FluidSynth | Both | Off)
    ├── channel remap        (source ch → target ch per engine)
    ├── CC filter            (block CC per engine)
    └── layering flag        (duplicate to both engines)
        │              │
        ▼              ▼
  CMT32Synth     CSoundFontSynth
  (munt)         (FluidSynth)
```

#### Key source files

| File | ~LOC | Role |
|------|------|------|
| `src/mt32pi.cpp` | 2 500 | Main orchestrator: init, loop, MIDI, sequencer, network, state |
| `src/net/webdaemon.cpp` | 3 000 | HTTP server — 5 pages + full REST API |
| `src/net/websocketdaemon.cpp` | 400 | WebSocket — JSON status push every 250 ms (configurable) |
| `src/audiomixer.cpp` | 600 | Dual-engine mix: volume, pan, solo, per-channel gain |
| `src/midirouter.cpp` | 500 | Per-channel routing, remapping, CC filtering, layering |
| `src/fluidsequencer.cpp` | 800 | SMF player wrapping `fluid_player_t` |
| `include/config.def` | — | Single-source config: one `CFG()` line generates INI parser + default |
| `tests/` | — | doctest suite — 125 tests / 1 190 assertions, host-native |

### New features summary

| Feature | Description |
|---------|-------------|
| **Web UI** | 5-page browser interface: Status, Sound, Config, Sequencer, Mixer — dark/light mode, toast notifications, responsive CSS |
| **MIDI Router** | Per-channel routing to MT-32 and/or FluidSynth, with remapping, CC filtering, and visual routing diagram |
| **Audio Mixer** | Independent volume (0–100%) and pan for each synth engine; card-grid UI with smart VU meters |
| **Sequencer** | Built-in SMF Type 0/1 player with loop, auto-next, pause/resume, prev/next, seek memory, and loading indicator |
| **Playlist** | Queue with shuffle and repeat modes |
| **MIDI Monitor** | Real-time 16-channel meters, piano roll, virtual keyboard, SysEx viewer |
| **MIDI Recording** | Record incoming MIDI to `.mid` files on the SD card |
| **MIDI Thru** | Universal MIDI Thru — all physical inputs (Serial/USB/GPIO) forwarded to UART TX |
| **OSC Control** | UDP Open Sound Control receiver with per-engine volume/pan and web UI integration |
| **Audio Effects** | Post-mix effects chain: parametric EQ, reverb, and limiter |
| **Audio Profiling** | Per-component timing stats streamed via WebSocket for performance monitoring |
| **NEON SIMD** | ARM NEON-optimized audio mixer hot path and float→int conversion in `AudioTask` |
| **WebSocket** | Live JSON status stream on port 8765 (configurable port and push interval); skips unchanged frames |
| **HTTP Keep-Alive** | Persistent connections via Circle httpdaemon patch for faster page loads |
| **Live control** | Change all synth parameters at runtime via web UI, REST API, or OSC messages |
| **SF3 support** | Ogg Vorbis-compressed SoundFonts via stb_vorbis (no external libs) |
| **DLS support** | Native DLS soundbank loader (e.g. Windows `gm.dls`, `RLNDGM2.DLS`) |
| **SoundFont Favorites** | Mark preferred SoundFonts via localStorage in the browser UI |
| **SoundFont Preview** | Audition SoundFonts from the web UI before applying |
| **Unit tests** | doctest suite covering MIDI router, audio mixer, MIDI parser, and config parser (125 tests, ~1 190 assertions) |

### Building

> ⚠️ **Only tested on Raspberry Pi 3 (64-bit AArch64).** Other boards are untested and may not work with this fork.

```bash
# Set cross-compiler path (adjust to your toolchain location)
export PATH="$HOME/arm-gnu-toolchain-14.3.rel1-x86_64-aarch64-none-elf/bin:$PATH"

# Build for Raspberry Pi 3 (64-bit)
make BOARD=pi3-64 -j$(nproc)

# Deploy via FTP (once the Pi is running)
curl -T kernel8.img ftp://<pi-ip>/kernel8.img --user mt32-pi:mt32-pi
```

> There are no pre-built releases. You must build from source.

### Running the test suite

```bash
cd tests && make clean && make run
```

The test suite uses [doctest](https://github.com/doctest/doctest) and covers `CMIDIRouter`, `CAudioMixer`, `CMIDIParser`, and the config parser (125 tests, ~1 190 assertions). Tests compile natively — no cross-compiler needed.

### Branch conventions

- `main` — stable, always CI-green; never push directly.
- `feat/<short-name>` — one branch per feature or fix; open a PR to merge.

---
