# Changelog — GPSDO FreeRTOS

**English** | [Polski](CHANGELOG_PL.md) | [Español](CHANGELOG_ES.md)

📖 [Project home](../README.md) · Back to [README](README_EN.md)

All notable changes to this project are documented here.

Project by **J. M. Niewiński** — <https://github.com/jmnlabs/GPSDO_FreeRTOS>
Based on **GPSDO v0.06c** by André Balsa
(<https://github.com/AndrewBCN/STM32-GPSDO>), FreeRTOS port and algorithms
3–10 by the author, with Claude AI as programming assistant and PCB design
by Scrachi (EEVBlog forum).

The version suffix `-rtos` marks the FreeRTOS port lineage.

---

## [v1.05-rtos] — 2026-08-20

Algorithm 12 made to work. It was shipped in v1.04 with the arithmetic right and
five separate faults in the machinery around it, each of which hid the next. The
loop now holds phase to 5–8 ns RMS over a 23-hour run with a single picDIV
re-arm, against 10–23 ns for the best previous reference — and every fix below
was simulated before it was flashed, because the two changes in this project that
went out on reasoning alone were both wrong.

### Fixed
- **The noise estimator could only ever fall.** The outlier gate was
  `dp_lim = 5*sigma`, read from the estimate it was feeding: once sigma was
  small, every difference large enough to raise it was rejected as an outlier.
  Measured on 14.08 — `sig` read exactly 2 ns for all 1020 samples of a run,
  and with the per-level limits derived from it the hierarchy pinned at the
  100-unit floor: 79 of 80 corrections fired at level 0. A multi-level
  accumulator that never leaves level 0 is not one. The gate is now absolute
  (300 ns), and the genuine outliers it existed to catch — differences taken
  across a NOPH/SYNC/re-arm gap — are excluded structurally by a contiguity flag
  instead of statistically. Sigma is floored at 5 ns, below which this detector
  cannot honestly resolve.
- **The frequency term had the wrong sign.** It carried `+polarity`, copied from
  algorithm 11's frequency branch — but that branch reads TIM2, and this
  firmware's own algo-11 comment records the hardware finding that TIM2 and the
  LTIC detector have opposite orientation on this wiring. The slope `f_nss` is
  not a TIM2 reading: it is the derivative of the same accumulator values that
  produce the phase term, from the same sensor. A quantity and its own time
  derivative, measured by one sensor, cannot need opposite feedback signs.
  Alan's `cvPWM` agrees — it pushes phase and slope through one conversion and
  adds them. With the plant measured rather than assumed (+319.5 µHz/LSB, from
  regressing the 100 s mean of PWM against the printed 100 s frequency average,
  correlation 0.999 at zero lag) the old sign worked out to `d(phase_rate) =
  +0.4*f_ns`. That is positive feedback.
- **`s_mla_wait` was never reset.** It appeared exactly twice in the file, at its
  declaration and at the `++` in the give-up test, and never went back to zero.
  So about five minutes into any run it passed 300 and the give-up test fired on
  the same second the flag was raised — which killed both things that flag gates:
  the zero-crossing correction and the suppression of new corrections while a
  slew is still walking the phase home. The run that found it: `zc` = 7 in 76
  minutes, all inside the first five, and 1072 of 1174 corrections exactly two
  seconds apart, which is the bare level-0 cadence with nothing holding it back.
  The zero-crossing mechanism had therefore never worked beyond the opening
  minutes of any run since it was introduced.
- **The FLL rescue was a bang-bang loop and could not have been anything else.**
  Its step was `-f*lsb_per_hz*0.10` clamped to ±64, which saturates at
  |f| = 0.256 Hz, while the gate below it only opened at 0.3 Hz — so the
  proportional part could never act. Driven once a second from a 100 s average,
  about 50 s of lag, that gives 64 LSB/s × 50 s = 3200 LSB of travel before the
  measurement responds: 1.0 Hz of overshoot. Both numbers are in the logs
  (462 of 655 consecutive steps exactly ±64; PWM sweeping 12 845 LSB; f100
  swinging −1.29 to +1.55 Hz). It now applies the whole computed correction once
  and holds off for as long as the average it came from needs to refresh, at two
  speeds: the 10 s average while the error is large, the 100 s average once it is
  small, with the hold-off always matching the window in use. The gate moved from
  0.3 Hz to 0.05 Hz, because above 0.147 Hz the phase crosses the whole ±940 ns
  detector band inside one 64 s horizon — the old gate left a dead zone from
  0.147 to 0.3 Hz in which the phase loop could not get a long enough look and
  the FLL considered its work done.
- **`instant_offset` wrapped.** `FREQ_LOWER`/`FREQ_UPPER` admit ±500 Hz and the
  field was `int8_t`, so anything past ±127 fed garbage to every gate that read
  it. Now `int16_t`, in all three files that touch it — the struct, the cast that
  fills it, and the snapshot that copies it. Fixing only one would have compiled
  cleanly and left the wrap in place.
- **The frequency and FLL branches took their sign from a hardcoded minus.**
  Correct only because `LPOL` is −1 on this board; positive feedback on an
  `LPOL +1` board. Both now take the board's `polarity`, which evaluates
  identically here and correctly elsewhere.
- **The dither wrote only one of its two DMA tables.** Double buffering
  alternates every pass, so the other table — still holding the previous code —
  replayed until the next write, and the output flipped between old and new code
  at about 3 Hz (one pass is 2^(24−N) carrier periods = 167.8 ms however N is
  chosen). A 0.8 Hz two-pole filter attenuates 3 Hz by only 14×. Both tables are
  now filled, under a mutex, because `pwm24_write()` is reached from both
  ControlTask and CliTask and two concurrent fills of one table interleave into a
  torn 168 ms replay. Writing the table the DMA is reading is safe by overtaking:
  the fill writes an entry every few microseconds where the DMA consumes one
  every 81.9 µs.
- **`PO` and `AO` could not be set to zero.** The range check was
  `v >= −3000 && v <= 3000 && v != 0.0f`, so the one value a user is most likely
  to want was the one value rejected. Ranges corrected to ±5000 Pa and ±3000 m,
  and both now carry their units in the help and in the echo.

- **The board did not always come up from cold, and the 3.3 V rail was never the
  reason.** `ubx_poll_svin_nav()` called `vTaskDelay()` unconditionally. Its
  sibling `ubx_poll_svin()` carries the guard and the comment explaining it —
  *"before vTaskStartScheduler() this must not be vTaskDelay(): calling it with
  no scheduler hangs the system"* — and the fix went into one of the two and not
  the other.

  Before the scheduler exists, `vTaskDelay()` writes through `pxCurrentTCB`,
  which is still NULL, so the board takes a hard fault and the default handler
  spins with interrupts off: no output, no watchdog, nothing but the reset
  button. The FreeRTOS failure hooks added in v1.04 cannot catch it — they need
  a running kernel.

  It hid behind the call order in `gpsdo_gps_init()`: NAV-SVIN is polled only
  when TIM-SVIN did not answer inside its 500 ms window. A receiver that is
  already running answers, and the board boots — which is the case after a
  reset, because the receiver keeps its own power. One still starting up does
  not, and the board stops dead. That is the cold power-on case, and that
  asymmetry is why this looked for a long time like a supply sagging as the rail
  came up.

  Found in a log with four consecutive boots, each ending after
  `UBX: CFG-NAV5 ACK` and before `LEA-T: starting survey-in`, with the reset
  cause reading `PIN/NRST` every time — the operator's button. The decoder
  prints `POWER-ON/BROWN-OUT` and a "check the 3V3 rail" line when the supply is
  at fault, and it never appeared once. A supply fault does not stop at the same
  source line four times.
- **The frequency digits did not follow algorithm 12's lock.** The colour logic
  has an authoritative branch for loops that publish a live state, and algorithm
  12 was not in the list — so it fell through to the frequency-average branch,
  which is precisely what the comment above that branch says green must not be.

  Measured over a 2.99 h run: the loop and the colour disagreed on **15.0%** of
  samples, and every one of those was the loop LOCKED with the digits WHITE,
  never the reverse. The loop reached LOCK at 108 s and the digits went green at
  1041 s — fifteen minutes of a disciplined oscillator looking undisciplined, on
  every run, because until the 1000 s average fills there is nothing for that
  branch to judge by and `locked` is false by construction.

  Algorithm 12 now takes its colour from its own trend, like 10 and 11. `CORR`
  and `ZC` count as locked: they are one-second states meaning the loop is doing
  its job, the same reasoning that keeps them from resetting `s_mla_quiet`.
  Without that the digits would blink white once per correction — sixteen times
  in the three hours measured. Agreement is now 100%.
- **The stale-echo guard was set to half a count.** It withdraws a long-average
  lock when the 10 s average has drifted, and the threshold was ±50 mHz. But the
  10 s average is a cycle count over ten seconds, so its granularity is 0.1 Hz:
  across three hours it took exactly three values — −100, 0 and +100 mHz — and
  nothing between. A 50 mHz threshold therefore did not mean "within 50 mHz", it
  meant "the counter must read exactly 10 000 000", and one count either way
  killed the green. That is 8.3% of settled samples and 46% of the disagreement
  above. Now ±0.15 Hz: one full count plus half a count of margin, so a
  single-count wobble passes and a real loss of discipline — many counts, which
  is what the guard exists for — still fails it. Algorithms 0-9 carry the same
  guard and get the same fix.

### Added
- **A level gate on the frequency term.** The slope's own noise is
  `sd(f_nss) = sigma * 2^((1−3L)/2)`, so at level 0 it is 1.41·sigma of pure
  noise scaled by 12.5 LSB per ns/s, against the phase term's 0.39 LSB per ns —
  a 32:1 noise-to-signal advantage for the wrong quantity. The log showed what it
  bought: 46% of corrections slammed into the ±470 clamp, one of them with the
  phase reading exactly 0 ns and the correction at full scale. The term is now
  used from level 3 upward, where the same estimate is averaged over 16 s pairs
  and is a measurement again.
- **A TIM2 frequency trim.** When the 100 s average shows more than 0.03 Hz, the
  correction's frequency component is taken from that measurement instead of the
  accumulator slope. It is dormant in steady state — in a 10 h simulation it
  never fired — and that is the point: it catches the frequency excursions that
  would otherwise ramp the phase out of the detector band, so the loop never has
  to re-acquire. The 23 h run that established the numbers above recorded a
  single picDIV re-arm, against 121 for the same settings without it.
- **`configUSE_MUTEXES` and `INCLUDE_xTaskGetSchedulerState`**, set explicitly.
  The dither lock needs both, neither was set by this project, and whether the
  library default enables them is not a thing to leave to chance: one missing
  macro is a build error rather than a runtime surprise.
- **The dither's low 8 bits now reach the loop.** v1.04 shipped the 24-bit
  output and said, in this changelog, that it did not yet give the loop finer
  steps: every algorithm called `gpsdo_dac_write16()`, which left-shifted into
  the top 16 bits so existing settings kept their voltage, and the low byte was
  always zero. It is not any more.

  The fraction is owned by `gpsdo_dac.cpp`, not by the control loop, and that is
  the whole design. The control value is written from 21 places — the `CT` and
  `LC` sweeps, the acquisition ramps, holdover steering, `SP`, and the loop
  itself — and twenty of them are deliberately coarse: a sweep that lands on
  30720.4 instead of 30720 is not a better sweep, it is one whose reference point
  nobody can state. Every coarse write clears the fraction as a side effect of
  arriving at `gpsdo_dac_write16()`, so no caller has to remember to. Keeping the
  fraction in the loop instead would have meant twenty places that each had to
  know to reset it, which is the same class of bug the single write point was
  introduced to prevent.

  What it buys, on the plant measured here: one 16-bit step is about 320 µHz,
  which is 3.2e-11 of 10 MHz — coarser than the 4e-12 the loop was measured
  holding over 10 000 s. It reached that by dithering between adjacent codes from
  one correction to the next, which works but leaves the control voltage hunting.
  With the fraction kept, a correction smaller than one step is applied instead
  of being truncated away, and the step becomes 1.25e-13.

  The truncation it removes was also biased: `(int32_t)` rounds toward zero, so
  every correction lost part of itself in the same direction — which reads to the
  loop as a gain error of up to a sixth at the 6-LSB corrections seen in normal
  operation.

  Nothing above the DAC layer changed. `gpsdo_dac_last16()` still returns a plain
  `uint16_t`, so the displays, the telemetry line and the flash ring see exactly
  what they saw before, and the settings block still stores 16 bits: a restore
  starts with a zero fraction and gives up at most 1.25e-13, which is below
  anything this hardware can show.
- **`DAC` — a command that says what the control voltage actually is.** The
  output path, and for the dither its carrier frequency and table RAM; the code
  in three views — 24-bit, the rounded 16-bit the displays and the flash ring
  use, and the exact fractional value with its difference from the rounded one;
  the measured Vctl; and the step size at both widths, in µHz and as a fraction
  of 10 MHz. A 24-bit code that is not a multiple of 256 is the proof that the
  fine path is driving the pin, which is why all three views are printed rather
  than one, and the command says in as many words whether the fine path is
  active or the output is rounding it away.

  The step figures need the plant gain, which only `CT` can supply. Without it
  the command says so, rather than printing a number derived from a default.
  Listed in the tuner's Help tab as well.

- **`MF` and `MFT` — the per-level limits get their own source, chosen
  independently of the gain.** The two shared one `if`, so `MG 0` meant "gain
  from CT **and** limits from the noise formula" and `MG > 0` meant "gain by
  hand **and** limits by hand". There is no reason they should be welded: the
  gain belongs to the OSCILLATOR — it is LSB per ns, and a different OCXO has a
  different Vctl sensitivity — while the limits belong to the PHASE NOISE the
  board sees, which is a property of the site and the receiver. "Measured gain,
  hand-set limits", which is what a noisy installation wants, could not be
  expressed at all.

  `MF 0` follows `MG` as before and is the default, so nothing moves until
  someone asks. `MF 1` holds the stored table, `MF 2` the noise formula, `MF 3`
  the measured table below. Both settings live in three padding bytes the
  algo-12 block already had, so the layout, the size and `SETTINGS_VER` are all
  unchanged and an older stored block still loads — reading back as 0/0, which
  is exactly the behaviour that build had.
- **`MF 3` — per-level limits measured instead of extrapolated.** The formula is
  `thr[L] = 8·σ·√(2^L)·√10`, and `√(2^L)` says the phase is WHITE, so that
  averaging 2^L samples reduces the test by 2^(L/2). Measured on two boards of
  this design — same PCB, same OCXO, different rooms — the exponent is **0.95
  and 1.03**, not 0.50. Averaging buys almost nothing here, because what matters
  is a slow wander (autocorrelation 0.96 at 60 s, 0.64 at 300 s) and not
  sample-to-sample noise. The error compounds with level: the formula understates
  the real spread about 5x at level 0 and over 100x at level 10, so its table
  falls 32x across the hierarchy where the phase itself falls by 1.3x.

  So the exponent is measured. Each level keeps the mean square of its own test
  statistic, a least-squares fit of log2(sd) against level gives amplitude and
  exponent together, and the table is built from the fit. Fitting ACROSS levels
  rather than trusting each alone is what makes it usable early — level 8 is
  evaluated once every 512 s and would need half a day to have a variance of its
  own, but the low levels populate in minutes and the fit extrapolates.

  Five hard-coded numbers leave with it: the 0.5 exponent, the `8.0` multiplier
  (now the normal quantile for the false-fire rate `MFT` states, which is the job
  the 8 was doing by hand — the hierarchy tests level 0 a thousand times more
  often than level 10), the `√10` white-noise propagation, the 5 ns σ floor —
  a property of this detector, not of the arithmetic — and the 100-unit floor.
  What is left is one number with a physical meaning: how long between
  corrections that noise alone triggered.

  The exponent is clamped to [0.5, 1.0] and that is physics rather than taste.
  Below 0.5 would mean averaging removes more than white noise allows; above 1.0
  the spread is growing faster than flat-in-ns, which is a phase RAMP and not a
  noisier board — and letting a ramp raise the threshold is the failure already
  recorded in this file, where sigma climbed 165 → 746 ns and the loop froze.

  Verified by replaying the firmware's own arithmetic over both boards' records:
  the workshop table comes out at 74 ns falling to 14, which is where that board
  was set by hand after auto proved unstable, and the home board reproduces its
  own settled behaviour. On a three-hour run the home board fitted **α = 1.00**
  and corrected at levels 5 to 9 — the first time this hierarchy has used more
  than one or two of its levels.

  **It is not automatically better.** On the home board, where the formula's
  much tighter table happened to suit a quiet site, the measured table doubles
  the phase RMS (11.6 ns median against 5.5 ns over the same window length)
  because it corrects a third as often. The two tables ask different questions —
  the formula asks whether a deviation is above the measurement noise, the
  measured table whether it is unusual for this board — and which one is right
  depends on the site. That is what `MF` is for.

### Changed
- `LOCK` in the trend field now means the hierarchy is quiet **and** the TIM2
  frequency is within 0.05 Hz, counted on consecutive quiet seconds rather than
  on `s_mla_count`, which resets at every correction and was a poor proxy for
  "how long since anything happened".
- **`GPSDO_PWM_DITHER` is on in the shipped configuration.** It went out in
  v1.04 switched off, while the output path was still being proven; with the
  fine path closed and a 23-hour run behind it, off is no longer the honest
  default. Commenting it out still falls back to the plain 16-bit PWM, and the
  pin, filter and wiring are the same either way.
- **The 320×240 panel's field arrangement now matches the 480×320's.** This
  manual has said since v0.93 that the operating screen is authored once and
  scaled, and that was true of the geometry and not of the content: the two
  panels had drifted apart field by field. qErr moved up to the Alt row, beside
  the fix data it belongs to; AHT and the phase field swapped columns, so the
  environmental sensors share the left column and the electrical ones the right;
  Vcc and Vdd now share the row that freed up. The small panel shows everything
  the large one does.

  Every field that combined a label with a varying-width value was split in two.
  A single right-anchored string pins the unit and drags the label sideways as
  the digits change width — visible on qErr as a label that moved once a second.
  Label and value are now separate slots with separate padding: the label holds
  the column's left edge, the value keeps the right-hand anchor, and only the gap
  between them changes. Same for `dph` and for the INA current.
- **Font 2 is proportional, and this layout had been arithmetic'd at 8 px per
  character.** Checked against the library's own width table, that overstates the
  small panel's strings by about a fifth — `Vph:1.951V` measures 70 px, not 80.
  The error was not academic: it is what had cost the `dph` label, and what had
  kept Vcc at two decimals where the 480 shows three. Both are back. The
  right-hand fields now share one alignment line at x=314 — the one Vdd was
  already anchored to — so qErr, dph, the INA current and Vdd form a column
  instead of four near-misses. Each field's padding is now the measured width of
  its own widest form rather than of today's reading, and the paddings in a row
  tile it exactly, so no background fill can erase a neighbour's edge.

### Credits
- **Alan Cashin** (MIS42N on the EEVBlog forum) is now credited where the work is
  his: in `V`, in the help header, on the tuner's About screen, and in the
  credits table of all three manuals. Algorithm 12, the zero-crossing
  correction, the dithered PWM and the `CS` self-assessment idea all come from
  his Budget GPSDO. He had been thanked for "dither / DAC discussion", which
  understated it considerably.

### Measured
Twenty-three hours, automatic thresholds, `MR 9`, dither at 13 bits:

| | this run | best previous |
|---|---|---|
| phase RMS, settled | **5–8 ns** | 10–23 ns |
| \|phase\| < 10 ns | **86.7%** of samples | — |
| picDIV re-arms | **1** | 121 |
| correction levels reached | **5–6 typical, up to 8** | 0 |
| 10 000 s frequency | **4e-12** | 1.4e-11 |
| interval between corrections | 254 s | 130 s |

`NOPH` three times in 82 572 samples; `FLL` once. Ambient pressure fell 4 hPa
across the run and the loop did not react.

---

## [v1.04-rtos] — 2026-08-11

### Added
- **`GPSDO_PWM_DITHER` — 24-bit control voltage from a dithered short PWM.**
  Idea from Alan Cashin (MIS42N): run the PWM at fewer bits than you need and
  vary the duty from period to period so the average carries the rest.

  The gain is the CARRIER, not the extra bits. Ripple has to be filtered below one
  output step, and how hard that is depends on the gap between carrier and filter
  corner: the 16-bit PWM at 2 kHz allows a 0.7 Hz corner and a 230 ms time
  constant, while 13-bit dithering at 12.2 kHz allows 4.2 Hz and 38 ms. Filter
  delay goes straight into the loop as phase lag, so a six-fold shorter filter is
  worth more than the resolution.

  Alan dithers in a timer interrupt because a PIC has no DMA. That would be 12 000
  interrupts a second here, competing with the 1PPS capture — the one interrupt
  that must not be delayed. But the pattern for a constant value is periodic, so
  it is computed once into a table and replayed by DMA into the compare register:
  0.012% CPU at 13 bits, and none of it in an interrupt. The average is exact by
  construction — the table holds exactly Y entries of X+1 among 2^(24-N).

  Same pin as before (PB9, TIM4 CH4), so the existing filter and wiring are
  unchanged. TIM4_UP drives DMA1 Stream 6 Channel 2; the 2 Hz tick is on TIM9 and
  the 1PPS chain on TIM2/TIM3, so nothing else is disturbed. Two buffers in
  hardware double-buffer mode mean a value change never glitches the pin.

  Off by default. Costs 8 KB of RAM at 13 bits, 16 KB at 12.

  **What this does not yet do** is give the loop finer steps: every algorithm
  calls `gpsdo_dac_write16()`, which left-shifts into the top 16 bits so existing
  settings keep their voltage. The low 8 bits wait for a loop that calls
  `gpsdo_dac_write24()`.
- **Zero-crossing correction, from Alan's flowchart.** After a limit correction
  changes the frequency, the phase keeps moving in the direction it was already
  going: it sweeps through zero, out the other side, and usually fails the limit
  again — so the loop corrects, overshoots, corrects back, and settles slowly.

  The instant the phase crosses zero is special. The phase error is nil, but the
  frequency error that carried it there is still present; cancelling the frequency
  error exactly then leaves the oscillator with the right frequency AND no phase
  error, rather than a state the loop has to iterate towards.

  Measured against Alan's own logs from the same design: his loop corrects every
  506 seconds where this one corrected every 130. Most of that gap is this test,
  which he describes as essential and which was missing here.

  Reported as `zc=` in telemetry, trend shows `ZC` at the moment it fires.
- **FreeRTOS failure hooks and a project-local `STM32FreeRTOSConfig.h`.**
  `configCHECK_FOR_STACK_OVERFLOW` and `configUSE_MALLOC_FAILED_HOOK` both
  default to 0, so a blown task stack silently corrupts a neighbour and
  `configASSERT` traps in a `for(;;)` with interrupts off — a dead white panel
  with nothing on the console. That is exactly how the last three faults
  presented: a CLI stack too small for the flash ring write, a NULL event group
  read before the scheduler started, and a struct declared in a dead branch that
  still grew the display task's frame.

  The override turns both hooks on and redefines `configASSERT` to print the file
  and line before trapping. The hooks name the offending task on the USB console
  and blink the LED, so the next one identifies itself in seconds rather than
  hours. Bare `Serial`, not `OUT_SERIAL`: a hook must not touch a mutex or a
  Bluetooth stream that may itself be the thing that failed.

  Credit to GLM-5.2 for writing this while the white-screen fault was being
  chased; it is adopted here essentially as written.
- **Algorithm 12 — multi-level accumulator.** After Alan Cashin's (MIS42N on
  EEVblog) Budget GPSDO. Every other loop here has one time constant, and that is
  a compromise nobody wins: measured against a rubidium reference, `LTC 60` is up
  to 1.58x better past 800 s while `LTC 240` is up to 1.44x better between 10 and
  400 s. This one does not choose. Readings accumulate into levels — level n
  covering 2^n seconds — and a correction fires at the **lowest** level whose
  error exceeds its limit, so a large error acts within two seconds and a small
  one waits for a longer average. There is no `LTC` to set.

  The levels come out of the bit pattern of the seconds counter rather than an
  array of buffers: eleven levels, 2 s to 2048 s, for 22 bytes.

  **Input is phase in nanoseconds from the LTIC detector.** The first attempt fed
  the TIM2 count error in whole hertz and was blind — a disciplined oscillator
  sits far below 1 Hz, so the field read zero in 83% and 95% of samples across two
  runs and nothing ever accumulated. Phase integrates where a one-second frequency
  count does not. Alan asked why 100 ns had been quoted when a TIC resolves 1 ns;
  he was right, that was the counter's resolution, not the detector's. Boards with
  no detector integrate the count into a phase estimate themselves.

  **The frequency test is gone**, on Alan's own advice: *"It was an experiment...
  what we want is a stable system where the tests always pass. So the frequency
  test is unnecessary."*

  New commands `MG`, `MR`, `MLP` and `ML`, saved with `ES ALGO12`. The per-level
  limits are runtime-editable and persisted because only **one** was ever derived
  — 125 ns at 128 s, from the original 10 MHz ±0.01 Hz specification. Alan calls
  the rest arbitrary, so there is nothing to reproduce faithfully beyond that
  anchor, and every board will want its own.
