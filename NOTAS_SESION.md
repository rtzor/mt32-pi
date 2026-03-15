# Notas de sesión - mt32-pi en Raspberry Pi 3 (64-bit) con FluidSynth 2.5.3

## Estado rapido (15 de marzo de 2026)
- Rama de trabajo activa: `feat/midi-mixer-router`.
- Base: `main` en `8ce1480`.
- Toolchain aarch64 validado: `~/arm-gnu-toolchain-14.3.rel1-x86_64-aarch64-none-elf/bin`.
- Build local: `make BOARD=pi3-64 -j$(nproc)` OK (kernel8.img ~1.51MB).
- Despliegue por FTP validado en `192.168.1.88` con credenciales `mt32-pi/mt32-pi`.
- Tests: doctest v2.4.11, 71 tests, 949 assertions pass.

### Estado funcional (ultimo despliegue 15 mar 2026)
- **MIDI Router** (`CMIDIRouter`): enrutamiento por canal a MT-32/FluidSynth.
  - 4 presets: SingleMT32, SingleFluid, SplitGM, Custom.
  - Channel remap (canal entrada → canal salida diferente).
  - Layering (duplicar NoteOn/Off a ambos motores en canales seleccionados).
  - CC filters per-engine (allow/block CCs individuales por motor).
- **Audio Mixer** (`CAudioMixer`): mezcla en float de ambos motores.
  - Volumen y pan independiente por motor.
  - Solo mode (solo un motor activo) / dual mode (ambos).
- **Web UI** — pagina `/mixer`:
  - Preset selector, enable/disable dual mode.
  - Tabla de 16 canales con: Engine, Remap, Layer, **Instrument name**, Activity meter.
  - Volumen/pan sliders por motor.
  - Audio performance monitor (render us, avg, peak, deadline, CPU load %).
  - Meters de actividad por canal via WebSocket en tiempo real.
- **Web UI** — pagina `/` (status):
  - Link al mixer funcional (HTML no escapado).
  - WebSocket: keyboard, piano roll, meters con colores por engine (cyan=MT-32, magenta=FluidSynth).
  - Meter labels muestran engine asignado en modo mixer.
- **Instrument names** en mixer:
  - FluidSynth: `fluid_synth_get_channel_preset()` + `fluid_preset_get_name()` por canal.
  - MT-32: `getPatchName(part)` mapeado via channel→part lookup. Canales sin parte = "—".
  - Respeta channel remap (pregunta al motor por el canal remapeado).
- **Stuck notes fix**: CC 120 + CC 123 al motor antiguo en cambio de engine, AllSoundOff en cambio de preset.
- **Sync Sound ↔ Mixer**: `SetMixerPreset()` sincroniza `m_pCurrentSynth` en presets Single.

### Archivos modificados (vs main 8ce1480)
**Modificados:**
- `Kernel.mk` — link audiomixer.o, midirouter.o
- `include/config.def`, `include/config.h` — opcion MixerEnabled
- `include/lcd/ui.h` — menu items mixer
- `include/mt32pi.h` — TMixerStatus, metodos mixer/router
- `include/synth/mt32synth.h` — GetChannelInstrumentName override
- `include/synth/soundfontsynth.h` — GetChannelInstrumentName override
- `include/synth/synthbase.h` — virtual GetChannelInstrumentName
- `src/config.cpp` — parseo MixerEnabled
- `src/lcd/ui_menu.cpp` — menu mixer entries
- `src/mt32pi.cpp` — mixer control, routing, stuck notes, preset sync, instrument query
- `src/net/webdaemon.cpp` — mixer page, API endpoints, status link fix, instrument column
- `src/net/websocketdaemon.cpp` — engine info + mixer flag en JSON
- `src/synth/mt32synth.cpp` — GetChannelInstrumentName impl
- `src/synth/soundfontsynth.cpp` — GetChannelInstrumentName impl

**Nuevos:**
- `include/audiomixer.h`, `src/audiomixer.cpp` — CAudioMixer
- `include/midirouter.h`, `src/midirouter.cpp` — CMIDIRouter
- `tests/` — unit tests (midirouter, audiomixer, doctest)

### Proximo paso recomendado
- PR desde `feat/midi-mixer-router` hacia `main`.

## Registro de sesiones

