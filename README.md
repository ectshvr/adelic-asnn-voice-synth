# Adelic ASNN Synthesizer Engine

A real-time, ultra-low-latency hybrid voice-reactive synthesizer featuring a **3-adic ($\mathbb{Q}_3$) neural state engine** coupled with a continuous complex phase space ($\mathbb{C}$) and an analog-modelled digital signal processing (DSP) output stage.

Designed for experimental audio generation, non-linear system simulation, and bare-metal DSP architectures requiring emergent behavior without heavy machine learning overhead.

---

## Architectural Overview

The engine bridges two distinct mathematical domains:
1. **Non-Archimedean Field ($\mathbb{Q}_3$):** Utilizes 3-adic integer arithmetic and Hensel division to simulate biological-style ternary neurons ($t_n \in \{-1, 0, +1\}$) acting as discrete state propagators.
2. **Archimedean Field ($\mathbb{C}$):** Manages continuous complex oscillator modulation, phase vectors, and real-time audio-rate transformations.

### Key Subsystems
* **$\mathbb{Q}_3$ Neural Engine:** Executes local state updates via 3-adic valuations, translating microphonic input energy (RMS) into non-linear local field potentials (LFP).
* **16-Step Arpeggiator:** Deterministic sequence generator modulated dynamically by the underlying adadic trit states.
* **24dB Moog-Style Ladder Filter:** A multi-stage digital implementation providing resonant low-pass filtering and dynamic feedback scaling.
* **Duplex Audio Pipeline:** Powered by `miniaudio` with strict device-filtering heuristics to isolate physical USB audio codecs from virtual loopbacks.

---

## Technical Specifications

* **Latency:** Ultra-low (~256 frame buffer size, hardware-dependent duplex stream).
* **Arithmetic:** Integer-driven state transitions combined with optimized floating-point DSP paths.
* **Input Modulation:** Real-time RMS extraction driving filter cutoff frequencies and saturation dynamics.
* **Dependencies:** Standard C++17 libraries, POSIX terminal control, and the single-header `miniaudio` library.

---

## Build & Execution

### Prerequisites
* A C++17 compliant compiler (`g++`, `clang++`)
* POSIX-compliant environment (Linux/macOS)

### Compilation

```bash
g++ -std=c++17 -O2 adelic_asnn_voice_synth.cpp -o adelic_synth -lpthread -ldl -lm