- **Reset-cause reporting at boot.** `RCC->CSR` is read and decoded before
  anything else runs, so an intermittent restart no longer looks identical whether
  it came from a brown-out, the reset pin or a software reset. Added after a board
  rebooted repeatedly at the same point in GPS configuration with no way to tell
  which.

### Fixed
- **Algorithm 12's thresholds are now measured, not inherited.** They were taken
  from Alan's design and scaled by the ratio of counter step sizes, which is the
  wrong quantity: what a threshold must clear is the NOISE on the phase
  measurement, and that differs between builds for reasons a step size does not
  capture. Measured on this board: mean phase -1 ns with a standard deviation of
  462 ns — the oscillator was correctly tuned and all of that spread was noise,
  while the level-0 threshold sat at 462 ns. 41% of samples crossed it. 620
  corrections in 1685 seconds, the hierarchy resetting every 2.7 s and never
  reaching level 2.

  The firmware now estimates the phase noise continuously and sets each level's
  threshold from it. A second error surfaced doing so: the threshold applies to
  the test expression |3b - a|, whose deviation is sigma*sqrt(2^L)*sqrt(10), not
  to the mean phase, whose deviation is sigma/sqrt(N). Using the latter made the
  threshold 4.5x too low at level 0 and worse above. Six sigma on the correct
  quantity brings the correction interval to about a minute, against the 256 s
  Alan's design settles into.

  `ML` reports the measured noise and whether the limits are following it.
  Telemetry carries it as `sig=`. Setting `MG` above zero holds the stored table
  instead, for anyone who would rather tune by hand.
- **The tuner stopped reading anything back from the board.** `STATE_HINT` was
  added to `TelemetryParser` but referenced as `self.STATE_HINT` from
  `GpsdoTuner` — a different class. Every telemetry line then raised
  `AttributeError` inside the line handler, so no reply ever reached its absorber
  and the LL calibration fields and the algo-12 limit table both stayed empty.
  Two symptoms, one crash.

  The handler is now wrapped: a parse failure costs one line and prints to the
  monitor, rather than silently killing reception. That the fault presented as two
  unrelated parsing bugs, and was only found because the console traceback was
  quoted, is the argument for the wrapper.

- **`MG` and `LG` both answered `gain=`.** The Lars absorber runs first in the
  line handler and matched the algo-12 reply, returning before it could reach the
  algo-12 boxes. The firmware now answers `m_gain=` and `m_run_level=`, so the two
  command sets no longer share field names.
- **The tuner's algo-12 limit table showed zeros.** It was never read back: the
  parameter query asked for the scalars only, so the eleven spin boxes sat at zero
  until somebody typed in them — and pressing Send would then have written eleven
  zeros over a table the firmware had defaults for. A control that displays a
  value the device does not hold is worse than one that displays nothing.

  The table is now read on connect, `MLP n` with no value prints its setting, and
  Send refuses while any row is still zero. `MLP` and `ML` also print the phase
  threshold each limit represents alongside the raw accumulator value — the raw
  number is what you set, the nanoseconds are what it means, and only the latter
  can be compared against a scope.
- **Algorithm 12 ignored the detector polarity and confused nanoseconds with
  hertz.** Two faults in the same conversion, found together from one log.

  `LPOL -1` was not applied at all — algorithm 11 multiplies its phase term by
  `-polarity` and this did not — so on such a board every correction went the
  wrong way. And the average phase, in nanoseconds, was multiplied by LSB-per-hertz
  as though the two were the same quantity: nulling P ns over T seconds needs
  P/(100*T) Hz at 10 MHz, so the correction came out 100*T times too large, from
  200x at level 0 to 102400x at level 9. Every correction hit the +/-2000 clamp.

  Positive feedback four orders of magnitude too strong is a fair description of
  what the log showed: 6000 counts of PWM swing over 148 corrections.

- **`MG` and `MR` were accepted and stored but never read.** The commands worked,
  the tuner sent them, `ML` listed them back, and the algorithm used neither — a
  hand-set gain did nothing and the forced-correction level did not exist. Both
  are now wired: `MG` overrides the CT-derived scale in LSB per ns, and `MR`
  forces a correction once its level is reached, whatever the limits say, which is
  what stops a drift slow enough to stay under every limit from accumulating
  forever.
- **Algorithm 12 now requires the LTIC detector, and holds instead of guessing.**
  It was written to fall back to integrating the TIM2 count error on boards with
  no detector. That fallback was actively destructive: the count is quantised to
  whole hertz and reads zero on a disciplined oscillator, so integrating it
  produced a random walk of quantisation noise rather than phase. The walk crossed
  a level limit, a correction fired and hit the clamp, the oscillator was thrown
  far enough to rail the detector, and railing kept the fallback running.
  Measured: 6000 counts of PWM swing across 148 corrections with the detector
  railed 58% of the time and the reported phase stuck at 0 throughout.

  `LA 12` now refuses without `GPSDO_LTIC`, and when the detector is fitted but
  not reading — railed, saturated or not yet armed — the algorithm holds and lets
  the picDIV bridge do its work. A silent fallback that destroys the lock is worse
  than refusing to run.
- **Algorithm 12 now arms the picDIV.** It did not, and the failure was silent:
  with the ramp railed the detector never returns a valid reading, so the code
  fell through to integrating the count error and the algorithm was blind again —
  the very thing moving to the detector was meant to fix, with nothing in the
  telemetry to say so. Same hold-off bridge as algorithm 11.
- **Algorithm 12 could be selected but never persisted.** `LA` gained a branch for
  12, but `settings_recall` still clamped the stored value to `<= 11`, so the
  setting saved correctly and was silently dropped on the next boot — which looks
  like the flash ring failing rather than a stale constant in the recall path.
- **White screen at boot.** The algorithm-12 telemetry declared a stats struct
  inside `print_human_report()`, which runs in the display task. A stack frame is
  sized at compile time, so a struct in a branch that never executes still
  reserves its space on every call; twenty bytes took the display task over its
  stack and it died before `tft.init()`. Now static.

### Changed
- **`SETTINGS_VER` 4 → 5** for the algo-12 block, **with migration**. A v4 block is
  accepted, its fields applied, and the algo-12 values left at defaults. Rejecting
  it outright would have discarded a working PID, LC and timezone because a new
  algorithm was added.

## [v1.03-rtos] — 2026-08-01

Built on v1.01. The v1.02 experiments — a sigma-delta DAC on PB5 and support for
STM32duino core 3.0.0 — are not carried forward: the first was measured and found
not to deliver what it promised, the second locked the board up on hardware.
v1.01 remains the tested base, with two additions.

### Fixed
- **A warm reboot no longer restarts a finished survey-in.** The receiver keeps
  its own power and its own state across `RB`, so a survey completed before the
  reset is still valid: the position it established has not moved. The firmware
  previously commanded a fresh survey regardless, discarding a result that took
  minutes to reach and dropping the module out of Time Mode while it repeated work
  already done. `gpsdo_gps_init()` now polls TIM-SVIN first and skips the start
  when the receiver reports valid=1 with active=0 — Time Mode with a finished
  survey behind it. Reported as *already in Time Mode from an earlier survey*.

  This required making `ubx_poll_svin()` safe to call before the scheduler: it
  yielded with `vTaskDelay()` unconditionally, which hangs the system when no
  scheduler is running. It now uses `delay()` in that case, the same pattern the
  ACK reader already used.

- **The board would not start: no LED, no console, nothing.** `setup()` writes the
  DAC three times — the initial 127, the recalled PWM and the default — before
  `xEventGroupCreate()` has run. The new correction statistics hang off the DAC
  write path, and their gate read `xSysEvents`, which was still NULL at that
  point. Passing NULL to `xEventGroupGetBits()` trips `configASSERT` and halts the
  processor, so the failure happened before the first blink and left nothing on
  the console to explain it. The gate now checks for NULL first; those early
  writes are commands rather than corrections, so excluding them is correct as
  well as safe.

### Added
- **`CS` — correction statistics, the loop assessing itself.** Algorithm 11 was
  validated against a rubidium standard on someone else's bench; almost nobody who
  builds this has one, and without it there is the author's word and a lock
  indicator. The correction the loop applies is the error it just observed, so the
  size of those corrections says whether the discipline is working — and GPS is
  the reference, so nothing better exists to compare frequency against. The
  firmware already computed these numbers and threw them away.

  Reports RMS correction over the last **100, 1 000, 10 000 and 100 000
  corrections**, in DAC counts and — once `CT` has measured the oscillator slope —
  in fractional frequency, directly comparable to an ADEV figure. Also the steady
  bias, non-zero when the loop is tracking real drift rather than noise.

  The windows count corrections rather than seconds because the correction rate
  depends on the algorithm: algorithm 11 steers once a second, algorithm 10 once
  per `LIV`. Labelling them in minutes would have meant one thing under one
  algorithm and sixty times that under the other — the same number describing two
  different spans. `CS` measures the actual interval and prints what the windows
  currently cover in wall-clock time, so the reader does not have to work it out.
  At one correction per second, 100 000 spans about 28 hours.

  These are exponential weights, not hard windows: roughly 63% of the weight falls
  inside N corrections and 95% inside 3N. That costs four multiply-adds per
  correction and no memory, where a buffer holding 100 000 samples would take most
  of the RAM budget to answer the same question no better.

  Counted only while locked and with no calibration running: the acquisition ramp,
  the three jumps `CT` makes and the `LC` sweep are commands, not corrections, and
  one of them would dominate the hour-long average long after it ended.
  Algorithms 0-9 have no lock state to gate on and are excluded, which `CS` says
  rather than reporting a number with no defined meaning.

  **The caveat is in the output, the header and the README:** it measures whether
  the LOOP IS SETTLED, not whether the OUTPUT IS GOOD. A noisy detector makes the
  loop chase noise; the corrections grow, `CS` reports them faithfully, and the
  oscillator was fine until the loop made it worse. Nothing measured from inside
  the loop can see that.

  The idea is Alan's (MIS42N on EEVblog), whose own design relies on exactly this
  and therefore needs no secondary standard.
- **`GPSDO_DAC_EXT` — external SPI DAC, planned, not implemented.** Enabling it is
  a compile error by design: `dac_ext.cpp` is a stub with no device chosen. The
  16-bit PWM gives about 50 uV per step at 3.3 V, near 2.7e-11 fractional on a
  5.3 Hz/V oscillator; an 18-bit part with a reference designed for the job reaches
  roughly 17 uV, near 9e-12, with no filter delay in the loop.

  No hardware SPI is needed or available — SPI1 belongs to the TFT and every SPI2
  pin on this package is taken — but the DAC is written once per second, so
  bit-banging costs microseconds. Suggested pins PB0, PB2, PB4, chosen to avoid
  PB6/PB7: those look free but are the default I2C1 pins that `Wire.begin()`
  claims, and a DAC there would break the sensors and the clock display.

### Changed
- **All 23 `analogWrite(PIN_VCTL_PWM, ...)` call sites now go through
  `gpsdo_dac_write16()`.** Adding a second output path by editing each of them
  would have invited a missed one, and a missed call site is the worst kind of bug
  here: the loop would steer correctly almost always and jump whenever the stale
  path was taken. Adding a DAC now means filling in one function.

## [v1.01-rtos] — 2026-07-29

> **Build with STM32duino core 2.12.0 or earlier.** Core 3.0.0 (23 July 2026)
> deploys ArduinoCore-API, which removes `ltoa()` and turns `HardwareSerial`
> into an abstract interface — both used here — and, more importantly, leaves
> TFT_eSPI unable to initialise the panel (white screen, CLI unaffected). The
> first two are small and could be made version-conditional; the third lives in
> the library. See the README for detail.

Milestone release: merges the flash-ring persistence branch with the algorithm 11
(LTIC-Lars) branch. Algorithm 11 is based on the original continuous-PI GPSDO
controller by the late **Lars Walenius**, shared with the time-nuts community; it
is extended here with the auto-calibration and acquisition work below, in his
memory.

