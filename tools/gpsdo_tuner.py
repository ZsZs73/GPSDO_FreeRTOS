#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
GPSDO Tuner — live parameter tuning and phase visualisation
============================================================

A real-time control panel for the GPSDO_FreeRTOS firmware. It reads the serial
telemetry, plots phase / control voltage / frequency error live, and exposes the
firmware's tuning commands (PID gains for every algorithm, the LTIC three-stage
loop, the FA damping windows, and the detector calibration) as direct controls —
so each builder can trim the loop to their own OCXO and phase detector instead of
chasing a single set of compile-time defaults that can never suit every board.

Every parameter is read back from the firmware before you touch it, written live
with a single click, and can be reverted from EEPROM (`ER`) or committed with
`ES`. The controls are deliberately direct: this is a bench tuning tool for people
who know their hardware, not a guard-railed appliance.

Credits
-------
Inspired by GPSDO_log.py by "lucido" (the live Vphase/Vctl/dPh/qErr logger with
PyQtGraph and a serial TX line) — this tool grew out of that idea and reuses its
overall shape: a serial worker thread, configurable live plots, a command line
and a raw monitor. The tuning panels, the parameter read-back, and the phase-ramp
visualiser are new here.

  Original logger .............. lucido
  GPSDO_FreeRTOS firmware ...... J. M. Niewiński (jmnlabs), from André Balsa's
                                 v0.06c Arduino GPSDO
  Algorithm 11 (continuous PI) . the late Lars Walenius
  Algorithm 12 (multi-level) ... Alan Cashin (MIS42N on EEVblog), whose Budget
                                 GPSDO is also the origin of the zero-crossing
                                 correction, the dithered PWM and the CS
                                 self-assessment idea. The Multi-level (algo 12)
                                 tab drives his design, so his name belongs on it.
  Measurements, algos 10 & 11 .. Dan Wiering (rubidium reference)
  This tuning tool ............. built for the jmnlabs GPSDO project

Dependencies:  pip install pyserial pyqtgraph PySide6

Usage:  python gpsdo_tuner.py

