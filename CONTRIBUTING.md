# Contributing to mt32-pi (Extended Edition)

This fork is developed incrementally, one PR per feature, always keeping `main` CI-green.

---

## How to compile

### Prerequisites

You need the **ARM GNU Toolchain 14.3.rel1** (or newer) for your host platform.
Download it from [developer.arm.com](https://developer.arm.com/downloads/-/arm-gnu-toolchain-downloads) and add both triplets to your `PATH`:

```bash
export PATH="$HOME/arm-gnu-toolchain-14.3.rel1-x86_64-aarch64-none-elf/bin:$PATH"
export PATH="$HOME/arm-gnu-toolchain-14.3.rel1-x86_64-arm-none-eabi/bin:$PATH"
```

### Fetch submodules and build dependencies

The first build takes a while — it compiles circle-stdlib, munt and FluidSynth from source:

```bash
make submodules   # initialise git submodules + apply patches + build deps
```

### Build targets

| Command | Output | Target hardware |
|---------|--------|-----------------|
| `make BOARD=pi2 -j$(nproc)` | `kernel7.img` | Raspberry Pi 2 |
| `make BOARD=pi3 -j$(nproc)` | `kernel8-32.img` | Raspberry Pi 3 (32-bit) |
| `make BOARD=pi3-64 -j$(nproc)` | `kernel8.img` | Raspberry Pi 3 (64-bit) ← default |
| `make BOARD=pi4 -j$(nproc)` | `kernel7l.img` | Raspberry Pi 4 (32-bit) |
| `make BOARD=pi4-64 -j$(nproc)` | `kernel8-rpi4.img` | Raspberry Pi 4 (64-bit) |

Append `HDMI_CONSOLE=1` to any target to enable the debug HDMI console build.

### Deploy

Copy the kernel image to the SD card, or use the embedded FTP server once the Pi is running:

```bash
curl -T kernel8.img ftp://<pi-ip>/ --user mt32-pi:mt32-pi
```

### Clean targets

```bash
make clean      # remove only mt32-pi object files and kernel images
make mrproper   # reverse all patches and wipe all build artefacts (full reset)
```

---

## How to run the test suite

Tests compile natively without any cross-compiler:

```bash
make -C tests clean run
```

The suite uses [doctest](https://github.com/doctest/doctest) and covers `CMIDIRouter`, `CAudioMixer`, and `CMIDIParser` (99 tests, ~1 000 assertions). CI runs this automatically on every PR via the `test` job.

---

## Branch conventions

| Branch prefix | Purpose |
|---------------|---------|
| `main` | Always CI-green; never push directly |
| `feat/<name>` | New feature or improvement |
| `fix/<name>` | Bug fix or regression |
| `docs/<name>` | Documentation-only change |
| `ci/<name>` | CI/workflow change |
| `chore/<name>` | Maintenance (formatting, cleanup) |

One branch → one PR. Keep PRs focused; large refactors should be split by concern.

---

## Code style

- **C++**: follow the existing style (Circle conventions — no STL, no RTTI, no exceptions outside Circle wrappers).
- **Indentation**: tabs, width 4.
- **Naming**: `CClassName`, `m_memberVar`, `pPointer`, `nInteger`, `bBool`.
- **No new external dependencies** without prior discussion — this is a baremetal build.

---

## Patch policy

The Makefile applies patches in `patches/` idempotently (only if not already applied):

- `patches/fluidsynth-2.5.3-circle.patch` — adapts FluidSynth for the Circle environment.
- `patches/circle-50-*.patch` — minor fixes to the Circle framework.

If you need to modify a patched file, update the patch instead of committing changes directly to the submodule.

---

## CI

Every PR must pass all three jobs before merging:

| Job | What it checks |
|-----|---------------|
| `lint` | `black`, `isort`, `flake8` on Python scripts; `shellcheck` on shell scripts |
| `test` | Native doctest suite (`make -C tests clean run`) |
| `build` | Cross-compiled kernel for pi2, pi3-64, pi4-64 (with and without HDMI console) |

The `nightly` workflow (manual trigger only) additionally tests patch integrity and does a full `mrproper` + cold build.