### Added
- **Algorithm 11 "LTIC-Lars"** — a single continuous PI loop (no ACQ/DPLL/LOCK
  state machine), disciplining the OCXO from the hardware TIC phase. Selectable
  with `LA 11`; trend LFQ (freq-led) / LPH (phase) / LLK (locked). Tuned live
  with LG/LD/LTC/LFD/LTO/LPL/LPF/LTK/LTR.
- **CT auto-calibration for algorithm 11.** gain defaults to 0 = auto: the loop
  derives its frequency scale from the CT-measured K (Hz per PWM LSB), the same
  constant algo 10 uses, so one CT calibrates the Lars loop too. A non-zero LG
  overrides with a manual scale.
- **Frequency-led acquisition** with a dominant self-braking proportional term,
  a step clamp and anti-windup, so a cold start pulls in without the runaway or
  the ±2 Hz oscillation seen during development.
- **picDIV phase-capture bridge**: once the frequency is settled but the phase is
  still railed, the picDIV is re-armed once to bring the phase into the detector
  window, where the phase branch completes the lock.
- **Tuner: versioned and matched to the firmware.** The tools now carry a
  TOOL_VERSION tracking the firmware release, and the tuner reads the board's own
  version on connect: a mismatch is reported in the status bar and the monitor
  rather than left to show up as fields that read oddly. The main window now opens
  maximised with the splash over it, and on Windows the console spawned by
  double-clicking the script is minimised to the taskbar (only when the tuner owns
  it — a terminal the operator opened is left alone).
- **Tuner: Help tab and boot splash.** The tuner gained a Help tab with the full
  command reference grouped by topic, and a three-second splash animating two
  phase-shifted sine waves converging into one — the same lock metaphor as the
  firmware's TFT boot screen (click to skip). It also reads every parameter group
  on connect (LTIC, FA, PID 3-9, Lars) instead of just two, and algorithm 11 has
  its own tab beside algorithm 10.
- **Flash-ring persistence for algorithm 11.** All g_lars parameters are stored
  in the flash ring alongside the LTIC settings (SETTINGS_VER 2); `ES LTIC` saves
  both. No EEPROM anywhere — persistence is 100% flash ring.

### Changed
- **`LC` warns when run before `CT`.** The two are not independent: `LC` needs the
  Hz-per-LSB slope that `CT` measures, and without it falls back to a generic
  value. The failure is quiet rather than obvious — one board reported
  ns_per_volt 1592.8 before `CT` and 921.2 after, a factor of 1.7, with nothing in
  the first run to suggest it was suspect. `LC` now says so up front and continues
  anyway, and the README states the order explicitly.
- **Every setting now says whether it was saved.** Preferences that do not touch
  the control loop — timezone (`TZ`/`TO`/`LT`), sensor offsets (`PO`/`AO`) and the
  boot/survey flags (`WU`/`SPL`/`SV`) — save themselves, and the reply names the
  group that was written. Loop tuning stays manual, and its reply names the exact
  command that would keep it, e.g. `[not saved — run 'ES LTIC' to keep it]`, so
  the group never has to be guessed. `SET_FLAGS` carries `SAW` and `LRN` alongside
  the boot flags, so an auto-save there commits them too; the message lists the
  whole group rather than hiding it. A rejected value is reported as such —
  `[not saved — value out of range; accepted range shown above]` — rather than
  offering an `ES` command for a change that never happened.
- **`LT` is now persistent.** The command was implemented but had no field in the
  settings block, so the UTC/local choice did not survive a reboot. Added to the
  timezone group (SETTINGS_VER 4).
- **`CT` auto-saves its result.** Like `LC`, the three-minute calibration now
  writes its coefficients to the flash ring on success instead of asking the
  operator to remember `ES PID`. Only the PID group is written, so live loop
  tuning in progress elsewhere is untouched.
- **Algorithm 11 trend labels renamed** to ACQ / PLL / LOCK, matching algorithm
  10's vocabulary so the displays, CLI and tuner read consistently. Algo 11 shows
  PLL where algo 10 shows DPLL, which still tells the two apart in a log.
- **Learn telemetry reports what actually steers each loop**: algo 11 shows gain
  mode / scale / filtered phase, algo 10 shows its state machine, algos 3-9 keep
  the LRN figures. qErr stays on every line (shared by both LTIC branches).
- **CT message** now states it tunes algos 3-9 plus LTIC 10 & 11.
- **Persistence wrappers renamed** eeprom_* → persist_* to reflect that storage
  is the flash ring, not EEPROM; the names no longer mislead.

### Fixed
- **Survey-in never went to the background after a reset.** The patience timer
  ran from the host's own boot, but a timing receiver keeps surveying across an
  MCU reset — it has its own power and state, and reports its own elapsed time.
  So every reflash restarted the clock and granted the survey another full cap in
  the foreground, indefinitely. Observed on the bench: the receiver reporting
  4450 s of survey against a host uptime of 7 minutes, with the timeout message
  never printed once. The deadline now expires when EITHER clock passes the cap,
  and the message says which one ran out.
- **Algorithm 10 could freeze during a healthy pull-in.** The runaway guard fired
  on a railed detector plus a large frequency error alone — but that is the normal
  state of a cold or far-off OCXO at the start of acquisition, and freezing there
  removes the only path back, since the frequency term is exactly what pulls the
  oscillator into the detector window. One observed run travelled 3855 LSB during
  a perfectly healthy DPLL pull-in and was frozen mid-recovery. Both guards now
  additionally require the error to have stopped improving for several cycles
  (LTIC_RUNAWAY_STALL), which a genuine wrong-polarity runaway trips and a healthy
  acquisition never does. The rail threshold is also taken from the LC calibration
  again instead of a fixed 3.28 V that only suited one board.
- **`LIV` was capped at 30 s.** Both the CLI and the loop clamped the LOCK
  correction interval to 30, and the loop snapped an out-of-range value to 5 s —
  so asking for a slower loop silently gave you the fastest one. Restored to
  1..600 s, clamping to the nearest bound. This mattered immediately: a tester
  comparing LIV 30 against LIV 60 would have had the 60 rejected.
- **Settings were never actually persisted.** The ring's slot header stored the
  payload length in a single byte, so anything over 255 B wrapped: a 324-byte
  settings block was recorded as 68. The data itself was written correctly and the
  CRC covered it, so nothing looked wrong — but every read returned a truncated
  length, leaving the tail of the recalled block as whatever was on the stack.
  That is where the stray `temp_coeff=-1` came from, and once the length was
  checked strictly the recall rejected the record outright and the board came up
  on defaults. The length field is now 16-bit (slot header 4 B → 6 B, payload
  506 B → 504 B) and the ring magic is bumped so an older ring reformats itself
  instead of decoding as garbage. This affected the flash-ring branch from the
  start — GML's own block was already 292 B, likewise over the limit.
- **Stack overflow when writing the flash ring.** Saving settings needs about
  1.4 KB of stack — `fr_write()` builds a 512-byte slot image plus a 512-byte
  read-back copy, and `settings_store` adds a ~324-byte block on top — but the CLI
  task had 1 KB and the control task 1.5 KB. The CLI task overflowed into its
  neighbour, and the board printed its save confirmation and then hung with the
  display frozen. Both stacks are raised with head-room (CLI 1 KB → 3 KB, control
  1.5 KB → 3.25 KB; 4 KB more RAM out of 128 KB). The hazard predates the
  auto-save work — `ES` was equally exposed — but auto-save made it easy to reach.
- **`EW` reported the wrong flash sector.** The ring has always lived in sector 7
  (0x08060000, the last sector, so firmware keeps the maximum contiguous space
  below it), but the `EW` message hardcoded "sector 6, 0x08040000" — the one place
  an operator looks was the one place that lied. The message now reads the address
  from the implementation via new `flash_ring_sector_no()` / `flash_ring_base_addr()`
  accessors, so it cannot drift again. The bring-up documents carried the same
  stale figures and are corrected in all three languages: the firmware ceiling is
  393216 B (384 KB), not 262144 B, and the J-Link erase range for wiping the ring
  is 0x08060000-0x0807FFFF, not the sector-6 range that would have left the ring
  untouched.
- **LC no longer throws away its own convergence.** The rate-nulling loop stopped
  after three tries and, if it had not yet landed in the acceptance band, fell
  back to `saved_pwm + offset` — which assumes the saved PWM sits at the lock
  point. Run before the oscillator is near 10 MHz that assumption is badly wrong:
  an observed run converged -244, -57, -16 ns/s (one step from the band), then
  discarded that and sampled at a PWM running at -244 ns/s, where the phase
  crosses the whole detector window between publications. Every picDIV arm landed
  on the rail and the calibration aborted. The loop now gets six tries, and when
  it runs out it keeps the steered PWM instead of jumping back.
- **FA / FAD / FAL restored.** The per-state damping-average window (and the
  `damp_e_freq` term it feeds in algorithm 10) was present in v0.97 but absent
  from the flash-ring branch, so it was lost in the merge. Restored, and now
  stored in the flash ring rather than EEPROM.
- **Settings records are length-checked.** `settings_recall` and
  `settings_save_partial` accepted any record of two bytes or more into a
  stack-allocated block, so a record shorter than the current struct left the
  tail as stack garbage — and a partial save would write that garbage back. Both
  now zero the block first and require the exact size.
- **settings_store.cpp now compiles.** It read three globals it could not see —
  g_pressure_offset, g_altitude_offset (defined in gpsdo_control.cpp, with no
  header of their own) and g_qerr_enable (declared in ubx_timtp.h, which was not
  included). Added the include and the two local externs, following the pattern
  the rest of the project uses.
- **LT command implemented.** The help had always documented `LT 0|1` and the
  display and report paths had always read g_show_local_time, but the CLI handler
  was never written, so the verb silently did nothing. It now toggles and reports
  UTC vs local time as documented.
- **Serial dph now matches the panel.** The TFT row subtracted the receiver
  sawtooth but the serial report did not, so the same instant read differently on
  the two — a whole sawtooth apart (~±10 ns on a LEA-6T, more on an M8T). The
  serial path now subtracts it as well, as its own comment already claimed.
- **CR (cold restart) now really erases the ring.** persist_erase() calls the new
  flash_ring_wipe(), which physically erases and reformats the ring sector, so a
  cold restart genuinely returns to defaults instead of merely marking state stale.

## [v0.95-rtos] — 2026-07-16

### Added
- **Timezones with DST, anywhere in the world.** `TZ Adelaide` is now enough to
  get the clock right, including its half-hour offset and its
  southern-hemisphere DST. City names are accepted on their own — they are
  unique across the entire IANA database, so the region is optional
  (`TZ Australia/Adelaide` works too), and case is ignored.

  The rule can also be typed in full: `TZ ACST-9:30ACDT,M10.1.0,M4.1.0/3`. That
  form matters when a government changes the rules and this firmware hasn't
  caught up — the user can fix it from the CLI rather than waiting for a
  release.

  407 zones and 88 rules are built in, generated from the system tzdata by
  `tools/gen_tz_table.py`. The full IANA database is ~2 MB, four times this
  MCU's entire flash, and its real value is being updated several times a year
  — which a GPSDO with no internet cannot benefit from anyway. The POSIX TZ
  string each zone reduces to is 4–44 bytes and captures the same present-day
  behaviour, so that is what is stored. Cost: ~7 KB of flash.
- **`H TZ`** — the first per-command help page. `TZ` takes two quite different
  arguments and the difference matters, so it gets a page of its own rather
  than a cramped line in the main list.
- **`TO` now accepts minutes**: `TO 9:30`, `TO -3:30`, `TO 5:45`. Plain hours
  still work.
- **Vcc on screen (480×320).** Requested by Dan Wiering alongside Vdd. The 5 V
  rail was already measured but had nowhere to go — every cell in both columns
  was spoken for. The `Alt` cell had ~134 px of slack after the altitude, so it
  gives up its right half, and the fields were regrouped to earn their keep:
  `qErr` moves up next to `Alt` (it is the receiver's own report on its 1PPS, so
  it belongs with the fix data) and `Vcc` takes the space `qErr` left beside
  `Vdd`, so the supplies sit together. `Vdd` regains its second decimal, which
  it only ever gave up to make room for `qErr`.

  Both are 480-only. At 320 `Alt` and `qErr` want ~168 px and the cell is 148,
  so that panel keeps the old arrangement.

### Fixed
- **Reported by Dan Wiering: auto timezone missed DST in South Australia.**
  Two separate bugs, only one of which was visible. `TO A` guesses the zone
  from longitude and applies the EU DST rule, so outside Europe it gave no DST
  at all — that was the reported symptom. But it also returned whole hours,
  and Adelaide is UTC+9:30, so the clock was half an hour out even in winter
  with DST fixed. India (+5:30), Nepal (+5:45), Newfoundland (−3:30) and
  Chatham (+12:45) had the same silent error.

  `TZ <zone>` resolves both. `TO A` is unchanged and still there — it is right
  across most of Europe and needs no configuration — but it now says plainly
  what it cannot do.
- **The frequency reading jumped sideways on the 320×240 panel.** v0.94 removed
  the `dtostrf` field width on the theory that a fixed-width font already keeps
  the digits in columns. It does — but a string that loses a character still
  gets re-centred, moving every glyph half a character. The field width is what
  makes the *string* a constant length, and it is back, as it has been since
  v0.89. The 480×320 panel is untouched: it anchors the reading by its right
  edge instead, which is verified on-panel.
- **The side rails vanished beside the frequency.** The frequency sprite clears
  its whole band before drawing, and drew only the separator line above itself
  — so the rails from the initial layout were wiped from that band on the very
  first update, and the frame appeared not to meet the header line. The sprite
  now carries the rails too. Both panels.
- **Vdd was only shown on an LTIC build.** It sits in the phase row, and the
  whole row was behind `#ifdef GPSDO_LTIC` — so a board without the TIC could
  not see its own 3.3 V rail, for no better reason than where the field happened
  to be written. The rails now sit outside that guard: `Vcc` and `Vdd` show
  whatever the panel, and the phase field alone stays LTIC-gated, leaving the
  row's left half empty without it. `qErr` stays gated too, since it only ever
  appears under algo 10.
- **`CT` displayed "Tune 0s" for its entire run.** It set the calibration flags
  but never seeded the countdown, unlike `C` and `LC`. Three points at
  `OCXO_CALIB_SECS` each, so 185 s.
- **`qErr` shifted about on the 480×320 panel.** Left-anchored, the field grew
  rightwards as the value changed width and "ns" walked back and forth.
  Right-anchoring the whole string fixed the unit but then dragged the `qErr:`
  label along with the digits instead. The label and the value are now two
  fields: the label is pinned to the slot's left edge, the value keeps a right
  anchor so the unit stays put, and only the gap between them changes — which is
  how the `Vph`/`dph` row has always behaved.

### Changed
- **`dph` printed a confident number long after the detector had stopped
  measuring.** `ns_per_volt` is a local slope, read around the anchor LC places
  at 0.632·Vsat; the ramp itself is `V = Vsat·(1 − e^(−φ/τ))`, so away from that
  anchor the curve flattens and the linear reading understates the phase. Past
  Vsat there is no reading at all — the stop pulse has missed its window and the
  capacitor charges on to the supply rail. The display reported that state twice
  as a rock-steady "+1561 ns", and both times it cost a measurement before
  anyone thought to check the voltage next to it. `dph` now reads `ovf` outside
  15–85% of Vsat, with `Vph` beside it saying which end it ran out of.

  Vsat is not stored — LC fits it, places the anchor and discards it — but the
  anchor is 0.632·Vsat by construction, so `zero_offset` recovers it. On this
  board that gives 2.91 V, which matches the 2.93 V the calibration comments
  quote for it.

  Worth noting separately: the loop's own runaway guard (`railed_now`) tests a
  hard-coded 3.28 V. On a detector saturating near 2.9 V it cannot fire, so the
  band between ~2.9 V and 3.28 V is saturated as far as the hardware is
  concerned and healthy as far as the loop is concerned. That is not addressed
  here.
- **`dph` on screen never had the sawtooth removed.** The display computed the
  phase down its own path — voltage, centre, `ns_per_volt` — and skipped the
  correction the loop applies in `ltic_phase_error_ns()`. So algo 10 steered on a
  corrected phase while showing an uncorrected one, and the two differed by the
  whole receiver sawtooth: ~14 ns of 1-sigma scatter on an otherwise flat
  reading, measured on air. The display now subtracts it too.

  This matters most away from algo 10. Algorithms 3–9 never call the loop's phase
  path, so `dph` was their only view of true phase and it was the noisy one —
  which is precisely the case where the TIC is worth having, since it resolves a
  frequency offset to ~5e-11 in 100 s where the cycle counter needs 1000 s to
  reach 1e-10. The `qErr` field and the `qErr=` figure in the serial report are
  no longer gated on algo 10 either: what was subtracted has to be visible, or
  the number cannot be checked after the fact.
- **Each column has one right-hand alignment line (480×320).** The left column
  ends where "hPa" does on the BMP row, the right where "ns" does on the phase
  row — those being the widest, most stable strings in each. `Vct`, `% rH` and
  the INA current are now anchored to those lines instead of each stopping
  wherever its own text ran out, which left the column edges as three near-misses
  a few pixels apart. `PWM:` and `INA:` keep their labels at the column's left
  edge, so both rows had to become two fields rather than one string.

  The lines are measured with `textWidth()` on first use rather than written in
  as constants: every value in these rows is fixed-width, so each edge is a
  constant — but it is a constant of the font's glyph metrics, which are not the
  sort of thing to guess at. The paddings are derived from the measurement too,
  so the fields tile the row whatever it turns out to be.
- **The sensor rows are grouped by column rather than by sensor (480×320).**
  BMP and AHT now fill the left column and the electrical fields the right —
  the phase readout on top, the supply rails directly beneath it. `AHT` and
  `Vph`/`dph` swapped places to get there. Moving the phase field into the
  narrower right column cost it one space before `dph:`; its padding is sized to
  the widest string it can produce, not to the column, so a shrinking value
  cannot leave a tail behind.
- **`dPh:` is now `dph:`**, matching `Vph:` next to it. Changed on the TFT and
  in the serial report together — the two labels were meant to agree, and only
  changing one would have made that worse rather than better.
- **The survey-in notice moved out of the header and onto the status bar.** It
  used to pulse between the product name and the clock, where on the 480 panel
  it never appeared at all — a failure that survived every reading of the code
  and several confident wrong diagnoses. Rather than keep hunting it, the notice
  now appends itself to whatever the status bar already says:
  `DISCIPLINED  FIX OK SURVEY`, or `SV` at 320 where the full word would overrun
  the band.

  The bar is the better home regardless of the bug. It repaints its entire
  background before drawing, so the word cannot be clipped by a neighbour's
  padding the way a header slot could; it is the one place on screen the eye
  already goes for state; and sitting there it does not need to blink to be
  noticed, so the pulse is gone too.

  The condition is unchanged, because it was never the problem: the notice
  appears once the survey-in monitor times out with the receiver still
  surveying, and clears the moment Time Mode arrives.
