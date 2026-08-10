#!/usr/bin/env python3
"""Bench controller for the DCO board.

Drives the DCO's whole parameter surface over its USB serial port, so the board can be
tested with no Input board and no Screen attached. Notes are not handled here: the DCO
already enumerates as a USB MIDI device, so play it from a MIDI keyboard or VMPK.

Requires the firmware to be built with ENABLE_USB_CONTROL (see DCO/DCO.ino).
Match SERIAL_FRAMING_COBS with --cobs or DCO_SERIAL_COBS=1.

    python3 app.py [--port /dev/ttyACM0] [--theme dark|light] [--cobs]
"""

from __future__ import annotations

import argparse
import os
import queue
import sys
import threading
import tkinter as tk
from tkinter import messagebox, simpledialog, ttk

import params
import presets
import protocol
import theme

try:
    import serial
except ImportError:
    sys.exit("pyserial is required:  pacman -S python-pyserial   (or pip install pyserial)")

# Coalesce slider drags into one write per interval (last-write-wins), so dragging
# cannot flood the link — important once PIO pulse reloads running SMs.
SEND_INTERVAL_MS = 20

PARAM_BY_PID = {p.pid: p for p in params.PARAMS}

# Oscillators tab layout (see _build_osc_tab).
OSC_PITCH_PIDS = (13, 14, 33, 15, 34)
OSC_SYNC_PIDS = (31, 36, 37, 17)
OSC_VOICE_PIDS = (26, 27, 18, 32, 28, 29, 30, 43, 21)
OSC_LEVEL_PIDS = (22, 23, 38, 24)
OSC_WAVE_MATRIX = (
    ("OSC1", (1, 2, 3)),
    ("OSC2", (84, 85, 86)),
    ("OSC3", (87, 88, 89)),
)
OSC_WAVE_COLS = ("Saw", "Pulse", "Tri")

# Reflow Oscillators tab: stack Pitch|Sync over Voice below this width (px).
OSC_SPLIT_STACK_WIDTH = 720

# Diagnostics tab compact layout (see _build_diag_tab).
DIAG_SPLIT_STACK_WIDTH = 720
DIAG_FRAME_PAD = 4
DIAG_SECTION_PADY = 4

# Envelopes tab layout (see _build_env_tab).
ENV_ADSR_BLOCKS = ("adsr_vca", "adsr_vcf", "adsr_dco")
ENV_CURVE_RESTART_PIDS = (8, 9, 48, 49, 50, 51)
ENV_CURVE_COLUMNS = (
    ("EnvVCA curves", 48, 49, 8),
    ("EnvVCF curves", 50, 51, 9),
    ("EnvDCO curves", None, None, None),
)
ENV_ADSR_VFADER_MIN = 80
ENV_ADSR_VFADER_MAX = 280
ENV_ADSR_VIEWPORT_FRAC = 0.35


def _bind_scale_jump(scale: ttk.Scale, on_change) -> None:
    """Button-1 on trough jumps to click position (clam default only steps)."""

    def on_press(event):
        part = scale.identify(event.x, event.y)
        if part and ("trough" in str(part) or "track" in str(part)):
            scale.set(scale.get(event.x, event.y))
            on_change(str(scale.get()))
            return "break"

    scale.bind("<Button-1>", on_press, add="+")


def _make_scale(parent, *, lo, hi, var, command, orient="horizontal", length=None) -> ttk.Scale:
    kwargs: dict = dict(from_=lo, to=hi, variable=var, orient=orient, command=command)
    if length is not None:
        kwargs["length"] = length
    scale = ttk.Scale(parent, **kwargs)
    _bind_scale_jump(scale, command)
    return scale


def ivar(var: tk.Variable) -> int:
    """Read a Tk variable as an int.

    ttk.Scale always writes floats, so slider variables are DoubleVar and have to be
    rounded on the way out rather than read as IntVar (which would raise on "3.5").
    """
    try:
        return int(round(float(var.get())))
    except (tk.TclError, ValueError):
        return 0


class Link:
    """Serial connection plus a background reader for the board's debug output."""

    def __init__(self) -> None:
        self.port = None
        self.rx: queue.Queue[str] = queue.Queue()
        self._reader: threading.Thread | None = None
        self._stop = threading.Event()

    @property
    def is_open(self) -> bool:
        return self.port is not None and self.port.is_open

    def open(self, device: str) -> None:
        self.close()
        self.port = serial.Serial(device, protocol.BAUD, timeout=0.1)
        self._stop.clear()
        self._reader = threading.Thread(target=self._read_loop, daemon=True)
        self._reader.start()

    def close(self) -> None:
        self._stop.set()
        if self._reader is not None:
            self._reader.join(timeout=0.5)
            self._reader = None
        if self.port is not None:
            try:
                self.port.close()
            except OSError:
                pass
            self.port = None

    def send(self, frame: bytes) -> None:
        if not self.is_open:
            return
        try:
            self.port.write(frame)
        except OSError as exc:
            self.rx.put(f"[link] write failed: {exc}\n")
            self.close()

    def _read_loop(self) -> None:
        # The board only ever sends plain debug text back, never frames, so decode
        # loosely and forward whatever arrives to the log pane.
        while not self._stop.is_set():
            try:
                data = self.port.read(256)
            except (OSError, AttributeError, TypeError):
                break
            if data:
                self.rx.put(data.decode("utf-8", errors="replace"))