### 15 de marzo de 2026 (mixer UX + instrument names)
- **Fix 1**: Link del Mixer en Status — `AppendSectionStart` escapaba el HTML del `<a>`. Reemplazado por HTML directo como hace Sequencer.
- **Fix 2**: Meters de actividad en pagina Mixer — columna "Activity" con barras por canal, WebSocket para updates en tiempo real con animacion attack/decay/peak hold.
- **Fix 3**: Sync Sound ↔ Mixer preset — `SetMixerPreset()` ahora sincroniza `m_pCurrentSynth` cuando el preset es SingleMT32 o SingleFluid, para que la pagina Sound refleje el motor correcto.
- **Feature**: Instrument names por canal en tabla del mixer:
  - Nuevo metodo virtual `CSynthBase::GetChannelInstrumentName(u8 nChannel)`.
  - MT-32: lee mapping MIDI channel→part via `readMemory(MemoryAddressMIDIChannels)`, llama `getPatchName(part)`. Canales sin parte asignada devuelven nullptr (muestra "—").
  - FluidSynth: `fluid_synth_get_channel_preset()` + `fluid_preset_get_name()`.
  - Respeta channel remap: pregunta al motor por el canal remapeado, no el de entrada.
  - Columna "Instrument" en tabla del mixer con texto truncado (max 120px, ellipsis).
  - Campo `"instrument"` en JSON de `/api/mixer/status`.
- Archivos tocados: `synthbase.h`, `mt32synth.h/.cpp`, `soundfontsynth.h/.cpp`, `mt32pi.h`, `mt32pi.cpp`, `webdaemon.cpp`.
- Build OK, deploy FTP OK, verificado en Pi 192.168.1.88.

### 13-14 de marzo de 2026 (mixer infrastructure + web UI)
- **CMIDIRouter**: implementacion completa del router MIDI por canal.
  - 4 presets, channel remap, layering, CC filters per-engine.
  - SysEx routing: Roland→MT-32, universal→both, resto→FluidSynth.
  - Unit tests: 71 tests, 949 assertions.
- **CAudioMixer**: mezcla float stereo de ambos motores.
  - Volumen/pan per-engine, solo mode, master volume.
- **Web UI mixer page** (`/mixer`): preset selector, channel routing table, engine levels, audio performance.
- **WebSocket**: engine info (`"eng":0/1`) y mixer flag (`"mixer":true/false`) por canal.
- **Engine-colored UI**: keyboard, piano roll y meter labels coloreados por engine (cyan=MT-32, magenta=FluidSynth).
- **Stuck notes fix**: CC 120+123 al motor antiguo en `SetMixerChannelEngine()`, `AllSoundOff()` en ambos motores en `SetMixerPreset()`.
- Config: `mixer_enabled` en `[midi]` section.
- Menu LCD: entries para mixer enable/disable.
- Build OK, deploy FTP OK.

### 12 de marzo de 2026 (web runtime + deploy)
- Se movio el control en vivo de audio a pagina dedicada `/sound` (separado de `/config`).
- Se anadieron endpoints runtime:
  - `POST /api/runtime/set`
  - `GET /api/runtime/status`
- Se anadieron tabs `MT-32`/`SoundFont` y secciones condicionales en la UI.
- Se anadieron medidores MIDI en vivo (16 canales) en `/sound`.
- Se extendio la parte MT-32 en web/API con controles de reverb, modos y calidad de render.
- Build validado: `make BOARD=pi3-64` OK.
- Kernel subido por FTP a `192.168.1.88` con `mt32-pi/mt32-pi`.
- Verificado por HTTP que `/api/runtime/status` devuelve los parametros MT-32 nuevos.
- Nota de operacion: el FTP embedded no expone comando de reboot estandar; reiniciar por reset/power-cycle cuando sea necesario.

### 9 de marzo de 2026
- Se desinstalió `gcc-arm-none-eabi` 13.x del sistema (paquete apt incompatible con circle-stdlib)
- Se descargó manualmente `gcc-arm-none-eabi-9-2019-q4` y se extrajo en la raíz del proyecto
  - El tarball `gcc-arm-none-eabi-9-2019-q4-major-x86_64-linux.tar.bz2` sigue en el directorio raíz (~111 MB)