- `g_time_offset` (int8, hours) is now `g_time_offset_min` (int16, minutes),
  with a single writer. `g_tz_auto` (bool) became `g_tz_mode` (manual /
  auto-EU / POSIX): every command sets the mode, so there is no half-state
  where one mechanism is configured and another is quietly overriding it.

### EEPROM
- The timezone block moved to `[234..284]`: mode, manual offset in minutes, and
  the POSIX rule as text.
- **Existing settings are migrated automatically — no factory reset.** A
  pre-v0.95 EEPROM has never been written above `[233]`, so the block reads
  back as erased flash; that is the marker, and the old `[9]`/`[142]` pair is
  carried forward (hours × 60 is exactly what it meant). The signature is
  unchanged.
- **Downgrading is one-way, though.** The legacy `[9]` and `[142]` bytes are
  still written, so v0.94 flashed onto a v0.95 board reads a sane whole-hour
  offset — but a `TZ` rule cannot be represented there and will be lost.

### Documentation
- Moved to [`doc/`](../doc/), with the English files gaining an `_EN` suffix so
  all three languages are named alike. The root `README.md` is now a short index
  — GitHub renders it on the project page, and it points into `doc/` from there.
- The flash-ring bring-up guides were orphans: nothing linked to them and they
  linked nowhere. They now carry the same language nav as everything else.
- Their flash-budget figure was five versions stale (~170 KB at v0.90). It reads
  216976 B (212 KB) at v0.95, ~44 KB of head-room below the ring at
  0x08040000 — measured, not estimated. That number is the whole point of the
  check, so it should not be left to rot. The guide now also warns that the
  IDE's percentage counts against the full 512 KB and reads far rosier than
  the truth: "41%" is really 83% of what firmware may use.

### Notes
- `tz_table.h` is generated. Re-run `tools/gen_tz_table.py` when tzdata is
  updated; a rule saved in EEPROM survives the regeneration.
- Africa/Casablanca and Africa/El_Aaiun degrade to their standard offset with a
  warning: their DST follows Ramadan, which the POSIX format cannot express at
  all. Every other zone in current tzdata resolves fully.

---

## [v0.94-rtos] — 2026-07-15

### Fixed
- **The 320×240 frequency field was still drawn with the GFX fonts.** v0.93
  moved the small panel back to the classic fonts, but the fix only reached the
  direct-draw path — and that path never runs, because the sprites are created
  on *both* panels, not just the 480×320 one. The sprite branch still had
  `GF_FREQ`/`GF_STATUS` hard-coded, so the reading (and `no signal`) kept
  rendering in FreeMono. It now goes through the same `TFT_FONT_*` macros as
  everything else.
- **The frequency twitched sideways on the 480×320 panel.** The reading was
  centred, so any change in string length moved every character: the averaging
  window changes the decimals, and 10000000.0000 → 9999999.9999 drops a
  character outright, with centring splitting that difference across both ends.
  The reading is now anchored by its right edge at x=464, chosen so the nominal
  `10000000.0000 Hz` (16 chars x 28 px fixed-width = 448 px) still lands dead
  centre, leaving 16 px of air on each side. "Hz" no longer moves; only the
  digits do. Busy messages stay centred — they use the proportional status
  font, where there are no columns to align.

### Changed
- **The frame is white on both panels.** Besides matching the big panel, this
  is what lets the 1-bit data sprite carry the frame itself: that sprite has
  exactly two colours (white and background), so the navy frame could not be
  drawn *into* it and had to be repainted on the panel after every push. White
  means frame and text now go out together in one atomic transfer, on both
  sizes. The header separator moved into the frequency sprite for the same
  reason (its 4-bit palette already holds white).
- **The splash no longer uses the GFX fonts.** It was the last GFX holdout on
  the small panel, which meant anyone upgrading from v0.92 had to add
  `LOAD_GFXFF` to `User_Setup.h` or watch the subtitle collapse to a lone "p" —
  an obscure failure for a cosmetic gain. The subtitle now uses classic font 4
  (which carries the full alphabet — fonts 6/8 are the letterless ones) and the
  credits use font 1 on both panels. **A 320×240 build now needs only
  `LOAD_GLCD`, `LOAD_FONT2` and `LOAD_FONT4`**; `LOAD_GFXFF` is required for
  the 480×320 build alone. The orphaned `GF_TITLE`/`GF_SUB`/`GF_CREDIT` macros
  and the dead 320 branch of the `GF_*` block are gone with it.
- Status bar labels sit 2 px lower on the 320×240 panel. They are all-caps, so
  the descender space at the bottom of the glyph box is empty and geometric
  centring reads high; the nudge centres what the eye actually sees. The
  480×320 panel is unchanged.
- Version bump to v0.94-rtos, including the per-file headers (which still read
  v0.92).

## [v0.93-rtos] — 2026-07-14

### Fixed
- **Countdowns ran slower than the clock.** The OCXO warmup and the
  calibrations timed their seconds with `vTaskDelay(1000)`, which sleeps *for*
  a second rather than *until* the next one — so the ADC reads, the serial
  prints and any preemption were all added on top, and the displayed figure
  drifted behind real time (worse the busier the system). Both now use
  `vTaskDelayUntil`, which absorbs the work time and keeps each step a true
  second. The calibration countdown also stopped at 1 instead of reaching 0.
- **A survey-in that outlives its monitor window is no longer invisible.** When
  the safety timeout fires, the firmware stops polling but the receiver keeps
  surveying ("continuing anyway" in the log) — and with the frequency band back
  to showing the frequency, nothing on screen said so. A slow-pulsing `SURVEY`
  now sits in the header between the version and the clock, and clears itself
  when the receiver reports Time Mode (`HDOP: TIME`), which is the survey's real
  completion signal.
- **`qErr` left stale characters on the ILI9488 panel** (shown as
  `qErr: -1.6 nsss`). The field's text padding was 55 authored units (~82 px)
  while the widest value, `qErr: -21.3ns`, needs ~104 px in FreeSans 9pt —
  TFT_eSPI only repaints the background under the padding, so the tail of the
  previous, longer string survived. Padding widened to 75 authored units
  (~112 px), which covers the text and still clears the right-anchored `Vdd`
  field.
- **Vctl / Vcc / Vdd read 0.000 V for the whole OCXO warmup.** Those ADC
  averages are sampled in the control task's main loop, but `do_warmup()` runs
  *before* that loop is entered and only slept — so nothing ever filled them.
  The warmup countdown now samples the same three channels every second, the
  way `wait_secs_pwm()` already does during calibration.
- **Frequency readout sat right of centre and jumped sideways.** The value was
  formatted with `dtostrf(..., 14, ...)`, left-padding it to 14 characters;
  `MC_DATUM` then centred the string *including* those invisible spaces, so the
  visible digits sat ~40 px right of centre — and because the pad count varies
  with the averaging window (1–4 spaces), the readout shifted whenever
  precision changed. The width is dropped: `GF_FREQ` is FreeMonoBold, which
  already holds the digits in fixed columns, so the padding bought nothing. The
  480-panel trailing-space workaround is gone with it.

### Changed
- **The 320×240 panels go back to the classic fonts for the operating screen.**
  v0.92 moved every panel to the GFX free fonts; on 480×320 that was a clear
  win, but at 320×240 the proportional faces are too wide for a layout authored
  around the numeric ones — values ran past their columns into the neighbour and
  the centre divider cut through the overflow. There was no smaller face to fall
  back on either (FreeSans starts at 9 pt; below it is only the unreadable
  3×5 TomThumb). The small panel now uses font 2 for the header and grid, font 4
  for the status bar, and font 1 ×3 (fixed-width) for the frequency, while the
  splash keeps GFX on both panels. The `TFT_FONT_*` macros pick this at compile
  time — still one layout, not two. The centre column divider is now 480-only
  (no room for it at 320), and the frame reverts to navy on the small panel.
- **The live display regions are double-buffered as sprites.** The header, the
  frequency band and the data area are each rendered into a `TFT_eSprite` in RAM
  and pushed to the panel in one continuous SPI transfer, instead of erasing the
  panel with `setTextPadding` and then drawing on top of it. That erase-then-draw
  was visible as a once-a-second flicker, especially on the 480×320 panel where
  it wipes 2.4x the pixels. Palettes keep it cheap (4-bit header/freq, 1-bit
  data; ~25 KB total on the big panel). If `createSprite()` fails on a
  fragmented heap, each band falls back to direct drawing — the old flicker
  returns but nothing breaks; the boot log reports which path is live.
- **Status messages now spell themselves out, and name which calibration is
  running.** `WARMUP 285s` → `OCXO warmup 285s`, `SVIN 120s 5m` →
  `Survey 120s +/-5m`, and the ambiguous `CAL 245s` becomes `Calibrate`, `Tune`
  or `LTIC cal` — C, CT and LC take very different times, so a bare countdown
  told the operator little. Both panels. Note the two figures differ in kind:
  warmup and the calibrations count down, while survey-in counts up (the
  receiver reports elapsed time, and completion also depends on accuracy, so a
  "remaining" figure would be a guess).
- **`SPI_FREQUENCY 40000000` is now the documented setting** (was 27 MHz in the
  README while `gpsdo_config.h` already said 40). The F411's SPI1 tops out at
  50 MHz, so 40 leaves headroom; it matters most on the 480×320 panel, where a
  sprite push is a single transfer whose duration scales with the clock. Drop
  to 27 MHz if long jumper leads misbehave.
- **Splash credit lines get more leading on the 480×320 panel.** The authored
  12-unit gap scales to only ~16 px there, and the credits are FreeSans 9pt
  (~13 px tall), so the two lines merged visually. The large panel now uses a
  16-unit gap (~21 px, ~1.6x leading); the 320×240 panel keeps 12, which suits
  its 6x8 font1.
- `dPh:` and `qErr:` on the ILI9488 drop the space before their `ns` unit.
- Version bump to v0.93-rtos.

## [v0.92-rtos] — 2026-07-12


### Changed
- **Splash simplified and operating-screen proportions refined.** The large
  green "GPSDO" title was removed from the boot splash; the "GPS Disciplined
  OCXO" subtitle now sits raised at the top, matching the original 320×240
  layout. On the operating screen the header text dropped to the data-font size,
  the bottom status bar was halved in height with a smaller status font, and the
  reclaimed space went into wider line spacing between the telemetry rows (row
  pitch 17→20 authored) so the grid breathes. Data font stays at FreeSans 9pt.
- **All TFT text migrated to Adafruit GFX free fonts (GFXFF).** The header, big
  frequency readout, data grid, status bar and splash title/subtitle now render
  in FreeSans / FreeMono instead of the classic numeric GLCD fonts. This fixes a
  long-standing bug where lettered strings drawn in the numeric fonts (6/8,
  which contain only `0-9 . : - a p m`) collapsed to a single glyph — most
  visibly the splash subtitle "GPS Disciplined OCXO" rendering as a lone "p", and
  the status-bar label appearing blank on a coloured bar. A per-role, per-panel
  font layer (`GF_DATA` / `GF_HEAD` / `GF_STATUS` / `GF_TITLE` / `GF_SUB` /
  `GF_FREQ` in `gpsdo_config.h`) picks FreeSans 9/12 pt, FreeSansBold 12/18/24 pt
  and FreeMonoBold 18/24 pt automatically for the 320×240 and 480×320 panels, so
  the same layout code serves both. The big frequency uses FreeMonoBold so its
  digits stay fixed-width and don't shuffle as the value changes.
- **Requires `#define LOAD_GFXFF` in `User_Setup.h`** (see README). The old
  `LOAD_FONT2/4/6/8` lines are no longer needed; `LOAD_GLCD` is retained only
  for the two fine-print splash credit lines.
- **Operating-screen layout re-geometried for 480×320.** Band boundaries (freq,
  grid, sensors, status) recomputed so the taller free-font rows never cross a
  separator on either panel, the two data columns fill the full width with a
  faint centre divider, and the status bar fills the whole band to the screen
  bottom (no dead colour strip below the label). Grid values are right-datum
  anchored so changing widths stay pinned instead of drifting. Verified on the
  ILI9486 480×320 panel.

### Fixed
- **Stale "not yet implemented / phase A" wording removed from CLI and
  telemetry.** `LA` with a bad value said "0..9 (10=LTIC, not yet available)",
  `LL` printed "(loop not yet implemented — phase A)", and the help/comments
  still described algo 10 as an unimplemented preview. Algorithm 10 has
  disciplined the loop for many releases; all these now describe the live
  3-stage ACQ→DPLL→LOCK phase loop. (`Vdd:` on the TFT also gained a space
  before its value to match the other labels.)
- **LED spinner animations (warmup / survey-in / calibration) ran ~5x too slow
  and looked choppy.** The display task wakes on the 1 Hz PPS notification, but
  the spinners step their frame every 200 ms — so at the 1100 ms wake cadence
  they advanced only once a second. The task now wakes ~every 150 ms while an
  animation is active (and keeps the slow 1100 ms cadence otherwise, since the
  clock only changes once a second). To stop the faster wake from re-pushing
  identical segments over the software-bit-banged TM1637 (~5–8 ms per write), a
  small write cache (`tm_set`) skips the transfer when the pattern is unchanged.
  This is a scheduling/caching fix — no DMA involved; DMA remains a separate
  future step for the TFT SPI path.
- **A raised damping floor did not take effect after reflash — lock oscillated
  LOCK↔DPLL.** The damping multiplier is persisted in the flash ring (live
  data) and restored on boot. Flash written by a build with the old 0.30 floor
  therefore reloaded damp = 0.30 even after the floor was raised to 0.45, and
  since damp only adapts on limit-cycle crossings it stayed stuck there — the
  loop ran at 30 % correction authority, the phase climbed past the lock
  threshold, and the loop hunted LOCK↔DPLL every ~30 s (seen on air). The
  restored damp is now clamped into the current legal band on load (both flash
  ring and EEPROM), so a reflash takes effect immediately. The damp bounds
  moved to the shared header so storage and the learner agree.
- **TFT now shows qErr, and phase gets a `dPh:` label.** On algo 10 with SAW
  active, the right sensor row leads with the receiver sawtooth `qErr:…ns` and
  `Vdd:` shortened to 1 decimal. The two are drawn separately — qErr
  left-aligned, Vdd anchored to the right screen edge — so Vdd no longer drifts
  sideways as qErr changes width. With SAW off, Vdd alone is shown at full
  precision, still right-anchored. The LTIC phase on the left is labelled
  `dPh:±…ns` (no space after `Vph:`) for a clearer, consistent readout; the
  serial report uses the same `dPh:` label after `Vphase:` so the two match.
  Both
  qErr and dPh use a fixed-width signed field (sign always shown, magnitude
  right-justified), so the digits and units stay put instead of jumping
  sideways when the value crosses zero or changes digit count.
### Fixed
- **LOCK could lose lock on a drifting OCXO — now carries a gentle frequency
  term.** In the normal LOCK branch the frequency path was disabled
  (freq_term = 0), so the only defence against a real OCXO drift was the slow
  drift feed-forward. On warm hardware that lagged badly and the phase walked
  out of the lock window (11 → −425 ns in 51 s, then LOCK→DPLL→ACQ). LOCK now
  applies a light 0.1×Kp frequency term — enough to cancel the live drift each
  update, gentle enough not to inject TIM2 noise into a quiet lock. Combines
  with the faster drift feed-forward (below). Root-cause analysis: GML-5.2.
- **ACQ limit cycle (±150 LSB PWM hunting) removed.** Algorithm 10 took its
  frequency error from avg10 (0.1 Hz quantisation); times Kp (~1550 LSB/Hz)
  that produced ±150 LSB PWM jumps on a ~10 s cycle, which kept the phase from
  settling under the lock threshold and slowed acquisition. It now uses avg100
  (0.01 Hz), 10× finer, and acquisition settles cleanly. Analysis: GML-5.2.
- **Drift feed-forward now bootstraps after lock.** Its first learning window
  was a slow 30 s, so a fast post-lock drift escaped before it moved. It now
  runs three fast 8 s windows at a larger step right after lock (absorbing the
  drift in ~10–20 s) then relaxes to the quiet 30 s regime.
- **Damping floor raised 0.30 → 0.45.** The learner could damp so hard the
  loop had only 30 % correction authority and couldn't follow drift; 0.45 still
  damps a limit cycle but keeps enough authority to track.

### Changed
- **LC calibration anchor is now universal — 0.632·Vsat, derived per board.**
  The detector is an RC charge ramp V(φ) = Vsat·(1 − e^(−φ/τ)); the point
  φ = τ, where V = 0.632·Vsat, is the same fractional height on every
  exponential detector regardless of Vsat. LC now recovers Vsat with a 1-D fit
  (linearise −ln(1 − V/Vsat) vs t, pick the lowest-residual Vsat) and anchors
  there. The previous hard-coded 1.85 V only worked because Marek's and Dan
  Wiering's detectors both happen to have Vsat ≈ 2.93 V; a detector with a
  different Vsat would have missed the band. LC is now self-adapting per board
  with no configuration, and `LTIC_ZERO_ANCHOR_V` is retired. Verified on
  logged runs: Vsat recovered to ~0.3 %, anchors agree ~0.8 % run to run.
  Physics and derivation: GML-5.2.
- **Splash subtitle** now reads `GPS Disciplined OCXO` (space, not hyphen).

### Credits
- Loop-anomaly diagnosis and the universal-anchor derivation in this release
  were contributed by **GML-5.2**, cross-checked here against the logged data
  and hardware behaviour. Field logs and testing: **danieljw** (Rb reference)
  and **lucido**.

---

## [v0.91-rtos] — 2026-07-11

### Added
- **LC calibration — anchored operating point + local-slope ns/V (Option D).**
  The ramp phase detector is exponential (1k/1n, τ≈1 µs), so ns/V is not
  constant along the ramp and a whole-transit average (range/span) drifted
  ~15–20 % between runs depending on where the picDIV arm parked the phase.
  ns/V is now taken from the LOCAL slope dV/dt in a window around a fixed
  operating point (LTIC_ZERO_ANCHOR_V = 1.85 V). zero_offset is anchored to
  that point — the repeatable middle of the ramp, clear of the detector dead
  zones Dan Wiering measured (the Schottky drop + pull-down below ~0.05 V, and
  the ADC rail/wraparound near 3.3 V). If a sweep never crosses the anchor band
  the code falls back to the previous range/span average and says so.

  Bench findings across several 1 s-resolved LC runs:
  * The anchor is exact — back-to-back runs land zero_offset on 1.8500 V every
    time.
  * Run-to-run ns/V spread fell from ~15–20 % (old range/span average) to a few
    percent. With both runs swept at the SAME rate it is ~2.8 %; the residual is
    dominated by the sweep-rate quantisation, not the slope fit — avg100 resolves
    the rate to 1 ns/s, so a "−5" vs "−6" label carries ±0.5 ns/s and the two
    ns/V confidence bands overlap. This does not hurt LOCK: the loop uses the
    exact ns/V it measured, at the voltage where it actually works.
  * The fit window was widened to ±0.20 V (LTIC_ANCHOR_WIN_V): more points in
    the band (~70 vs ~35) average down the ADC noise, taking the same-rate
    spread from ~5.9 % at ±0.10 V to ~2.8 %.
