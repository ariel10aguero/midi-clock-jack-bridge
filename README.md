# MIDI Clock to JACK Transport Bridge

A lightweight C++ program that bridges MIDI clock signals to JACK transport, allowing hardware sequencers, drum machines and other MIDI-clock-capable devices to control tempo in JACK/PipeWire-based audio environments.

---

## Overview

This program listens to incoming MIDI clock messages (via ALSA) and translates them into JACK transport tempo and BBT (Bar/Beat/Tick) information.
It acts as a JACK timebase master, providing stable tempo synchronization for JACK-aware applications.
The tool was specifically created to work seamlessly with Carla as a primary use case, ensuring tight synchronization when using external MIDI-clock hardware with Carla-hosted instruments and plugins.

```
┌─────────────────────────────────────────────────────────────────┐
│                    🎹 EXTERNAL CLOCK SOURCE                     │
│               MIDI Clock Source (HW/DAW)                        │
│               • Sends F8 (clock ticks at 24 PPQN)               │
│               • Optional: FA/FB/FC transport messages           │
└────────────────────────────┬────────────────────────────────────┘
                             │ MIDI Protocol (ALSA)
                             ▼
┌─────────────────────────────────────────────────────────────────┐
│                🔄 MIDI CLOCK SYNC PROGRAM                       │
│                     ** BPM TRANSLATOR **                        │
├─────────────────────────────────────────────────────────────────┤
│  ✓ Opens ALSA MIDI input port                                  │
│  ✓ Listens to MIDI Clock ticks (F8)                            │
│  ✓ Measures time between ticks                                 │
│  ✓ Calculates current BPM from tick intervals                  │
│  ✓ Continuously updates JACK transport BPM                     │
│  ✓ Provides BBT position as JACK timebase master               │
└────────────────────────────┬────────────────────────────────────┘
                             │ Writes BPM to JACK Transport
                             ▼
┌─────────────────────────────────────────────────────────────────┐
│                   🎚️  JACK / PIPEWIRE ENGINE                   │
│  • Current BPM ← continuously updated                           │
│  • BBT time information                                         │
│  • Transport state (rolling/stopped)                            │
└────────────────────────────┬────────────────────────────────────┘
                             │ All clients read shared BPM
                             ▼
┌─────────────────────────────────────────────────────────────────┐
│                        🎛️  CARLA / ARDOUR                      │
│  • Registers as JACK client                                     │
│  • Reads current BPM from JACK transport                        │
│  • Plugins stay in sync with external MIDI clock                │
└─────────────────────────────────────────────────────────────────┘
```

---

## Features

* **MIDI Clock Detection:** Listens for 24 PPQN MIDI clock
* **Adaptive BPM Smoothing:** Intelligent smoothing & stability detection
* **JACK Timebase Master:** Provides tempo, time signature (4/4), BBT
* **Transport Control:** Responds to Start/Stop/Continue messages
* **Auto-start:** Begins JACK transport on first received MIDI clock
* **Realtime Status Reports:** View status using `SIGUSR1`
* **PipeWire Compatible:** Works via `pw-jack`

---

## Requirements

### Runtime

* ALSA
* JACK or PipeWire (with JACK compatibility)
* MIDI clock source (hardware or software)

### Build Dependencies

```bash
sudo apt-get install \
    build-essential \
    libasound2-dev \
    libjack-jackd2-dev \
    pipewire-jack
```

---

## Installation

### 1. Clone Repo

```bash
git clone https://github.com/yourusername/midi-clock-jack-sync.git
cd midi-clock-jack-sync
```

### 2. Build

```bash
chmod +x build.sh
./build.sh
```

Build settings:

* C++17
* `-O3` optimization
* Links ALSA, JACK, pthread, atomic

---

## Usage

### 1. Identify ALSA MIDI Port

```bash
aconnect -l
```

Example:

```
client 24: 'Scarlett 2i4 USB'
    0 'Scarlett 2i4 USB MIDI 1'
```

### 2. Run (PipeWire)

```bash
pw-jack ./midi_clock_sync 24:0
```

Replace `24:0` with your MIDI source port.

---

## Using with Carla

For stable tempo sync, use **Multiple Clients** mode:

1. Open Carla
2. **Configure Carla → Engine**
3. Set **Process Mode: Multiple Clients**
4. Enable **Use JACK Transport**
5. Restart engine
6. Then run:

```bash
pw-jack ./midi_clock_sync <client>:<port>
```

### Recommended Startup Order

1. PipeWire running
2. Run `midi_clock_sync`
3. Launch Carla

---

## Checking Status

Send a `SIGUSR1`:

```bash
kill -USR1 $(pidof midi_clock_sync)
```

Example:

```
╔════════════════════════════════════════╗
║          MIDI CLOCK SYNC STATUS        ║
╠════════════════════════════════════════╣
║ Transport State: ▶  PLAYING            ║
║ JACK BPM:        86.00                 ║
║ Position:        Bar 4, Beat 1         ║
║ Frame:           416768                ║
║ Time Signature:  4.00/4.00             ║
║ Detected BPM:    86.00                 ║
║ Measurements:    16                    ║
║ Current Pos:     4:1:854               ║
╚════════════════════════════════════════╝