- `make clean && make BOARD=pi3` → **kernel8-32.img compilado OK** (997276 → gzip 494838 bytes)
- Se revertió `external/fluidsynth` a v2.5.3 limpio con `git checkout -- . && git clean -fd && git checkout v2.5.3`
- Submodulo `external/fluidsynth` apunta a HEAD `6b8fabbd` (v2.5.3)
- La Pi sigue funcionando en 192.168.1.100 con FluidSynth + SC-55 SoundFont
- Esta validacion quedo completada posteriormente sobre `main` el 11 de marzo de 2026

---

## Hardware objetivo
- Raspberry Pi 3 (64-bit)
- Salida de audio por jack 3.5mm (I2S desactivado)
- Alimentación: **requiere fuente 5V/2.5A o superior** (aparecen avisos de low voltage con fuentes débiles)
- Red: Ethernet (no WiFi)

---

## Toolchains

### arm-none-eabi (para Pi3 32-bit / circle-stdlib)
- **No usar el paquete apt** — la versión del repositorio Ubuntu (13.x) es incompatible
- Descargar manualmente la versión **9-2019-q4**:
  ```
  wget https://developer.arm.com/-/media/Files/downloads/gnu-rm/9-2019q4/gcc-arm-none-eabi-9-2019-q4-major-x86_64-linux.tar.bz2
  tar -xf gcc-arm-none-eabi-9-2019-q4-major-x86_64-linux.tar.bz2
  ```
- El tarball está extraído en `/home/rautor/Documentos/mt32-pi/gcc-arm-none-eabi-9-2019-q4-major/`
- Activar antes de compilar:
  ```bash
  export PATH=$PWD/gcc-arm-none-eabi-9-2019-q4-major/bin:$PATH
  ```

### aarch64-none-elf (para Pi3 64-bit)
- Ubicación validada en esta máquina: `~/arm-gnu-toolchain-14.3.rel1-x86_64-aarch64-none-elf/bin`
- Activar:
  ```bash
  export PATH="$HOME/arm-gnu-toolchain-14.3.rel1-x86_64-aarch64-none-elf/bin:$HOME/Documentos/mt32-pi/gcc-arm-none-eabi-9-2019-q4-major/bin:$PATH"
  ```

---

## Compilación

### Pi3 32-bit
```bash
export PATH=$PWD/gcc-arm-none-eabi-9-2019-q4-major/bin:$PATH
make clean
make BOARD=pi3
# → genera kernel8-32.img
```

### Pi3 64-bit (el que usa esta Pi)
```bash
export PATH="$HOME/arm-gnu-toolchain-14.3.rel1-x86_64-aarch64-none-elf/bin:$PWD/gcc-arm-none-eabi-9-2019-q4-major/bin:$PATH"
make clean
make BOARD=pi3-64
# → genera kernel8.img
```

> **IMPORTANTE**: El `sdcard/config.txt` tiene `arm_64bit=1` y `kernel=kernel8.img` en la sección `[pi3]`.
> Por tanto la Pi arranca en modo 64-bit y necesita `kernel8.img`, no `kernel8-32.img`.

### Subir kernel a la Pi por FTP
```bash
# Compilar kernel 64-bit
export PATH="$HOME/arm-gnu-toolchain-14.3.rel1-x86_64-aarch64-none-elf/bin:$PWD/gcc-arm-none-eabi-9-2019-q4-major/bin:$PATH"
make clean
make BOARD=pi3-64
# Subir por FTP
curl -T kernel8.img ftp://mt32-pi:mt32-pi@192.168.1.100/SD/kernel8.img
```

### Subir kernel con la SD montada en red
```bash
sudo cp /mnt/red/pi32/kernel8.img /mnt/red/pi32/kernel8.img.bak-$(date +%Y%m%d-%H%M%S)
sudo cp kernel8.img /mnt/red/pi32/kernel8.img
```

Notas:
- Este flujo se ha usado tambien en las pruebas recientes y deja copia de seguridad del kernel anterior.
- Sigue siendo necesario reiniciar la Pi para que cargue el nuevo kernel.

> **Reinicio**: el servidor FTP de mt32-pi **no tiene comando de reboot remoto**.
> Después de subir el kernel hay que **desconectar y reconectar la alimentación** (o pulsar el botón de reset si está conectado al GPIO).
> El nuevo kernel carga en el siguiente arranque.

### Flujo recomendado de validación local