- **LC per-second diagnostic log.** During the sampling sweep LC now prints one
  `t=/V=/n=` line per second, making the whole ramp visible in a capture (used
  to derive Option D).

### Fixed
- **Serial report printed twice per second in RD/RH when GPS had a fix.**
  vDisplayTask is notified by two ~1 Hz sources — the frequency relay (per PPS)
  and the GPS parser (per time sentence) — so with a fix it woke twice a second
  and emitted two report lines. The serial line is now gated on a change of the
  PPS counter, so exactly one line prints per second; the on-screen display
  still refreshes on every notification. Reported by Dan Wiering.
- **Credit spelling.** "Wieringa" → "Wiering" in the acknowledgements.
- **TFT `Vph` phase readout was dead code, and wrong if enabled.** It gated on
  the compile-time `LTIC_NS_PER_VOLT` (default 0, so the ns figure never showed
  once the detector was calibrated) and, had the constant been set, computed
  `V × ns_per_volt` from 0 V instead of relative to `zero_offset`. It now uses
  the MEASURED `g_ltic.ns_per_volt` and `zero_offset` from LC, showing a signed
  phase `(V − zero_offset) × ns/V` that matches the loop's own error, or just
  the volts when uncalibrated.
- **CT rejected narrow-span (better) OCXOs.** The plant-gain sanity check
  floored K at 0.1 mHz/LSB, but a narrow EFC span is desirable — smaller Hz/LSB
  means finer resolution and is one route to E-12. Dan Wiering's build measures
  0.048 mHz/LSB (~1.05 V EFC span) and was wrongly rejected. Floor lowered to
  0.02 mHz/LSB; only genuine noise/no-GPS runs are now refused.
- **ILI9488 (480×320) layout fixes — from user photos, not yet verified on a
  panel.** Early adopters Dan Wiering and lucido sent photos of their 480×320
  builds. Several issues were addressed from those images without an ILI9488 on
  hand: (1) the body font was over-scaled — TFT_F mapped font 2→4 (growing
  1.63× while rows scale only 1.33×), so lines overran vertically and the
  status bar was pushed off-screen; the body font is now kept at 2. (2) The
  BMP sensor row was trimmed (temperature and pressure to 1 decimal) so the
  wider scaled glyphs don't overrun the AHT column. (3) The `User_Setup.h`
  font instructions were missing `LOAD_FONT8`, which the frequency readout
  needs — without it that line stays blank. These are best-effort fixes from
  photographs; a final geometry pass will follow once an ILI9488 panel is in
  hand. Small 320×240 panels are unaffected (TFT_F is identity there).
- **LOCK could lose lock on a drifting OCXO (LOCK→DPLL→ACQ bounce).** With a
  real frequency drift (measured ~8.5 ns/s on warm hardware), the phase walked
  out of the lock window — 11 → −425 ns in 51 s — while the correction was too
  weak to follow: the damping learner had floored at 0.30 (correction at 30 %
  authority) and the drift feed-forward was still gathering its first 30 s
  window, so it never moved before lock was lost. Two changes: the damping
  floor was raised 0.30 → 0.45 (keeps enough authority to track drift while
  still damping a limit cycle), and the feed-forward now BOOTSTRAPS after lock
  — three fast 8 s windows at a larger step absorb the drift within ~10–20 s,
  then it relaxes to the slow, quiet 30 s regime. Simulated on the logged
  drift, phase now holds near −125 ns instead of running away. Stable, low-drift
  setups (e.g. a Rb-referenced build) are unaffected — the bootstrap converges
  immediately and the higher floor is still net damping.
- **Phase in ns added to the serial report**, after `Vphase:`, once LC has
  calibrated the detector — `(V − zero_offset) × ns/V`, the same convention as
  the loop and the TFT row.
- **LC anchor is now the measured ramp midpoint, not a fixed 1.85 V.** The
  local-slope anchor was hard-coded to Marek's detector band; a build whose
  ramp sweeps a different range (Dan's runs lower, ~1.3 V) missed the anchor
  window entirely and fell back to the coarse range/span average ("weak"
  result). The anchor is now `vlow + span/2` from the actual sweep, with the
  config `LTIC_ZERO_ANCHOR_V` used only when it genuinely falls inside the
  swept band. LC is now self-adapting per board.
- **TFT pressure could overrun into the AHT column.** BMP pressure was printed
  with 2 decimals (`1013.25hPa`), which at 4-digit pressures ran past the left
  column. Reduced to 1 decimal (`1013.2hPa`), matching the serial report.
- **Warm-boot LOCK bounce (wasted ~1 min of the ~8 min boot-to-lock).** A
  persisted LOCK/DPLL was resumed as long as the phase read was valid (on the
  ramp), even when it sat far from zero_offset — e.g. Vphase ≈2.09 V against a
  1.85 V anchor (~260 ns off). LOCK then engaged, DPLL judged the phase too far
  a minute later and dropped all the way to ACQ, so the full pull-in ran anyway
  after a needless detour. The boot guard now demotes a persisted LOCK/DPLL to
  ACQ unless the phase is valid AND within the ACQ window of zero_offset. Cold
  boot is unaffected (state defaults to ACQ); a genuinely centred warm boot
  still resumes LOCK immediately.

---

## [v0.90-rtos]

### Added
- **Wear-levelled flash ring buffer for "live" data.** Learned drift/damping,
  LC calibration and last PWM are now auto-saved to a dedicated flash sector
  (sector 6, 0x08040000, 128 KB) as a ring of 32-byte slots. Each save
  programs the next empty slot; the sector is erased only when the ring wraps
  (once per 4095 saves), so at 100 saves/day the flash lasts on the order of a
  thousand years. Each slot carries a CRC and a sequence number; a half-written
  slot (power loss) fails CRC and the previous good slot is used. A signature +
  format-version header makes the firmware robust to full-chip-erase,
  sector-only programming, first boot and leftover flash junk alike (a foreign
  or blank sector is detected and re-initialised).
- **Auto-save with hysteresis.** Live data is written only when it has settled
  on a new level: drift changed by > 8 LSB or damping by > 0.03, AND at least
  20 min since the last save. A successful `LC` calibration saves immediately.
- **`FR 0|1` command** (saved with `ES`, default on) toggles the ring buffer at
  runtime — no compile flag, so no build-cache surprises. `FR 0` stops all
  flash-ring activity.
- **`EW` command** shows flash-wear diagnostics: erase cycles and slots used.
- **Sawtooth (qErr) correction for LTIC (`SAW 0|1`).** u-blox timing receivers
  generate 1PPS by dividing an internal clock, so each pulse lands up to one
  clock period off true GPS time — a per-pulse quantization error the receiver
  reports as `qErr` in UBX-TIM-TP. A passive sniffer parses that message
  (qErr is a signed 32-bit picosecond field at the same offset on LEA-6T,
  LEA/NEO-M8T and ZED-F9T, so one parser serves all) and the TIC phase path
  subtracts it, removing the receiver's granularity sawtooth and leaving the
  OCXO's own error. On a LEA-6T (21 ns granularity) this is the dominant
  short-term phase term. TIM-TP is enabled automatically at GPS init; `SAW`
  toggles the correction (saved with `ES`, default off) and shows live qErr.

### Changed
- **`ES` no longer overwrites learned/calibration values when the ring is on.**
  With `FR 1`, calibration (ns_per_volt, zero_offset, range_ns, centre_v) and
  learned drift/damp are owned solely by the ring; `ES` writes only genuine
  settings (PID gains, thresholds, flags). With `FR 0`, `ES` still saves those
  live values to EEPROM as a fallback, and `eeprom_recall()` seeds them at boot
  so migrating an older EEPROM keeps its calibration.

### Fixed
- **`LC` no longer fights the discipline loop.** Running `LC` while algorithm
  10 was actively disciplining let the loop move PWM at the same time as the
  calibration sweep, so the two corrupted each other — the measured sweep rate
  came out at ±1 ns/s and the range as absurd values (1502 / 3518 ns), which
  the physics gate correctly rejected. The control loop is now suppressed
  whenever a calibration is active (`g_calib_active`), so `LC` can be run at
  any time, including under `LA 10`.
- **Calibration-safe PWM paths.** The same guard now also covers the algorithm
  9 thermal-holdover steering and the manual PWM commands (`up1`/`up10`/`dp1`/
  `dp10`/`SP`), which are refused with a clear message while `LC`/`CT` runs, so
  no path can perturb a sweep in progress.
- **`LC` no wrap is no longer flagged as a failure.** A detector that does not
  wrap within the sweep now passes with a good slope/centre/span and is
  auto-saved; only a genuinely weak result (tiny span or off-band centre) is
  called out, with the specific reason. Messages no longer tell the user to run
  `ES` after `LC` — a passing `LC` auto-saves to the flash ring (this is live
  data). `CT` still prompts for `ES`, since it tunes PID settings.

### Credits
- Attribution refined: André Balsa credited as author of v0.06c, the
  inspiration for the RTOS port. Repository link corrected.

---

## [v0.89-rtos]

### Added
- **Self-learning loop aid (`LRN`), shared by algorithm 7 and LTIC.** Two slow,
  passive learners — informed by Dan Wiering's overnight Rb-referenced traces
  (a ~9000 s ±80 ns phase sawtooth, an ADEV bump at the loop time constant, and
  8E-12/day drift): (1) a **drift feed-forward** that estimates the OCXO's mean
  phase slope over 30 s windows and adds a PWM term to cancel it, so the loop
  stops chasing a moving target and the phase goes flat; (2) a **damping
  adaption** that watches phase-error zero-crossings and eases the correction
  gain down on overshoot, up when sluggish — flattening the ADEV bump at the
  loop time constant. Both run ONLY when locked, update at most every 30 s, and
  are hard-clamped (feed-forward ±400 LSB, damping 0.5–1.5) so a bad estimate
  cannot destabilise the loop; neither injects any excitation. `LRN 1|0` enables
  /disables (default on), `LRN R` resets to theory, `LRN` alone prints live
  state; learned values are saved by `ES` (EEPROM 222–230) and recalled at
  boot. The serial report shows a live `Learn:` line (drift, slope, damping,
  observed limit-cycle period/amplitude).
- **Learning now covers every disciplining algorithm (3–10), not just 7/8.**
  A single `lrn_apply()` wrapper feeds each loop's own phase accumulator and
  frequency error to the learners; the NN (algo 9), having no explicit phase
  accumulator, uses damping only. `LRN` state is shared across algorithms.

### UI / Display
- **Colour TFT reworked for clarity and a little life.** Consistent single-space
  label formatting throughout (`Alt: 144m`, `PWM:...`, `Uptime: ...`); the value
  fields align optically in the proportional font. A navy frame (matching the
  header) now boxes the data area, with the three separators joined by side
  rails. The frequency turns green on lock. `DATE:` label added.
- **Boot splash refined**: title at the frequency's height, two oscillator waves
  that fade in out of phase, drift into agreement and merge into one green wave
  with a swelling-then-fading halo, followed by a scrolling hardware-detection
  list (fixed-height window, credits stay put).
- **`SPL 0|1` command** (saved with `ES`, default 1) toggles the boot animation.
  `SPL 0` shows just the title and credits for two seconds — for the
  art-indifferent.

---

## [v0.88-rtos]

### Fixed
- **TFT frequency field no longer keeps digit slivers after CAL/WARMUP/SVIN
  messages.** The busy messages and the big frequency use different text
  heights, so text padding wiped only the current font's band; the whole
  field is now cleared on every busy↔normal transition.

### Removed
- **SPI→T6963C bridge support removed** (an experiment): `T6963C_Bridge.h`,
  its display task section, config block and cross-references are gone.

### Docs
- READMEs (EN/PL/ES) updated with the LTIC v0.5x–v0.88 feature set (LC
  auto-calibration, autotuned gains, ADC median path, runaway guard, WU,
  LED animations, trustworthy lock colour) and a new section on colour TFT
  support: any TFT_eSPI panel at 320×240 or 480×320 with setup steps.

---

## [v0.87-rtos]

### Fixed
- **Zero dead time before sampling — the prep was eating the whole band.**
  The ADC keeps up fine (1 sample/s ≈ 8 mV/step at 9 ns/s); what failed was
  the ~60 s of settling and d1/d2 reads between commanding the ramp and the
  first sample. A fixed offset lands on top of whatever df the saved PWM
  already has (measured +9 ns/s on air), so the phase flew 0.061→2.62 V
  through the entire band BEFORE sampling began and the fit saw only
  saturation. LC now re-arms picDIV (deterministic bottom start), commands
  the offset and starts sampling within ~3 s; the exact rate is read AFTER
  the pass from clean avg100. If saturation still arrives before 10 fit
  points, the offset is halved, picDIV re-armed and the pass retried once.
  The pre-sweep d1/d2 measurement and the adaptive reduce/increase machinery
  are removed — the physics gate and the post-pass precise rate make them
  redundant.

---

## [v0.86-rtos]

### Changed
- **LC redesigned as a single bottom-to-top pass — no direction probing, no
  flipping, no wraps needed.** Field logs proved the picDIV arm parks the
  phase DETERMINISTICALLY ~60 ns above the sync point (Vphase ≈0.061 V after
  every re-arm), that the negative side below that point is DEAD (edge order
  inverts, the pulse vanishes — avg100 showed a real −3 ns/s drift while the
  voltage stood still), and that the positive side runs the whole band up
  into soft saturation. LC now exploits this: after arming it COMMANDS a
  positive ~+4 ns/s sweep (offset from the measured K), samples the entire
  band in one pass, and treats sustained upper saturation as the natural END
  of the measurement rather than a fault. The precise avg100 read-back
  (v0.85) scales ns/V exactly. The in-sweep direction flip and its restart
  machinery are removed.

---

## [v0.85-rtos]

### Fixed
- **The direction flip now COMMANDS a sweep rate instead of trusting a blind
  read — and the phase no longer parks at the band's edge.** On air the flip
  iteration stopped at a nominal "−1 ns/s" that was really ≈0: avg10
  quantises at 0.1 Hz (d1=0.1000, d2=0.0000 in the log), so below 0.1 Hz the
  read is noise. With df≈0 the phase sat wherever the picDIV re-arm dropped
  it (Vphase 0.061 V — the band's lower edge, where too-narrow pulses barely
  charge the RC), the sweep covered 5 mV, and the physics gate had to abort.
  Now, when the sign flips between iterations, LC interpolates the 10 MHz
  point P0 from the last two offsets and sets the ramp to P0 − 0.06 Hz·(LSB/Hz)
  — a COMMANDED −6 ns/s derived from the measured K, independent of the
  quantised read. At the end of the sweep (PWM constant throughout, so avg100
  is clean at 0.01 Hz resolution) the true rate is read back and replaces the
  commanded one before ns/V is computed, so the fit scale is exact.

---

## [v0.84-rtos]

### Fixed
- **The in-sweep direction flip now re-measures the rate and FORCES the sign
  to change.** v0.83's defences all fired correctly on air (soft-saturation →
  flip → clean restart → bad result rejected), but the flip itself had two
  defects: (1) the fit's ns/V divides by phase_rate, and the pre-flip rate was
  reused after the flip — a guaranteed wrong scale (ns/V=9.09e6 rejected by
  the guard); (2) mirroring the offset around saved_pwm does not change the
  drift sign when saved_pwm sits far from the true 10 MHz point (+70 gave
  +0.100 Hz, −70 still +0.054 Hz — the railing side, just slower). After the
  flip LC now re-measures df, and if the sign has not flipped it pushes the
  offset further by −2·df·(LSB/Hz) from the measured K and re-checks (≤3
  iterations); the glitch-rejection window is rescaled to the new rate.
  Simulated on the exact on-air numbers: one push lands at −0.054 Hz
  (−5.4 ns/s), the wrapping side at an ideal sweep speed.

---

## [v0.83-rtos]

### Fixed
- **`LC` can no longer be fooled by soft RC saturation.** A run with a fast
  initial offset (10 ns/s) let the phase drift into the RC's soft-saturation
  region (2.9-3.27 V — below the 3.28 V rail threshold, so "live"): the linear
  fit ingested flat saturation points (ns/V ×74 too big), the later drop out
  of saturation (a 2.57 V "jump") was accepted as a wrap, and the result
  (range=209204 ns, zero_offset=1.34 V — outside the detector band) even
  PASSED the volt-vs-volt self-consistency. Three band-relative gates close
  this class: (1) **physics gate** — the committed range cannot exceed what
  the sweep could physically cover (~rate × window × 1.5), else params
  unchanged; (2) **wrap-jump endpoints** must lie within the clean fitted
  band ±50%, so a drop out of saturation is not a wrap; (3) **soft-saturation
  skip** — once the fit has shape, samples far outside its band are treated
  like railed ones (skipped; they feed the in-sweep direction-flip logic).
  All three scale from the run's own observations — full-swing 3.3 V
  detectors are unaffected.