class App:
    def __init__(
        self,
        root: tk.Tk,
        preferred_port: str | None,
        mode: str = "dark",
        cobs: bool = False,
    ) -> None:
        protocol.use_cobs = cobs
        self.root = root
        self.link = Link()
        self.pending: dict[str, bytes] = {}  # dedup key -> most recent frame
        self.param_vars: dict[int, tk.Variable] = {}
        self.block_vars: dict[str, dict[str, tk.Variable]] = {}
        # Scale readout Labels: ("p", pid), ("b", block_key, field_key),
        # ("pio_pulse",), or ("char_jitter", hi).
        # ttk.Scale often skips command on programmatic var.set(); we sync these after apply.
        self._readouts: dict[tuple, ttk.Label] = {}
        self.blocks_by_key = {b.key: b for b in params.BLOCKS}
        self.pio_pulse_var: tk.DoubleVar | None = None
        # Character-tab diagnostic jitters (PARAM_DEBUG_COMMAND); not in presets / Send all.
        self.character_jitter_vars: dict[int, tk.DoubleVar] = {}
        self.mode = mode if mode in theme.MODES else "dark"
        self.dot_on = False
        self.bank = presets.empty_bank()
        self._clean_fp = ""
        self._preset_loading = False
        self._tab_canvases: list[tk.Canvas] = []
        self._wheel_canvas: tk.Canvas | None = None
        self.notebook: ttk.Notebook | None = None

        root.title("DCO bench controller")
        # Wide enough that the toolbar's right-hand side is never squeezed out by pack().
        root.geometry("1140x960")
        root.minsize(900, 600)
        # Before building, so every widget is created already styled.
        theme.apply(root, self.mode)

        self._build_toolbar(preferred_port)
        self._build_preset_bar()
        self.paned = ttk.Panedwindow(root, orient=tk.VERTICAL)
        self.paned.pack(fill="both", expand=True, padx=8, pady=(0, 8))
        self._build_tabs()
        self._build_log()
        self._init_presets()
        # The canvases and log pane are plain Tk and were created after apply()'s own pass.
        theme.retint(root)
        self._style_log_tags()
        # Board output starts compact; drag the sash up when dumping profiler tables.
        self.root.after_idle(self._shrink_log_pane)

        self.root.after(SEND_INTERVAL_MS, self._flush)
        self.root.after(50, self._drain_log)
        self.root.protocol("WM_DELETE_WINDOW", self._on_close)

    # --- toolbar ---------------------------------------------------------

    def _build_toolbar(self, preferred_port: str | None) -> None:
        bar = ttk.Frame(self.root, padding=(8, 6))
        bar.pack(fill="x")

        ttk.Label(bar, text="Port").pack(side="left")
        self.port_var = tk.StringVar()
        self.port_combo = ttk.Combobox(bar, textvariable=self.port_var, width=28, state="readonly")
        self.port_combo.pack(side="left", padx=6)
        self._refresh_ports(preferred_port)

        ttk.Button(bar, text="Rescan", command=lambda: self._refresh_ports(None)).pack(side="left")
        self.connect_btn = ttk.Button(bar, text="Connect", command=self._toggle_connect)
        self.connect_btn.pack(side="left", padx=6)

        ttk.Separator(bar, orient="vertical").pack(side="left", fill="y", padx=10)
        ttk.Button(bar, text="Send all", command=self.send_all,
                   style="Accent.TButton").pack(side="left")
        ttk.Button(bar, text="Reset to defaults", command=self.reset_defaults).pack(side="left", padx=6)

        self.status_var = tk.StringVar(value="not connected")
        ttk.Label(bar, textvariable=self.status_var, style="Status.TLabel").pack(side="right")
        self.status_dot = ttk.Label(bar, text="\u25cf", style="Dot.TLabel")
        self.status_dot.pack(side="right", padx=(10, 5))

        self.theme_btn = ttk.Button(bar, command=self._toggle_theme, width=7)
        self.theme_btn.pack(side="right", padx=(0, 12))
        self._sync_theme_button()

    # --- preset bar ------------------------------------------------------

    def _build_preset_bar(self) -> None:
        bar = ttk.Frame(self.root, padding=(8, 0, 8, 6))
        bar.pack(fill="x")

        ttk.Button(bar, text="<", width=3, command=self._preset_prev).pack(side="left")
        self.preset_num_var = tk.StringVar(value="000")
        self.preset_spin = ttk.Spinbox(
            bar,
            from_=0,
            to=presets.NUM_SLOTS - 1,
            textvariable=self.preset_num_var,
            width=4,
            command=self._preset_number_committed,
        )
        self.preset_spin.pack(side="left", padx=(6, 0))
        self.preset_spin.bind("<Return>", lambda _e: self._preset_number_committed())
        self.preset_spin.bind("<FocusOut>", lambda _e: self._preset_number_committed())

        self.preset_dirty_var = tk.StringVar(value="")
        ttk.Label(bar, textvariable=self.preset_dirty_var, width=1).pack(side="left", padx=(8, 0))

        self.preset_name_var = tk.StringVar(value="Init")
        self.preset_name_entry = ttk.Entry(bar, textvariable=self.preset_name_var, width=28)
        self.preset_name_entry.pack(side="left", padx=(2, 6), fill="x", expand=True)

        ttk.Button(bar, text=">", width=3, command=self._preset_next).pack(side="left")
        ttk.Button(bar, text="Load", command=self._preset_load).pack(side="left", padx=(10, 0))
        ttk.Button(bar, text="Save", command=self._preset_save).pack(side="left", padx=(6, 0))
        ttk.Button(bar, text="Save as…", command=self._preset_save_as).pack(side="left", padx=(6, 0))
        ttk.Button(bar, text="Init", command=self._preset_init).pack(side="left", padx=(6, 0))

    def _init_presets(self) -> None:
        self.bank = presets.load_bank()
        for var in self.param_vars.values():
            var.trace_add("write", lambda *_a: self._refresh_dirty())
        for fields in self.block_vars.values():
            for var in fields.values():
                var.trace_add("write", lambda *_a: self._refresh_dirty())
        self._preset_recall(int(self.bank["current"]), send=False, persist_current=False)

    def _preset_slot_index(self) -> int:
        try:
            n = int(str(self.preset_num_var.get()).strip())
        except ValueError:
            n = int(self.bank.get("current", 0))
        return max(0, min(presets.NUM_SLOTS - 1, n))

    def _set_preset_number(self, index: int) -> None:
        self.preset_num_var.set(f"{index:03d}")

    def _current_ui_slot(self, name: str | None = None) -> dict:
        slot = presets.capture(self.param_vars, self.block_vars, self._param_value)
        if name is None:
            name = self.preset_name_var.get().strip() or "Untitled"
        slot["name"] = name[:48]
        return slot

    def _refresh_dirty(self) -> None:
        if self._preset_loading:
            return
        fp = presets.slot_fingerprint(self._current_ui_slot())
        self.preset_dirty_var.set("*" if fp != self._clean_fp else "")

    def _push_if_connected(self) -> None:
        if self.link.is_open:
            self.send_all()

    def _sync_readouts(self) -> None:
        """Refresh Scale numeric labels from live vars (command often skipped on var.set)."""
        for key, rd in self._readouts.items():
            if key[0] == "p":
                var = self.param_vars.get(key[1])
            elif key[0] == "pio_pulse":
                var = self.pio_pulse_var
            elif key[0] == "char_jitter":
                var = self.character_jitter_vars.get(key[1])
            else:
                var = self.block_vars.get(key[1], {}).get(key[2])
            if var is None:
                continue
            rd.config(text=str(ivar(var)))

    def _preset_recall(self, index: int, *, send: bool, persist_current: bool) -> None:
        index = max(0, min(presets.NUM_SLOTS - 1, index))
        slot = self.bank["slots"][index]
        empty = presets.slot_is_empty(slot)
        self._preset_loading = True
        try:
            presets.apply(self.param_vars, self.block_vars, None if empty else slot)
            if empty:
                self.preset_name_var.set("Init")
                self._clean_fp = presets.slot_fingerprint(presets.defaults_slot())
            else:
                self.preset_name_var.set(slot["name"])
                self._clean_fp = presets.slot_fingerprint(slot)
            self._set_preset_number(index)
            self.bank["current"] = index
        finally:
            self._preset_loading = False
        self._sync_readouts()
        self.preset_dirty_var.set("")
        if persist_current:
            presets.save_bank(self.bank)
        if empty:
            self.log(f"[preset] {index:03d} empty -- Init defaults loaded\n")
        else:
            self.log(f"[preset] loaded {index:03d} {slot['name']}\n")
        if send:
            self._push_if_connected()

    def _preset_number_committed(self) -> None:
        index = self._preset_slot_index()
        if index == int(self.bank.get("current", -1)):
            self._set_preset_number(index)
            return
        self._preset_recall(index, send=True, persist_current=True)

    def _preset_prev(self) -> None:
        index = (self._preset_slot_index() - 1) % presets.NUM_SLOTS
        self._preset_recall(index, send=True, persist_current=True)

    def _preset_next(self) -> None:
        index = (self._preset_slot_index() + 1) % presets.NUM_SLOTS
        self._preset_recall(index, send=True, persist_current=True)

    def _preset_load(self) -> None:
        self._preset_recall(self._preset_slot_index(), send=True, persist_current=True)

    def _preset_save(self, index: int | None = None) -> None:
        if index is None:
            index = self._preset_slot_index()
        typed = self.preset_name_var.get().strip()
        existing = self.bank["slots"][index]
        if typed:
            name = typed[:48]
        elif not presets.slot_is_empty(existing):
            name = existing["name"]
        else:
            name = "Untitled"
        slot = self._current_ui_slot(name)
        self.bank["slots"][index] = slot
        self.bank["current"] = index
        presets.save_bank(self.bank)
        self.preset_name_var.set(name)
        self._set_preset_number(index)
        self._clean_fp = presets.slot_fingerprint(slot)
        self.preset_dirty_var.set("")
        self.log(f"[preset] saved {index:03d} {name}\n")

    def _preset_save_as(self) -> None:
        current_index = self._preset_slot_index()
        dest = simpledialog.askinteger(
            "Save as…", "Slot (0–127):",
            initialvalue=current_index, minvalue=0, maxvalue=presets.NUM_SLOTS - 1,
            parent=self.root,
        )
        if dest is None:
            return
        existing = self.bank["slots"][dest]
        if dest != current_index and not presets.slot_is_empty(existing):
            if not messagebox.askyesno(
                "Save as…",
                f"Slot {dest:03d} already has “{existing['name']}”. Overwrite?",
                parent=self.root,
            ):
                return
        current_name = self.preset_name_var.get().strip() or "Untitled"
        name = simpledialog.askstring(
            "Save as…", "Preset name:", initialvalue=current_name, parent=self.root
        )
        if name is None:
            return
        name = name.strip()[:48] or "Untitled"
        self.preset_name_var.set(name)
        self._preset_save(dest)

    def _preset_init(self) -> None:
        self._preset_loading = True
        try:
            presets.apply(self.param_vars, self.block_vars, None)
            self.preset_name_var.set("Init")
        finally:
            self._preset_loading = False
        self._sync_readouts()
        self._refresh_dirty()
        self.log("[preset] Init defaults in UI -- Save to store in this slot\n")
        self._push_if_connected()

    def _toggle_theme(self) -> None:
        self.mode = "light" if self.mode == "dark" else "dark"
        # ttk resolves styles at draw time, so this restyles the live widget tree in place.
        theme.apply(self.root, self.mode)
        self._style_log_tags()
        self._sync_theme_button()
        self._set_status_dot(self.link.is_open)

    def _sync_theme_button(self) -> None:
        self.theme_btn.config(text="Light" if self.mode == "dark" else "Dark")

    def _set_status_dot(self, connected: bool) -> None:
        self.status_dot.config(style="DotOn.TLabel" if connected else "Dot.TLabel")
        self.dot_on = connected

    def _refresh_ports(self, preferred: str | None) -> None:
        values = [f"{p.device}  ({p.description})" for p in protocol.find_dco_ports()]
        if preferred:
            values.insert(0, preferred)
        self.port_combo["values"] = values
        if values:
            self.port_combo.current(0)
        else:
            self.port_var.set("")

    def _selected_device(self) -> str:
        raw = self.port_var.get().strip()
        return raw.split()[0] if raw else ""

    def _toggle_connect(self) -> None:
        if self.link.is_open:
            self.link.close()
            self.connect_btn.config(text="Connect")
            self.status_var.set("not connected")
            self._set_status_dot(False)
            self.log("[link] disconnected\n")
            return
        device = self._selected_device()
        if not device:
            self.log("[link] no serial port selected\n")
            return
        try:
            self.link.open(device)
        except (OSError, ValueError) as exc:
            self.log(f"[link] could not open {device}: {exc}\n")
            return
        self.connect_btn.config(text="Disconnect")
        self.status_var.set(f"connected to {device}")
        self._set_status_dot(True)
        self.log(f"[link] connected to {device} -- pushing UI state\n")
        self.log(f"[link] framing={'COBS' if protocol.use_cobs else 'RAW'}\n")
        n = self.send_all()
        self.log(f"[link] pushed UI state ({n} frames)\n")

    # --- tabs ------------------------------------------------------------

    def _shrink_log_pane(self) -> None:
        """Park the Board output sash near the bottom so tabs get most of the window."""
        height = self.paned.winfo_height()
        if height <= 1:
            self.root.after(50, self._shrink_log_pane)
            return
        # Leave a short log strip; the sash can be dragged up for long dumps.
        self.paned.sashpos(0, max(200, height - 140))

    def _build_tabs(self) -> None:
        notebook = ttk.Notebook(self.paned)
        self.notebook = notebook
        self.paned.add(notebook, weight=5)
        self._tab_canvases = []

        for group in params.GROUP_ORDER:
            page = ttk.Frame(notebook, padding=8)
            notebook.add(page, text=group)
            inner = self._scrollable(page)
            if group == params.GROUP_OSC:
                self._build_osc_tab(inner)
            elif group == params.GROUP_ENV:
                self._build_env_tab(inner)
            elif group == params.GROUP_CHARACTER:
                row = 0
                for param in [p for p in params.PARAMS if p.group == group]:
                    row = self._add_param(inner, param, row)
                row = self._add_character_jitter_sliders(inner, row)
                inner.columnconfigure(1, weight=1)
            elif group == params.GROUP_CAL:
                row = 0
                for block in [b for b in params.BLOCKS if b.group == group]:
                    row = self._add_block(inner, block, row)
                for param in [p for p in params.PARAMS if p.group == group]:
                    row = self._add_param(inner, param, row)
                row = self._add_pio_pulse_slider(inner, row)
                panel = self._add_diag_panel(
                    inner, row=row, column=0, title="Dev tables",
                    commands=params.CAL_DEBUG_COMMANDS,
                    note="PARAM_DEBUG_COMMAND 30: force-write fake amp-comp + PW "
                         "to LittleFS (development placeholder).",
                )
                panel.grid_configure(columnspan=3, sticky="ew")
                # Single-button panel: place button + note without diag reflow.
                for i, btn in enumerate(panel._diag_buttons):  # type: ignore[attr-defined]
                    btn.grid(row=i, column=0, sticky="w", pady=1)
                panel._diag_note_label.grid(  # type: ignore[attr-defined]
                    row=len(panel._diag_buttons), column=0, sticky="w", pady=(4, 0))  # type: ignore[attr-defined]
                inner.columnconfigure(1, weight=1)
            else:
                row = 0
                for block in [b for b in params.BLOCKS if b.group == group]:
                    row = self._add_block(inner, block, row)
                for param in [p for p in params.PARAMS if p.group == group]:
                    row = self._add_param(inner, param, row)
                inner.columnconfigure(1, weight=1)

        diag = ttk.Frame(notebook, padding=8)
        notebook.add(diag, text=params.GROUP_DIAG)
        diag_inner = self._scrollable(diag)
        self._build_diag_tab(diag_inner)

        notebook.bind("<<NotebookTabChanged>>", self._on_notebook_tab_changed)
        for seq in ("<Button-4>", "<Button-5>", "<MouseWheel>"):
            notebook.bind(seq, self._on_tab_wheel)
        self.root.bind_all("<Button-4>", self._on_tab_wheel, add="+")
        self.root.bind_all("<Button-5>", self._on_tab_wheel, add="+")
        self.root.bind_all("<MouseWheel>", self._on_tab_wheel, add="+")
        self._on_notebook_tab_changed()

    def _scrollable(self, parent: ttk.Frame) -> ttk.Frame:
        canvas = tk.Canvas(parent, highlightthickness=0)
        bar = ttk.Scrollbar(parent, orient="vertical", command=canvas.yview)
        inner = ttk.Frame(canvas)
        canvas.configure(yscrollcommand=bar.set)
        canvas.pack(side="left", fill="both", expand=True)
        bar.pack(side="right", fill="y")
        window = canvas.create_window((0, 0), window=inner, anchor="nw")

        def on_configure(_event=None):
            canvas.configure(scrollregion=canvas.bbox("all"))
            canvas.itemconfigure(window, width=canvas.winfo_width())

        inner.bind("<Configure>", on_configure)
        canvas.bind("<Configure>", on_configure)

        self._tab_canvases.append(canvas)
        for seq in ("<Button-4>", "<Button-5>", "<MouseWheel>"):
            canvas.bind(seq, self._on_tab_wheel)
            inner.bind(seq, self._on_tab_wheel)
        return inner

    def _on_notebook_tab_changed(self, _event=None) -> None:
        if self.notebook is None or not self._tab_canvases:
            self._wheel_canvas = None
            return
        try:
            idx = self.notebook.index(self.notebook.select())
            self._wheel_canvas = self._tab_canvases[idx]
        except (tk.TclError, IndexError):
            self._wheel_canvas = self._tab_canvases[0]

    def _as_widget(self, widget) -> tk.Misc | None:
        # bind_all often delivers event.widget as a Tcl path string, not a Misc.
        if widget is None:
            return None
        if isinstance(widget, str):
            try:
                return self.root.nametowidget(widget)
            except (KeyError, tk.TclError):
                return None
        return widget

    def _widget_is_descendant(self, widget, ancestor) -> bool:
        w = self._as_widget(widget)
        anc = self._as_widget(ancestor)
        if w is None or anc is None:
            return False
        while w is not None:
            if w == anc:
                return True
            w = w.master
        return False

    def _wheel_over_log(self, widget) -> bool:
        log = getattr(self, "log_text", None)
        return log is not None and self._widget_is_descendant(widget, log)

    def _on_tab_wheel(self, event) -> str | None:
        if self._wheel_over_log(event.widget):
            return None
        if self.notebook is not None:
            x, y = self.root.winfo_pointerxy()
            under = self.root.winfo_containing(x, y)
            if under is None or not self._widget_is_descendant(under, self.notebook):
                return None
        canvas = self._wheel_canvas
        if canvas is None:
            return None
        if event.num == 4:
            canvas.yview_scroll(-2, "units")
        elif event.num == 5:
            canvas.yview_scroll(2, "units")
        elif event.delta:
            canvas.yview_scroll(int(-1 * (event.delta / 120)), "units")
        return "break"

    def _build_osc_tab(self, parent: ttk.Frame) -> None:
        parent.columnconfigure(0, weight=1)

        self._osc_split = ttk.Frame(parent)
        self._osc_split.grid(row=0, column=0, sticky="ew", pady=(0, 4))
        self._osc_col_left = ttk.Frame(self._osc_split)
        self._osc_col_right = ttk.Frame(self._osc_split)

        row = 0
        row = self._add_osc_section(self._osc_col_left, row, "Pitch", OSC_PITCH_PIDS, columnspan=1)
        row = self._add_osc_section(self._osc_col_left, row, "Sync", OSC_SYNC_PIDS, columnspan=1)
        self._add_osc_section(self._osc_col_right, 0, "Voice & drift", OSC_VOICE_PIDS, columnspan=1)

        row = self._add_osc_levels_row(parent, 1, OSC_LEVEL_PIDS)
        self._add_osc_wave_matrix(parent, row)

        self._osc_reflow_width = -1
        parent.bind("<Configure>", self._osc_layout_reflow, add="+")
        parent.after_idle(self._osc_layout_reflow)

    def _osc_layout_reflow(self, event=None) -> None:
        split = getattr(self, "_osc_split", None)
        if split is None:
            return
        width = split.winfo_width()
        if width < 8:
            return
        if width == self._osc_reflow_width:
            return
        self._osc_reflow_width = width

        stacked = width < OSC_SPLIT_STACK_WIDTH
        left = self._osc_col_left
        right = self._osc_col_right

        if stacked:
            left.grid(row=0, column=0, sticky="ew")
            right.grid(row=1, column=0, sticky="ew", pady=(8, 0))
            split.columnconfigure(0, weight=1)
            split.columnconfigure(1, weight=0)
        else:
            left.grid(row=0, column=0, sticky="nsew", padx=(0, 8))
            right.grid(row=0, column=1, sticky="nsew")
            split.columnconfigure(0, weight=1)
            split.columnconfigure(1, weight=1)

        left.columnconfigure(0, weight=1)
        right.columnconfigure(0, weight=1)

        cells = getattr(self, "_osc_level_cells", ())
        levels_frame = getattr(self, "_osc_levels_frame", None)
        if levels_frame is not None and cells:
            compact = stacked or width < OSC_SPLIT_STACK_WIDTH + 120
            for c in range(4):
                levels_frame.columnconfigure(c, weight=0)
            for r in range(2):
                levels_frame.rowconfigure(r, weight=0)
            if compact:
                levels_frame.columnconfigure(0, weight=1)
                levels_frame.columnconfigure(1, weight=1)
                for i, cell in enumerate(cells):
                    cell.grid(row=i // 2, column=i % 2, sticky="ew", padx=4, pady=4)
            else:
                for i, cell in enumerate(cells):
                    levels_frame.columnconfigure(i, weight=1)
                    cell.grid(row=0, column=i, sticky="ew", padx=4, pady=0)

    def _build_env_tab(self, parent: ttk.Frame) -> None:
        parent.columnconfigure(1, weight=1)
        self._env_scales = []
        self._env_fader_length = -1
        self._env_inner = parent

        times = ttk.Frame(parent)
        times.grid(row=0, column=0, columnspan=3, sticky="ew", pady=(0, 8))
        self._env_times = times
        for col, key in enumerate(ENV_ADSR_BLOCKS):
            self._add_adsr_block_vertical(times, self.blocks_by_key[key], col)
        for c in range(len(ENV_ADSR_BLOCKS)):
            self._env_times.columnconfigure(c, weight=1, uniform="env")
        self._build_env_curves_row(times)

        row = 1
        for p in params.PARAMS:
            if p.group == params.GROUP_ENV and p.pid not in ENV_CURVE_RESTART_PIDS:
                row = self._add_param(parent, p, row)

        parent.bind("<Configure>", self._env_fader_reflow, add="+")
        parent.after_idle(self._env_fader_reflow)

    def _env_fader_reflow(self, _event=None) -> None:
        scales = getattr(self, "_env_scales", None)
        inner = getattr(self, "_env_inner", None)
        if not scales or inner is None:
            return
        canvas = inner.master
        if not isinstance(canvas, tk.Canvas):
            return
        viewport_h = canvas.winfo_height()
        if viewport_h < 2:
            return
        length = int(max(
            ENV_ADSR_VFADER_MIN,
            min(ENV_ADSR_VFADER_MAX, viewport_h * ENV_ADSR_VIEWPORT_FRAC),
        ))
        if length == self._env_fader_length:
            return
        self._env_fader_length = length
        for scale in scales:
            scale.configure(length=length)

    def _add_adsr_block_vertical(self, parent: ttk.Frame, block: params.Block, column: int) -> None:
        frame = ttk.LabelFrame(parent, text=block.label, padding=8)
        frame.grid(row=0, column=column, sticky="ew", padx=(0, 8 if column < 2 else 0))
        parent.columnconfigure(column, weight=1, uniform="env")

        self.block_vars[block.key] = {}
        for i, f in enumerate(block.fields):
            cell = ttk.Frame(frame)
            cell.grid(row=0, column=i, padx=4, sticky="new")
            cell.columnconfigure(0, weight=1)
            frame.columnconfigure(i, weight=1)
            ttk.Label(cell, text=f.label).grid(row=0, column=0)
            var = tk.DoubleVar(value=f.default)
            self.block_vars[block.key][f.key] = var
            readout = ttk.Label(cell, text=str(f.default), style="Readout.TLabel")

            def on_slide(_v, key=block.key, var=var, rd=readout):
                rd.config(text=str(ivar(var)))
                if self._preset_loading:
                    return
                self.queue_block(key)

            scale = _make_scale(
                cell, lo=f.lo, hi=f.hi, var=var, command=on_slide,
                orient="vertical", length=ENV_ADSR_VFADER_MIN,
            )
            scale.grid(row=1, column=0, pady=4)
            readout.grid(row=2, column=0)
            self._env_scales.append(scale)
            self._readouts[("b", block.key, f.key)] = readout

    def _add_env_curve_spin(self, cell: ttk.Frame, p: params.Param, short_label: str) -> None:
        cell.columnconfigure(0, weight=1)
        ttk.Label(cell, text=short_label).grid(row=0, column=0)
        labels = [c[0] for c in p.choices]
        lookup = dict(p.choices)
        var = tk.StringVar(value=next((c[0] for c in p.choices if c[1] == p.default), labels[0]))
        self.param_vars[p.pid] = var

        def on_pick(_e=None, pid=p.pid, var=var, lookup=lookup):
            if self._preset_loading:
                return
            self.queue_param(pid, lookup[var.get()])

        combo = ttk.Combobox(cell, textvariable=var, values=labels, state="readonly",
                             width=18)
        combo.grid(row=1, column=0, pady=4, sticky="ew")
        combo.bind("<<ComboboxSelected>>", on_pick)
        tip = f"{p.label}  [{p.pid}]"
        if p.note:
            tip += f"\n{p.note}"
        self._tooltip(combo, tip)

    def _add_env_curve_column(self, parent: ttk.Frame, column: int, title: str,
                              attack_pid: int | None, decay_pid: int | None,
                              restart_pid: int | None, *, row: int = 1) -> None:
        frame = ttk.LabelFrame(parent, text=title, padding=8)
        frame.grid(row=row, column=column, sticky="ew", padx=(0, 8 if column < 2 else 0),
                   pady=(8, 0) if row else 0)
        parent.columnconfigure(column, weight=1, uniform="env")

        if attack_pid is not None and decay_pid is not None:
            for i, (pid, label) in enumerate(((attack_pid, "Attack"), (decay_pid, "Decay"))):
                cell = ttk.Frame(frame)
                cell.grid(row=0, column=i, padx=4, sticky="new")
                frame.columnconfigure(i, weight=1)
                self._add_env_curve_spin(cell, PARAM_BY_PID[pid], label)

        if restart_pid is not None:
            rp = PARAM_BY_PID[restart_pid]
            restart_cell = ttk.Frame(frame)
            restart_cell.grid(row=1, column=0, columnspan=2, sticky="w", pady=(4, 0))
            ttk.Label(restart_cell, text="Restart").grid(row=0, column=0, padx=(0, 4))
            var = tk.IntVar(value=rp.default)
            self.param_vars[rp.pid] = var

            def on_check(pid=rp.pid, var=var):
                if self._preset_loading:
                    return
                self.queue_param(pid, ivar(var))

            btn = ttk.Checkbutton(restart_cell, variable=var, command=on_check,
                                  style=theme.check_style())
            btn.grid(row=0, column=1)
            tip = f"{rp.label}  [{rp.pid}]"
            if rp.note:
                tip += f"\n{rp.note}"
            self._tooltip(btn, tip)

    def _build_env_curves_row(self, times: ttk.Frame, row: int = 1) -> None:
        for col, (title, attack_pid, decay_pid, restart_pid) in enumerate(ENV_CURVE_COLUMNS):
            self._add_env_curve_column(times, col, title, attack_pid, decay_pid, restart_pid, row=row)

    def _add_osc_section(self, parent: ttk.Frame, row: int, title: str,
                         pids: tuple[int, ...], *, columnspan: int = 3) -> int:
        frame = ttk.LabelFrame(parent, text=title, padding=8)
        frame.grid(row=row, column=0, columnspan=columnspan, sticky="ew", pady=(4, 10))
        frame.columnconfigure(1, weight=1)
        subrow = 0
        for pid in pids:
            subrow = self._add_param(frame, PARAM_BY_PID[pid], subrow)
        return row + 1

    def _add_osc_levels_row(self, parent: ttk.Frame, row: int, pids: tuple[int, ...]) -> int:
        frame = ttk.LabelFrame(parent, text="Levels", padding=8)
        frame.grid(row=row, column=0, sticky="ew", pady=(4, 10))
        self._osc_levels_frame = frame
        self._osc_level_cells = []
        for pid in pids:
            cell = ttk.Frame(frame)
            self._osc_level_cells.append(cell)
            self._add_osc_level_cell(cell, PARAM_BY_PID[pid])
        return row + 1

    def _add_osc_level_cell(self, cell: ttk.Frame, p: params.Param) -> None:
        ttk.Label(cell, text=p.label).grid(row=0, column=0, columnspan=2, sticky="w")
        var = tk.DoubleVar(value=p.default)
        self.param_vars[p.pid] = var
        readout = ttk.Label(cell, width=4, text=str(p.default), anchor="e",
                            style="Readout.TLabel")

        def on_slide(_v, pid=p.pid, var=var, rd=readout):
            rd.config(text=str(ivar(var)))
            if self._preset_loading:
                return
            self.queue_param(pid, ivar(var))

        _make_scale(cell, lo=p.lo, hi=p.hi, var=var, command=on_slide).grid(
            row=1, column=0, sticky="ew", pady=4)
        readout.grid(row=1, column=1, sticky="e", padx=(4, 0))
        cell.columnconfigure(0, weight=1)
        self._readouts[("p", p.pid)] = readout

    def _add_osc_wave_matrix(self, parent: ttk.Frame, row: int) -> None:
        frame = ttk.LabelFrame(parent, text="Waveforms", padding=8)
        frame.grid(row=row, column=0, sticky="ew", pady=(4, 10))
        for col, title in enumerate(OSC_WAVE_COLS, start=1):
            ttk.Label(frame, text=title).grid(row=0, column=col, padx=12, pady=(0, 4))
        for r, (osc_label, pids) in enumerate(OSC_WAVE_MATRIX, start=1):
            ttk.Label(frame, text=osc_label).grid(row=r, column=0, sticky="e", padx=(0, 12))
            for c, pid in enumerate(pids, start=1):
                self._wire_check(frame, PARAM_BY_PID[pid], row=r, column=c)

    def _wire_check(self, parent: ttk.Frame, p: params.Param, *, row: int, column: int) -> None:
        var = tk.IntVar(value=p.default)
        self.param_vars[p.pid] = var

        def on_check(pid=p.pid, var=var):
            if self._preset_loading:
                return
            self.queue_param(pid, ivar(var))

        btn = ttk.Checkbutton(parent, variable=var, command=on_check,
                              style=theme.check_style())
        btn.grid(row=row, column=column, padx=12, pady=2)
        tip = f"{p.label}  [{p.pid}]"
        if p.note:
            tip += f"\n{p.note}"
        self._tooltip(btn, tip)

    def _add_pio_pulse_slider(self, parent: ttk.Frame, row: int) -> int:
        """Calibration-only: PARAM_DEBUG_COMMAND 160 values 200..50000 set pioPulseLength."""
        row_pad = 8
        lo, hi, default = params.PIO_PULSE_LO, params.PIO_PULSE_HI, params.PIO_PULSE_DEFAULT
        label = ttk.Label(parent, text=f"PIO pulse length (Y)  [{params.DEBUG_PARAM_ID}]")
        label.grid(row=row, column=0, sticky="w", pady=row_pad, padx=(0, 10))
        self._tooltip(
            label,
            "Reset pulse width in system clock cycles. Sent as unsigned 16-bit on "
            "PARAM_DEBUG_COMMAND 160 (values 200–50000). Reloads running SMs via defer "
            "reset (audible while a note is held); large changes may need amp-comp redo.",
        )
        var = tk.DoubleVar(value=default)
        self.pio_pulse_var = var
        readout = ttk.Label(parent, width=6, text=str(default), anchor="e",
                            style="Readout.TLabel")
        self._readouts[("pio_pulse",)] = readout

        def on_slide(_v, var=var, rd=readout):
            rd.config(text=str(ivar(var)))
            if self._preset_loading:
                return
            self.queue_debug_u16(ivar(var))

        _make_scale(parent, lo=lo, hi=hi, var=var, command=on_slide).grid(
            row=row, column=1, sticky="ew", pady=row_pad)
        readout.grid(row=row, column=2, sticky="e", padx=(8, 0))
        return row + 1

    def _add_character_jitter_sliders(self, parent: ttk.Frame, row: int) -> int:
        """Character tab: diagnostic jitter amounts via packed PARAM_DEBUG_COMMAND 160."""
        row_pad = 8
        lo = params.CHARACTER_JITTER_LO
        hi = params.CHARACTER_JITTER_HI
        default = params.CHARACTER_JITTER_DEFAULT
        for label_text, type_hi in params.CHARACTER_JITTERS:
            label = ttk.Label(
                parent, text=f"{label_text}  [{params.DEBUG_PARAM_ID}:0x{type_hi:02X}xx]")
            label.grid(row=row, column=0, sticky="w", pady=row_pad, padx=(0, 10))
            self._tooltip(
                label,
                f"Diagnostic only (not a ParamId). Sent as unsigned 16-bit on "
                f"PARAM_DEBUG_COMMAND 160: (0x{type_hi:02X} << 8) | amount, amount 0..128. "
                f"Stored in firmware; not applied to DSP yet.",
            )
            var = tk.DoubleVar(value=default)
            self.character_jitter_vars[type_hi] = var
            readout = ttk.Label(parent, width=6, text=str(default), anchor="e",
                                style="Readout.TLabel")
            self._readouts[("char_jitter", type_hi)] = readout

            def on_slide(_v, var=var, rd=readout, type_hi=type_hi):
                amount = ivar(var)
                rd.config(text=str(amount))
                if self._preset_loading:
                    return
                self.queue_debug_u16((type_hi << 8) | amount)

            _make_scale(parent, lo=lo, hi=hi, var=var, command=on_slide).grid(
                row=row, column=1, sticky="ew", pady=row_pad)
            readout.grid(row=row, column=2, sticky="e", padx=(8, 0))
            row += 1
        return row

    def _add_param(self, parent: ttk.Frame, p: params.Param, row: int) -> int:
        row_pad = 8
        label = ttk.Label(parent, text=f"{p.label}  [{p.pid}]")
        label.grid(row=row, column=0, sticky="w", pady=row_pad, padx=(0, 10))
        if p.note:
            self._tooltip(label, p.note)

        if p.kind == "slider":
            var = tk.DoubleVar(value=p.default)
            self.param_vars[p.pid] = var
            readout = ttk.Label(parent, width=6, text=str(p.default), anchor="e",
                                style="Readout.TLabel")

            def on_slide(_v, pid=p.pid, var=var, rd=readout):
                rd.config(text=str(ivar(var)))
                if self._preset_loading:
                    return
                self.queue_param(pid, ivar(var))

            _make_scale(parent, lo=p.lo, hi=p.hi, var=var, command=on_slide).grid(
                row=row, column=1, sticky="ew", pady=row_pad)
            readout.grid(row=row, column=2, sticky="e", padx=(8, 0))
            self._readouts[("p", p.pid)] = readout

        elif p.kind == "combo":
            labels = [c[0] for c in p.choices]
            lookup = dict(p.choices)
            var = tk.StringVar(value=next((c[0] for c in p.choices if c[1] == p.default), labels[0]))
            self.param_vars[p.pid] = var
            combo = ttk.Combobox(parent, textvariable=var, values=labels, state="readonly")
            combo.grid(row=row, column=1, sticky="ew", pady=row_pad)

            def on_pick(_e=None, pid=p.pid, var=var, lookup=lookup):
                if self._preset_loading:
                    return
                self.queue_param(pid, lookup[var.get()])

            combo.bind("<<ComboboxSelected>>", on_pick)

        elif p.kind == "check":
            self._wire_check(parent, p, row=row, column=1)

        elif p.kind == "pulse":
            def on_pulse(pid=p.pid, value=p.pulse_value, label=p.label):
                self.send_now(protocol.param16(pid, value))
                self.log(f"[send] {label} (param {pid} = {value})\n")

            ttk.Button(parent, text="Send", command=on_pulse).grid(
                row=row, column=1, sticky="w", pady=row_pad)

        return row + 1

    def _add_block(self, parent: ttk.Frame, block: params.Block, row: int,
                   *, columnspan: int = 3) -> int:
        frame = ttk.LabelFrame(parent, text=block.label, padding=8)
        frame.grid(row=row, column=0, columnspan=columnspan, sticky="ew", pady=(4, 10))
        frame.columnconfigure(1, weight=1)

        self.block_vars[block.key] = {}
        for i, f in enumerate(block.fields):
            ttk.Label(frame, text=f.label).grid(row=i, column=0, sticky="w", padx=(0, 10), pady=6)
            var = tk.DoubleVar(value=f.default)
            self.block_vars[block.key][f.key] = var
            readout = ttk.Label(frame, width=6, text=str(f.default), anchor="e",
                                style="Readout.TLabel")

            def on_slide(_v, key=block.key, var=var, rd=readout):
                rd.config(text=str(ivar(var)))
                if self._preset_loading:
                    return
                self.queue_block(key)

            _make_scale(frame, lo=f.lo, hi=f.hi, var=var, command=on_slide).grid(
                row=i, column=1, sticky="ew", pady=6)
            readout.grid(row=i, column=2, sticky="e", padx=(8, 0))
            self._readouts[("b", block.key, f.key)] = readout

        if block.note:
            ttk.Label(frame, text=block.note, wraplength=700, style="Muted.TLabel").grid(
                row=len(block.fields), column=0, columnspan=3, sticky="w", pady=(6, 0))
        return row + 1

    def _add_diag_panel(self, parent: ttk.Frame, *, row: int, column: int, title: str,
                        commands: tuple[tuple[str, int], ...], note: str) -> ttk.LabelFrame:
        frame = ttk.LabelFrame(parent, text=title, padding=DIAG_FRAME_PAD)
        frame.grid(row=row, column=column, sticky="ew", pady=DIAG_SECTION_PADY)
        buttons: list[ttk.Button] = []
        for label, value in commands:
            def on_click(value=value, label=label):
                self.send_now(protocol.param16(params.DEBUG_PARAM_ID, value))
                self.log(f"[send] {label}\n")

            buttons.append(ttk.Button(frame, text=label, command=on_click))
        frame._diag_buttons = buttons  # type: ignore[attr-defined]
        note_label = ttk.Label(frame, text=note, wraplength=340, style="Muted.TLabel")
        frame._diag_note_label = note_label  # type: ignore[attr-defined]
        return frame

    def _diag_panel_ncol(self, panel_w: int, n_buttons: int, stacked: bool) -> int:
        if n_buttons <= 1:
            return 1
        ncol = 2
        if stacked and panel_w >= 640:
            ncol = 3
        elif not stacked and panel_w >= 400:
            ncol = 3
        return min(ncol, n_buttons)

    def _diag_reflow_panel(self, frame: ttk.LabelFrame, panel_w: int, stacked: bool,
                           wrap: int) -> None:
        buttons: list[ttk.Button] = frame._diag_buttons  # type: ignore[attr-defined]
        ncol = self._diag_panel_ncol(panel_w, len(buttons), stacked)
        for c in range(max(ncol, 1)):
            frame.columnconfigure(c, weight=1)
        for i, btn in enumerate(buttons):
            btn.grid(row=i // ncol, column=i % ncol, padx=2, pady=1, sticky="ew")
        btn_rows = (len(buttons) - 1) // ncol + 1 if buttons else 0
        note_label: ttk.Label = frame._diag_note_label  # type: ignore[attr-defined]
        note_label.config(wraplength=wrap)
        note_label.grid(row=btn_rows, column=0, columnspan=ncol, sticky="ew", pady=(4, 0))

    def _build_diag_tab(self, parent: ttk.Frame) -> None:
        parent.columnconfigure(0, weight=1)
        parent.columnconfigure(1, weight=1)
        self._diag_grid = parent
        self._diag_reflow_width = -1

        panel_specs = (
            ("Diagnostics", params.DEBUG_COMMANDS,
             "Output appears in the log below. Period probes only hold while no note "
             "is playing. Dump RAM (13) prints heap + per-core stack (needs ENABLE_MEM_DIAG). "
             "14/15 disable/enable mem_diag loop polls (A/B vs profiler). "
             "Note retrig 26/27 A/B EXACT_Y vs SYNC_JMP (needs oscSync ≥ 1); "
             "Board ack note_retrig=… when RUNNING_AVERAGE.", 0, 0),
            ("Hot-path profiler", params.BENCH_COMMANDS,
             "Needs RUNNING_AVERAGE in the firmware (otherwise these are no-ops). "
             "Dump is asynchronous: core 0 prints after both cores snapshot. "
             "Toggle prints 'bench periodic on/off' immediately; the tables land "
             "in the Board output pane (drag the sash to enlarge it).", 0, 1),
            ("Amp-comp method / bench", params.AMP_COMP_COMMANDS,
             "Method buttons (20–22) switch the live lookup for profiler / speed A/B "
             "(PWM difference is tiny — expect Board ack amp_comp method=…). "
             "FLOAT_QUAD=cached walk. "
             "Speed/accuracy (24–25) need AMP_COMP_BENCHMARK + "
             "RUNNING_AVERAGE; confirm live_method= on the speed report.",
             1, 0),
            ("Pitch-interp bench", params.PITCH_INTERP_COMMANDS,
             "Speed/accuracy (28–29) need RUNNING_AVERAGE (paced Board output). "
             "Compares FLOAT / RATIO_Q16 / Q12 (private tables; Q20 slope is inside RATIO). "
             "Profiler dump (10) prints build: … pitch=… flags.", 1, 1),
            ("Clkdiv GOLD_REF / LIVE / FLOAT / Q16 / Q8 / Q4", params.CLKDIV_HP_COMMANDS,
             "Speed/accuracy (32–33) need RUNNING_AVERAGE (paced Board output). "
             "All seven methods on both voice engines. GOLD_REF = true-Hz llround (speed 100%). "
             "Fixed: Q24 helpers. Float: native Hz for GOLD/FLOAT; Hz→Q24 then helper for integer. "
             "Accuracy vs GOLD_REF. Live float honors CLKDIV_MODE via clkdiv_live_hz.",
             2, 0),
        )
        self._diag_panels = []
        for title, commands, note, grid_row, grid_col in panel_specs:
            frame = self._add_diag_panel(
                parent, row=grid_row, column=grid_col, title=title,
                commands=commands, note=note,
            )
            self._diag_panels.append((frame, grid_row, grid_col))

        parent.bind("<Configure>", self._diag_layout_reflow, add="+")
        parent.after_idle(self._diag_layout_reflow)

    def _diag_layout_reflow(self, _event=None) -> None:
        grid = getattr(self, "_diag_grid", None)
        panels = getattr(self, "_diag_panels", None)
        if grid is None or not panels:
            return
        width = grid.winfo_width()
        if width < 2 or width == self._diag_reflow_width:
            return
        self._diag_reflow_width = width
        stacked = width < DIAG_SPLIT_STACK_WIDTH
        panel_w = width if stacked else max(200, width // 2 - 16)
        wrap = 700 if stacked else max(280, width // 2 - 24)
        for i, (frame, wide_row, wide_col) in enumerate(panels):
            if stacked:
                frame.grid(row=i, column=0, columnspan=2, sticky="ew", pady=DIAG_SECTION_PADY)
            else:
                frame.grid(row=wide_row, column=wide_col, columnspan=1, sticky="ew",
                           padx=(0, 4 if wide_col == 0 else 0), pady=DIAG_SECTION_PADY)
            self._diag_reflow_panel(frame, panel_w, stacked, wrap)

    def _tooltip(self, widget: tk.Widget, text: str) -> None:
        state: dict[str, tk.Toplevel | None] = {"win": None}

        def show(_e):
            if state["win"] is not None:
                return
            # Created on hover, so the startup retint() cannot reach it: colour it here.
            p = theme.palette()
            win = tk.Toplevel(widget)
            win.wm_overrideredirect(True)
            win.wm_geometry(f"+{widget.winfo_rootx() + 20}+{widget.winfo_rooty() + widget.winfo_height() + 4}")
            win.configure(background=p["border"])
            tk.Label(win, text=text, justify="left", wraplength=420, padx=8, pady=5,
                     background=p["surface"], foreground=p["fg"],
                     relief="flat", borderwidth=0).pack(padx=1, pady=1)
            state["win"] = win

        def hide(_e):
            if state["win"] is not None:
                state["win"].destroy()
                state["win"] = None

        widget.bind("<Enter>", show)
        widget.bind("<Leave>", hide)

    # --- log -------------------------------------------------------------

    def _build_log(self) -> None:
        frame = ttk.LabelFrame(self.paned, text="Board output", padding=6)
        self.paned.add(frame, weight=1)
        self.log_text = tk.Text(frame, height=6, wrap="none")
        vbar = ttk.Scrollbar(frame, orient="vertical", command=self.log_text.yview)
        hbar = ttk.Scrollbar(frame, orient="horizontal", command=self.log_text.xview)
        self.log_text.configure(yscrollcommand=vbar.set, xscrollcommand=hbar.set)
        self.log_text.grid(row=0, column=0, sticky="nsew")
        vbar.grid(row=0, column=1, sticky="ns")
        hbar.grid(row=1, column=0, sticky="ew")
        ttk.Button(frame, text="Clear", command=lambda: self.log_text.delete("1.0", "end")).grid(
            row=2, column=0, columnspan=2, sticky="e", pady=(4, 0))
        frame.columnconfigure(0, weight=1)
        frame.rowconfigure(0, weight=1)

        def on_log_wheel(event):
            if event.num == 4:
                self.log_text.yview_scroll(-2, "units")
            elif event.num == 5:
                self.log_text.yview_scroll(2, "units")
            elif event.delta:
                self.log_text.yview_scroll(int(-1 * (event.delta / 120)), "units")
            return "break"

        for seq in ("<Button-4>", "<Button-5>", "<MouseWheel>"):
            self.log_text.bind(seq, on_log_wheel)

    def _style_log_tags(self) -> None:
        p = theme.palette()
        self.log_text.tag_configure("link", foreground=p["accent"])
        self.log_text.tag_configure("send", foreground=p["muted"])
        self.log_text.tag_configure("ui", foreground=p["muted"])

    def log(self, text: str) -> None:
        # Tag our own lines so they read as commentary, leaving board output plain.
        tag = ""
        for name in ("link", "send", "ui"):
            if text.startswith(f"[{name}]"):
                tag = name
                break
        self.log_text.insert("end", text, tag or ())
        # Keep the buffer bounded; DCO_DEBUG_REPORT is chatty.
        if int(self.log_text.index("end-1c").split(".")[0]) > 2000:
            self.log_text.delete("1.0", "500.0")
        self.log_text.see("end")

    def _drain_log(self) -> None:
        try:
            while True:
                self.log(self.link.rx.get_nowait())
        except queue.Empty:
            pass
        if self.dot_on and not self.link.is_open:
            # A failed write closes the link from the reader thread's side.
            self._set_status_dot(False)
            self.status_var.set("not connected")
            self.connect_btn.config(text="Connect")
        self.root.after(50, self._drain_log)

    # --- sending ---------------------------------------------------------

    def _enqueue(self, key: str, frame: bytes) -> None:
        """Stage a frame; last write for the same key wins until the next flush."""
        self.pending[key] = protocol.stuff(frame)

    def queue_param(self, pid: int, value: int) -> None:
        """Stage a 16-bit parameter frame (coalesced by the 20 ms flush)."""
        self._enqueue(f"p{pid}", protocol.param16(pid, int(value)))

    def queue_debug_u16(self, value: int) -> None:
        """Stage unsigned 16-bit PARAM_DEBUG_COMMAND (e.g. pioPulseLength)."""
        self._enqueue(
            "p_debug_u16", protocol.param16u(params.DEBUG_PARAM_ID, int(value)))

    def queue_block(self, key: str) -> None:
        block = self.blocks_by_key[key]
        values = {k: ivar(v) for k, v in self.block_vars[key].items()}
        self._enqueue(f"b{key}", block.builder(values))

    def send_now(self, frame: bytes) -> None:
        if not self.link.is_open:
            self.log("[link] not connected\n")
            return
        self.link.send(protocol.stuff(frame))

    def _flush_pending(self) -> int:
        """Send staged frames if connected. Clears only after a successful send."""
        if not self.pending or not self.link.is_open:
            return 0
        frames = list(self.pending.values())
        self.link.send(b"".join(frames))
        n = len(frames)
        self.pending.clear()
        return n

    def _flush(self) -> None:
        """Periodic coalesce tick: deliver pending when linked; keep staging offline."""
        self._flush_pending()
        self.root.after(SEND_INTERVAL_MS, self._flush)

    def send_all(self) -> int:
        """Push the sound patch once. Returns the number of frames sent.

        Calibration, Diagnostics, and bench/debug controls are omitted — those send
        only when the user operates them. Needed after connecting: the board boots
        with its own defaults and has no idea what this window is showing.
        """
        if not self.link.is_open:
            self.log("[link] not connected\n")
            return 0
        # UI is authoritative; drop any stale offline queue before the full push.
        self.pending.clear()
        n = 0
        for p in presets.patch_params():
            self.queue_param(p.pid, self._param_value(p))
            n += 1
        for block in presets.patch_blocks():
            self.queue_block(block.key)
            n += 1
        self._flush_pending()
        self.log(f"[send] patch {n} frames\n")
        return n

    def _param_value(self, p: params.Param) -> int:
        var = self.param_vars.get(p.pid)
        if var is None:
            return p.default
        if p.kind == "combo":
            return dict(p.choices)[var.get()]
        return ivar(var)

    def reset_defaults(self) -> None:
        self._preset_loading = True
        try:
            for p in params.PARAMS:
                var = self.param_vars.get(p.pid)
                if var is None:
                    continue
                if p.kind == "combo":
                    var.set(next(c[0] for c in p.choices if c[1] == p.default))
                else:
                    var.set(p.default)
            for block in params.BLOCKS:
                for f in block.fields:
                    self.block_vars[block.key][f.key].set(f.default)
            if self.pio_pulse_var is not None:
                self.pio_pulse_var.set(params.PIO_PULSE_DEFAULT)
            for var in self.character_jitter_vars.values():
                var.set(params.CHARACTER_JITTER_DEFAULT)
        finally:
            self._preset_loading = False
        self._sync_readouts()
        self._refresh_dirty()
        self.log("[ui] controls reset to defaults -- press 'Send all' to push them\n")

    def _on_close(self) -> None:
        self.link.close()
        self.root.destroy()


def main() -> None:
    ap = argparse.ArgumentParser(description="DCO bench controller")
    ap.add_argument("--port", help="serial device, e.g. /dev/ttyACM0 (default: auto-detect)")
    ap.add_argument("--theme", choices=theme.MODES, default="dark",
                    help="colour scheme; also switchable from the toolbar (default: dark)")
    ap.add_argument("--cobs", action="store_true",
                    help="COBS on-wire framing (match firmware SERIAL_FRAMING_COBS)")
    args = ap.parse_args()

    env_cobs = os.environ.get("DCO_SERIAL_COBS", "").strip().lower() in ("1", "true", "yes")
    cobs = args.cobs or env_cobs

    root = tk.Tk()
    App(root, args.port, args.theme, cobs=cobs)
    root.mainloop()


if __name__ == "__main__":
    main()