Validación mínima tras un cambio pequeño:
```bash
export PATH="$HOME/arm-gnu-toolchain-14.3.rel1-x86_64-aarch64-none-elf/bin:$PWD/gcc-arm-none-eabi-9-2019-q4-major/bin:$PATH"
make BOARD=pi3-64 -j$(nproc)
curl -T kernel8.img ftp://mt32-pi:mt32-pi@192.168.1.100/SD/kernel8.img
```

Validación fuerte tras cambios de Circle, toolchain, parches o sistema de build:
```bash
export PATH="$HOME/arm-gnu-toolchain-14.3.rel1-x86_64-aarch64-none-elf/bin:$PWD/gcc-arm-none-eabi-9-2019-q4-major/bin:$PATH"
make mrproper
make BOARD=pi3-64 -j$(nproc)
curl -T kernel8.img ftp://mt32-pi:mt32-pi@192.168.1.100/SD/kernel8.img
```

Checklist recomendada despues del reboot real de la Pi:
- `ping 192.168.1.100`
- acceso FTP correcto
- prueba UDP MIDI a `192.168.1.100:1999`
- comprobar sintesis SoundFont y MT-32 segun la configuracion activa

---

## FluidSynth 2.5.3

### Cambios respecto al original (2.3.1)
El proyecto original usaba FluidSynth 2.3.1. Se actualizó a 2.5.3 con los siguientes cambios:

1. **Submodulo**: apunta a tag `v2.5.3` del repo upstream
2. **Patch**: `patches/fluidsynth-2.5.3-circle.patch` (reemplaza al anterior `fluidsynth-2.3.1-circle.patch`)
3. **Makefile** (`Makefile`): cambios en target `fluidsynth`:
   - Patch actualizado a `fluidsynth-2.5.3-circle.patch`
   - Añadido `-Dosal=embedded` (requerido en 2.5.x)
   - Añadido `CXXFLAGS` junto a `CFLAGS`
   - Añadido `-DCMAKE_CXX_FLAGS_RELEASE`
   - Añadido `-Denable-native-dls=OFF` (nuevo en 2.5.x)
   - Cambiado `-Denable-sdl2=OFF` → `-Denable-sdl3=OFF`

### Parche clave en fluid_sys_embedded.h
En `external/fluidsynth/src/utils/fluid_sys_embedded.h`, `fluid_stat` debe ser silenciado para evitar ruido en logs:
```c
// Cambiar STUB_FUNCTION → STUB_FUNCTION_SILENT:
STUB_FUNCTION_SILENT(fluid_stat, int, -1, (const char *path, fluid_stat_buf_t *buffer))
```
Este cambio está incluido en `patches/fluidsynth-2.5.3-circle.patch`.

### Aplicar el patch manualmente (si hace falta recompilar desde cero)
```bash
cd external/fluidsynth
git checkout v2.5.3
cd /home/rautor/Documentos/mt32-pi
# El Makefile aplica el patch automáticamente al compilar
make BOARD=pi3-64
```

---

## Configuración de la Pi (mt32-pi.cfg)

Fichero en SD: `/SD/mt32-pi.cfg` (accesible por FTP en `ftp://mt32-pi:mt32-pi@192.168.1.100/SD/mt32-pi.cfg`)

Valores clave cambiados respecto al default:
```ini
verbose = on            # para ver logs por HDMI (útil para debug)
default_synth = soundfont
mode = ethernet         # era: off
ftp = on                # era: off
soundfont = 9           # SC-55 SoundFont v1.2a1.sf2 (índice alfabético)
```

### SoundFonts en la SD (índice 0-based, orden alfabético)
| # | Archivo |
|---|---------|
| 0 | 2MBGMGSMT.sf2 |
| 1 | 4MBGMGSMT.sf2 |
| 2 | 8MBGMGS.sf2 |
| 3 | AWE ROM GM.sf2 |
| 4 | AWE32.sf2 |
| 5 | GeneralUser GS v1.511.sf2 |
| 6 | GM.sf2 |
| 7 | GUS.sf2 |
| 8 | Roland JV-1010.sf2 |
| 9 | SC-55 SoundFont v1.2a1.sf2 |

> **SC-55 (índice 9)**: el más auténtico para música de los años 90 (DOOM, etc.)