### Added
- **Survey-in animation on the LED displays.** An upper-'o' spinner (segments
  A→B→G→F chasing around the digit's top loop), phase-shifted per digit into a
  wave — visually distinct from the warmup's lower-'o' wave.

---

## [v0.82-rtos]

### Fixed
- **ACQ parked the phase half a range away from the handover point — permanent
  ACQ (1401 cycles on air with Δf≈0).** The ACQ pull target was computed as
  `zero_offset + span/2`, a relic from before v0.66 when zero_offset was the
  band's floor; since then zero_offset IS the band middle, so the loop held the
  phase at its own "centre" while the ACQ→DPLL threshold (measured against
  zero_offset) could never be satisfied. One point of truth now: ACQ pulls
  exactly to zero_offset. A fresh `LC` also clears any old `LCV` override
  (which could silently re-introduce the same stalemate from EEPROM).

### Added
- **Warmup animation on the LED displays.** During OCXO warmup every digit of
  the TM1637/HT16K33 shows the lowercase-'o' chasing-segment spinner,
  phase-shifted per digit so the pattern travels across the display like a
  wave (survey-in keeps the dashes).

### Note
- After upgrading, re-run `LC` once: the previous calibration was taken
  through the old 10-second-averaged ADC path and its zero_offset/range are
  blurred; the rebuilt burst-median path (v0.79) gives a sharper measurement.

---

## [v0.81-rtos]

### Fixed
- **Build fix:** `p_eff` was used by the DPLL/LOCK integrator before its
  declaration (v0.79/v0.80 did not compile). The deadband/soft-knee block is
  now computed first, so both the integrator and the phase term see it.
- **Calibration countdown shows the REAL total time.** The counter used to
  restart for every internal wait segment (30 s, 20 s…), so the display never
  reflected the whole procedure. `LC`/`CT` now preload a realistic total and
  adaptive phases (ramp increase, rail-backoff, direction flip, sweep restart)
  top it up as they occur; every exit path clears it.
- **OCXO warmup restored and made a saved setting.** Warmup was silently
  skipped whenever the EEPROM was valid — so it "disappeared" once a
  configuration was saved, and a cold-started OCXO was disciplined while still
  drifting thermally. Warmup now runs by default on every boot and can be
  disabled with the new `WU 0` command (`WU 1` re-enables; state saved by `ES`
  in EEPROM byte 221, fresh-flash default: on).

### Added
- **LED "CAL" + spinner during every calibration.** TM1637 and HT16K33 show
  CAL on the first three digits and, on the fourth, a chasing-segment
  animation (G→C→D→E) tracing a lowercase 'o' — a clear "working" cue.

---

## [v0.80-rtos]

### Fixed
- **The green frequency colour now means a trustworthy, CURRENT lock.** After
  LTIC dropped from LOCK to ACQ, the display stayed green because the 1000-s
  average still read ~10 MHz — an echo of the past, not the present. Rules now:
  for algorithm 10 green comes ONLY from the loop's live LOCK state (no
  average fallback); for algorithms 0-9 the long-window criterion remains but
  must be backed by the fast 10-s average still within ±50 mHz of 10 MHz, so a
  loss of discipline kills the green in ~10 s instead of minutes.

---

## [v0.79-rtos]

