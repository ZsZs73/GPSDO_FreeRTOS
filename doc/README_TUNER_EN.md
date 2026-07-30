# GPSDO Tuner

A desktop console for tuning the loop live and watching what it does: three
scrolling plots, one tab per parameter group, and a manual command box for
anything the tabs do not cover.

It is a **tuning aid**, not a measurement instrument — see *Limitations* below
before drawing conclusions from what it shows.

---

## Requirements

**Python** 3.9 or newer, plus four packages:

```
pip install PySide6 pyqtgraph pyserial tzdata
```

| Package | Used for |
|---------|----------|
| PySide6 | the Qt user interface |
| pyqtgraph | the live plots |
| pyserial | talking to the board |
| tzdata | the **Generate tz_table.h** button only |

`tzdata` is optional if you never press that button, and on Linux or macOS the
system already provides the same zone data. On Windows it is the only source,
since no IANA database ships with the OS. Refresh it later with
`pip install -U tzdata`.

Run with `python gpsdo_tuner.py`, or double-click it on Windows: the console
window that opens is minimised to the taskbar automatically, and stays
available in case a traceback needs reading.

---

## Version matching

The tuner carries a `TOOL_VERSION` tracking the firmware release it was written
for. On connect it reads the board's own version and compares:

- **match** — the status bar shows `connected — firmware vX.YZ`
- **mismatch** — the status bar and the Raw monitor both say so

A mismatch is not fatal and the tuner will still talk to the board, but expect
fields to read oddly or commands to be rejected: an older tuner does not know
about newer telemetry, and a newer one may send verbs the board has never heard
of. Use the pair that shipped together.

---

## Tabs

| Tab | Purpose |
|-----|---------|
| **LTIC (algo 10)** | Per-stage PID for the three-stage phase loop, plus the detector calibration |
| **LTIC-Lars (algo 11)** | The continuous-PI parameters (`LG`, `LD`, `LTC`, …) |
| **FA damping** | Damping-average window, per stage |
| **PID algo 3-9** | Kp / Ki / Kd / I_LIMIT for the frequency-domain algorithms |
| **Calibration** | `LC`, `CT` and the detector constants |
| **Raw monitor** | Everything the board sends, unparsed |
| **Help** | The full firmware command reference |

Every parameter group is read on connect, so the panels start populated rather
than empty.

---

## Plots

Three panes, updated once per second. What the upper two show depends on which
algorithm the board reports:

| | Algorithms 10 / 11 (LTIC) | Algorithms 0-9 |
|---|---|---|
| Top | Phase `dph` (ns) | Learned drift (LSB) |
| Middle | Detector `Vphase` (V), with band guides | Control voltage `Vctl` (V) |
| Bottom | Frequency error (Hz) | Frequency error (Hz) |

Only the LTIC loops have a phase detector, so under any other algorithm those
two panes would sit empty for the entire session. They are repointed instead,
and the titles follow automatically — no setting to change.

### Span and Follow

**Span** sets how much history is visible: 1 min, 5 min, 15 min, 1 h, or *all*.
With a span selected the trace scrolls leftwards at a constant scale instead of
the axis stretching to cover the whole buffer.

**Follow live** keeps the window pinned to the newest sample. Drag or wheel any
plot and it unticks itself, handing the axis to the mouse so the whole buffer
can be explored; tick it again — or change the Span — to jump back to live.

**Clear plots** discards every buffered sample and restarts the time axis at
zero — useful after a false start, and quicker than restarting the tool and
losing the connection with it. It clears the buffers as well as the traces, so
nothing scrolls back into view afterwards.

**About** replays the boot animation, for no reason beyond the pleasure of it.

---

## Limitations

**History is capped at 30 hours.** The tuner holds 108 000 samples at the
1 Hz telemetry rate. That covers a full 24-hour acquisition with room to spare,
but anything older is discarded as new data arrives and cannot be recovered.
Nothing is written to disk.

**The plots are not a logger.** Plotted data lives in memory only and is lost
when the window closes. Use **Start logging** (Raw monitor tab) for anything you
intend to keep: it writes every received line to
`gpsdo_YYYY-MM-DD_HH-MM-SS.log` next to the script, line-buffered, so a run that
ends badly still leaves usable data. Note it captures the *raw telemetry text*,
not the plotted series — for ADEV and long comparisons against a reference, feed
that file to TimeLab or similar.

**Generate tz_table.h** rebuilds the firmware's timezone table from this
machine's IANA data and writes `tz_table.h` next to the script. It replaces the
old `gen_tz_table.py`, so the tuner is now the only script to keep.

Zone data comes from the system database on Linux/macOS, or from the `tzdata`
Python package — which is how it works on Windows, where no IANA database ships
with the OS. If the button reports no data, run `pip install tzdata`; to refresh
it later, `pip install -U tzdata`. The generated header records which IANA
release it came from, where that can be established.

> IANA itself publishes *source* that needs the `zic` compiler, so downloading
> from them directly would not help — the `tzdata` package is the same data
> already compiled.

**The Raw monitor pane holds only the last ~2500 lines** (under five minutes at
the telemetry rate). That is a display limit, not a logging one: once logging is
started the file gets everything regardless of what the pane still shows.

**Resolution is the telemetry rate.** One sample per second, so anything faster
than about 2 s is invisible: a fast limit cycle or per-PPS jitter will not show
up, and what you see has already been averaged inside the firmware.

**One connection at a time.** The serial port is exclusive. Close any other
terminal on the same port first, and remember the tuner holds it while open.

**The plots trust the board.** Values are parsed from the telemetry text as
sent. If the firmware reports a stale or wrong figure, the tuner draws it
faithfully — it does not cross-check anything.

**Writes are not persistent.** Setting a parameter changes it in RAM only.
Loop-tuning parameters need an explicit `ES` (the reply names the exact
command); preferences save themselves and say so.

---

*Part of GPSDO FreeRTOS — [firmware manual](README_EN.md) · [changelog](CHANGELOG_EN.md) · [repository](https://github.com/jmnlabs/GPSDO_FreeRTOS)*