### Cambiar SoundFont por FTP sin tocar la SD físicamente
```bash
curl -s ftp://mt32-pi:mt32-pi@192.168.1.100/SD/mt32-pi.cfg -o /tmp/mt32-pi.cfg
sed -i 's/^soundfont = [0-9]*/soundfont = 9/' /tmp/mt32-pi.cfg
curl -s -T /tmp/mt32-pi.cfg ftp://mt32-pi:mt32-pi@192.168.1.100/SD/mt32-pi.cfg
# Luego reiniciar la Pi
```

---

## Red y acceso

- **IP de la Pi**: `192.168.1.100`
- **FTP**: puerto 21, usuario/contraseña `mt32-pi`/`mt32-pi`
- **FTP root**: `/SD/` → raíz de la tarjeta SD
- **UDP MIDI**: puerto 1999

### Verificar acceso
```bash
ping 192.168.1.100
curl ftp://mt32-pi:mt32-pi@192.168.1.100/SD/
```

---

## Scripts utiles del repo

Los scripts auxiliares estan en `scripts/`:

- `scripts/mt32pi_installer.sh`
  - Instalador interactivo para una SD nueva.
  - Enfocado a primera instalacion, especialmente util en MiSTer o Linux.

- `scripts/mt32pi_updater.py`
  - Actualizador por FTP para un mt32-pi ya desplegado.
  - Requiere red y FTP activos en la Pi.

- `scripts/mt32pi_updater.sh`
  - Wrapper para lanzar el updater en entornos tipo MiSTer.

- `scripts/mt32pi_updater.cfg`
  - Configuracion de host/credenciales para el updater.

Referencia general: `scripts/README.md`

---

## Enviar MIDI por UDP (python3-mido)

### Instalar (solo una vez)
```bash
sudo apt install python3-mido
```

### Enviar un fichero MIDI
```python
python3 - << 'EOF'
import mido, socket
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
mid = mido.MidiFile('/ruta/al/fichero.mid')
print(f"Reproduciendo ({mid.length:.1f}s)...")
for msg in mid.play():
    if not msg.is_meta:
        sock.sendto(bytearray(msg.bytes()), ('192.168.1.100', 1999))
print("Fin.")
EOF
```

---

## Estado de submodulos

| Submodulo | Versión actual | Última disponible |
|-----------|---------------|-------------------|
| fluidsynth | v2.5.3 | v2.5.3 ✅ |
| munt | munt_2_7_0 | munt_2_7_0 ✅ |
| circle-stdlib | v19.1 (Circle Step50.1 / 5d819ab2) | v19.1 ✅ |
| inih | r62 | r62 ✅ |

---

## Git / rama de trabajo

- **Fork**: `https://github.com/rtzor/mt32-pi`
- **Rama principal actual**: `main`
- **Estado actual**: `origin/main` y `main` alineados en `4867f78`
- **PRs mergeadas relevantes**:
  - `#6` (`chore/step50-circle-v19.1`)
  - `#7` (`test/toolchain-aarch64-14.3-rel1`)
  - `#8` (`chore/ci-modernize-fork-releases`)
- La actualizacion a Circle Step50.1 (`5d819ab2` en `external/circle-stdlib/libs/circle`) quedo integrada en `main` con:
  - `Makefile`
  - `patches/circle-50-minimal-usb-drivers.patch`
  - `patches/circle-50-cp210x-remove-partnum-check.patch`
  - `external/circle-stdlib` → `v19.1`
- Rama de trabajo de UI creada y subida, pendiente de merge en `main`:
  - `feature/live-mt32-ui-controls`
  - commit punta: `98ac5ba`
  - PR a abrir/usar: `https://github.com/rtzor/mt32-pi/pull/new/feature/live-mt32-ui-controls`

### Para futuros clones
```bash
git clone https://github.com/rtzor/mt32-pi
cd mt32-pi
git submodule update --init --recursive
```

### Activar toolchain y compilar
```bash
export PATH="$HOME/arm-gnu-toolchain-14.3.rel1-x86_64-aarch64-none-elf/bin:$PWD/gcc-arm-none-eabi-9-2019-q4-major/bin:$PATH"
make BOARD=pi3-64
curl -T kernel8.img ftp://mt32-pi:mt32-pi@192.168.1.100/SD/kernel8.img
```

---

## Problemas conocidos