### Fixed
- **LTIC ADC path rebuilt — the 10-second moving average was poisoning the
  loop.** The old path took ONE raw ADC read per PPS through a 10-sample
  (=10 s) moving average: ~5 s group delay (the loop corrected on stale data)
  and, worse, pre- and post-wrap voltages blended into phantom mid-levels — the
  loop saw a smooth ~30 ns/s drift that did not physically exist and kicked the
  real phase (LOCK steps up to 152 LSB, LOCK↔DPLL bouncing). Now each PPS slot
  takes a 16-read burst (~1 ms) and its MEDIAN — no cross-second memory, no
  lag, no wrap blending, single-read glitches fall out — plus an outlier gate:
  a jump >25% of the calibrated span must repeat in the next read to be
  believed (real wraps persist; glitches don't). Note: reading the ADC more
  often would add nothing — the detector charges the capacitor once per PPS,
  so phase information is inherently 1 Hz; the burst maximises the quality of
  that one sample.
- **LOCK is gentle by design: deadband + soft knee + step cap.** Inside a
  deadband (range/40, ≥6 ns — the ADC noise floor) the phase error counts as
  zero and the integrator holds; outside, the error ramps from zero (soft
  knee); the final LOCK step is hard-capped at ≈4 mHz (from measured K). Small
  offsets now get proportionally small pushes instead of full-gain kicks.

---

## [v0.78-rtos]

### Fixed
- **First confirmed on-air LOCK with the LTIC three-stage loop.** Two follow-ups:
  (1) the TFT frequency readout now turns green on LTIC LOCK — it only
  recognised the legacy "hit" trend, so the colour would have waited for the
  1000/10000-s averages to reach mHz; (2) the EEPROM recall guard rejected
  algorithm 10 (`algo > 9 → 0`), so a saved LTIC configuration silently
  reverted to algorithm 0 on reboot — now `> 10`. With this, `ES` fully
  preserves the LTIC setup: algorithm 10, the LC calibration and polarity are
  stored, and the loop gains are re-derived by autotune from the stored
  measurements on every entry, so a reboot comes back locked-capable with no
  manual steps.

---

## [v0.77-rtos]

### Fixed
- **State transitions no longer bounce on the stepped detector read.** With the
  frequency finally held (−0.02 Hz), the loop still ping-ponged ACQ↔DPLL: the
  ADC updates the phase voltage in steps, and each step produced a phantom
  50-100 ns/s "slope" that tripped the V-derived slope gates (entry to DPLL
  blocked for 183 cycles; DPLL demoted after 6). All frequency-quality gates in
  the transitions now use TIM2's Δf (immune to the stepping) — ACQ→DPLL at
  |Δf|≤0.05 Hz, DPLL→LOCK at ≤0.03 Hz, demotions at Δf>0.30 / 0.10 Hz — while
  the voltage is used only for phase POSITION. DPLL demotion also gained the
  same 3-strike persistence LOCK already had, so a single stepped read cannot
  demote. Simulated with stepped reads: no false demotions, clean promotion to
  LOCK.

---

## [v0.76-rtos]

### Added
- **Full LTIC auto-tuning — no hand-set coefficients.** `ltic_autotune()`
  derives EVERY loop gain from the two measured hardware constants: K (Hz/LSB
  from CT) and ns/V + range (from LC). Freq loop cancels ~50% of Δf per step;
  phase loop pulls with τ≈20 s; LOCK is 4× gentler; the ACQ threshold becomes a
  quarter of the measured detector range. Runs automatically after each
  successful LC and on entry to algorithm 10, and prints the derived values.

### Fixed
- **ACQ now drives the TIM2 frequency error, not the voltage-derived drift.**
  The stepped detector read goes flat at a band edge (on air: phase parked at
  0.336 V while a real −0.3 Hz offset persisted, with ACQ↔DPLL bouncing) — a
  V-derived slope is blind there; TIM2 is not.
- **Board polarity no longer inverts the frequency path.** K is positive on
  every board (+PWM → +f), so frequency terms take no `pol`; only the phase
  (Vphase) path does. Routing e_freq through pol=−1 had been inverting a
  correct frequency correction in DPLL — a co-cause of the state bouncing.

---

## [v0.75-rtos]

### Fixed
- **ACQ oscillated (±1 Hz swings, twice frozen by the runaway guard) once the
  calibration was finally CORRECT.** The drift gain used a guessed fixed
  multiplier (×60) that had been implicitly tuned against the old, wrongly
  scaled calibration; with the true ns/V the numeric drift grew ~2.3× and the
  loop over-corrected ~1.8× per step — a textbook overshoot oscillation. The
  gain is now derived from the MEASURED OCXO sensitivity (CT stores 0.40/K in
  g_pid[7].Kp, so LSB-per-Hz is recovered as Kp7/0.40) with a 0.5 damping
  factor: ~60% of the error cancelled per step, unconditionally stable on any
  unit, no per-board tuning. The DPLL frequency term (fixed ×1000, ~6× too weak
  on this unit) is scaled from measured K the same way.

---

## [v0.74-rtos]

### Fixed
- **Wrap-jump quality gate — closes the last known way LC could go wrong.** The
  stepped ADC can report a wrap mid-step, yielding a PARTIAL jump; one was
  accepted as the full span (0.122 V on a ~0.33 V detector), which parked
  zero_offset near the floor (0.09 V) and sent the loop chasing a false centre
  until the frequency ran 3 Hz away. A jump now counts only if it starts from a
  live (un-railed) sample AND is ≥80% of the min–max band actually observed;
  partial jumps are named in the log and the observed band (or time
  cross-check) is used instead. `zero_offset` is now ALWAYS the middle of the
  observed band, never derived from the jump position.
- **Operator verdict line.** LC ends with an explicit "PASSED checks — review
  LL, then 'ES'" or "MARGINAL result — prefer re-running LC before 'ES'", so a
  weak calibration is hard to save by accident.

---

## [v0.73-rtos]

### Fixed
- **Runaway guard rebuilt after a real 3 Hz escape reached PWM 63500 — the old
  guard had three false assumptions.** (1) Its baseline re-anchored on every
  un-railed sample, but during a runaway the phase periodically WRAPS (briefly
  un-railed), so the baseline chased the escape and the 6000-LSB trip never
  fired. It now re-baselines only when genuinely healthy (un-railed AND
  |Δf| < 0.25 Hz). (2) An LSB threshold silently assumes the OCXO's Hz/LSB
  sensitivity; the primary criterion is now the measured frequency error
  itself: phase railed AND |Δf| > 0.5 Hz → freeze (a 2000-LSB backstop
  remains). (3) Freezing the step left the DPLL/LOCK integrator winding up,
  ready to slam PWM on recovery — it is re-seeded to the held PWM while
  frozen. Behavioural test: old guard let the simulated escape reach 6.15 Hz;
  the new one freezes at 0.51 Hz.

---

## [v0.72-rtos]

### Fixed
- **Direction flip now happens IN the sweep, where the rail actually shows.**
  v0.71's 8 s pre-check could not catch the wrong direction: in the rail-prone
  direction the phase exits the sync window only after ~a full range of drift —
  tens of seconds into the sweep (the pre-check passed, then 137 samples
  railed). LC now counts consecutive railed samples during the sweep itself;
  a sustained run (≥15 s) is the direction verdict: it flips the offset sign
  (mirrored around the saved PWM), re-arms picDIV, wipes every accumulator and
  restarts the sweep once. Verified in simulation: wrong side rails at 40 s →
  flip at ~54 s → clean sweep from the good side with the full-span wrap jump
  captured. If both directions rail, the existing mostly-railed abort still
  reports it.

---

## [v0.71-rtos]

### Fixed
- **`LC` auto-detects the ramp DIRECTION — the root cause of every railed
  calibration.** Comparing all field runs revealed the pattern: every failed
  cal had a positive df (ramp pushing the frequency above 10 MHz) and the single
  clean one (range=318) had a negative df. On this detector family the phase
  wraps sawtooth-style only when drifting one way; the other way the pulse just
  widens until the RC pins at the 3.3 V rail and stays. The good direction is
  board-dependent, so LC now probes it: after settling it watches the phase for
  ~8 s and, if pinned to a rail, flips the offset sign, re-arms picDIV and
  settles again (aborting cleanly only if BOTH directions rail). The adaptive
  ramp keeps the detected direction. Also verified: algorithm 7 does NOT run
  during LC (the calibration blocks the control task), so loop interference is
  ruled out.

---

## [v0.70-rtos]

### Changed
- **`LC` is fully self-contained: it ignores the previous calibration.** Per a
  good operator principle — you recalibrate precisely because the stored values
  may be wrong — LC no longer inherits anything from EEPROM/g_ltic: the ramp
  target, wrap threshold, glitch window and prep criterion all start from
  neutral assumptions and everything is measured fresh. This ends the poisoning
  cascade where one bad cal (range=6035) mis-steered the next three runs.
- **Single-wrap range measurement.** The voltage JUMP at a wrap (sawtooth top →
  bottom in one sample) IS the full detector span, so one wrap suffices:
  range = |jump| × ns/V. The ramp target drops to one wrap in the window, i.e. a
  much gentler sweep that no longer pushes the phase out of the picDIV sync
  window onto a rail (the failure seen at 9-22 ns/s). Two wraps, when they occur
  naturally, still enable the independent time cross-check.
- **Prep criterion is universal:** waits for a valid, un-railed, steady phase —
  no assumed centre voltage (detector bands legitimately differ between builds).

---

## [v0.69-rtos]

### Fixed
- **`LC` adaptive ramp is now hardware-agnostic and self-limiting.** The v0.68
  log showed a cascade: a poisoned prior cal (range_ns=6035 from a noise fit)
  set an absurd ramp-speed target, the adaptive increase chased it (offset up to
  1120, 15 ns/s), and the fast ramp pushed the phase out of the picDIV sync
  window entirely — the detector pulse went wide and the voltage pinned at the
  rail for the whole sweep ("180 railed samples"). Three hardware-agnostic
  defences (no detector band is assumed; different builds range from ~0.3 V to
  full 3.3 V swings): (1) the stored range only *guides* the ramp target through
  a wide anti-garbage clamp (20..5000 ns); (2) **rail-backoff** — after each
  ramp increase LC watches ~8 s and, if the phase pins to a rail, halves the
  offset back, re-arms picDIV to regain sync, and proceeds at the speed the
  hardware allows; (3) **self-consistency gate** — results are committed only if
  range ÷ slope implies a physically possible voltage span (≤3.3 V), otherwise
  the previous calibration is left untouched (a bad LC can no longer poison the
  next one).

---

## [v0.68-rtos]

### Fixed
- **`LC` no longer produces garbage when the ramp lands near the OCXO's 10 MHz
  point.** A +70 LSB offset can barely detune the OCXO (df=0.01 Hz → 1 ns/s), so
  no real wrap could occur in the window — yet read glitches (the phase voltage
  updates in steps) exceeded the wrap threshold and produced fake "2 wraps", a
  noise-only fit, and absurd results (ns_per_volt=38615, range_ns=6035). Three
  defences added: (1) **adaptive ramp increase** — if the drift is too slow for
  two wraps in the window, the offset is doubled (capped ±4000) and re-settled;
  (2) **time-validated wraps** — a jump sooner than ~half the expected crossing
  time after the previous wrap is a glitch and is ignored; (3) **volt/time
  range cross-check** — the time between two wraps × phase rate gives an
  independent range measure; if it disagrees >2× with the voltage-span measure,
  the slope is suspect and the TIME range wins (ns/V rescaled to match).

---

## [v0.67-rtos]

### Added
- **`LC` now auto-preps before ramping (operator convenience).** Running `LC`
  used to require a manual `LA 7` / `AP` / "wait for the phase to reach centre"
  sequence first; starting with the phase against a rail was the main cause of
  poor calibrations. `LC` now, on its own: (1) arms picDIV to sync to 1PPS if a
  GPS fix is present, then (2) waits up to ~60 s for the phase voltage to settle
  inside the central band of the detector (centre ± ¼ range, held a few seconds)
  before starting the ramp. It prints each step and proceeds with a clear note
  if the phase can't be centred in time. Just run `LC` — no manual prep needed.

---

## [v0.66-rtos]

### Fixed
- **`LC` now measures the FULL detector range (was a fraction, e.g. <75 ns).**
  Two bugs collapsed `range_ns` on a narrow detector: (1) the wrap threshold was
  a fixed 0.5 V — larger than the whole ~0.33 V detector range — so wraps were
  never detected; (2) `range_ns` was taken from the small slice the phase
  happened to sweep during the ramp, not the detector's full unambiguous span.
  `LC` now sweeps until it has seen **two wraps** (one full cycle), tracks the
  true min/max across wraps for the range, and still fits the slope (ns/V) on
  the clean pre-wrap segment. The wrap threshold is now relative to the detector
  span. Ramp/window retuned (offset 70 LSB, 180 s) so both a long clean slope
  segment and two wraps fit. `LC` reports whether it saw 0/1/2 wraps so you know
  if the range is exact, approximate, or a lower bound.

---

## [v0.65-rtos]

### Fixed
- **DPLL corrected too infrequently for a narrow detector (looked "frozen").**
  DPLL only adjusted PWM every 10 s and LOCK every `lock_interval_s`; on a
  narrow detector the phase sweeps its whole range in ~10-15 s of residual
  drift, so between corrections the phase wandered and wrapped while PWM sat
  still (seen as PWM pinned at one value for 114 samples). DPLL now corrects
  every 2 s. This is *not* a schematic error: in every state PWM (via the RC
  filter → EFC) drives the OCXO — Vphase is only the ADC feedback measurement,
  so there is correctly no analog Vphase→EFC path.
- **LOCK interval clamped to a sane range (1..30 s).** A corrupted
  `lock_interval_s` (e.g. the 50373 seen in a log) would have made LOCK correct
  roughly once every 14 hours; it is now bounded at runtime and in the `LIV`
  command so LOCK keeps tracking.

---

## [v0.64-rtos]

### Changed
- **Removed the unreliable polarity auto-probe; polarity is now set manually.**
  The single-cycle probe could not separate the PWM effect from the phase's own
  drift on a narrow, drifting detector, so it repeatedly detected the wrong sign
  (+1 where the board is −1). ACQ now holds and prints a reminder to run
  `LPOL -1` (or `+1`) then `ES` when polarity is unset, and DPLL/LOCK already
  hold when polarity is unknown. Once `LPOL` is set and saved, all three stages
  use it consistently — this is reliable where the probe was not.

---

## [v0.63-rtos]

### Fixed
- **Detected polarity is now shared by all three stages.** The auto-detected
  sign lived in a static local inside ACQ, invisible to DPLL/LOCK, which then
  fell back to +1 and — on a reversed board with polarity unsaved — drove the
  phase to the ceiling rail with PWM climbing and frequency walking away from
  10 MHz. ACQ now writes the detected sign into `g_ltic.polarity`, so every
  stage uses it (and it prints a reminder to `ES`).
- **DPLL/LOCK hold instead of guessing when polarity is unknown.** With no
  established sign they now output zero correction and let the machine fall back
  to ACQ (which probes), rather than assuming +1 and running away.
- **Runaway guard.** If the phase is pinned to a rail while PWM is pushed more
  than ~6000 LSB from where the loop started, the loop freezes and warns once
  ("check LPOL / re-centre") instead of sliding PWM to an extreme and
  undisciplining the OCXO.

### Note
- Save your polarity: after the loop prints "detected …polarity -1", run `ES`
  so it survives a reboot (this was the root cause of the last runaway — the
  sign was set but never saved, so it reverted to auto/one).

---

## [v0.62-rtos]

### Fixed
- **DPLL and LOCK now apply the board polarity (was ACQ-only).** ACQ used the
  detected/forced `LPOL` sign, but DPLL and LOCK did not — so on a reversed
  board they drove the phase the wrong way, shoving Vphase onto the floor rail
  and dropping straight back to ACQ (the phase would centre in ACQ, hand over to
  DPLL, then get pushed to ~0 V and fall back). All three stages now share the
  same polarity, so DPLL/LOCK pull the phase toward centre instead of into a
  rail. With ACQ handover already working (v0.61), this is what lets DPLL hold
  and progress to LOCK.

---

## [v0.61-rtos]

### Fixed
- **ACQ now nulls the phase drift instead of chasing phase position.** With the
  polarity correct (`LPOL -1`) PWM stopped running away, but the phase still
  swept the whole detector and wrapped, so ACQ never met the "in-window + low
  slope" exit. The residual frequency offset (~-0.26 Hz) drove the phase at
  ~26 ns/s across a 318 ns detector — far too fast. ACQ's dominant term now acts
  on the phase DRIFT (dPhase/dt), driving the frequency offset to zero so the
  phase stops moving; a weak centring term parks it mid-range only once the
  drift is already small. Wrap-induced drift spikes (phase jumping >½ range in a
  step) are rejected so they don't corrupt the drift estimate or the
  slope-gated transitions.

---

## [v0.60-rtos]

### Fixed
- **ACQ ran PWM away when the board polarity was reversed.** ACQ walked PWM in a
  fixed direction toward `zero_offset`; on hardware where increasing PWM lowers
  the phase voltage (opposite sign), that drove PWM ever downward while the
  phase wrapped chaotically, so ACQ never settled (observed as a long ACQ hang
  with PWM sliding from ~41000 to ~17000). ACQ now **auto-detects the PWM→phase
  polarity** with a small probe step, then drives toward the target with the
  correct sign. A new `LPOL -1/0/1` command forces the sign (0 = auto).
- **ACQ now centres on the middle of the detector range, not `zero_offset`.**
  On a narrow low-band detector `zero_offset` can sit near the floor (e.g.
  0.097 V), so targeting it kept the phase against the rail (risking latch-up /
  wrap, per Dan's note about choosing mid-scale). ACQ now aims at the range
  middle, overridable with `LCV <volts>`.

### Added
- `LPOL` (PWM→phase polarity) and `LCV` (ACQ centring target) CLI commands,
  both persisted to EEPROM and shown by `LL`.

---

## [v0.59-rtos]

### Changed
- **Phase-slope gating on state transitions (algorithm 10).** On advice from
  Dan (time-nuts), both LTIC state transitions now check the phase SLOPE
  (dPhase/dt), not just the phase magnitude. Since frequency is the first
  derivative of phase, a small slope means the frequency is already close to
  10 MHz — so ACQ→DPLL now requires a wide slope window and DPLL→LOCK a ~5×
  tighter one, preventing a handover while the phase is merely sweeping through
  centre at speed (which would lock the wrong frequency). LOCK also drops back
  to DPLL if the slope grows. This is what makes the frequency land very close
  to nominal at each handover.

---

## [v0.58-rtos]

### Fixed
- **`LC` ramp far too fast for a narrow detector.** On hardware whose detector
  spans only a fraction of the ADC (e.g. ~0.33 V per unambiguous period), the
  old +2000 LSB ramp drove the phase across the whole detector every ~1-2 s, so
  every sample railed or wrapped and `LC` aborted with "mostly railed". The
  default ramp offset is now a gentle 60 LSB (≈4-5 ns/s on a typical OCXO), and
  `LC` adaptively steps the offset down further if the measured drift would
  cross the detector in under ~15 s. The frequency-measurement fix from v0.56
  is confirmed working (real df now reported, e.g. 1.4-2.0 Hz, not the old
  hard-coded 0.6).

---

## [v0.57-rtos]

### Fixed
- **ACQ now actively centres the phase (was frequency-only).** The ACQ stage
  previously corrected only the TIM2 frequency error; once the OCXO was already
  near 10 MHz nothing drove the phase, so it could sit stuck against a detector
  rail forever and never satisfy the ACQ→DPLL exit test (observed as an
  overnight hang with Vphase parked low). ACQ now walks PWM toward the detector
  centre when the reading is railed, and drives proportionally to the phase
  error once it is in-window.
- **Phase centre taken from calibration, not a hard-coded 1.65 V.** Real
  hardware can have a narrow detector band far from mid-ADC (e.g. 0..0.45 V), so
  the loop now centres on the calibrated `zero_offset` (with a coarse 0.22 V
  fallback) instead of assuming 1.65 V. Run `LC` so `zero_offset`/`ns_per_volt`
  reflect the real band.

---

## [v0.56-rtos]

### Fixed
- **`LC` frequency measurement.** The calibration read the 10 s frequency
  average once, immediately after a 10 s settle — on real hardware that window
  had not yet caught up to the forced ramp, so the ramp rate (and therefore
  `ns_per_volt`) came out wrong. `LC` now settles 30 s, then samples the 100 s
  average (steadier, with a 10 s fallback) twice ~5 s apart and averages them.
- **`LC` rail handling.** Samples where the TIC voltage sits at the ADC ceiling
  or floor (phase outside the detector window) are now skipped rather than
  flattening the least-squares fit, and `LC` aborts with a clear message if the
  ramp is mostly railed (telling you to centre Vphase near mid-rail first).
- **Build fix:** removed a duplicate `g_ltic_voltage` extern in
  GPSDO_algorithms.cpp that conflicted with the `gpsdo_state.h` declaration.

---

## [v0.55-rtos]

### Added
- **Algorithm 10 (LTIC three-stage PLL) — the loop is now implemented.**
  `LA 10` disciplines the OCXO from the hardware TIC phase (PA1) through a
  hybrid ACQ → DPLL → LOCK state machine. ACQ is frequency-led (TIM2) to pull
  the OCXO close to 10 MHz so the phase ramps slowly enough to catch; DPLL adds
  the LTIC phase term for fast centring; LOCK is phase-led with slow updates
  every `lock_interval_s` and a hysteresis band for dropping back to DPLL. The
  picDIV is armed automatically on entering ACQ. The loop works in nanoseconds
  when the TIC is calibrated (`LC`), and falls back to a nominal volt-based
  phase with a one-time warning when it is not. The state persists in
  `g_ltic.state`, so a warm reboot (`RB`) resumes mid-sequence rather than
  restarting from ACQ. The trend field shows `ACQ` / `DPLL` / `LOCK`.
- **Third PID set (ACQ).** `LticParams_t` gained an `acq` PID alongside `dpll`
  and `lock`, with its own CLI verbs `AQP` / `AQI` / `AQD` / `AQL` and EEPROM
  storage. `LL` now lists all three sets.

### Changed
- **EEPROM layout extended to 216 bytes (reserved to 224).** The ACQ PID block
  [200..215] was appended under the same `GPSD2` signature with the usual
  NaN/`0xFF` guards, so older saves still load with the ACQ gains defaulting.

---

## [v0.54-rtos]

### Added
- **`LC` — LTIC self-calibration.** Automatically measures the TIC's
  voltage→time slope without any external reference. `LC` forces a small PWM
  offset so the phase ramps linearly, derives the ramp rate from the TIM2
  frequency error (`phase_rate = df / BASE_FREQ × 1e9` ns/s), least-squares
  fits the TIC voltage against time to get `dV/dt`, and computes
  `ns_per_volt = phase_rate / (dV/dt)`. It also records the swept voltage span
  as `range_ns` and a mid-scale `zero_offset`, detecting one wrap to keep a
  single clean ramp segment. Runs in the control task like `CT`, with the same
  safety pattern (PWM saved and restored, range-guarded results, abort on
  no-GPS / too-few-points / singular or flat fit — params left unchanged on
  any failure). Results go to the live LTIC params; review with `LL`, then
  `ES` to save. New config constants `LTIC_CAL_PWM_OFFSET`, `LTIC_CAL_SECS`,
  `LTIC_CAL_MIN_POINTS`. This fills the calibration fields that the phase-A
  loop will need; the loop itself is still not implemented.

---

## [v0.53-rtos]

### Added
- **Warm/cold restart commands `RB` and `CR`.** `RB` does a warm reboot
  (`NVIC_SystemReset()`) keeping the EEPROM, so the still-warm OCXO recalls its
  disciplined state. `CR YES` does a cold restart: erases the EEPROM (back to
  factory defaults — PWM, model, calibration, LTIC params all reset) then
  reboots; the `YES` confirmation is required because it discards the learned
  OCXO model.
- **Algorithm 10 (LTIC) infrastructure — parameters, CLI and EEPROM.** Full
  parameter set, CLI editing and EEPROM persistence for the planned LTIC
  three-stage PLL (ACQ→DPLL→LOCK), so the configuration is ready before the
  loop itself is written ("phase A"). New `LticParams_t` holds TIC calibration
  (ns/V, zero offset, range), two PID sets (wide-band DPLL + narrow-band LOCK),
  state-transition thresholds, the LOCK interval, and the resumable state.
  Fifteen CLI commands set/show these (`LL`, `LNV/LZO/LRN`, `DPP/DPI/DPD/DPL`,
  `LKP/LKI/LKD/LKL`, `LAT/LDT/LIV`). `LA 10` is accepted by the parser but
  reports "not implemented yet" and refuses to select, so the OCXO is never
  left undisciplined. The loop itself is not implemented — that is phase A,
  pending the LTIC hardware.

### Changed
- **EEPROM layout extended to 200 bytes (reserved to 208).** The LTIC block
  [144..207] was added under the **same `GPSD2` signature**; every new field is
  NaN/`0xFF`-guarded, so EEPROM images saved by older firmware load cleanly with
  the LTIC parameters defaulting until set. No migration or re-init needed.

---

## [v0.52-rtos]

### Added
- **LTIC (Lars' TIC) phase-voltage preview.** The TIC voltage on PA1 was
  already sampled and sent over serial telemetry, but had no on-screen
  presence. Added (all gated by `GPSDO_LTIC`, so zero effect on builds without
  the TIC):
  - a **TFT row** showing `Vph:x.xxxV` (and `… NNNns` once calibrated);
  - an **LTIC entry in the boot-splash hardware checklist** (`[x] LTIC phase
    (PA1)` — shown when compiled in, like the TM1637/TFT, since the TIC is
    read-only and cannot be probed);
  - a **`LTIC_NS_PER_VOLT` calibration constant** in the config (0 =
    uncalibrated → volts only). When set to the measured ramp slope, the
    display and the planned phase-discipline algorithm convert volts to ns.
  This is a **preview/telemetry layer only** — the control loop does not yet
  discipline the OCXO from the TIC (planned as a separate phase, a new
  LTIC-based algorithm). OLED/LCD were intentionally left unchanged (their
  layouts are full); Vphase remains available there via serial logging, which
  is what characterising the TIC needs at this stage.

---

## [v0.52-rtos]

### Added
- **LTIC (Lars' TIC) phase-voltage preview layer.** When `GPSDO_LTIC` is
  compiled in, the latched TIC voltage (`g_ltic_voltage`, already sampled on
  PA1 and discharged each PPS) is now surfaced as a preview: a dedicated
  `Vph:` row on the TFT (below the sensor row, shown only with LTIC built in),
  and an `LTIC phase (PA1)` entry in the boot checklist. Serial telemetry
  already carried Vphase. A new `LTIC_NS_PER_VOLT` calibration constant lets a
  future build convert the voltage to a phase in nanoseconds: while it is 0
  (default, uncalibrated) the displays show volts only; once set, the TFT row
  also shows `<n>ns`. This is preview/telemetry only — the control loop does
  not yet discipline on LTIC; that is a planned separate algorithm. OLED/LCD
  layouts are unchanged (both are full); Vphase will be added there when LTIC
  becomes an operational loop input.

---

## [v0.51-rtos]

### Added
- **CLI commands are now case-insensitive.** The command dispatcher compared
  verbs with `strcmp()`, so `LA` worked but `la` did not. Command matching now
  uses a small case-insensitive helper (`cli_ieq`), so any letter case is
  accepted (`LA` / `la` / `La` are equivalent), including the lowercase verbs
  (`up1`, `dp10`, …) and the `KP`/`KI`/`KD`/`IL` family (whose parameter
  letter is also matched case-insensitively). Command arguments are unchanged;
  `TO A` already accepted either case.

### Changed
- **ZED-F9T (Gen9) support is no longer experimental.** The CFG-VALSET
  survey-in path and the NAV-SVIN monitor fallback were tested on real
  hardware by EEVblog user danieljw, so the "experimental / untested" markings
  have been removed from the code, config and READMEs. No code change to the
  F9T path itself — only its status.

---

## [v0.50-rtos]

### Added
- **ZED-F9T (Gen9) timing-receiver support — experimental, untested.** A third
  survey-in path was added alongside the proven LEA-6T / LEA-M8T ones.
  `ubx_start_survey_in()` now also sends a `CFG-VALSET` (0x06 0x8A) frame
  setting the Gen9 configuration keys `CFG-TMODE-MODE` (survey-in),
  `CFG-TMODE-SVIN_MIN_DUR` and `CFG-TMODE-SVIN_ACC_LIMIT` (the latter converted
  from mm to the F9T's 0.1 mm unit). The survey-in monitor gained a parallel
  `NAV-SVIN` (0x01 0x3B) parser and falls back to it when `TIM-SVIN` does not
  answer, since the F9 generation reports survey-in through NAV-SVIN. ⚠️
  Written from u-blox documentation/ubxtool with no F9T on hand — key IDs, the
  0.1 mm unit and the NAV-SVIN payload offsets are NOT verified on hardware.
  The legacy `CFG-NAV5` stationary frame may NAK on an F9T (harmless; the
  survey-in path is independent). The two tested receivers are unaffected:
  TIM-SVIN is still tried first, so LEA-6T / LEA-M8T / NEO-M8T behaviour is
  unchanged. Documented as experimental in the README and config.

### Changed
- **LCD 20×4 splash subtitle** changed from `GPS-Disciplined Osc.` to
  `GPS-Disciplined OCXO`, matching the TFT splash (both 20 chars, full width).

### Notes
- **NEO-M8T** confirmed (by datasheet analysis) fully compatible with the
  existing LEA-M8T path — same M8 silicon + FW3, same CFG-TMODE2 / TIM-SVIN —
  no code change required. Documented in the timing-receiver section.

---

## [v0.49-rtos]

### Fixed
- **Config macro ordering: `OUT_SERIAL` now respects `GPSDO_BLUETOOTH`.** The
  `OUT_SERIAL` routing macro was evaluated near the top of `gpsdo_config.h`,
  *before* `GPSDO_BLUETOOTH` (and several other feature switches) were defined
  further down. As a result `OUT_SERIAL` always resolved to USB `Serial` even
  when Bluetooth was enabled, and a build with `GPSDO_BLUETOOTH` commented out
  could fail to compile depending on what referenced it. All feature switches
  are now grouped together near the top of the file, and macros derived from
  them (`OUT_SERIAL`) are evaluated afterwards in a dedicated "Derived macros"
  section. No functional change to any enabled feature beyond Bluetooth output
  now actually going to Serial2. A scan of the other source files found no
  further define-after-use ordering issues.

### Changed
- **HT16K33 startup pattern unified with the TM1637.** At power-up the
  HT16K33 now shows `----` (segment-G dashes) instead of `oooo`, matching the
  TM1637's startup pattern — both LED clocks signal "alive, waiting for GPS"
  the same way. The `oooo` indicator is retained for the no-fix-during-
  operation case (where the TM1637 also shows `oooo`), so the two displays now
  behave identically in every state.
- **TFT splash credit line** changed from `jmnlabs + with Claude (Anthropic)`
  to `jmnlabs with Claude (Anthropic)` (dropped the `+`).

---

## [v0.48-rtos]

### Added
- **ILI9488 480×320 SPI TFT support (`GPSDO_TFT_ILI9488`).** ⚠️ Untested — no
  panel on hand yet. The existing 320×240 ILI9341/ST7789 operating screen and
  animated splash are shared and auto-scaled to 480×320 at compile time:
  width ×1.5 and height ×1.33 via independent `TFT_SX`/`TFT_SY` macros (the
  panel aspect differs from a pure 1.5×), and TFT_eSPI fonts mapped up one
  size via `TFT_F`. Geometry verified to fit the panel; not yet run on real
  hardware. Set `ILI9488_DRIVER` + `TFT_WIDTH 320`/`TFT_HEIGHT 480` (+
  `LOAD_FONT6`) in TFT_eSPI `User_Setup.h`.
- **SPI→T6963C bridge as a new display backend (`GPSDO_T6963C`).**
  ⚠️ Experimental / untested — backend is complete and compiles, but the link
  is not yet validated on clean hardware (long-wire bring-up showed ringing
  and spurious CS edges; same on the reference master → a signal-integrity
  issue, not firmware). Disabled by default; leave off until tested on
  short, point-to-point wiring.
  Drives a PowerTip PG240128 (240×128 mono) panel through the external
  `T6963C_SPI_bridge` over SPI1 using high-level drawing commands
  (`T6963C_Bridge.h`). Selectable in the config like the other displays;
  mutually exclusive with the TFT (shared SPI1 pins / display slot).
  - Reuses the TFT's SPI1 pins: `SCK PA5`, `MOSI PA7`, `CS PB13`,
    `READY PB12`; frees `PB15` (was TFT_RST).
  - Condensed 240×128 layout mirroring the TFT screen: header (title + LMT
    time), large frequency (LOGISOSO fonts), status row, value rows
    (PWM/Vctl, INA219, sensors) and a survey-in progress bar.
  - Monochrome panel → the lock/holdover colour cue becomes an inverted
    (filled) box around the status word (`LOCK` / `HOLD` / `H-LOST` /
    `NOFIX`).
  - One batched SPI transaction per refresh (single READY wait), with the
    bridge library's auto-split as a safety net; per-field change-cache to
    skip redundant redraws.
  - Static boot splash (logo + subtitle + hardware checklist); no wave
    animation, since batched SPI rendering would make it costly on a small
    mono panel.

---

## [v0.47-rtos]

### Added
- **`SV` CLI command** — enable/disable survey-in (Time Mode) on a timing
  receiver at runtime, stored in EEPROM (byte 143). `SV` shows state, `SV 0`
  disables (stay in nav mode — handy for bench testing), `SV 1` enables;
  `ES` saves, applied at next boot. Defaults to enabled on fresh EEPROM.

### Fixed
- **Survey-in polling no longer stalls the displays.** `ubx_poll_svin()`
  waited up to 1000 ms with a busy `delay()`, starving the higher-priority
  GPS task's siblings — the display task visibly lagged (worst on the
  slower-responding LEA-6T). The poll now uses a ~500 ms window that yields
  with `vTaskDelay()` between reads, so the display task runs normally while
  still reliably catching the module's TIM-SVIN reply (100-200 ms latency).
  NMEA bytes seen while scanning are forwarded to TinyGPS++ so the fix is
  not disrupted. Once a survey has replied, occasional missed polls no
  longer abort the monitor (the survey is in progress); gaps in the
  `svin dur=` sequence are gone.
- **Survey-in now exits reliably when its criteria are met.** Completion is
  declared when EITHER the receiver flags the mean position valid, OR the
  user criteria are met (accuracy ≤ limit AND duration ≥ minimum) — some
  receivers (notably the LEA-6T) reached ~0.45 m well past the minimum but
  left the survey "active", so the old `valid && !active` test never fired.
  The safety backstop is now `3 × SVIN_MIN` (min 600 s) so a slow-converging
  survey on a weak antenna gets a fair chance.
- TIM-SVIN early-survey accuracy of `0xFFFFFFFF` ("no estimate") is clamped
  to 65535 mm instead of overflowing.

### Changed
- **TFT precision**: INA219 now shows bus voltage to 3 decimals and current
  to 2 decimals; the PWM control voltage (Vctl) shows 3 decimals.

### Documentation
- README (EN/PL) notes that survey-in needs a good outdoor antenna with a
  full sky view, and records the field observation that the LEA-6T is more
  sensitive than the LEA-M8T in marginal conditions. Both modules were
  verified completing survey-in and entering Time Mode on a professional
  outdoor (survey-grade) antenna. Corrected a couple of stale source
  comments (EEPROM size 144 B, TIM-SVIN vs NAV-SVIN).

---

## [v0.46-rtos]

### Removed
- **Compile-time OCXO selection (CTI / Vectron) dropped entirely.** The `CT`
  command measures the plant gain and derives all coefficients for whatever
  oscillator is fitted, so per-OCXO defines, PID tables and the
  `DEFAULT_PWM` switch are no longer needed. The loop starts from a
  universal mid-range PWM (32767 ≈ 1.65 V) before the first `CT`.

### Added
- **Multi-variant survey-in start.** The LEA-6T and LEA-M8T accept
  different Time Mode commands (both verified in u-center), so the firmware
  tries each in turn and stops at the first ACK: `CFG-TMODE2` 0x06 0x3D
  (LEA-M8T), then the classic `CFG-TMODE` 0x06 0x1D (LEA-6T, u-blox 6). This
  auto-adapts to either module. If neither is ACKed the module is assumed to
  be already timing and is monitored anyway.

### Fixed
- **TIM-SVIN accuracy was nonsense (showed ~467 km).** The `meanV` field is
  a position *variance* in mm², not a distance — the firmware now takes its
  square root to report a 1-sigma accuracy in mm (verified against u-center:
  18113534 mm² → ~4.3 m). Survey-in duration/accuracy now read sensibly.
- **Boot hang when survey-in actually started (LEA-M8T).** The survey-in
  progress loop ran inside `gpsdo_gps_init()` — before the scheduler — and
  used `vTaskDelay()`, which hangs the system when called before
  `vTaskStartScheduler()`. It never showed on the LEA-6T because that unit
  NAKed the start and skipped the loop; the M8T ACKs it, entered the loop,
  and froze (blue LED stuck). Survey-in now only *starts* in init; progress
  is polled non-blocking from `vGpsTask` after the scheduler runs.
- **Intermittent boot hang / black displays** — `STACK_DISPLAY` raised from
  768 to 1024 words. Font scaling and the OLED clear loop had made 768
  marginal; with no stack-overflow hook this showed as a silent, sometimes-
  boots hang.
- **LEA-M8T timing module now works.** It was stuck in a 3D nav fix
  (HDOP ≈ 1) because the firmware sent it `CFG-TMODE3`, which its firmware
  (TIM 1.10, PROTVER 22) does not support. u-center confirmed the LEA-M8T
  uses the **same** `CFG-TMODE2` / `TIM-SVIN` messages as the LEA-6T. The
  timing path is unified to a single TMODE2 implementation; the separate
  `GPSDO_GPS_LEA6T` / `GPSDO_GPS_LEA8T` options are replaced by one
  `GPSDO_GPS_TIMING`, and the TMODE3 / NAV-SVIN branch is removed.
- **OLED**: the lower half of the big `GPSDO` splash (drawn with a two-row
  font) lingered behind the LMT clock — the panel is now cleared, every row
  blanked, the 2x2 font reset and the row cache invalidated when the splash
  ends. `GPSDO` and the version line are centred; footer uses
  `jmnlabs+Claude`.
- **LCD 20x4**: title/version line shifted right (two leading spaces) so the
  `-rtos` suffix is no longer truncated.
- EEPROM layout header comment corrected (143 bytes, was mislabelled 134).

### Changed
- **TFT**: the white frequency value uses a fixed-width font (font 1,
  size 3) so its digits keep a constant column position; subtitle enlarged
  and changed to `GPS-Disciplined OCXO`; logo, subtitle and the
  converging-wave animation raised; hardware checklist reveals more slowly
  with a lead-in pause so the first items are not missed; footer credit
  uses `+`. Sensor values (BMP/AHT temperature, pressure, humidity) now show
  two decimal places.

---

## [v0.45-rtos]

### Changed
- **TFT splash reworked again** to a phase-lock metaphor: the credits are
  drawn first and persist; two 2px sine waves (blue above, amber below)
  start with a visible phase offset and small vertical gap, then slowly
  converge until they coincide and merge into a single 4px green wave,
  held ~1.8 s. The hardware checklist follows.
- Serial human-readable report now shows `HDOP:TIME` in Time Mode (the
  tab-delimited machine format keeps the numeric value for plotting).

### Removed
- Redundant `SERIAL_*_BUFFER_SIZE` defines in `gpsdo_config.h` (they never
  reached the core anyway). The buffer sizes live solely in `build_opt.h`
  (`RX=256, TX=512`).

---

## [v0.44-rtos]

### Added
- **`build_opt.h`** enlarging the serial RX/TX buffers to 256 bytes
  (`-DSERIAL_RX_BUFFER_SIZE=256 -DSERIAL_TX_BUFFER_SIZE=256`). STM32duino
  applies these to the whole build including the core, which a sketch-level
  `#define` cannot reach. This prevents NMEA sentences being dropped or
  merged at 38400 baud when the GPS task is briefly preempted (the cause of
  the garbled NMEA seen on the LEA-6T).

### Changed
- **TFT boot splash reworked**: two sine waves of different colours (blue
  from the left, amber from the right) converge to the centre and merge
  into a single green 10 MHz wave — a synchronism metaphor — with the
  GPSDO logo and hardware checklist below. Timings stretched for
  readability.

### Notes
- Only GGA + RMC NMEA sentences are kept (GLL/GSA/GSV/VTG disabled), which
  together with the larger buffer keeps the bus well within budget.

---

## [v0.43-rtos]

### Added
- **Time Mode detection / `HDOP:TIME`.** A timing receiver in time-only
  mode keeps a frozen valid position but reports HDOP ≈ 99.99. Instead of
  showing that meaningless number, the displays now show `HDOP:TIME` when a
  valid position coincides with a non-meaningful HDOP (≥ 50.00). New
  `gGps.time_mode` flag.

### Changed
- **Survey-in NAK is handled gracefully.** Some timing modules (e.g.
  surplus units with a stored Time-Mode config) NAK `CFG-TMODE2/3`. The
  firmware no longer treats this as an error — it logs that the module may
  already be timing and continues; runtime Time Mode detection then reports
  the real state.
- Boot splash durations lengthened (TFT ~7 s, OLED/LCD ~4.5 s) so the
  welcome screen can actually be read.

### Fixed
- OLED splash footer no longer clips the last character (`jmnlabs/Claude`,
  spaces around the slash removed to fit the 16-column width).

---

## [v0.42-rtos]

### Fixed
- **Build error in the survey-in code** (`get_ubx_ack` called with
  class/id/timeout instead of the message-buffer pointer it expects). Both
  `ubx_start_survey_in` branches now pass the frame buffer, matching the
  function signature. LEA timing builds compile again.

### Notes
- The u-blox M8 timing module (**LEA-M8T**) is the same generation as the
  8T and uses CFG-TMODE3 / NAV-SVIN — enable `GPSDO_GPS_LEA8T` for it.

---

## [v0.41-rtos]

### Added
- **Animated boot splash on TFT**: a sweeping 10 MHz sine, the GPSDO logo,
  and a hardware checklist reconstructed from the real detection flags
  (modules show `[x]` / `[ ]`), with a discreet `jmnlabs · with Claude
  (Anthropic)` footer. Plays once, then the operating screen is drawn.
- **Boot splash on OLED** (character mode, U8x8): double-size `GPSDO`,
  version, accent line and footer.
- **Boot splash on LCD 20x4**: four-line welcome with title, subtitle and
  footer.

### Fixed
- **TFT did not update PWM / Vctl during calibration.** The display
  returned early after drawing the countdown, freezing the info grid. It
  now falls through so the PWM/Vctl cell keeps updating live during
  `C` / `CT` — matching the OLED behaviour.

---

## [v0.40-rtos]

### Added
- **LEA-6T / LEA-8T timing receiver support** (`GPSDO_GPS_LEA6T` /
  `GPSDO_GPS_LEA8T`). On these modules the firmware runs a survey-in at
  every power-up (CFG-TMODE2 on the 6T, CFG-TMODE3 on the 8T), then the
  receiver switches to a fixed-position time-only solution with a much
  cleaner 1PPS. Survey-in ends when either the minimum duration
  (`GPSDO_SVIN_MIN_SECS`, default 120 s) or the accuracy limit
  (`GPSDO_SVIN_ACC_LIMIT`, default 2000 mm) is met.
- Survey-in progress is shown on every display (`SVIN nnns nnm` on
  OLED/LCD/TFT, dashes on the LED clocks), via the new `g_svin_*` state.
- Position keeps streaming in NMEA throughout Time Mode, so location
  display and automatic timezone (`TO A`) continue to work — using the
  averaged, frozen survey-in position.
- `CHANGELOG.md` and `CHANGELOG_PL.md` are now included in the project archive.

### Notes
- NEO-6M / NEO-8M behaviour is unchanged (neither LEA option defined).

---

## [v0.39-rtos]

### Added
- OCXO warmup is now shown on every display with a live countdown
  (`WARMUP nnn s` on OLED/LCD/TFT, dashes on TM1637/HT16K33), driven by the
  new `g_warmup_active` / `g_warmup_remaining` state.

---

## [v0.38-rtos]

### Fixed
- **Steady-state PWM dither on the phase-locked algorithms (4, 5, 7, 8).**
  The dead-zone now tests the accumulated phase as well as the frequency
  error: when `|e| < 1 mHz` and `|phase| < 5 Hz·s` (≈500 ns) the loop holds
  the PWM and reports `hit`, so a locked oscillator stops being nudged by
  GPS noise every period. Small phase noise is held; real drift is still
  corrected.
- All phase algorithms now actually emit the `hit` trend on lock; FLL
  algorithms (3, 6) gained an equivalent frequency-only lock hold.
- PWM and Vctl readings on the displays now update live **during** `C` /
  `CT` calibration (a new `wait_secs_pwm` publishes PWM and samples the
  Vctl ADC each second while the main loop is busy).

---

## [v0.37-rtos]

### Changed
- `LP 8` and `LP 9` now show where those algorithms actually read their
  gains: algo 8 (hybrid) uses `g_pid[6]` (FLL branch) + `g_pid[7]` (PLL
  branch); algo 9 (NN) uses fixed network weights, so only `NS` / `IL`
  apply. Prevents the empty `g_pid[8]/[9]` from looking "untuned" after
  `CT`.

---

## [v0.36-rtos]

### Added
- Calibration progress shown on all displays: `CAL nnn s` countdown in the
  frequency field (OLED/LCD/TFT) and `CAL` on the LED clocks (TM1637 /
  HT16K33), via `g_calib_active` / `g_calib_remaining`.

---

## [v0.35-rtos]

### Added
- **`CT` (Calibrate & Tune) command.** Measures the plant gain `K` from a
  three-point PWM sweep (1.5 / 2.0 / 2.5 V) with a least-squares fit, finds
  the PWM for exactly 10 MHz, and derives PID coefficients for all
  algorithms from `K` (PLL: `Kp = 0.40/K`; FLL: `Kp = 0.35/K`,
  `Ki = Kp/300`, `Kd = Kp·73`; NN: `max_step = 0.05/K`). Sanity-checked,
  non-destructive; `ES` saves the result.

---

## [v0.34-rtos]

### Changed
- **Two-timescale PLL tuning for "fast capture, gentle phase-hold".** The
  dominant term acts on the frequency error (`Kp ≈ 0.4/K`) for quick,
  overshoot-free capture; small phase terms remove slow drift. A shared
  output stage adds a slew-rate limit (≈12 LSB/step for the PLLs, 40 for
  the hybrid) and a near-lock dead-zone, so a large overnight phase drift
  is spread over several periods instead of one big PWM jump.

---

## [v0.33-rtos]

### Fixed
- **Algorithm 9 (NN) ran away upward.** The previous "trained" weights had a
  large output bias (≈ −0.96 at zero error → constant PWM ramp). Replaced
  with an analytically constructed, bias-free, odd-symmetric network: zero
  input gives exactly zero output.
- **Algorithms 4 / 5 / 7 and the PLL branch of 8 drifted.** They used a
  rolling-window average as a stand-in for phase, which lagged the 10 s
  update by 500–1000 s and wound the integrator up. Replaced with true
  phase accumulation (`phase += (avg10 − 10 MHz)·10 s`, the exact cycle
  count), feeding back with a 10 s lag.
- The `GPS fix acquired` message now distinguishes the first fix after boot
  from a genuine recovery after fix loss.

### Added
- **Automatic timezone (`TO A`).** Local time follows the GPS position: a
  compact European civil-zone rule set plus the EU DST rule, or a solar
  `round(lon/15)` zone elsewhere. `TO <n>` keeps the manual mode. The mode
  is saved to EEPROM (byte 142, now 143 bytes total) and restored at boot.

---

## [v0.32-rtos]

### Fixed
- **Hardware detection report.** Added a robust dual-verification I2C probe
  (address ACK + 1-byte read-back). OLED and HT16K33 were previously
  reported `OK` unconditionally / on an unreliable ACK; they now report
  real presence. TM1637 and TFT are marked `enabled (write-only — not
  verifiable)`.
- **TFT frequency colour.** The green "locked" colour is now derived from
  the actual deviation from 10 MHz (≤1 mHz on the 10000 s window or ≤10 mHz
  on 1000 s), independent of the algorithm — so a locked algo 8 turns green
  too, rather than only on the rarely-emitted `hit` trend.

---

## [v0.31-rtos]

### Added
- **HT16K33 4-digit clock support** (I2C 0x70): a self-contained driver
  (HH:MM with blinking colon, `oooo` when searching), shareable with the
  LCD on the same bus — no extra pins. TM1637 retained.
- Unified startup hardware report: every optional device reports `OK` or
  `not found` in a consistent `HW:` format.
- New hardware architecture diagram in both READMEs (TFT + HT16K33).

---

## [v0.30-rtos]

### Added
- **TFT 240×320 support (ILI9341 / ST7789)** via TFT_eSPI on hardware SPI1
  (SCK PA5, MOSI PA7, RES PB15, DC PB12, CS PB13). Landscape layout: header
  bar, large colour-coded frequency, two-column info grid, sensor row, and
  a colour-coded status bar. Selective per-cell redraw keeps SPI traffic
  low. DisplayTask stack raised to 768 words when the TFT is enabled.
  Both controllers tested on hardware.

---

## [v0.29-rtos]

### Fixed
- **picDIV synchronisation.** Arming is now deferred until a GPS fix is
  present (a stopped divider with no 1PPS on Sync would otherwise hang
  dead); a dedicated flag replaces the millis-timestamp guard (wrap-safe);
  auto-arm after calibration was removed (the loop hasn't converged yet).
  Added clear serial feedback. README documents FLL phase random-walk vs
  PLL phase-lock for long-term 1PPS alignment.

---

## [v0.28-rtos]

### Fixed
- **PWM range with 3.3 V DAC.** The STM32 PWM reaches only 0–3.3 V of the
  0–4 V EFC input (82.5 %), so the accessible tuning is −10…+14.75 Hz (CTI)
  and −20…+13 Hz (Vectron). Default PWM corrected per-OCXO: 32767 (CTI,
  1.65 V midpoint) and 39718 (Vectron, 2.0 V nominal).

---

## [v0.27-rtos]

### Fixed
- **Vectron C4550A1-0213 parameters.** Corrected to its real operating
  point: 5 V supply, 0–4 V EFC, Kv = 10 Hz/V (0.504 mHz/LSB), scale factor
  1.333 vs CTI (gains × 0.75), shared default PWM.

### Changed
- `README_EN.md` renamed to `README.md` (GitHub default); `README_PL.md`
  unchanged.

---

## [v0.26-rtos]

### Added
- **OCXO selection** in `gpsdo_config.h` (`GPSDO_OCXO_CTI_OSC5A2B02` /
  `GPSDO_OCXO_VECTRON_C4550`), with per-OCXO compile-time PID defaults and
  default PWM. Falls back to CTI values if none is selected.
- `SP`, `F`, `C`, `T` documented in the help text and READMEs.

---

## [v0.25-rtos]

### Added
- `g_pressure_offset` (`PO`) and `g_altitude_offset` (`AO`) now saved to and
  restored from EEPROM (bytes 134–141, 142 bytes total).
- `V` command expanded with full author/credit information and GitHub links.

---

## [v0.24-rtos]

### Fixed
- **Bluetooth output.** All runtime messages route through an `OUT_SERIAL`
  macro (Serial2 when `GPSDO_BLUETOOTH` is defined, else USB Serial).

### Added
- Report pause/resume (`RP` / `RR`) to quiet the data stream during
  configuration.
- Algorithm PID parameters saved to EEPROM (signature `GPSD2`).
- Professional file-header documentation across all source files; README
  rewritten from scratch (project description, hardware principle, software
  architecture) in Polish and English; GitHub URL added to every file and
  to the serial banner.

---

## [v0.23-rtos]

### Added
- **Runtime PID tuning over CLI** — `LP`, `KP`, `KI`, `KD`, `IL` for
  algorithms 3–7, `BC` / `BS` for the algo 8 blend, `NS` for the algo 9 NN
  step. Coefficients moved to a global `g_pid[10]` array.

---

## [v0.22-rtos]

### Added
- Yellow LED 4-state machine (off / on / slow pulse = manual holdover /
  fast pulse = auto-holdover) and automatic holdover on GPS fix loss with
  `H` / `A` indicators on OLED and LCD.

---

## [v0.21-rtos]

### Added
- OLED row-0 clock (local time + day of week) after the version splash;
  LCD line-2 date/day rotating view. Day-of-week (Zeller) and local-time
  offset helpers.

---

## [v0.20-rtos]

### Changed
- Unified 4-character trend strings; corrected OLED/LCD frequency
  formatting; build-time guard against LCD + TM1637 together; fixed the
  André Balsa source URL.

---

## [v0.19-rtos]

- First tracked FreeRTOS port baseline: STM32F411CE BlackPill, frequency
  measurement via TIM2 ETR + TIM3 1PPS capture, ring-buffer averaging,
  PWM-DAC discipline loop, GPS/NMEA parsing, OLED / LCD / TM1637 displays,
  optional AHT/BMP/INA sensors, and the initial control algorithms.