Full docs: doc/gpsdo_tuner_EN.md (also PL, ES).
"""

import sys
import os
import re
import time
import math

# Tracks the firmware release it was built against. The tuner reads the board's
# own version on connect and says so when the two disagree — an older tuner
# against newer firmware silently mis-parses telemetry and writes commands the
# board no longer understands, which is a confusing way to lose an evening.
# Bump this whenever the firmware version changes, even if nothing here moved.
TOOL_VERSION = "1.05"
from collections import deque, defaultdict

# Force pyqtgraph to use the same Qt binding as the rest of this file (PySide6).
# pyqtgraph auto-detects a binding, and if PyQt5/PyQt6 is also installed it may
# pick that instead — then its PlotWidget is a PyQt widget that PySide6's layouts
# reject ("addWidget called with wrong argument types"). Pinning the env var
# before pyqtgraph is imported keeps everything on one Qt.
os.environ["PYQTGRAPH_QT_LIB"] = "PySide6"

try:
    import serial
    import serial.tools.list_ports
except ImportError:
    print("pyserial not found — run: pip install pyserial")
    sys.exit(1)

try:
    from PySide6.QtCore import Qt, QThread, Signal, QTimer
    from PySide6.QtWidgets import (
        QApplication, QMainWindow, QWidget, QVBoxLayout, QHBoxLayout,
        QGridLayout, QLabel, QLineEdit, QPushButton, QComboBox, QTextEdit,
        QDoubleSpinBox, QSpinBox, QTabWidget, QGroupBox, QSplitter, QCheckBox,
        QScrollArea,
        QMessageBox,
    )
    from PySide6.QtGui import (QFont, QPainter, QPen, QColor, QLinearGradient,
                               QPalette)
    import pyqtgraph as pg
except ImportError:
    print("PySide6 / pyqtgraph not found — run: pip install pyqtgraph PySide6")
    sys.exit(1)


def theme_colours(widget):
    """Text colours that survive both light and dark desktop themes.

    The Help tab and the small note labels used to hardcode colours chosen
    against a white background — #333 descriptions and a navy heading. Under the
    Windows 11 dark theme that put dark grey on near-black and the command
    reference was effectively invisible. Qt already knows which palette the OS
    handed us, so ask it and pick accordingly rather than guessing."""
    dark = widget.palette().color(QPalette.Window).lightness() < 128
    if dark:
        return {"muted": "#9aa4b2",   # secondary text
                "head":  "#7ab8ff",   # section headings
                "verb":  "#ffb27a",   # command verbs
                "desc":  "#d6dbe3"}   # descriptions
    return {"muted": "#666666",
            "head":  "#1a4d80",
            "verb":  "#8a2b2b",
            "desc":  "#333333"}


# ------------------------------------------------------------------------------
# Firmware command registry
# ------------------------------------------------------------------------------
# Each tuning parameter maps to a firmware CLI verb. The firmware accepts the
# verb with no argument to READ (it prints the value), or with an argument to
# WRITE. The GUI uses exactly the same grammar so the two never drift apart.
#
# PID for algorithms 3-9:  KP/KI/KD/IL <algo> <value>   (read back with LP <algo>)
# LTIC three-stage PID:    ACQ  -> AQP/AQI/AQD/AQL <value>
#                          DPLL -> DPP/DPI/DPD/DPL <value>
#                          LOCK -> LKP/LKI/LKD/LKL <value>
# LTIC calibration:        LNV/LZO/LRN <value>,  LPOL -1|0|1,  LCV <value>
# LTIC thresholds:         LAT (acq ns), LIV (lock interval s)
# FA damping windows:      FAD/FAL/FA 10|100|1000
# ------------------------------------------------------------------------------

# LTIC stage PID verbs: (Kp, Ki, Kd, I_limit)
LTIC_STAGE_VERBS = {
    "ACQ":  ("AQP", "AQI", "AQD", "AQL"),
    "DPLL": ("DPP", "DPI", "DPD", "DPL"),
    "LOCK": ("LKP", "LKI", "LKD", "LKL"),
}

# LTIC calibration verbs and their sensible ranges (lo, hi, decimals)
LTIC_CAL = {
    "LNV": ("ns_per_volt", 0.0, 1e6, 3),
    "LZO": ("zero_offset V", 0.0, 3.3, 4),
    "LRN": ("range_ns (not self-learn)", 0.0, 1e9, 2),
    "LCV": ("centre_v", 0.0, 3.3, 3),
    "LAT": ("acq_thresh ns", 1.0, 5000.0, 2),
    "LIV": ("lock_interval s", 1.0, 3600.0, 0),
}

FA_VALUES = ["10", "100", "1000"]

# Visible span of the live plots, as (label, seconds); 0 means "everything held".
# Without a window the X axis stretches to cover the whole buffer, so after an
# hour of acquisition 3600 points are crushed into the plot width and the trace
# stops appearing to move — new samples just thicken the right-hand edge. A
# fixed span makes the plot scroll instead, which is what the eye needs to judge
# a loop that is settling.
PLOT_SPANS = [("1 min", 60), ("5 min", 300), ("15 min", 900),
              ("1 h", 3600), ("all", 0)]
PLOT_SPAN_DEFAULT = "5 min"

# Which telemetry field feeds each of the two upper plots, per algorithm family.
# The LTIC loops (10, 11) are the only ones with a phase detector, so under any
# other algorithm those two panes would sit empty for the whole session. Drift
# and Vctl are the informative pair there: drift is what the self-learning
# feed-forward is doing, Vctl is the voltage actually reaching the OCXO. PWM was
# the obvious alternative for the second pane but carries the same information as
# Vctl — same signal either side of the RC — so it would waste a window.
PLOT_SERIES = {
    "ltic": [("dph",    "Phase  dph (ns)",                        "#22aa44"),
             ("Vphase", "Detector Vphase (V) — ramp position",    "#2277cc")],
    "pid":  [("drift",  "Learned drift (LSB) — LRN feed-forward", "#22aa44"),
             ("Vctl",   "Control voltage Vctl (V)",               "#2277cc")],
    # Algorithm 12 does not use the self-learning feed-forward, and its phase
    # comes straight from the detector rather than through a loop filter, so
    # neither pair above describes it. The top pane shows the phase it is
    # actually accumulating.
    "mlacc": [("ph",     "Phase error (ns, LTIC detector)",        "#22aa44"),
              ("Vctl",   "Control voltage Vctl (V)",               "#2277cc")],
}

# Algorithm 11 (LTIC-Lars) parameters: verb -> (label, lo, hi, decimals).
# These share the LTIC tab, shown when algorithm 11 is active. Saved by ES LTIC
# alongside the algo-10 block. gain is board-specific (see firmware note).
LARS_PARAMS = {
    "LG":  ("gain (DAC/ns)",   0.0, 10000.0, 3),
    "LD":  ("damping",         0.0, 1000.0,  3),
    "LTC": ("time_const s",    1.0, 600.0,   0),
    "LFD": ("filter_div",      1.0, 100.0,   0),
    "LTO": ("tic_offset ADC",  0.0, 4095.0,  0),
    "LPL": ("lock_ns_lim",     1.0, 10000.0, 0),
    "LPF": ("lock_factor",     1.0, 100.0,   0),
    "LTK": ("temp_coeff",  -32000.0, 32000.0, 0),
    "LTR": ("temp_ref ADC",    0.0, 4095.0,  0),
}

# Firmware prints each algo 11 parameter as "<field>=<value>" when queried with
# no argument. Map that field name back to the verb so the readback lands in the
# right spinbox. Field names match the firmware's cli_puts() strings.
LARS_FIELD_NAMES = {
    "LG":  "gain",
    "LD":  "damping",
    "LTC": "time_const_s",
    "LFD": "filter_div",
    "LTO": "tic_offset",
    "LPL": "lock_ns_lim",
    "LPF": "lock_factor",
    "LTK": "temp_coeff",
    "LTR": "temp_ref",
}

# Algorithm 12 (multi-level accumulator, after Alan Cashin) parameters:
# verb -> (label, lo, hi, decimals). The per-level limit table (MLP/MLS) is
# 22 values and is edited via CLI (ML lists it); only the scalar tuning is
# exposed as spinboxes here. Saved by ES ALGO12. gain is board-specific.
MLACC_PARAMS = {
    "MG":  ("gain (LSB/ns)", 0.0, 10000.0, 3),
    "MR":  ("run level",     0.0,    10.0, 0),
    # Limit source and its one parameter. Kept beside the gain in the GUI but
    # deliberately separate from it in the firmware: the gain is a property of
    # the oscillator (LSB per ns, so it changes with Vctl sensitivity) and the
    # limits are a property of the phase noise the board sees.
    "MF":  ("limits 0-3",    0.0,     3.0, 0),
    "MFT": ("target s",      0.0, 65535.0, 0),
}

# Per-level phase limits, in nanoseconds. Eleven levels spanning 2 s to 2048 s;
# only one was ever derived — 125 ns at 128 s, from the original 10 MHz +/-0.01 Hz
# specification — and Alan describes the rest as arbitrary. They are exposed for
# editing because they are the thing most likely to need changing per oscillator.
MLACC_LEVEL_SECS = [2, 4, 8, 16, 32, 64, 128, 256, 512, 1024, 2048]

# Firmware prints each algo 12 scalar as "<field>=<value>" when queried with
# no argument. Map that field name back to the verb so the readback lands in
# the right spinbox. Field names match the firmware's cli_puts() strings.
# Prefixed, because algorithm 11 answers LG with "gain=" too. Without the prefix
# the Lars absorber — which runs first — swallowed the reply to MG and returned,
# so the algo-12 boxes never filled and neither did anything after them in the
# same pass. Two commands answering to the same field name is a trap worth
# avoiding rather than ordering around.
MLACC_FIELD_NAMES = {
    "MG":  "m_gain",
    "MR":  "m_run_level",
    "MF":  "m_thr_src",
    "MFT": "m_thr_tgt",
}


# ------------------------------------------------------------------------------
# Telemetry parser
# ------------------------------------------------------------------------------
class TelemetryParser:
    """Pulls numeric fields out of the firmware's serial lines.

    Field extraction is deliberately case-insensitive and tolerant of the
    firmware's spacing, matching the same approach lucido's logger used so that
    a rename like dPh->dph never breaks a plot. It also parses the LL and LP
    read-backs so the tuning panels can show the firmware's current values.
    """

    # Live telemetry fields -> label used on plots / spin boxes
    LIVE_FIELDS = ["Vphase", "Vctl", "dph", "qErr", "PWM", "drift", "damp",
                   "scale", "phase",
                   # Algorithm 12: the phase it accumulates (ns), which level
                   # last acted, and the cumulative correction count.
                   # All three come from the Learn line.
                   "ph", "level", "corr", "arm", "sig", "zc"]

    def extract(self, line, name):
        """Return the first number following `name` on the line, or None.
        The leading \\b prevents a short field name from matching inside a longer
        one — e.g. 'phase' (algo 11) must not match the 'phase' inside 'Vphase'."""
        pattern = r"\b" + re.escape(name) + r"\s*[:=]?\s*([-+]?\d+(?:\.\d+)?)"
        m = re.search(pattern, line, re.IGNORECASE)
        return float(m.group(1)) if m else None

    # What each loop state means, shown beside the label so the reader does not
    # have to know the firmware. Algorithm 12 has its own vocabulary: it holds in
    # several distinct ways and saying which one matters when nothing is moving.
    STATE_HINT = {
        "LOCK":   "locked",
        "DPLL":   "phase tracking",
        "PLL":    "phase tracking",
        "ACQ":    "acquiring",
        "SURVEY": "survey-in",
        "WARMUP": "OCXO warming",
        "HOLD":   "holdover",
        "CORR":   "correction applied",
        "NOPH":   "holding - detector not reading",
        "NoCT":   "holding - run CT first",
        "NoLT":   "algo 12 needs the LTIC detector",
        "WAIT":   "waiting for frequency data",
    }

    def parse_state(self, line):
        """Loop state from the PWM/Vctl telemetry line, e.g. '... LOCK'.

        The algorithm-12 states were missing here, so parse_state returned None
        for them and the display kept showing whatever it had last recognised —
        the telemetry said NOPH while the tuner still read ACQ. A state the
        firmware can report and the tuner cannot name is worse than no state at
        all, because it looks like information.
        """
        # DPLL must be tested before PLL: "DPLL" contains "PLL", and \b would
        # otherwise let the shorter algo-11 label match an algo-10 line. NoCT and
        # NoLT differ only in one letter, so both are listed explicitly.
        for st in ("LOCK", "DPLL", "PLL", "ACQ", "SURVEY", "WARMUP", "HOLD",
                   "NOPH", "NoCT", "NoLT", "CORR", "WAIT"):
            if re.search(r"\b" + st + r"\b", line):
                return st
        return None

    def parse_freq(self, line):
        """Best available frequency estimate from a 'Freq:' line."""
        m = re.search(r"1ks:\s*([\d.]+)", line)
        if m:
            return float(m.group(1))
        m = re.search(r"100s:\s*([\d.]+)", line)
        if m:
            return float(m.group(1))
        m = re.search(r"Freq:\s*([\d.]+)", line)
        return float(m.group(1)) if m else None

    def parse_ll(self, text):
        """Parse an LL read-back block into {stage: {Kp,Ki,Kd,IL}, cal:{...}}.

        The firmware prints LL with one field per line — the ACQ/DPLL/LOCK label
        appears with its Kp on one line, then Ki, Kd, IL each follow on their own
        lines. So this scans line by line, tracking which stage the Kp most
        recently named, and files the following Ki/Kd/IL under it. It also picks
        up the cal fields (LNV/LZO/LRN/...) wherever they land.
        """
        out = {"ACQ": {}, "DPLL": {}, "LOCK": {}, "cal": {}}
        current = None
        for raw in text.splitlines():
            line = raw.strip()
            # a stage line names the stage and carries its Kp
            m = re.match(r"(ACQ|DPLL|LOCK):\s*Kp=([-+]?[\d.]+)", line, re.IGNORECASE)
            if m:
                current = m.group(1).upper()
                out[current]["Kp"] = float(m.group(2))
                continue
            # Ki/Kd/IL on their own lines belong to the current stage
            if current:
                m = re.match(r"(Ki|Kd|IL)=([-+]?[\d.]+)", line, re.IGNORECASE)
                if m:
                    out[current][m.group(1)] = float(m.group(2))
                    if m.group(1).upper() == "IL":
                        current = None   # IL is the last field of a stage
                    continue
            # cal / threshold / polarity fields, wherever they appear
            for key, rx in (("LNV", r"LNV=([-+]?[\d.]+)"),
                            ("LZO", r"LZO=([-+]?[\d.]+)"),
                            ("LRN", r"LRN=([-+]?[\d.]+)"),
                            ("LAT", r"LAT=([-+]?[\d.]+)"),
                            ("LIV", r"LIV=([-+]?\d+)"),
                            ("LPOL", r"LPOL=([-+]?\d+)"),
                            ("LCV", r"LCV=([-+]?[\d.]+)")):
                m = re.search(rx, line, re.IGNORECASE)
                if m:
                    out["cal"][key] = float(m.group(1))
        return out

    def parse_lp(self, text):
        """Parse an 'Algo N Kp=..' block; Ki/Kd/IL may be on following lines."""
        m = re.search(r"Algo\s+(\d+)\s+Kp=([-+]?[\d.]+)", text, re.IGNORECASE)
        if not m:
            return None, None
        algo = int(m.group(1))
        vals = {"Kp": float(m.group(2))}
        for k in ("Ki", "Kd", "IL"):
            mm = re.search(k + r"=([-+]?[\d.]+)", text, re.IGNORECASE)
            if mm:
                vals[k] = float(mm.group(1))
        return algo, vals

    def parse_fa(self, text):
        """Parse 'FA windows: DPLL=100s LOCK=100s'."""
        m = re.search(r"DPLL=(\d+)s?\s+LOCK=(\d+)", text, re.IGNORECASE)
        if m:
            return int(m.group(1)), int(m.group(2))
        return None, None


# ------------------------------------------------------------------------------
# Serial worker thread
# ------------------------------------------------------------------------------
# A background QThread owns the serial port: it reads lines and emits them, and
# accepts outgoing commands via send(). Keeping all port I/O off the GUI thread
# is the same pattern lucido's logger used, and it keeps the plots smooth while
# commands are flying back and forth.
class SerialWorker(QThread):
    line_received = Signal(str)
    connection_changed = Signal(bool, str)

    def __init__(self):
        super().__init__()
        self.ser = None
        self.port = None
        self.baud = 115200
        self._running = False
        self._tx_queue = deque()

    def configure(self, port, baud):
        self.port = port
        self.baud = baud

    def send(self, text):
        """Queue a command for transmission (CR-terminated)."""
        if not text.endswith("\r") and not text.endswith("\n"):
            text += "\r\n"
        self._tx_queue.append(text.encode("ascii", errors="ignore"))

    def stop(self):
        self._running = False

    def run(self):
        try:
            self.ser = serial.Serial(self.port, self.baud, timeout=0.1)
        except serial.SerialException as e:
            self.connection_changed.emit(False, str(e))
            return
        self._running = True
        self.connection_changed.emit(True, self.port)
        buf = b""
        while self._running:
            # drain TX queue
            while self._tx_queue:
                try:
                    self.ser.write(self._tx_queue.popleft())
                except serial.SerialException:
                    pass
            # read available bytes, split into lines
            try:
                data = self.ser.read(256)
            except serial.SerialException:
                break
            if data:
                buf += data
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    text = line.decode("utf-8", errors="replace").rstrip("\r")
                    if text:
                        self.line_received.emit(text)
            else:
                time.sleep(0.01)
        try:
            if self.ser and self.ser.is_open:
                self.ser.close()
        except Exception:
            pass
        self.connection_changed.emit(False, "disconnected")


# ------------------------------------------------------------------------------
# Main window
# ------------------------------------------------------------------------------
class GpsdoTuner(QMainWindow):
    # 30 h at 1 Hz telemetry: enough headroom to scroll back through a full
    # 24-hour acquisition and still hold the run-up to it. Costs a few MB of
    # host RAM, which is free on a PC; the firmware is unaffected either way.
    MAXPTS = 108000

    def __init__(self):
        super().__init__()
        self.setWindowTitle(f"GPSDO Tuner v{TOOL_VERSION} — live tuning & phase visualisation")
        # Fit a 1366x768 laptop panel with room for the taskbar, rather than
        # assuming a desktop monitor. showMaximized() below takes whatever is
        # actually available.
        self.resize(1180, 700)

        self.parser = TelemetryParser()
        self.worker = SerialWorker()
        self.worker.line_received.connect(self.on_line)
        self.worker.connection_changed.connect(self.on_conn)

        # rolling data buffers
        self.t0 = time.time()
        self.data = defaultdict(lambda: deque(maxlen=self.MAXPTS))
        self.tbuf = deque(maxlen=self.MAXPTS)
        self.last_state = "?"
        self._active_algo = None   # set from telemetry; toggles LTIC tab sections
        self._logfile = None       # open handle while logging to file, else None
        self._log_path = ""
        self._log_lines = 0
        self._monitor_lines = 0
        self._ll_buf = []
        self._ll_active = False
        self._lp_buf = []
        self._lp_active = False

        # widgets that read-back panels populate (verb -> spinbox)
        self.ltic_boxes = {}     # (stage, k) -> spinbox
        self.cal_boxes = {}      # verb -> spinbox
        self.pid_boxes = {}      # k -> spinbox (for the currently shown algo)

        self._build_ui()

        # periodic replot (decoupled from serial rate)
        self.timer = QTimer(self)
        self.timer.timeout.connect(self.refresh_plots)
        self.timer.start(250)

    # ---- UI construction --------------------------------------------------
    def _build_ui(self):
        central = QWidget()
        self.setCentralWidget(central)
        outer = QVBoxLayout(central)

        outer.addLayout(self._build_connect_row())

        split = QSplitter(Qt.Orientation.Horizontal)
        split.addWidget(self._build_plot_area())
        split.addWidget(self._build_control_tabs())
        split.setStretchFactor(0, 3)
        split.setStretchFactor(1, 2)
        # Stretch factors alone are advisory — they divide the SPARE space, not
        # the total. Give the splitter explicit starting sizes as well, or a wide
        # tab still claims more than its share on the first show.
        # Proportional, not absolute. Fixed sizes of 1100 + 620 add up to 1720,
        # which on a 1366-wide screen made Qt report a minimum the display could
        # not satisfy — "Unable to set geometry ... minimum size: 1383x363". The
        # panes are told to take three fifths and two fifths of whatever the
        # window actually has, and both are allowed to shrink; the tabs scroll if
        # their content will not fit, which is what the scroll areas are for.
        split.setSizes([600, 400])
        split.setChildrenCollapsible(True)
        for i, w in enumerate(( 400, 260)):
            split.widget(i).setMinimumWidth(w)
        outer.addWidget(split, 1)

        outer.addLayout(self._build_command_row())

    def _build_connect_row(self):
        row = QHBoxLayout()
        self.port_combo = QComboBox()
        self.refresh_ports()
        refresh = QPushButton("Refresh")
        refresh.clicked.connect(self.refresh_ports)
        self.baud_combo = QComboBox()
        self.baud_combo.addItems(["115200", "57600", "38400", "9600"])
        self.connect_btn = QPushButton("Connect")
        self.connect_btn.clicked.connect(self.toggle_connect)
        self.status_lbl = QLabel("disconnected")
        self.state_lbl = QLabel("state: ?")
        self.state_lbl.setStyleSheet("font-weight: bold;")
        for w in (QLabel("Port:"), self.port_combo, refresh,
                  QLabel("Baud:"), self.baud_combo, self.connect_btn,
                  self.status_lbl):
            row.addWidget(w)
        row.addStretch(1)
        row.addWidget(self.state_lbl)
        return row

    def _build_plot_area(self):
        pg.setConfigOptions(antialias=True)
        w = QWidget()
        v = QVBoxLayout(w)

        bar = QHBoxLayout()
        bar.addWidget(QLabel("Span:"))
        self.span_combo = QComboBox()
        for label, _ in PLOT_SPANS:
            self.span_combo.addItem(label)
        self.span_combo.setCurrentText(PLOT_SPAN_DEFAULT)
        self.span_combo.currentTextChanged.connect(self._on_span_changed)
        bar.addWidget(self.span_combo)

        # Follow keeps the window pinned to the newest sample. Untick it (or just
        # drag a plot) to hold the view still and scroll back through everything
        # the buffer holds — the axis is then left entirely to the mouse, so pan
        # and zoom behave normally instead of being yanked back once a second.
        self.follow_chk = QCheckBox("Follow live")
        self.follow_chk.setChecked(True)
        self.follow_chk.toggled.connect(self._on_follow_toggled)
        bar.addWidget(self.follow_chk)

        clr = QPushButton("Clear plots")
        clr.setToolTip("Discard all buffered samples and restart the time axis")
        clr.clicked.connect(self.clear_plots)
        bar.addWidget(clr)

        about = QPushButton("About")
        about.clicked.connect(self.show_about)
        bar.addWidget(about)

        self.span_hint = QLabel("")
        self.span_hint.setStyleSheet("font-size:11px;")
        bar.addWidget(self.span_hint)
        bar.addStretch(1)
        holder = QWidget(); holder.setLayout(bar)
        v.addWidget(holder)

        self.plot_phase = pg.PlotWidget(title="Phase  dph (ns)")
        self.plot_vph = pg.PlotWidget(title="Detector Vphase (V) — ramp position")
        self.plot_freq = pg.PlotWidget(title="Frequency error (Hz, from 1ks avg)")
        for p in (self.plot_phase, self.plot_vph, self.plot_freq):
            p.showGrid(x=True, y=True, alpha=0.3)
            p.setLabel("bottom", "time", units="s")
        self.curve_phase = self.plot_phase.plot(pen=pg.mkPen("#22aa44", width=2))
        self.curve_vph = self.plot_vph.plot(pen=pg.mkPen("#2277cc", width=2))
        self.curve_freq = self.plot_freq.plot(pen=pg.mkPen("#cc7722", width=2))

        # Dragging or wheeling a plot means the operator wants to look at
        # something, so stop following rather than fighting them for the axis.
        # sigRangeChangedManually fires only for mouse-driven changes, so the
        # tool's own setXRange() calls do not trip it.
        for plot in (self.plot_phase, self.plot_vph, self.plot_freq):
            try:
                plot.getViewBox().sigRangeChangedManually.connect(
                    self._on_manual_range)
            except Exception:
                pass          # older pyqtgraph without the signal: checkbox only

        # Vphase band guides: anchor and the 15-85% Vsat window get drawn once
        # calibration is known (updated from LL). They make it obvious at a
        # glance when the detector is drifting toward a rail.
        # Guide-line pens — deliberately the most primitive form pyqtgraph
        # accepts: a colour string and an integer width. Three rounds of builder
        # testing showed every fancier form breaking on some version pairing:
        # mkPen(style=enum) crashes one pyqtgraph, a hand-built QPen is treated
        # as a *colour* by another, and a non-cosmetic QPen draws data-unit-wide
        # bands. Solid thin lines in distinct colours carry the same meaning
        # (grey = anchor, red = band edges) and work everywhere.
        self.vph_anchor = pg.InfiniteLine(angle=0, pen=pg.mkPen("#888888", width=1))
        self.vph_lo = pg.InfiniteLine(angle=0, pen=pg.mkPen("#cc3333", width=1))
        self.vph_hi = pg.InfiniteLine(angle=0, pen=pg.mkPen("#cc3333", width=1))
        for ln in (self.vph_anchor, self.vph_lo, self.vph_hi):
            self.plot_vph.addItem(ln)

        for p in (self.plot_phase, self.plot_vph, self.plot_freq):
            v.addWidget(p)
        return w

    def _build_control_tabs(self):
        tabs = QTabWidget()
        # Every tab goes inside a scroll area. A QSplitter will not shrink a pane
        # below its content's minimum size hint, so one crowded tab silently sets
        # the width of the whole right-hand panel and the plots lose the space —
        # which is exactly what the algo-12 limit table did. Wrapped like this, a
        # tab that does not fit scrolls instead of pushing.
        def scrolled(widget):
            sa = QScrollArea()
            sa.setWidget(widget)
            sa.setWidgetResizable(True)
            sa.setFrameShape(QScrollArea.Shape.NoFrame)
            sa.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAsNeeded)
            return sa

        tabs.addTab(scrolled(self._tab_ltic()), "LTIC (algo 10)")
        tabs.addTab(scrolled(self._tab_lars()), "LTIC-Lars (algo 11)")
        tabs.addTab(scrolled(self._tab_algo12()), "Multi-level (algo 12)")
        tabs.addTab(scrolled(self._tab_fa()), "FA damping")
        tabs.addTab(scrolled(self._tab_pid()), "PID algo 3-9")
        tabs.addTab(scrolled(self._tab_cal()), "Calibration")
        tabs.addTab(self._build_monitor(), "Raw monitor")
        tabs.addTab(scrolled(self._tab_help()), "Help")
        return tabs

    def _spin(self, lo, hi, dec, step):
        s = QDoubleSpinBox()
        s.setRange(lo, hi)
        s.setDecimals(dec)
        s.setSingleStep(step)
        s.setMinimumWidth(110)
        return s

    def _tab_ltic(self):
        """Three-stage ACQ/DPLL/LOCK PID, read from LL, written live."""
        w = QWidget()
        g = QGridLayout(w)
        g.addWidget(QLabel("<b>Stage PID — direct write, live</b>"), 0, 0, 1, 6)

        hdr = ["", "Kp", "Ki", "Kd", "I-limit", ""]
        for c, h in enumerate(hdr):
            g.addWidget(QLabel(f"<b>{h}</b>"), 1, c)

        row = 2
        for stage in ("ACQ", "DPLL", "LOCK"):
            g.addWidget(QLabel(stage), row, 0)
            for c, k in enumerate(("Kp", "Ki", "Kd", "IL"), start=1):
                box = self._spin(0.0, 100000.0, 4, 0.1)
                self.ltic_boxes[(stage, k)] = box
                g.addWidget(box, row, c)
            btn = QPushButton("Apply")
            btn.clicked.connect(lambda _, s=stage: self.apply_ltic_stage(s))
            g.addWidget(btn, row, 5)
            row += 1

        read_btn = QPushButton("Read from device (LL)")
        read_btn.clicked.connect(lambda: self.worker.send("LL"))
        g.addWidget(read_btn, row, 0, 1, 3)
        save_btn = QPushButton("Save (ES LTIC)")
        save_btn.clicked.connect(lambda: self.confirm_send("ES LTIC",
                                 "Commit LTIC tuning to EEPROM?"))
        g.addWidget(save_btn, row, 3, 1, 2)
        revert_btn = QPushButton("Revert (ER)")
        revert_btn.clicked.connect(lambda: self.confirm_send("ER",
                                   "Reload all parameters from EEPROM?"))
        g.addWidget(revert_btn, row, 5)
        row += 1

        note = QLabel("Values read back from LL. Apply writes one stage live via "
                      "AQ*/DP*/LK* verbs. Nothing is saved until ES.")
        note.setWordWrap(True)
        note.setStyleSheet(f"color:{theme_colours(w)['muted']}; font-size:11px;")
        g.addWidget(note, row, 0, 1, 6)
        row += 1

        g.setRowStretch(row + 1, 1)
        return w

    def _tab_lars(self):
        """Algorithm 11 (LTIC-Lars) parameters on their own tab, mirroring the
        algo-10 LTIC tab for a consistent view."""
        w = QWidget()
        g = QGridLayout(w)
        row = 0
        title = QLabel("Algorithm 11 — LTIC-Lars continuous PI")
        title.setStyleSheet("font-weight:bold;")
        g.addWidget(title, row, 0, 1, 6); row += 1

        self.lars_boxes = {}
        grid = QGridLayout()
        for c, (verb, (label, lo, hi, dec)) in enumerate(LARS_PARAMS.items()):
            gr, gc = divmod(c, 3)          # 3 params per row
            cell = QVBoxLayout()
            cell.addWidget(QLabel(label))
            box = self._spin(lo, hi, dec, 0.1 if dec else 1.0)
            self.lars_boxes[verb] = box
            cell.addWidget(box)
            btn = QPushButton("Set")
            btn.clicked.connect(lambda _, v=verb: self.apply_lars_param(v))
            cell.addWidget(btn)
            holder = QWidget(); holder.setLayout(cell)
            grid.addWidget(holder, gr, gc)
        gwrap = QWidget(); gwrap.setLayout(grid)
        g.addWidget(gwrap, row, 0, 1, 6); row += 1

        read11 = QPushButton("Read all (LG/LD/…)")
        read11.clicked.connect(self.read_lars_params)
        g.addWidget(read11, row, 0, 1, 2)
        saveb = QPushButton("Save (ES LTIC)")
        saveb.clicked.connect(lambda: self.confirm_send(
            "ES LTIC", "Save LTIC + Lars params to the flash ring?"))
        g.addWidget(saveb, row, 2, 1, 2); row += 1

        note = QLabel("gain=0 means auto from CT calibration; set a non-zero LG "
                      "for a manual scale. Trend: ACQ=freq-led, PLL=phase, "
                      "LOCK=locked. Saved together with LTIC by ES LTIC.")
        note.setWordWrap(True)
        note.setStyleSheet(f"color:{theme_colours(w)['muted']}; font-size:11px;")
        g.addWidget(note, row, 0, 1, 6); row += 1

        g.setRowStretch(row + 1, 1)
        return w

    def _tab_algo12(self):
        """Algorithm 12 (multi-level accumulator, after Alan Cashin).

        Two scalars and an eleven-row limit table. The Lars tab lays its nine
        parameters out three to a row, which suits nine; with only two the same
        grid stretches each cell across half the window and the Set buttons come
        out enormous. So the scalars go in a fixed-width row here, and the space
        that frees is given to the limit table — which is the thing most likely
        to need changing per oscillator, and which previously had no GUI at all.
        """
        w = QWidget()
        g = QGridLayout(w)
        row = 0
        title = QLabel("Algorithm 12 — multi-level accumulator (after Alan Cashin, MIS42N)")
        title.setStyleSheet("font-weight:bold;")
        g.addWidget(title, row, 0, 1, 6); row += 1

        # ---- scalars, side by side, not stretched -------------------------
        self.algo12_boxes = {}
        sc = QHBoxLayout()
        for verb, (label, lo, hi, dec) in MLACC_PARAMS.items():
            cell = QVBoxLayout()
            lab = QLabel(label)
            cell.addWidget(lab)
            box = self._spin(lo, hi, dec, 0.1 if dec else 1.0)
            box.setMaximumWidth(110)
            self.algo12_boxes[verb] = box
            cell.addWidget(box)
            btn = QPushButton("Set")
            btn.setMaximumWidth(110)
            btn.clicked.connect(lambda _, v=verb: self.apply_algo12_param(v))
            cell.addWidget(btn)
            holder = QWidget(); holder.setLayout(cell)
            holder.setMaximumWidth(120)
            sc.addWidget(holder)
        sc.addStretch(1)          # keep the cells at their natural width
        scw = QWidget(); scw.setLayout(sc)
        g.addWidget(scw, row, 0, 1, 6); row += 1

        # ---- per-level phase limits ---------------------------------------
        lt = QLabel("Phase limits per level (ns) — a correction fires at the "
                    "lowest level whose error exceeds its limit")
        lt.setWordWrap(True)
        g.addWidget(lt, row, 0, 1, 6); row += 1

        # Two columns, not four. Each cell is a label plus a spin box, and at four
        # across the tab demanded about 770 px — more than the splitter would give
        # the right-hand panel, so the plots were squeezed to a third of the window.
        # A splitter cannot shrink a pane below its minimum size hint, so the tab
        # content has to be narrow rather than the splitter told to be firmer.
        # The per-row Set buttons are gone too: one button sends the whole table,
        # which is what you want after adjusting several rows anyway.
        self.algo12_lim = {}
        lim = QGridLayout()
        lim.setHorizontalSpacing(14)
        lim.setVerticalSpacing(2)
        for i, secs in enumerate(MLACC_LEVEL_SECS):
            gr, gc = divmod(i, 2)
            tag = QLabel(f"{secs}s")
            tag.setMinimumWidth(38)
            tag.setAlignment(Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter)
            box = self._spin(0.0, 500000.0, 0, 500.0)
            box.setMaximumWidth(84)
            self.algo12_lim[i] = box
            lim.addWidget(tag, gr, gc * 2)
            lim.addWidget(box, gr, gc * 2 + 1)
        lim.setColumnStretch(4, 1)
        limw = QWidget(); limw.setLayout(lim)
        g.addWidget(limw, row, 0, 1, 6); row += 1

        # ---- actions -------------------------------------------------------
        acts = QHBoxLayout()
        read12 = QPushButton("Read all")
        read12.clicked.connect(self.read_algo12_params)
        acts.addWidget(read12)
        setlim = QPushButton("Send limits")
        setlim.clicked.connect(self.apply_algo12_limits)
        acts.addWidget(setlim)
        listb = QPushButton("List (ML)")
        listb.clicked.connect(lambda: self.worker.send("ML"))
        acts.addWidget(listb)
        saveb = QPushButton("Save (ES ALGO12)")
        saveb.clicked.connect(lambda: self.confirm_send(
            "ES ALGO12", "Save algo-12 parameters to the flash ring?"))
        acts.addWidget(saveb)
        acts.addStretch(1)
        actw = QWidget(); actw.setLayout(acts)
        g.addWidget(actw, row, 0, 1, 6); row += 1

        note = QLabel(
            "Input is phase in nanoseconds from the LTIC detector, which is "
            "required — there is no fallback. gain=0 "
            "takes the scale from the CT calibration. run level forces a "
            "correction when reached, whatever the limits say. There is no time "
            "constant to set: the error picks its own averaging time.\n"
            "UNTUNED — only the 128 s limit was ever derived (125 ns, from "
            "10 MHz +/-0.01 Hz); Alan describes the rest as arbitrary.")
        note.setWordWrap(True)
        note.setStyleSheet(f"color:{theme_colours(w)['muted']}; font-size:11px;")
        g.addWidget(note, row, 0, 1, 6); row += 1

        g.setRowStretch(row, 1)
        return w

    def absorb_algo12_limit(self, line):
        """Fill one limit box from 'lim[6]=32350 (126 ns over 128s)'."""
        # Two formats reach here, because MLP and ML print differently:
        #   MLP n  ->  "lim[6]=32350 (126 ns over 128s)"
        #   ML     ->  "     6:   128s    32350 =  126 ns"
        # Matching only the first meant the ML reply — the one the tuner sends on
        # connect — filled nothing, and the table stayed at zeros.
        m = re.search(r"\blim\[(\d+)\]\s*=\s*(\d+)", line)
        if m:
            level, val = int(m.group(1)), int(m.group(2))
        else:
            m = re.match(r"\s*(\d+):\s*\d+s\s+(\d+)\s*=\s*-?\d+\s*ns", line)
            if not m:
                return False
            level, val = int(m.group(1)), int(m.group(2))
        box = self.algo12_lim.get(level)
        if box is None:
            return False
        box.blockSignals(True)
        box.setValue(float(val))
        box.blockSignals(False)
        return True

    def apply_algo12_limits(self):
        """Send the whole limit table, one MLP per level.

        One button rather than eleven: after changing a limit you almost always
        want to change its neighbours too, and eleven Set buttons cost the width
        that the plots need.
        """
        vals = {lv: int(b.value()) for lv, b in self.algo12_lim.items()}
        if any(v < 1 for v in vals.values()):
            # Zero means "never read back", not "set this to zero". Sending it
            # would be eleven rejected commands, or worse on a firmware that
            # accepted it.
            QMessageBox.information(
                self, "Limits not read",
                "Some rows are still zero, which means the table has not been "
                "read from the board yet.\n\nPress \u201cRead all\u201d first, "
                "then adjust and send.")
            return
        for level in sorted(vals):
            self.worker.send(f"MLP {level} {vals[level]}")

    def _tab_help(self):
        """Command reference, grouped the way the firmware's own H output is.
        Kept in sync with gpsdo_cli.cpp — if a verb changes there, change it
        here too, or the console starts lying to the operator."""
        w = QWidget()
        outer = QVBoxLayout(w)

        intro = QLabel(
            "Firmware CLI reference. Anything here can also be typed into the "
            "manual command box at the bottom of the window. Most parameter "
            "verbs READ when given no argument and WRITE when given one "
            "(e.g. <b>LG</b> shows the gain, <b>LG 0.3</b> sets it). "
            "Nothing is persistent until you run <b>ES</b>.")
        intro.setWordWrap(True)
        intro.setTextFormat(Qt.RichText)
        intro.setStyleSheet(f"color:{theme_colours(w)['muted']}; font-size:11px;")
        outer.addWidget(intro)

        body = QTextEdit()
        body.setReadOnly(True)
        body.setFont(QFont("Consolas" if os.name == "nt" else "Monospace", 9))

        sections = [
            ("General", [
                ("V",           "Version, authors and links"),
                ("H / ?",       "Firmware help (H TZ for timezone details)"),
                ("SW",          "Stack watermarks (FreeRTOS diagnostic)"),
                ("RB",          "Reboot (warm — settings kept)"),
                ("CR YES",      "Cold restart: wipe the flash ring, factory defaults"),
            ]),
            ("Reporting", [
                ("RH / RD",     "Report format: human readable / tab delimited"),
                ("RP / RR",     "Report pause / resume"),
                ("F",           "Flush the frequency ring buffers"),
                ("T [baud]",    "GPS tunnel on USB for u-center (300 s)"),
                ("CS",          "Correction statistics: RMS of the loop's own"),
                ("",            "  corrections over the last 100 / 1k / 10k / 100k"),
                ("",            "  CORRECTIONS - not seconds, because the rate"),
                ("",            "  depends on the algorithm (algo 11 steers once a"),
                ("",            "  second, algo 10 once per LIV). CS measures the"),
                ("",            "  interval and prints what the windows span."),
                ("",            "  Also in fractional frequency once CT has run."),
                ("",            "  Counted only while locked and not calibrating."),
                ("",            "  Says whether the LOOP IS SETTLED, not whether"),
                ("",            "  the OUTPUT IS GOOD - a noisy detector makes the"),
                ("",            "  loop chase noise and this reports it faithfully."),
            ]),
            ("Discipline mode", [
                ("MH / MD",     "Mode holdover / mode disciplined"),
                ("LA <0-12>",   "Loop algorithm select. 10 = LTIC 3-stage,"),
                ("",            "  11 = LTIC-Lars, 12 = multi-level accumulator"),
                ("SP <n>",      "Set the PWM DAC directly (1-65535) — manual override."),
                ("",            "  A whole-LSB intent, so it clears the fine fraction"),
                ("",            "  (see DAC). CT, LC, the ramps and holdover do too."),
                ("DAC",         "Control-voltage output: which path is driving the pin,"),
                ("",            "  the 24-bit code beside the 16-bit view the displays"),
                ("",            "  and the flash ring use, the fraction between them,"),
                ("",            "  and one step expressed in uHz and fractional"),
                ("",            "  frequency for both widths."),
                ("",            "  'fine: ACTIVE' means a correction smaller than one"),
                ("",            "  16-bit step is applied rather than truncated away."),
                ("",            "  That matters more than it sounds: the truncation it"),
                ("",            "  replaces discarded such a correction ENTIRELY, not"),
                ("",            "  partly, because rounding back to the same code also"),
                ("",            "  skipped the write. Needs the dithered PWM or an"),
                ("",            "  external DAC compiled in; on plain PWM it says so."),
                ("",            "  The Hz figures need CT — without it the resolution"),
                ("",            "  is real but its meaning in frequency is unknown,"),
                ("",            "  and the command says that instead of guessing."),
                ("AP",          "Arm picDIV (resync the divider to 1PPS)"),
            ]),
            ("Calibration", [
                ("C",           "Auto-calibration: centre the PWM"),
                ("CT",          "Calibrate + auto-tune: measures K, tunes PID (algos 3-9)"),
                ("",            "  and both LTIC loops (10 and 11)"),
                ("LC",          "LTIC self-calibrate: ns/V, zero offset, range"),
                ("LL",          "List all LTIC parameters and state"),
                ("LNV / LZO / LRN", "Calibration: ns per volt, zero-offset V, range ns"),
                ("LPOL [-1/0/1]", "PWM->phase polarity (0 = auto-detect)"),
                ("SAW 0|1",     "Sawtooth (qErr) correction on/off"),
            ]),
            ("PID algorithms 3-9", [
                ("LP [n]",      "List PID parameters (algo n, or the current one)"),
                ("KP n val",    "Set Kp for algo n"),
                ("KI n val",    "Set Ki for algo n"),
                ("KD n val",    "Set Kd for algo n"),
                ("IL n val",    "Set I_LIMIT for algo n"),
                ("BC / BS",     "Algo 8 blend crossover / blend scale (Hz)"),
                ("NS [val]",    "Algo 9 neural-net max step (LSB)"),
                ("LRN 0|1|R",   "Self-learning drift/damping (R = reset)"),
            ]),
            ("Algorithm 10 — LTIC 3-stage", [
                ("AQP/AQI/AQD/AQL", "ACQ stage PID: Kp / Ki / Kd / I_LIMIT"),
                ("DPP/DPI/DPD/DPL", "DPLL stage PID: Kp / Ki / Kd / I_LIMIT"),
                ("LKP/LKI/LKD/LKL", "LOCK stage PID: Kp / Ki / Kd / I_LIMIT"),
                ("LAT / LDT / LIV", "ACQ threshold, DPLL->LOCK threshold, LOCK interval s"),
                ("LCV [V]",     "ACQ centring target (0 = range middle)"),
                ("ACG g [cap]", "ACQ centring drive: LSB per volt and max step"),
            ]),
            ("Algorithm 11 — LTIC-Lars", [
                ("LG [val]",    "Gain. 0 = auto from the CT calibration, else a manual scale"),
                ("LD [val]",    "Damping"),
                ("LTC [s]",     "Loop time constant (1-600 s)"),
                ("LFD [n]",     "Filter divisor — pre-filter constant = LTC / this"),
                ("LTO [adc]",   "TIC offset: phase target in ADC counts"),
                ("LPL [ns]",    "Lock phase limit — the window width"),
                ("LPF [n]",     "Lock factor: window must hold for LPF x LTC seconds"),
                ("LTK [val]",   "Temperature coefficient feed-forward (0 = off)"),
                ("LTR [adc]",   "Temperature reference, ADC counts"),
                ("",            "  Trend: ACQ = frequency-led, PLL = phase, LOCK = locked"),
            ]),
            ("Algorithm 12 — multi-level accumulator", [
                ("MG [v]",      "Gain, LSB per ns. 0 = derive it from the CT"),
                ("",            "  calibration, which is usually what you want."),
                ("MR [n]",      "Force a correction once level n is reached,"),
                ("",            "  whatever the limits say (default 9 = 1024 s)."),
                ("MLP <n> [ns]","Phase limit for level n. Without a value it"),
                ("",            "  prints the current one."),
                ("MF [0-3]",    "Where the limits come from, INDEPENDENTLY of MG:"),
                ("",            "  0 follow MG (as before), 1 stored table,"),
                ("",            "  2 sigma formula, 3 measured. The gain belongs"),
                ("",            "  to the oscillator, the limits to the site's"),
                ("",            "  phase noise; they were welded together until"),
                ("",            "  now, so 'measured gain, hand-set limits' -"),
                ("",            "  what a noisy install wants - was unaskable."),
                ("MFT [s]",     "MF 3 only: how long between corrections that"),
                ("",            "  noise alone triggers. Every per-level"),
                ("",            "  multiplier follows from it. Default 3600."),
                ("ML",          "List gain, run level, limit source and limits"),
                ("ES ALGO12",   "Save the algo-12 block to the flash ring"),
                ("",            ""),
                ("",  "After Alan Cashin (MIS42N). Input is phase in ns from"),
                ("",  "the LTIC detector — about 1 ns, against 100 ns for the"),
                ("",  "frequency counter this first used. That first version"),
                ("",  "was blind: a disciplined oscillator sits far below 1 Hz"),
                ("",  "of error, so the count read zero and nothing ever"),
                ("",  "accumulated. The detector is REQUIRED - LA 12 refuses"),
                ("",  "without it. The old fallback integrated quantisation"),
                ("",  "noise into a random walk and destroyed the lock."),
                ("",  ""),
                ("",  "Readings accumulate into levels: level n covers 2^n"),
                ("",  "seconds. A correction is applied at the LOWEST level"),
                ("",  "whose error exceeds that level's limit, so a large error"),
                ("",  "acts within 2 s and a small one waits for a longer"),
                ("",  "average. The error chooses its own time constant -"),
                ("",  "there is no LTC to set."),
                ("",  ""),
                ("",  "UNTUNED: only the 128 s limit was ever derived (125 ns,"),
                ("",  "from the 10 MHz +/-0.01 Hz specification). Alan calls the"),
                ("",  "rest arbitrary, so they are exposed for editing."),
                ("",  "Trend shows CORR at a correction, ACQ for the first"),
                ("",  "64 s, LOCK after. NoCT means CT has not run."),
            ]),
            ("Storage (flash ring)", [
                ("ES [obj]",    "Save settings. Group: TZ / PID / LTIC / FLAGS / ALGO / ALGO12 / PO"),
                ("ER",          "Recall settings from the ring"),
                ("EE",          "Erase settings (back to defaults)"),
                ("EW",          "Flash wear stats: erase cycles and slots used"),
                ("FR 0|1",      "Flash ring on/off"),
            ]),
            ("GPS and environment", [
                ("SV <0|1>",    "Survey-in / Time Mode on a timing receiver"),
                ("TZ <zone>",   "Timezone with DST, e.g. TZ Adelaide (see H TZ)"),
                ("TO <n|A>",    "Fixed UTC offset (h or h:mm), or A to guess from position"),
                ("LT 0|1",      "Show UTC or local time"),
                ("PO <f>",      "Pressure offset"),
                ("AO <f>",      "Altitude offset"),
            ]),
            ("Boot behaviour", [
                ("WU 0|1",      "OCXO warm-up on boot"),
                ("SPL 0|1",     "Boot animation: 1 = full show, 0 = static"),
            ]),
        ]

        col = theme_colours(w)
        html = ["<style>"
                f"h3{{margin:10px 0 3px 0;color:{col['head']};font-size:12px;}}"
                "table{border-collapse:collapse;margin-bottom:4px;}"
                "td{padding:1px 10px 1px 0;vertical-align:top;font-size:11px;}"
                f"td.v{{color:{col['verb']};font-weight:bold;white-space:nowrap;}}"
                f"td.d{{color:{col['desc']};}}"
                "</style>"]
        for title, rows in sections:
            html.append(f"<h3>{title}</h3><table>")
            for verb, desc in rows:
                v = verb.replace("<", "&lt;").replace(">", "&gt;")
                d = desc.replace("<", "&lt;").replace(">", "&gt;")
                html.append(f"<tr><td class='v'>{v}</td><td class='d'>{d}</td></tr>")
            html.append("</table>")
        body.setHtml("".join(html))
        outer.addWidget(body, 1)
        return w

    def _tab_fa(self):
        """FA damping windows — the acquisition/steady-state split."""
        w = QWidget()
        g = QGridLayout(w)
        g.addWidget(QLabel("<b>Damping-term averaging window</b>"), 0, 0, 1, 4)
        g.addWidget(QLabel("Shorter windows damp a limit cycle but pass more "
                           "short-tau noise. 100 = firmware default."),
                    1, 0, 1, 4)

        g.addWidget(QLabel("DPLL (acquisition):"), 2, 0)
        self.fa_dpll = QComboBox(); self.fa_dpll.addItems(FA_VALUES)
        self.fa_dpll.setCurrentText("100")
        g.addWidget(self.fa_dpll, 2, 1)
        b1 = QPushButton("Apply FAD")
        b1.clicked.connect(lambda: self.worker.send(f"FAD {self.fa_dpll.currentText()}"))
        g.addWidget(b1, 2, 2)

        g.addWidget(QLabel("LOCK (steady state):"), 3, 0)
        self.fa_lock = QComboBox(); self.fa_lock.addItems(FA_VALUES)
        self.fa_lock.setCurrentText("100")
        g.addWidget(self.fa_lock, 3, 1)
        b2 = QPushButton("Apply FAL")
        b2.clicked.connect(lambda: self.worker.send(f"FAL {self.fa_lock.currentText()}"))
        g.addWidget(b2, 3, 2)

        readb = QPushButton("Read (FA)")
        readb.clicked.connect(lambda: self.worker.send("FA"))
        g.addWidget(readb, 4, 0)
        saveb = QPushButton("Save (ES LTIC)")
        saveb.clicked.connect(lambda: self.confirm_send("ES LTIC",
                              "Commit FA windows to EEPROM?"))
        g.addWidget(saveb, 4, 1, 1, 2)
        g.setRowStretch(5, 1)
        return w

    def _tab_pid(self):
        """KP/KI/KD/IL for a selectable algorithm 3-9."""
        w = QWidget()
        g = QGridLayout(w)
        g.addWidget(QLabel("<b>Classic PID — algorithms 3-9</b>"), 0, 0, 1, 4)
        g.addWidget(QLabel("Algorithm:"), 1, 0)
        self.pid_algo = QComboBox()
        self.pid_algo.addItems([str(n) for n in range(3, 10)])
        g.addWidget(self.pid_algo, 1, 1)
        readb = QPushButton("Read (LP)")
        readb.clicked.connect(lambda: self.worker.send(f"LP {self.pid_algo.currentText()}"))
        g.addWidget(readb, 1, 2)

        row = 2
        for k, dec in (("Kp", 4), ("Ki", 6), ("Kd", 3), ("IL", 1)):
            g.addWidget(QLabel(k), row, 0)
            box = self._spin(0.0, 100000.0, dec, 0.1)
            self.pid_boxes[k] = box
            g.addWidget(box, row, 1)
            verb = {"Kp": "KP", "Ki": "KI", "Kd": "KD", "IL": "IL"}[k]
            btn = QPushButton(f"Apply {verb}")
            btn.clicked.connect(lambda _, vv=verb, kk=k: self.apply_pid(vv, kk))
            g.addWidget(btn, row, 2)
            row += 1

        saveb = QPushButton("Save (ES PID)")
        saveb.clicked.connect(lambda: self.confirm_send("ES PID",
                              "Commit algo 3-9 PID to EEPROM?"))
        g.addWidget(saveb, row, 0, 1, 3)
        row += 1
        note = QLabel("KP/KI/KD apply to algos 3-7; IL (I-limit) to 3-9. "
                      "Read LP first to load the current values.")
        note.setWordWrap(True)
        note.setStyleSheet(f"color:{theme_colours(w)['muted']}; font-size:11px;")
        g.addWidget(note, row, 0, 1, 3)
        g.setRowStretch(row + 1, 1)
        return w

    def _tab_cal(self):
        """LTIC detector calibration + polarity."""
        w = QWidget()
        g = QGridLayout(w)
        g.addWidget(QLabel("<b>LTIC detector calibration</b>"), 0, 0, 1, 3)
        row = 1
        for verb, (lbl, lo, hi, dec) in LTIC_CAL.items():
            g.addWidget(QLabel(f"{verb} — {lbl}"), row, 0)
            box = self._spin(lo, hi, dec, 0.001 if dec > 2 else 1.0)
            self.cal_boxes[verb] = box
            g.addWidget(box, row, 1)
            btn = QPushButton("Apply")
            btn.clicked.connect(lambda _, vv=verb: self.apply_cal(vv))
            g.addWidget(btn, row, 2)
            row += 1

        g.addWidget(QLabel("LPOL — polarity"), row, 0)
        self.pol_combo = QComboBox()
        self.pol_combo.addItems(["-1", "0", "+1"])
        g.addWidget(self.pol_combo, row, 1)
        polb = QPushButton("Apply LPOL")
        polb.clicked.connect(lambda: self.worker.send(
            f"LPOL {self.pol_combo.currentText().lstrip('+')}"))
        g.addWidget(polb, row, 2)
        row += 1

        readb = QPushButton("Read (LL)")
        readb.clicked.connect(lambda: self.worker.send("LL"))
        g.addWidget(readb, row, 0)
        saveb = QPushButton("Save (ES LTIC)")
        saveb.clicked.connect(lambda: self.confirm_send("ES LTIC",
                              "Commit calibration to EEPROM?"))
        g.addWidget(saveb, row, 1, 1, 2)
        g.setRowStretch(row + 1, 1)
        return w

    def _build_monitor(self):
        w = QWidget()
        v = QVBoxLayout(w)
        self.monitor = QTextEdit()
        self.monitor.setReadOnly(True)
        self.monitor.setFont(QFont("Consolas", 9))
        self.monitor.setLineWrapMode(QTextEdit.LineWrapMode.NoWrap)
        v.addWidget(self.monitor)
        row = QHBoxLayout()
        clear = QPushButton("Clear")
        clear.clicked.connect(self.monitor.clear)
        row.addWidget(clear)

        # Continuous logging rather than a buffer dump. The monitor keeps only the
        # last 2000 lines — under five minutes at the telemetry rate — so a "save
        # what is on screen" button would quietly hand back five minutes of a
        # thirty-hour test. Writing each line as it arrives captures the whole
        # session, costs no memory, and leaves the data on disk if the tool or the
        # machine falls over mid-run.
        self.log_btn = QPushButton("Start logging")
        self.log_btn.setToolTip("Write every received line to a .log file next to this script")
        self.log_btn.clicked.connect(self.toggle_logging)
        row.addWidget(self.log_btn)

        tzb = QPushButton("Generate tz_table.h")
        tzb.setToolTip("Rebuild the timezone table from this machine's IANA tzdata")
        tzb.clicked.connect(self.generate_tz_table)
        row.addWidget(tzb)

        self.log_lbl = QLabel("not logging")
        self.log_lbl.setStyleSheet("font-size:11px;")
        row.addWidget(self.log_lbl)
        row.addStretch(1)
        holder = QWidget(); holder.setLayout(row)
        v.addWidget(holder)
        return w

    def _build_command_row(self):
        row = QHBoxLayout()
        self.cmd_edit = QLineEdit()
        self.cmd_edit.setPlaceholderText("Type any firmware command (e.g. LL, FAL 10, ES LTIC) and Enter")
        self.cmd_edit.returnPressed.connect(self.send_manual)
        send = QPushButton("Send")
        send.clicked.connect(self.send_manual)
        for q in ("LL", "FA", "H", "SAW", "WU"):
            b = QPushButton(q)
            b.setMaximumWidth(52)
            b.clicked.connect(lambda _, cc=q: self.worker.send(cc))
            row.addWidget(b)
        row.addWidget(self.cmd_edit, 1)
        row.addWidget(send)
        return row

    # ---- actions ----------------------------------------------------------
    def apply_ltic_stage(self, stage):
        verbs = LTIC_STAGE_VERBS[stage]
        for verb, k in zip(verbs, ("Kp", "Ki", "Kd", "IL")):
            val = self.ltic_boxes[(stage, k)].value()
            self.worker.send(f"{verb} {val:g}")

    def apply_pid(self, verb, k):
        algo = self.pid_algo.currentText()
        val = self.pid_boxes[k].value()
        self.worker.send(f"{verb} {algo} {val:g}")

    def apply_cal(self, verb):
        val = self.cal_boxes[verb].value()
        self.worker.send(f"{verb} {val:g}")

    def apply_lars_param(self, verb):
        """Write one algo 11 (LTIC-Lars) parameter live."""
        _, _, _, dec = LARS_PARAMS[verb]
        val = self.lars_boxes[verb].value()
        self.worker.send(f"{verb} {int(val) if dec == 0 else val:g}"
                         if dec == 0 else f"{verb} {val:g}")

    def read_lars_params(self):
        """Query every algo 11 parameter; each replies 'name=value' on one line."""
        for verb in LARS_PARAMS:
            self.worker.send(verb)

    def apply_algo12_param(self, verb):
        """Write one algo 12 (multi-level accumulator) scalar parameter live."""
        _, _, _, dec = MLACC_PARAMS[verb]
        val = self.algo12_boxes[verb].value()
        self.worker.send(f"{verb} {int(val) if dec == 0 else val:g}"
                         if dec == 0 else f"{verb} {val:g}")

    def read_algo12_params(self):
        """Query the algo 12 scalars and the whole limit table.

        The limits were missing from this, so their spin boxes sat at zero until
        somebody typed in them — and pressing Send would then have written eleven
        zeros over a table the firmware had defaults for. A control that shows a
        value the device does not hold is worse than one that shows nothing.

        `MLP n` with no value prints the current setting, so eleven of those fill
        the table.
        """
        for verb in MLACC_PARAMS:
            self.worker.send(verb)
        for level in range(len(MLACC_LEVEL_SECS)):
            self.worker.send(f"MLP {level}")

    def _set_active_algo(self, algo):
        """Track the active algorithm from telemetry and retitle the plots.

        Algorithms 10 and 11 have a phase detector; nothing else does. Leaving
        the phase and detector panes up under a PID algorithm gives two panes
        that stay empty for the whole session, so they are repointed at drift
        and Vctl instead and the titles follow."""
        if algo == self._active_algo:
            return
        self._active_algo = algo
        self._apply_plot_series()

    def _plot_family(self):
        if self._active_algo in (10, 11): return "ltic"
        if self._active_algo == 12:        return "mlacc"
        return "pid"

    def _apply_plot_series(self):
        """Point the two upper curves at the fields that suit the active family
        and relabel them. Clears the curves so a switch cannot leave the previous
        algorithm's trace on screen looking like current data."""
        if not hasattr(self, "curve_phase"):
            return
        fam = PLOT_SERIES[self._plot_family()]
        for (field, title, colour), plot, curve in (
                (fam[0], self.plot_phase, self.curve_phase),
                (fam[1], self.plot_vph,   self.curve_vph)):
            plot.setTitle(title)
            curve.setPen(pg.mkPen(colour, width=2))
            curve.setData([], [])
        self._series_top = fam[0][0]
        self._series_mid = fam[1][0]
        # The Vphase band guides only mean anything against the detector trace,
        # so they come down when that pane is showing a control voltage instead.
        show_guides = (self._plot_family() == "ltic")
        for attr in ("vph_anchor", "vph_lo", "vph_hi"):
            ln = getattr(self, attr, None)
            if ln is not None:
                try:
                    ln.setVisible(show_guides)
                except Exception:
                    pass
        self.refresh_plots()

    def send_manual(self):
        txt = self.cmd_edit.text().strip()
        if txt:
            self.worker.send(txt)
            self.cmd_edit.clear()

    def confirm_send(self, cmd, question):
        r = QMessageBox.question(self, "Confirm", question,
                                 QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.No)
        if r == QMessageBox.StandardButton.Yes:
            self.worker.send(cmd)

    # ---- serial plumbing --------------------------------------------------
    def refresh_ports(self):
        self.port_combo.clear()
        for p in serial.tools.list_ports.comports():
            self.port_combo.addItem(p.device)

    def toggle_connect(self):
        if self.worker.isRunning():
            self.worker.stop()
            self.worker.wait(1000)
            return
        port = self.port_combo.currentText()
        if not port:
            return
        # A QThread can't be restarted once its run() has returned, so make a
        # fresh worker for each connection rather than reusing the old object.
        self.worker = SerialWorker()
        self.worker.line_received.connect(self.on_line)
        self.worker.connection_changed.connect(self.on_conn)
        self.worker.configure(port, int(self.baud_combo.currentText()))
        self.worker.start()

    def on_conn(self, ok, msg):
        self.status_lbl.setText(msg if ok else f"disconnected ({msg})")
        self.connect_btn.setText("Disconnect" if ok else "Connect")
        if ok:
            # Pull the FULL device state so every panel starts populated, not just
            # the LTIC/FA tabs. Commands are spaced out with staggered timers so
            # the replies don't collide on the UART or overrun the RX buffer.
            self._read_all_params()

    def _read_all_params(self):
        """Query every parameter group the panels display, spaced over time.
        LL (LTIC algo 10), FA (damping), LP n (PID for each algo 3-9), the algo 11
        Lars params, the algo 12 multi-level scalars, and LL again for the
        calibration fields (which share LL)."""
        seq = []
        seq.append("V")                  # firmware version — checked against TOOL_VERSION
        seq.append("LL")                 # LTIC algo 10 + calibration fields
        seq.append("FA")                 # FA damping
        for n in range(3, 10):           # PID algos 3-9, one reply block each
            seq.append(f"LP {n}")
        seq += list(LARS_PARAMS.keys())  # algo 11: LG/LD/LTC/LFD/LTO/LPL/LPF/LTK/LTR
        seq += list(MLACC_PARAMS.keys()) # algo 12: MG/MR
        # ML lists the whole limit table in one reply, which is cheaper than
        # eleven MLP queries and fills the same boxes — the absorber matches the
        # "lim[n]=..." lines either way. Without this the table showed zeros
        # until somebody pressed Read all by hand.
        seq.append("ML")
        # 150 ms between commands keeps the link unsaturated; the whole sweep
        # finishes in ~3 s, after which the panels reflect the device.
        for i, cmd in enumerate(seq):
            QTimer.singleShot(300 + i * 150, lambda c=cmd: self.worker.send(c))

    def on_line(self, line):
        """Entry point for every received line.

        Wrapped, because a single exception in here stops the tuner receiving
        ANYTHING — not just the line that raised. That is how one misplaced
        attribute took down the LL calibration readback and the algo-12 limit
        table at the same time: the traceback fired on every telemetry line, so
        no reply ever reached its absorber, and the symptom looked like two
        unrelated parsing faults rather than one crash.

        A parse failure should cost one line, not the connection.
        """
        try:
            self._on_line(line)
        except Exception as e:                                  # noqa: BLE001
            self.monitor.append(f"*** line handler error: {e!r}")

    def _on_line(self, line):
        # Data arriving proves the link is live — if the status label somehow
        # missed the connect signal (e.g. a restarted worker), correct it here.
        if self.worker.isRunning() and self.connect_btn.text() == "Connect":
            self.connect_btn.setText("Disconnect")
            self.status_lbl.setText(self.worker.port or "connected")

        if self._logfile is not None:
            try:
                self._logfile.write(line + "\n")
                self._log_lines += 1
            except OSError as e:
                self._stop_logging()
                self.monitor.append(f"*** logging failed, stopped: {e}")

        # raw monitor — keep it bounded without touching cursor enums (which
        # differ between Qt5/Qt6 bindings). Once it grows past the cap, drop the
        # oldest lines by rewriting from the retained tail.
        self.monitor.append(line)
        self._monitor_lines += 1
        # Trim in blocks, not on every line. Rebuilding the document costs O(n),
        # so doing it once per line past the cap meant several full rebuilds per
        # second — for thirty hours that is close to a million of them. Letting it
        # overshoot to 2500 before cutting back to 1500 does the same job for a
        # fraction of the work, and the operator cannot see the difference.
        if self._monitor_lines > 2500:
            text = self.monitor.toPlainText()
            kept = text.split("\n")[-1500:]
            self.monitor.setPlainText("\n".join(kept))
            self._monitor_lines = len(kept)
            # scroll to the bottom after the rewrite
            sb = self.monitor.verticalScrollBar()
            sb.setValue(sb.maximum())

        # live numeric fields
        now = time.time() - self.t0
        got_any = False
        for field in ("Vphase", "Vctl", "dph", "PWM", "drift", "damp", "qErr",
                      "scale", "phase", "ph", "level", "corr", "arm", "sig", "zc"):
            v = self.parser.extract(line, field)
            if v is not None:
                self.data[field].append(v)
                got_any = True
        f = self.parser.parse_freq(line)
        if f is not None:
            self.data["freq_err"].append((f - 10_000_000.0))
            got_any = True
        if got_any:
            self.tbuf.append(now)

        st = self.parser.parse_state(line)
        if st:
            self.last_state = st
            hint = self.parser.STATE_HINT.get(st, "")
            self.state_lbl.setText(f"state: {st}" + (f"  ({hint})" if hint else ""))

        # Track the active algorithm from the Learn line so the LTIC tab can show
        # the algo-10 stage PID or the algo-11 Lars params as appropriate. The
        # Both LTIC loops now share the ACQ/LOCK labels, so only the middle state
        # tells them apart: DPLL is algo 10, PLL is algo 11. The "algo=N" field on
        # the Learn line stays the primary and unambiguous source.
        m_algo = re.search(r"algo=(\d+)", line)
        if m_algo:
            self._set_active_algo(int(m_algo.group(1)))
        elif st == "PLL":
            self._set_active_algo(11)
        elif st == "DPLL":
            self._set_active_algo(10)

        # Read-back blocks arrive one field per line, so accumulate them into a
        # buffer from the header until the block ends, then parse the whole thing.
        # The LL block starts at "LTIC ... parameters:" and ends at "state=";
        # the LP block is the "Algo N" line plus its Ki/Kd/IL followers.
        if "parameters:" in line and "LTIC" in line:
            self._ll_buf = [line]
            self._ll_active = True
            return
        if getattr(self, "_ll_active", False):
            self._ll_buf.append(line)
            if "state=" in line:               # end of the LL block
                self._ll_active = False
                self._absorb_ll_block("\n".join(self._ll_buf))
            return
        if line.strip().startswith("Algo ") and "Kp=" in line:
            self._lp_buf = [line]
            self._lp_active = True
            return
        if getattr(self, "_lp_active", False):
            # Ki/Kd/IL follow the Algo line; stop at the first non-matching line
            if re.match(r"\s*(Ki|Kd|IL)=", line):
                self._lp_buf.append(line)
                if "IL=" in line:
                    self._lp_active = False
                    self._absorb_lp_block("\n".join(self._lp_buf))
                return
            else:
                self._lp_active = False
                self._absorb_lp_block("\n".join(self._lp_buf))
                # fall through to handle this line normally
        # Firmware version banner, e.g. "GPSDO v1.00-rtos". Compared against the
        # release this tuner was written for; a mismatch is reported once and not
        # treated as fatal, because an older board is still worth talking to — the
        # operator just needs to know why something might read oddly.
        m_ver = re.match(r"\s*GPSDO\s+v(\d+\.\d+)", line)
        if m_ver:
            fw = m_ver.group(1)
            if fw == TOOL_VERSION:
                self.status_lbl.setText(f"connected — firmware v{fw}")
            else:
                self.status_lbl.setText(
                    f"connected — firmware v{fw}, tuner v{TOOL_VERSION} (MISMATCH)")
                self.monitor.append(
                    f"*** Version mismatch: firmware v{fw}, tuner v{TOOL_VERSION}."
                    f" Some fields may read wrong or some commands may be"
                    f" rejected. Use the matching pair.")
            return

        # Algo 11 (LTIC-Lars) single-line readbacks: "gain=0.300", "damping=3.000",
        # "time_const_s=60", etc. Map the firmware field name back to its verb and
        # drop the value into the matching spinbox without echoing a write.
        for verb, fname in LARS_FIELD_NAMES.items():
            m = re.match(rf"\s*{fname}=([-+]?\d+(?:\.\d+)?)\s*$", line)
            if m and verb in self.lars_boxes:
                self.lars_boxes[verb].blockSignals(True)
                self.lars_boxes[verb].setValue(float(m.group(1)))
                self.lars_boxes[verb].blockSignals(False)
                return

        # Algo 12 (multi-level accumulator) single-line readbacks:
        #   m_gain=0.000 (auto from CT)      m_run_level=9
        #   m_thr_src=3 (measured)           m_thr_tgt=3600s (default)
        # Same mechanism as Lars, but the pattern has to tolerate a trailing
        # unit and a parenthesised gloss. Anchoring hard at the value — which is
        # what this did — meant every annotated reply was silently dropped: the
        # gain box stayed empty for the whole of MG 0, which is the default, and
        # MF/MFT would never have filled at all. A control that shows nothing
        # while the board is answering is the same failure as one that shows a
        # value the board does not hold.
        for verb, fname in MLACC_FIELD_NAMES.items():
            m = re.match(rf"\s*{fname}=([-+]?\d+(?:\.\d+)?)\s*[A-Za-z/]*\s*(?:\(.*\))?\s*$", line)
            if m and verb in self.algo12_boxes:
                self.algo12_boxes[verb].blockSignals(True)
                self.algo12_boxes[verb].setValue(float(m.group(1)))
                self.algo12_boxes[verb].blockSignals(False)
                return

        # Limit table: "lim[6]=32350 (126 ns over 128s)", one line per level from
        # MLP with no value, and eleven of them from ML.
        if self.absorb_algo12_limit(line):
            return

        if "DPLL=" in line and "LOCK=" in line:
            d, l = self.parser.parse_fa(line)
            if d:
                self.fa_dpll.setCurrentText(str(d))
            if l:
                self.fa_lock.setCurrentText(str(l))

    def _absorb_ll_block(self, block):
        """Populate LTIC PID + calibration panels from a full LL block."""
        parsed = self.parser.parse_ll(block)
        for stage in ("ACQ", "DPLL", "LOCK"):
            for k in ("Kp", "Ki", "Kd", "IL"):
                if (stage, k) in self.ltic_boxes and k in parsed[stage]:
                    self.ltic_boxes[(stage, k)].setValue(parsed[stage][k])
        cal = parsed["cal"]
        for verb in LTIC_CAL:
            if verb in cal and verb in self.cal_boxes:
                self.cal_boxes[verb].setValue(cal[verb])
        # LPOL combo (stored as -1/0/1)
        if "LPOL" in cal:
            self.pol_combo.setCurrentText(
                {-1: "-1", 0: "0", 1: "+1"}.get(int(cal["LPOL"]), "0"))
        # Vphase band guides from the ramp geometry
        if "LZO" in cal:
            anchor = cal["LZO"]
            self.vph_anchor.setPos(anchor)
            vsat = anchor / 0.63212 if anchor > 0 else 0
            if vsat:
                self.vph_lo.setPos(0.15 * vsat)
                self.vph_hi.setPos(0.85 * vsat)

    def _absorb_lp_block(self, block):
        """Populate the PID panel from a full 'Algo N ...' block."""
        algo, vals = self.parser.parse_lp(block)
        if vals and str(algo) == self.pid_algo.currentText():
            for k in ("Kp", "Ki", "Kd", "IL"):
                if k in self.pid_boxes and k in vals:
                    self.pid_boxes[k].setValue(vals[k])

    # ---- tz_table.h generation ------------------------------------------
    # Absorbed from the former tools/gen_tz_table.py so the tuner is the only
    # script anyone has to keep. The logic is unchanged; what is new is that the
    # zone data is looked for in several places rather than assuming
    # /usr/share/zoneinfo, which does not exist on Windows — where this tool
    # mostly runs. Python's own zoneinfo module knows where to look, and the
    # tzdata PyPI package is the usual answer on Windows.

    @staticmethod
    def _tz_pkg_dir():
        """Directory of the tzdata package, or "" if it is not installed."""
        try:
            import tzdata
            d = os.path.join(os.path.dirname(tzdata.__file__), "zoneinfo")
            return d if os.path.isdir(d) else ""
        except Exception:
            return ""

    @staticmethod
    def _tz_iana_version():
        """Which IANA release the data came from, when it can be established.

        The tzdata package states it directly. A system zoneinfo directory
        usually cannot: TZif files carry no version field, which is a known gap
        in the format, so an OS-supplied database is stamped as unknown rather
        than guessed at."""
        try:
            import tzdata
            return getattr(tzdata, "IANA_VERSION", "") or ""
        except Exception:
            return ""

    @staticmethod
    def _tz_sources():
        """Directories that may hold TZif files, best candidate first."""
        paths = []
        try:
            import zoneinfo
            paths.extend(str(x) for x in zoneinfo.TZPATH)
        except Exception:
            pass
        try:
            import tzdata
            paths.append(os.path.join(os.path.dirname(tzdata.__file__), "zoneinfo"))
        except Exception:
            pass
        for p in ("/usr/share/zoneinfo", "/usr/lib/zoneinfo", "/etc/zoneinfo"):
            paths.append(p)
        seen, out = set(), []
        for p in paths:
            if p and p not in seen and os.path.isdir(p):
                seen.add(p); out.append(p)
        return out

    @staticmethod
    def _tz_collect(base):
        """Zones as (name, posix_rule). The final line of a TZif file is the
        POSIX TZ string for the CURRENT rule, which is all a GPSDO needs."""
        zones = []
        for root, dirs, files in os.walk(base):
            dirs[:] = [d for d in dirs if d not in ("posix", "right")]
            for f in files:
                fp = os.path.join(root, f)
                if os.path.islink(fp):          # aliases: US/Eastern -> America/New_York
                    continue
                name = os.path.relpath(fp, base).replace(os.sep, "/")
                if name.endswith((".tab", ".list")) or "/" not in name:
                    continue
                if name.startswith("Etc/"):     # GMT+N aliases with inverted signs
                    continue
                try:
                    data = open(fp, "rb").read()
                except OSError:
                    continue
                if not data.startswith(b"TZif"):
                    continue
                parts = data.split(b"\n")
                if len(parts) >= 2 and parts[-2]:
                    zones.append((name, parts[-2].decode("ascii", "ignore")))
        return zones

    def generate_tz_table(self):
        """Write tz_table.h next to this script from the machine's own tzdata."""
        sources = self._tz_sources()
        zones = []
        used = ""
        for base in sources:
            zones = self._tz_collect(base)
            if zones:
                used = base
                break
        if not zones:
            self.monitor.append(
                "*** tz_table: no zone data on this machine. Run:  pip install tzdata")
            self.monitor.append(
                "*** (IANA publishes source that needs the zic compiler; the tzdata "
                "package is the same data already compiled, and Windows ships none "
                "of its own.)")
            return

        # The lookup in gpsdo_tz.cpp bisects on the city name, so the table must
        # be sorted exactly the way it compares and must not contain duplicates.
        # Both are checked here rather than debugged on the bench.
        # Resolve city-name collisions before sorting. On Linux the legacy zone
        # names (US/Eastern, America/Buenos_Aires) are symlinks and were already
        # skipped above; the tzdata package stores them as real files, so the same
        # zone arrives twice under two names and the bisect would be ambiguous.
        # Rather than guess which entry is an alias, keep the most specific path —
        # America/Argentina/Buenos_Aires over America/Buenos_Aires — and drop the
        # rest. Deterministic, and a no-op on a system zoneinfo tree, which has no
        # collisions left to resolve.
        by_city = {}
        for n, r in zones:
            by_city.setdefault(n.split("/")[-1], []).append((n, r))
        zones, dropped = [], []
        for ent in by_city.values():
            if len(ent) == 1:
                zones.append(ent[0])
                continue
            ent.sort(key=lambda e: (-e[0].count("/"), e[0]))
            zones.append(ent[0])
            dropped.extend(e[0] for e in ent[1:])

        zones.sort(key=lambda z: z[0].split("/")[-1].lower())
        cities = [n.split("/")[-1] for n, _ in zones]
        if len(set(cities)) != len(cities):
            self.monitor.append("*** tz_table: internal, collisions unresolved")
            return
        if cities != sorted(cities, key=str.lower):
            self.monitor.append("*** tz_table: internal sort mismatch, aborted")
            return

        rules = sorted({s for _, s in zones})
        regions = sorted({n.split("/")[0] for n, _ in zones})
        if len(rules) > 255 or len(regions) > 255:
            self.monitor.append("*** tz_table: rule/region count exceeds uint8_t")
            return
        rule_id = {s: i for i, s in enumerate(rules)}
        reg_id = {r: i for i, r in enumerate(regions)}

        blob, offs = bytearray(), []
        for n, _ in zones:
            offs.append(len(blob))
            blob += n.split("/")[-1].encode("ascii") + b"\0"
        if len(blob) > 65535:
            self.monitor.append("*** tz_table: string blob exceeds uint16 offsets")
            return

        out = []
        w = out.append
        # Only claim an IANA release when the data actually came from the tzdata
        # package, which states its version. A system zoneinfo tree carries no
        # version field, so quoting the installed package's number against it
        # would be asserting something we have not checked.
        iana = self._tz_iana_version() if used == self._tz_pkg_dir() else ""
        w("/* AUTO-GENERATED by the GPSDO Tuner (Raw monitor tab) -- do not edit.\n"
          f" * IANA release: {iana or 'unknown (system zoneinfo carries no version)'}\n"
          f" * Generated:    {time.strftime('%Y-%m-%d %H:%M:%S')}\n"
          " * Zone data comes from this machine. To refresh it:\n"
          " *   pip install -U tzdata      (any OS; zic-compiled IANA data)\n"
          " * then press the button again. */\n")
        w("#ifndef TZ_TABLE_H\n#define TZ_TABLE_H\n\n#include <stdint.h>\n\n")
        w(f"#define TZ_NZONES   {len(zones)}\n")
        w(f"#define TZ_NRULES   {len(rules)}\n")
        w(f"#define TZ_NREGIONS {len(regions)}\n\n")
        w("/* POSIX TZ rule strings, deduplicated across zones. */\n")
        w("static const char *const tz_rule_str[TZ_NRULES] = {\n")
        for s in rules:
            w(f'    "{s}",\n')
        w("};\n\n")
        w("static const char *const tz_region_str[TZ_NREGIONS] = {\n")
        for r in regions:
            w(f'    "{r}",\n')
        w("};\n\n")
        w("/* City names, NUL-separated, sorted case-insensitively. */\n")
        w("static const char tz_city_blob[] =\n")
        line = ""
        for n, _ in zones:
            piece = f'"{n.split("/")[-1]}\\0"'
            if len(line) + len(piece) > 72:
                w(f"    {line}\n"); line = ""
            line += piece
        if line:
            w(f"    {line}\n")
        w(";\n\n")
        w("static const uint16_t tz_city_off[TZ_NZONES] = {\n")
        for i in range(0, len(offs), 12):
            w("    " + ",".join(f"{x:5d}" for x in offs[i:i + 12]) + ",\n")
        w("};\n\n")
        w("static const uint8_t tz_zone_region[TZ_NZONES] = {\n")
        ids = [reg_id[n.split("/")[0]] for n, _ in zones]
        for i in range(0, len(ids), 20):
            w("    " + ",".join(f"{x:3d}" for x in ids[i:i + 20]) + ",\n")
        w("};\n\n")
        w("static const uint8_t tz_zone_rule[TZ_NZONES] = {\n")
        ids = [rule_id[s] for _, s in zones]
        for i in range(0, len(ids), 20):
            w("    " + ",".join(f"{x:3d}" for x in ids[i:i + 20]) + ",\n")
        w("};\n\n")
        w("#endif /* TZ_TABLE_H */\n")

        path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "tz_table.h")
        try:
            with open(path, "w", encoding="utf-8") as fh:
                fh.write("".join(out))
        except OSError as e:
            self.monitor.append(f"*** tz_table: cannot write {path}: {e}")
            return

        approx = (len(blob) + len(zones) * 4
                  + sum(len(s) + 1 for s in rules)
                  + sum(len(r) + 1 for r in regions))
        self.monitor.append(
            f"*** tz_table.h written: {len(zones)} zones, {len(rules)} rules, "
            f"{len(regions)} regions, ~{approx / 1024:.1f} KB flash")
        self.monitor.append(
            f"*** source: {used}"
            + (f"  (IANA {iana})" if iana
               else "  (system tree — carries no version field)"))
        self.monitor.append(f"*** file:   {path}")
        if dropped:
            self.monitor.append(
                f"*** {len(dropped)} legacy alias(es) dropped in favour of their "
                f"canonical names, e.g. {', '.join(sorted(dropped)[:3])}")

    def toggle_logging(self):
        """Start or stop writing every received line to a file.

        The file is created next to this script as gpsdo_YYYY-MM-DD_HH-MM-SS.log
        and opened line-buffered, so a run that ends badly still leaves usable
        data behind rather than an empty file full of unflushed buffers."""
        if self._logfile is not None:
            self._stop_logging()
            return
        name = time.strftime("gpsdo_%Y-%m-%d_%H-%M-%S.log")
        path = os.path.join(os.path.dirname(os.path.abspath(__file__)), name)
        try:
            self._logfile = open(path, "w", encoding="utf-8",
                                 errors="replace", buffering=1)
        except OSError as e:
            self._logfile = None
            self.log_lbl.setText(f"cannot open log: {e}")
            return
        self._log_lines = 0
        self._log_path = path
        self.log_btn.setText("Stop logging")
        self.log_lbl.setText(f"logging to {name}")
        self.monitor.append(f"*** logging started: {path}")

    def _stop_logging(self):
        if self._logfile is None:
            return
        try:
            self._logfile.close()
        except OSError:
            pass
        name = os.path.basename(self._log_path)
        lines = self._log_lines
        self._logfile = None
        self.log_btn.setText("Start logging")
        self.log_lbl.setText(f"saved {name} ({lines} lines)")
        self.monitor.append(f"*** logging stopped: {lines} lines written")

    def clear_plots(self):
        """Drop every buffered sample and restart the time axis at zero.

        The Raw monitor has always had a Clear; the plots did not, so the only
        way to get a clean trace after a false start was to restart the script
        and lose the connection with it. Buffers, curves and the time origin all
        go together — resetting t0 as well keeps the axis starting from 0 rather
        than resuming at whatever the session clock had reached."""
        for d in self.data.values():
            d.clear()
        self.tbuf.clear()
        self.t0 = time.time()
        for curve in (self.curve_phase, self.curve_vph, self.curve_freq):
            curve.setData([], [])
        if hasattr(self, "follow_chk"):
            self.follow_chk.setChecked(True)
        self._update_span_hint()

    def show_about(self):
        """Replay the boot animation on demand. Purely for the pleasure of it."""
        self._about = SplashScreen(lambda: None)
        self._about.show()
        self._about.raise_()
        self._about.activateWindow()

    def _on_span_changed(self, _label):
        """Changing the span is an explicit request to see that window live."""
        if hasattr(self, "follow_chk"):
            self.follow_chk.setChecked(True)
        self.refresh_plots()

    def _on_follow_toggled(self, on):
        if on:
            self.refresh_plots()          # snap back to the live window
        else:
            for plot in (self.plot_phase, self.plot_vph, self.plot_freq):
                plot.enableAutoRange(axis="x", enable=False)
        self._update_span_hint()

    def _on_manual_range(self, *_):
        """Mouse-driven pan or zoom: hand the axis over to the operator."""
        if hasattr(self, "follow_chk") and self.follow_chk.isChecked():
            self.follow_chk.blockSignals(True)
            self.follow_chk.setChecked(False)
            self.follow_chk.blockSignals(False)
            for plot in (self.plot_phase, self.plot_vph, self.plot_freq):
                plot.enableAutoRange(axis="x", enable=False)
        self._update_span_hint()

    def _update_span_hint(self):
        if not hasattr(self, "span_hint"):
            return
        held = len(self.tbuf)
        if hasattr(self, "follow_chk") and not self.follow_chk.isChecked():
            self.span_hint.setText(
                f"paused — {held} s held, drag/zoom to explore, tick Follow to resume")
        else:
            self.span_hint.setText(f"{held} s held")

    def _plot_span(self):
        """Seconds of history the plots should show, 0 for everything held."""
        label = self.span_combo.currentText() if hasattr(self, "span_combo") \
                else PLOT_SPAN_DEFAULT
        for name, secs in PLOT_SPANS:
            if name == label:
                return secs
        return 300

    def refresh_plots(self):
        if not self.tbuf:
            return
        t = list(self.tbuf)
        now = t[-1]
        span = self._plot_span()

        # Trim to the visible span and pin the X axis to it, so the trace scrolls
        # leftwards at a constant scale instead of the axis stretching to cover
        # the whole buffer. Sending every held point and letting the view
        # auto-range is what made an hour of acquisition collapse into an
        # unreadable band with no apparent motion.
        following = (not hasattr(self, "follow_chk")) or self.follow_chk.isChecked()

        # While paused the whole buffer is drawn, so panning and zooming reach
        # every sample held rather than stopping at the edge of the live window.
        if span > 0 and following:
            cutoff = now - span
            keep = sum(1 for x in t if x >= cutoff)
            keep = max(keep, 2)          # a single point draws nothing
        else:
            keep = len(t)

        # Decimate for display. Paused on a full buffer this would otherwise push
        # 108 000 points per curve into the view once a second, which pyqtgraph
        # will draw but not quickly. No plot pane is more than a couple of
        # thousand pixels wide, so beyond that the extra points cost time and show
        # nothing; stepping through the data keeps panning responsive.
        MAX_DRAW = 4000
        def series(name):
            d = list(self.data[name])
            n = min(len(d), len(t), keep)
            xs, ys = t[-n:], d[-n:]
            if n > MAX_DRAW:
                step = (n // MAX_DRAW) + 1
                xs, ys = xs[::step], ys[::step]
            return xs, ys

        top = getattr(self, "_series_top", "dph")
        mid = getattr(self, "_series_mid", "Vphase")
        tx, a = series(top);         self.curve_phase.setData(tx, a)
        tx, b = series(mid);         self.curve_vph.setData(tx, b)
        tx, fe = series("freq_err"); self.curve_freq.setData(tx, fe)

        # Left edge is clamped to the oldest sample we actually hold, so a fresh
        # session grows from t0 to the full width and only then starts sliding.
        # Pinning it to now-span from the start would put the axis into negative
        # time and squeeze the first minutes into the right-hand edge.
        if following:
            left = max(t[0], now - span) if span > 0 else t[0]
            for plot in (self.plot_phase, self.plot_vph, self.plot_freq):
                if span > 0:
                    plot.setXRange(left, max(now, left + 1.0), padding=0)
                else:
                    plot.enableAutoRange(axis="x")
        self._update_span_hint()

    def closeEvent(self, ev):
        self._stop_logging()
        if self.worker.isRunning():
            self.worker.stop()
            self.worker.wait(1000)
        ev.accept()


class SplashScreen(QWidget):
    """Five-second animated splash mirroring the firmware's TFT boot screen:
    two phase-shifted sine waves (blue and amber) that drift into agreement and
    merge into a single green trace — GPS and OCXO pulling into phase lock.

    Timeline over ~5 s: fade the waves up, converge them, then hold the merged
    green wave briefly before closing. Purely cosmetic."""

    # Colours chosen to match the TFT splash constants (RGB565 0x3D7F / 0xFD80).
    COL_BG    = QColor(12, 14, 20)
    COL_UPPER = QColor(60, 172, 255)    # blue  — the GPS reference
    COL_LOWER = QColor(255, 176, 0)     # amber — the free-running OCXO
    COL_MERGE = QColor(64, 224, 128)    # green — locked
    COL_TEXT  = QColor(220, 226, 235)
    COL_DIM   = QColor(120, 132, 150)

    DURATION_MS = 5000
    FRAME_MS    = 25
    CYCLES      = 5.0        # sine cycles across the width
    PHASE0      = 2.5        # initial phase offset [rad]

    def __init__(self, on_done):
        super().__init__(None, Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint)
        self._on_done = on_done
        self.resize(560, 300)
        self._t = 0.0                     # 0..1 progress
        self._timer = QTimer(self)
        self._timer.timeout.connect(self._tick)
        self._timer.start(self.FRAME_MS)
        # centre on the primary screen
        scr = QApplication.primaryScreen()
        if scr:
            geo = scr.availableGeometry()
            self.move(geo.center().x() - self.width() // 2,
                      geo.center().y() - self.height() // 2)

    def _tick(self):
        self._t += self.FRAME_MS / self.DURATION_MS
        if self._t >= 1.0:
            self._timer.stop()
            self.close()
            self._on_done()
            return
        self.update()

    def mousePressEvent(self, ev):
        """Let an impatient user skip the animation."""
        self._timer.stop()
        self.close()
        self._on_done()

    def paintEvent(self, ev):
        p = QPainter(self)
        p.setRenderHint(QPainter.Antialiasing, True)
        w, h = self.width(), self.height()
        p.fillRect(0, 0, w, h, self.COL_BG)

        t = max(0.0, min(1.0, self._t))
        # Stage weights: fade in (0..0.30), converge (0.30..0.80), hold (0.80..1)
        fade    = min(1.0, t / 0.30)
        if t <= 0.30:
            conv = 0.0
        else:
            conv = min(1.0, (t - 0.30) / 0.50)
        merged  = conv >= 0.999

        # Wave geometry: the two traces start apart and close on the midpoint.
        yc      = int(h * 0.56)
        amp     = h * 0.085
        gap_top = h * 0.10
        gap_bot = h * 0.16
        meet    = (gap_bot - gap_top) / 2.0
        off_top = -gap_top * (1.0 - conv) + meet * conv
        off_bot = +gap_bot * (1.0 - conv) + meet * conv
        # Phase converges alongside position, so they truly coincide at the end.
        ph_top  = self.PHASE0 * (1.0 - conv)

        def wave_points(y_off, phase):
            pts = []
            for x in range(0, w + 1, 3):
                y = yc + y_off + amp * math.sin(x / w * 2.0 * math.pi
                                                * self.CYCLES + phase)
                pts.append((x, y))
            return pts

        def draw_wave(pts, colour, alpha, width_px):
            c = QColor(colour)
            c.setAlphaF(max(0.0, min(1.0, alpha)))
            pen = QPen(c, width_px)
            pen.setCapStyle(Qt.RoundCap)
            p.setPen(pen)
            for i in range(1, len(pts)):
                p.drawLine(int(pts[i-1][0]), int(pts[i-1][1]),
                           int(pts[i][0]),   int(pts[i][1]))

        if merged:
            # Single thicker green trace once the two have coincided.
            draw_wave(wave_points(meet, 0.0), self.COL_MERGE, 1.0, 3.0)
        else:
            # As they converge, blend each toward green so the merge looks earned
            # rather than abrupt.
            def blend(a, b, f):
                return QColor(int(a.red()   + (b.red()   - a.red())   * f),
                              int(a.green() + (b.green() - a.green()) * f),
                              int(a.blue()  + (b.blue()  - a.blue())  * f))
            f = conv ** 2
            draw_wave(wave_points(off_top, ph_top),
                      blend(self.COL_UPPER, self.COL_MERGE, f), fade, 2.0)
            draw_wave(wave_points(off_bot, 0.0),
                      blend(self.COL_LOWER, self.COL_MERGE, f), fade, 2.0)

        # ---- text ----------------------------------------------------------
        title = QFont(); title.setPointSize(19); title.setBold(True)
        p.setFont(title)
        p.setPen(self.COL_TEXT)
        p.drawText(0, int(h * 0.16), w, 34, Qt.AlignHCenter, "GPSDO Tuner")

        sub = QFont(); sub.setPointSize(10)
        p.setFont(sub)
        p.setPen(self.COL_DIM)
        p.drawText(0, int(h * 0.16) + 34, w, 22, Qt.AlignHCenter,
                   f"v{TOOL_VERSION}  —  GPS Disciplined OCXO tuning console")

        # About replays this splash, so it is the only place a user is shown
        # who the loops come from. Two lines, small, under the subtitle.
        cred = QFont(); cred.setPointSize(8)
        p.setFont(cred)
        p.drawText(0, int(h * 0.16) + 58, w, 18, Qt.AlignHCenter,
                   "firmware J. M. Niewiński (jmnlabs) · after André Balsa v0.06c")
        p.drawText(0, int(h * 0.16) + 74, w, 18, Qt.AlignHCenter,
                   "algo 11 Lars Walenius · algo 12 Alan Cashin (MIS42N) · logger lucido")
        p.setFont(sub)

        # status line fades in with the merge, echoing the firmware's metaphor
        if conv > 0.55:
            lock = QFont(); lock.setPointSize(10); lock.setBold(True)
            p.setFont(lock)
            c = QColor(self.COL_MERGE)
            c.setAlphaF(min(1.0, (conv - 0.55) / 0.45))
            p.setPen(c)
            p.drawText(0, int(h * 0.84), w, 22, Qt.AlignHCenter,
                       "PHASE LOCKED" if merged else "acquiring…")
        p.end()


def _minimise_spawned_console():
    """Double-clicking a .py file on Windows spawns a console window that then
    sits behind the GUI doing nothing. Send it to the taskbar — minimised rather
    than hidden, so a traceback is still reachable if something goes wrong.

    Only a console this process owns is touched. If the tuner was started from a
    terminal the operator already had open, that window is theirs and minimising
    it would be rude; GetConsoleProcessList reporting exactly one attached
    process is what distinguishes the two cases. Cosmetic throughout, so every
    failure path is silent — this must never stop the tuner from starting."""
    if os.name != "nt":
        return
    try:
        import ctypes
        from ctypes import wintypes
        k32 = ctypes.windll.kernel32
        hwnd = k32.GetConsoleWindow()
        if not hwnd:
            return                      # no console at all (e.g. run as .pyw)
        buf = (wintypes.DWORD * 8)()
        if k32.GetConsoleProcessList(buf, 8) != 1:
            return                      # shared terminal — leave it alone
        ctypes.windll.user32.ShowWindow(hwnd, 6)   # SW_MINIMIZE
    except Exception:
        pass


def main():
    app = QApplication(sys.argv)
    _minimise_spawned_console()

    # The main window goes up first and maximised, with the splash laid over it,
    # so the animation plays against the console it is introducing rather than
    # against the desktop — and there is no jarring resize when it clears.
    win = GpsdoTuner()
    win.showMaximized()

    def splash_done():
        win.raise_()
        win.activateWindow()

    splash = SplashScreen(splash_done)
    splash.show()
    splash.raise_()
    splash.activateWindow()

    sys.exit(app.exec())


if __name__ == "__main__":
    main()