| Problema | Causa | Solución |
|----------|-------|----------|
| `arm-none-eabi-g++: No existe el archivo` | PATH no tiene el toolchain | `export PATH=$PWD/gcc-arm-none-eabi-9-2019-q4-major/bin:$PATH` |
| `make clean` necesario antes de recompilar | Stale objects de compilación previa | `make clean && make BOARD=...` |
| Pantalla negra en Pi3 | `config.txt` pide `kernel8.img` (64-bit) pero se compiló 32-bit | Compilar con `BOARD=pi3-64` |
| SF2 no suena | `default_synth = mt32` en cfg | Cambiar a `default_synth = soundfont` |
| FTP no accesible | `mode = off` o `ftp = off` en cfg | `mode = ethernet`, `ftp = on` |
| Log "fluid_stat is a stub" | `STUB_FUNCTION` en lugar de `STUB_FUNCTION_SILENT` | Ya corregido en el patch 2.5.3 |
| Low voltage warning | Fuente de alimentación insuficiente | Cambiar a fuente 5V/2.5A o superior |
| Hostname corrupto (`mt32-pish1106_i2c`) | Bug cosmético del firmware | No afecta al funcionamiento |
| **Pi se cuelga al primer MIDI** | `USE_PWM_AUDIO_ON_ZERO` en Config.mk | Eliminar del Makefile — es solo para Pi Zero 2 W (GPIO12/13), en Pi 3 el jack 3.5mm usa GPIO40/45 |

---

## 11 de marzo de 2026 - depuracion Step50 en Pi 3 AArch64

### Sintoma final observado
- Con Circle Step50 el arranque llegaba a inicializar SD, USB, SoundFonts y multicore, pero la Pi quedaba congelada durante el bucle principal.
- Con UART activado (`enable_uart=1` y `logdev=ttyS1 loglevel=4 fast=true`) se vio que el bloqueo ocurria en `UpdateNetwork()`.

### Diagnostico
- El LCD/HDMI no eran canales fiables para depurar este fallo; la UART si.
- El bloqueo real no estaba en `Awaken()` ni en el arranque de audio.
- En Pi 3, usando Ethernet via USB (`smsc951x`), las comprobaciones repetidas de estado de red en el hilo principal quedaban atrapadas al migrar a Circle Step50.
- `CNetSubSystem`/Circle ya gestionan la pila de red en su propio `CNetTask`; no hace falta sondear el estado del enlace Ethernet de forma bloqueante en cada iteracion del `MainTask()`.

### Cambio aplicado
- En `src/mt32pi.cpp`, `UpdateNetwork()` se dejo no bloqueante para Ethernet:
  - Si `dhcp = on`, la red se considera lista cuando la interfaz ya tiene IP asignada.
  - Si `dhcp = off`, la red se considera lista desde el inicio.
  - Wi-Fi sigue usando `m_pNet->IsRunning()` como antes.
- Se retiraron las trazas temporales de depuracion (`Core 0 stalled`, logs extra en `Awaken()`, mensajes `SF scan/settings/create/load`).

### Resultado verificado
- El sistema vuelve a arrancar completo.
- Se levantan correctamente los servicios de red:
  - IP `192.168.1.100`
  - AppleMIDI
  - UDP MIDI puerto `1999`
  - FTP
- Se verifico reproduccion MIDI real por UDP enviando notas sueltas y `StarWars.mid` desde la VM.

### Comandos utiles de verificacion
```bash
ping 192.168.1.100
python3 - << 'EOF'
import mido, socket
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
mid = mido.MidiFile('StarWars.mid')
for msg in mid.play():
    if not msg.is_meta:
        sock.sendto(bytearray(msg.bytes()), ('192.168.1.100', 1999))
EOF
```

### 11 de marzo de 2026 - validacion limpia sobre main

#### Estado de git
- `main` local y `origin/main` quedaron alineados primero en `0dfda56`; actualmente estan alineados en `4867f78`
- La rama temporal de PR `chore/step50-circle-v19.1` ya fue borrada en local y remoto tras el merge
- Se oculto localmente `sdcard/mt32-pi.cfg` con `skip-worktree` para que el arbol de trabajo quede limpio durante pruebas

#### Build limpio validado
Comando ejecutado:
```bash
export PATH="$HOME/arm-gnu-toolchain-14.3.rel1-x86_64-aarch64-none-elf/bin:$PWD/gcc-arm-none-eabi-9-2019-q4-major/bin:$PATH"
make clean
make BOARD=pi3-64 -j$(nproc)
```

