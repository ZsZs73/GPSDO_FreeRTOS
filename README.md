# GPSDO FreeRTOS v1.05

A GPS-disciplined 10 MHz OCXO on an STM32 BlackPill (F411CE), running FreeRTOS.
Thirteen disciplining algorithms, a time-interval counter with sub-nanosecond
phase readout, TFT/OLED/LED displays, a serial CLI, and wear-levelled flash
storage for learned state.

Project by **J. M. Niewiński** — based on **GPSDO v0.06c** by André Balsa
([STM32-GPSDO](https://github.com/AndrewBCN/STM32-GPSDO)), with the FreeRTOS
port and algorithms 3–12 by the author, Claude Opus 5, GLM-5.3 Max and Qwen3.8-Max as programming assistants, and
PCB design by Scrachi (EEVBlog forum). Algorithm 11 (continuous-PI loop) is
based on the original GPSDO controller design by the late **Lars Walenius**,
extended here with CT auto-calibration, a frequency-led acquisition branch and a
picDIV phase-capture bridge.

Algorithm 12 (multi-level accumulator) is based on the Budget GPSDO by
**Alan Cashin** (MIS42N on the EEVBlog forum), which is also the origin of the
zero-crossing correction, the dithered PWM that reaches 24 bits from a short
one, and the `CS` self-assessment idea. It is implemented here on the LTIC
phase detector rather than a counter.

The algorithm-10 (LTIC 3-stage) concept, and the measurement and field
testing of algorithms 10–12, by **Dan Wiering**, whose
rubidium-referenced ADEV runs shaped both loops.

---


## Documentation

The full documentation lives in [`doc/`](doc/). Everything is maintained in
three languages:

| | English | Polski | Español |
|---|---|---|---|
| **Manual** — hardware, wiring, algorithms, CLI, display | [README_EN](doc/README_EN.md) | [README_PL](doc/README_PL.md) | [README_ES](doc/README_ES.md) |
| **Changelog** — what changed and why | [CHANGELOG_EN](doc/CHANGELOG_EN.md) | [CHANGELOG_PL](doc/CHANGELOG_PL.md) | [CHANGELOG_ES](doc/CHANGELOG_ES.md) |
| **Flash ring bring-up** — first-time setup of the flash ring buffer | [BRINGUP_EN](doc/FLASH_RING_BRINGUP_EN.md) | [BRINGUP_PL](doc/FLASH_RING_BRINGUP_PL.md) | [BRINGUP_ES](doc/FLASH_RING_BRINGUP_ES.md) |

New here? Start with the manual in your language — it covers the build, the
wiring and the first calibration run.

---

## What's new in v1.05

**Algorithm 12 works.** The multi-level accumulator, after Alan Cashin's
(MIS42N) Budget GPSDO, shipped in v1.04 with the arithmetic right and five
separate faults in the machinery around it — each of which hid the next. Fixed
and measured over 23 hours:

| | v1.05 | best previous |
|---|---|---|
| phase RMS, settled | **5–8 ns** | 10–23 ns |
| picDIV re-arms | **1** | 121 |
| 10 000 s frequency | **4e-12** | 1.4e-11 |

Every fix was simulated before it was flashed. The two changes in this project
that went out on reasoning alone were both wrong, and the changelog says which.

**24 bits of control voltage, and the loop now uses them.** A 13-bit PWM whose
duty is dithered from period to period, the table replayed by DMA so it costs no
interrupt — Alan's idea again. The point is the carrier, not the bits: 12.2 kHz
instead of 2 kHz lets the filter's time constant drop from 230 ms to 38 ms, and
filter delay goes straight into the loop as phase lag. v1.04 could not yet steer
with the low 8 bits; v1.05 keeps the fraction of every correction instead of
truncating it, which takes the step from 3.2e-11 to 1.25e-13. On by default now.

**`DAC`** reports the whole output path — which one is compiled in, the code in
24-bit, 16-bit and exact fractional views, the measured Vctl, and the step size
in µHz and in df/f.

**The 320×240 display matches the 480×320.** The two panels had drifted apart
field by field; the small one now shows everything the large one does, in the
same places.

See the [changelog](doc/CHANGELOG_EN.md) for the full list and the reasoning,
including the ideas that were tried, measured and abandoned.

## Quick start

1. **Build** — Arduino IDE with the STM32duino core, board *Generic STM32F4 →
   BlackPill F411CE*. Display and peripherals are selected in
   `gpsdo_config.h`.
2. **Flash** — via ST-Link or DFU.
3. **Connect** — serial at 115200. `H` lists every command.
4. **Calibrate** — `CT` first (~3 min): it measures the oscillator's Hz-per-LSB
   slope and tunes every algorithm from it. Then `LC` to calibrate the phase
   detector. **That order matters** — `LC` without a measured slope falls back to
   a generic value and comes out quietly wrong.
5. **Choose the loop** — `LA 11` for the continuous-PI loop (recommended), or
   `LA 10` for the three-stage one. Gain is derived automatically from `CT`.
6. **Save** — `ES` writes the settings to the flash ring. Preferences save
   themselves; loop tuning needs an explicit `ES` and the reply says which.

The [manual](doc/README_EN.md) covers each of these properly.

---

## Repository layout

```
GPSDO_FreeRTOS.ino      entry point: init, task creation, scheduler start
gpsdo_config.h          display/peripheral selection, pins, constants
gpsdo_tasks.cpp         FreeRTOS tasks: display, sensors, uptime
gpsdo_control.cpp       control loop, calibration (C/CT/LC)
GPSDO_algorithms.cpp    disciplining algorithms 0-11
gpsdo_freq.cpp          frequency counting and averaging windows
gpsdo_gps.cpp           NMEA/UBX parsing, survey-in
gpsdo_isr.cpp           interrupt handlers: 1PPS capture, timer overflow
ubx_timtp.cpp           UBX-TIM-TP sawtooth (qErr) decoding
gpsdo_tz.cpp            timezone resolution (POSIX TZ rules, named zones)
gpsdo_cli.cpp           serial command interface
gpsdo_state.cpp         shared state and persistence wrappers
gpsdo_dac.cpp           control-voltage output: PWM or external DAC
dac_ext.cpp             external SPI DAC — stub, no device chosen yet
gpsdo_health.cpp        correction statistics behind the CS command
flash_ring.cpp          wear-levelled flash storage, settings and live data
settings_store.cpp      the settings block: what is saved and in which group
live_store.cpp          learned drift and damping, saved as they change
TeeSerial.h             mirrors one stream onto USB and Bluetooth together
TM1637Display.cpp       vendored driver, patched for FreeRTOS
tz_table.h              generated: 503 zones, 88 rules (see tools/)
tools/gpsdo_tuner.py    PC tuning console; also regenerates tz_table.h
doc/                    manual, changelog, tuner guide, bring-up (EN/PL/ES)
```

---

## Links

- **Repository** — <https://github.com/jmnlabs/GPSDO_FreeRTOS>
- **Original project** — [STM32-GPSDO](https://github.com/AndrewBCN/STM32-GPSDO)
  by André Balsa
- **Discussion** — the GPSDO thread on the EEVBlog forum
