# OSC — Open Sound Control

mt32-pi incluye un servidor OSC sobre UDP que permite controlar el sintetizador y enviar eventos MIDI desde cualquier dispositivo en la red local.

## Activación

En `mt32-pi.cfg`, sección `[network]`:

```ini
[network]
osc     = on      # off por defecto
osc_port = 8000   # puerto UDP (por defecto 8000)
```

También desde la web UI en `/config → Network → OSC`.

Reiniciar para que los cambios surtan efecto.

---

## Protocolo

OSC sobre UDP (datagrama, sin confirmación). Cada datagrama puede ser:

- **Mensaje** — empieza por `/`
- **Bundle** — empieza por `#bundle\0`, contiene uno o más mensajes con timestamp

Se soportan los tipos de argumento: `i` (int32), `f` (float32), `s` (string), `b` (blob), `T`, `F`, `N`.

---

## Mensajes soportados

### MIDI — `/midi/*`

Todos los mensajes MIDI se inyectan directamente en el pipeline de audio (mismo camino que MIDI hardware o red).

Los canales van de **0 a 15** (canal MIDI 1 = 0).

| Dirección | Tipos | Descripción |
|---|---|---|
| `/midi/note_on` | `iii` ch note vel | Note On — canal, nota (0–127), velocidad (0–127) |
| `/midi/note_off` | `iii` ch note vel | Note Off — canal, nota (0–127), velocidad (0–127) |
| `/midi/cc` | `iii` ch cc val | Control Change — canal, controlador (0–127), valor (0–127) |
| `/midi/program_change` | `ii` ch pg | Program Change — canal, programa (0–127) |
| `/midi/pitch_bend` | `ii` ch val | Pitch Bend — canal, valor 14 bit (0–16383, centro = 8192) |
| `/midi/raw` | `b` blob | Bytes MIDI crudos — útil para SysEx u otros mensajes |

### Control — `/mt32pi/*`

| Dirección | Tipos | Descripción |
|---|---|---|
| `/mt32pi/volume` | `i` 0–127 | Volumen maestro |
| `/mt32pi/synth` | `s` | Cambiar sintetizador activo: `"mt32"` o `"soundfont"` |
| `/mt32pi/soundfont` | `i` | Cambiar SoundFont por índice |
| `/mt32pi/sequencer/play` | `s` filename | Cargar y reproducir archivo MIDI de la SD |
| `/mt32pi/sequencer/stop` | — | Detener reproducción |
| `/mt32pi/sequencer/pause` | — | Pausar reproducción |
| `/mt32pi/sequencer/resume` | — | Reanudar reproducción |
| `/mt32pi/sequencer/next` | — | Siguiente canción de la playlist |
| `/mt32pi/sequencer/prev` | — | Canción anterior de la playlist |

---

## Ejemplos con `oscsend`

```bash
# Volumen maestro al 80%
oscsend 192.168.1.88 8000 /mt32pi/volume i 80

# Note On en canal 0, nota C4 (60), velocidad 100
oscsend 192.168.1.88 8000 /midi/note_on i 0 i 60 i 100

# Note Off
oscsend 192.168.1.88 8000 /midi/note_off i 0 i 60 i 0

# Control Change: Sustain (CC 64) on en canal 0
oscsend 192.168.1.88 8000 /midi/cc i 0 i 64 i 127

# Program Change: programa 40 en canal 0
oscsend 192.168.1.88 8000 /midi/program_change i 0 i 40

# Pitch Bend centrado en canal 0
oscsend 192.168.1.88 8000 /midi/pitch_bend i 0 i 8192

# Cambiar a SoundFont
oscsend 192.168.1.88 8000 /mt32pi/synth s soundfont

# Cambiar a SoundFont índice 2
oscsend 192.168.1.88 8000 /mt32pi/soundfont i 2

# Reproducir un archivo MIDI
oscsend 192.168.1.88 8000 /mt32pi/sequencer/play s /midi/song.mid

# Pausa / reanuda / siguiente / anterior
oscsend 192.168.1.88 8000 /mt32pi/sequencer/pause
oscsend 192.168.1.88 8000 /mt32pi/sequencer/resume
oscsend 192.168.1.88 8000 /mt32pi/sequencer/next
oscsend 192.168.1.88 8000 /mt32pi/sequencer/prev
oscsend 192.168.1.88 8000 /mt32pi/sequencer/stop
```

> `oscsend` es parte del paquete **liblo** (`sudo apt install liblo-tools` en Debian/Ubuntu).

---

## Clientes OSC recomendados

| Cliente | Plataforma | Notas |
|---|---|---|
| `oscsend` / `oscdump` | Linux/macOS | CLI, parte de liblo |
| [TouchOSC](https://hexler.net/touchosc) | iOS / Android / Desktop | Interfaz gráfica configurables |
| [Pure Data](https://puredata.info/) | Linux/macOS/Win | Entorno de parcheo, objeto `[udpsend]` |
| [Max/MSP](https://cycling74.com/) | macOS/Win | Entorno de parcheo comercial |
| [ossia score](https://ossia.io/) | Linux/macOS/Win | Secuenciador de show, soporte OSC nativo |

---

## Notas técnicas

- El servidor OSC escucha en **todos los interfaces** de red activos.
- Los mensajes se procesan en el task de red (Core 0), igual que UDP MIDI y RTP-MIDI.
- El tamaño máximo de datagrama aceptado es **1472 bytes** (MTU Ethernet estándar menos cabeceras IP+UDP).
- Los bundles OSC se desempaquetan recursivamente; el timestamp se ignora (procesamiento inmediato).
- No hay respuesta de confirmación (OSC es fire-and-forget sobre UDP).