Resultado:
- Build limpio completado correctamente desde `main`
- `make` reporto:
  - `kernel8.img => 1372752`
  - `gzip kernel8.img => 661985`
- Artefacto final en disco tras el build: `kernel8.img` = `661985` bytes

#### Estado pendiente real
- Hacer prueba funcional en hardware con reinicio completo de la Pi despues de subir el kernel
- Verificar en la Pi real que siguen funcionando:
  - Ethernet `192.168.1.100`
  - FTP
  - UDP MIDI (`1999`)
  - Sintesis SoundFont y MT-32 segun configuracion activa

#### Validacion con `mrproper`
Comando ejecutado:
```bash
export PATH="$HOME/arm-gnu-toolchain-14.3.rel1-x86_64-aarch64-none-elf/bin:$PWD/gcc-arm-none-eabi-9-2019-q4-major/bin:$PATH"
make mrproper
make BOARD=pi3-64 -j$(nproc)
```

Resultado:
- Reconstruccion completa correcta desde cero
- `make` reporto:
  - `kernel8.img => 1372736`
  - `gzip kernel8.img => 661990`
- Se subio el `kernel8.img` resultante por FTP a `ftp://mt32-pi:mt32-pi@192.168.1.100/SD/kernel8.img`
- Se envio `StarWars.mid` por UDP MIDI a `192.168.1.100:1999` (`10182` mensajes MIDI no-meta enviados)

---

## 11 y 12 de marzo de 2026 - controles live de UI para MT-32 y SoundFont

### Trabajo implementado
- Se ampliaron los controles de UI para exponer parametros live en MT-32 directamente sobre `MT32Emu::Synth`.
- Se añadieron 8 controles live en MT-32 sin reabrir el sintetizador:
  - `Output Gain`
  - `Reverb Gain`
  - `Reverb Enable`
  - `NiceAmpRamp`
  - `NicePanning`
  - `NicePartialMixing`
  - `DAC Input Mode`
  - `MIDI Delay Mode`
- Se añadieron 3 parametros MT-32 que si requieren reapertura segura del core:
  - `Analog Output Mode`
  - `Renderer Type`
  - `Partial Count`
- Se ampliaron tambien los controles paralelos del menu para SoundFont:
  - `Gain`
  - `Reverb Damping`
  - `Reverb Width`
  - `Chorus Level`
  - `Chorus Voices`
  - `Chorus Speed`

### Implementacion tecnica
- `CMT32Synth` gano setters/getters thread-safe para todos los parametros nuevos.
- Se implemento `ReopenCurrentROMSet()` para reabrir el motor MT-32 con rollback si falla la reapertura.
- El menu LCD paso a exponer 12 items para SoundFont y 12 items para MT-32.
- Se actualizaron `EnterMenu()`, `DrawMenu()`, `FormatMenuValue()` y `MenuEncoderEvent()` para capturar, mostrar y editar todos los nuevos estados.
- Los cambios de esta rama afectan exactamente a:
  - `include/lcd/ui.h`
  - `include/synth/mt32synth.h`
  - `include/synth/soundfontsynth.h`
  - `src/lcd/ui.cpp`
  - `src/lcd/ui_menu.cpp`
  - `src/synth/mt32synth.cpp`
  - `src/synth/soundfontsynth.cpp`

### Verificacion realizada
- El build de `kernel8.img` compilo correctamente tras los cambios.
- El kernel se desplego en la Pi real.
- Se verifico conectividad posterior en `192.168.1.100`.
- Se envio `StarWars.mid` por UDP MIDI a `192.168.1.100:1999` como prueba funcional de extremo a extremo.

### Rama y PR
- Inicialmente estos cambios se hicieron por error sobre `chore/ci-modernize-fork-releases`.
- Despues se movieron correctamente a una rama dedicada creada desde `main`:
  - `feature/live-mt32-ui-controls`
- La rama fue publicada en remoto y contiene el commit:
  - `98ac5ba` - `feat: implement live MT-32 parameter controls and menu integration`
- Estado actual:
  - `main` no contiene todavia estos cambios de UI.
  - Los cambios estan listos para PR/merge desde `feature/live-mt32-ui-controls` hacia `main`.

