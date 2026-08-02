#!/usr/bin/env python3
"""GUI desktop ringan untuk Pekalongan Memory Editor."""

from __future__ import annotations

import argparse
import os
import queue
import shlex
import subprocess
import sys
import threading
import tkinter as tk
from pathlib import Path
from tkinter import filedialog, messagebox, ttk


class EditorProcess:
    def __init__(self, executable: Path, output: queue.Queue[str]) -> None:
        creation_flags = 0
        if os.name == "nt":
            creation_flags = subprocess.CREATE_NO_WINDOW
        self._output = output
        self._process = subprocess.Popen(
            [str(executable)],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            bufsize=0,
            creationflags=creation_flags,
        )
        threading.Thread(target=self._read_output, daemon=True).start()

    def _read_output(self) -> None:
        assert self._process.stdout is not None
        while True:
            chunk = self._process.stdout.read(1)
            if not chunk:
                break
            self._output.put(chunk)
        self._output.put("\n[engine berhenti]\n")

    def send(self, command: str) -> None:
        if self._process.poll() is not None:
            raise RuntimeError("engine memory editor sudah berhenti")
        assert self._process.stdin is not None
        self._process.stdin.write(command + "\n")
        self._process.stdin.flush()

    def close(self) -> None:
        if self._process.poll() is None:
            try:
                self.send("quit")
                self._process.wait(timeout=1)
            except (RuntimeError, subprocess.TimeoutExpired):
                self._process.terminate()


class MemoryEditorGui(tk.Tk):
    def __init__(self, executable: Path) -> None:
        super().__init__()
        self.title("Pekalongan Memory Editor")
        self.geometry("1080x720")
        self.minsize(900, 620)
        self.configure(bg="#10141c")

        self._queue: queue.Queue[str] = queue.Queue()
        try:
            self._engine = EditorProcess(executable, self._queue)
        except OSError as exc:
            messagebox.showerror("Engine gagal dijalankan", str(exc))
            self.destroy()
            raise

        self._configure_style()
        self._build_layout()
        self.after(30, self._drain_output)
        self.protocol("WM_DELETE_WINDOW", self._on_close)

    def _configure_style(self) -> None:
        style = ttk.Style(self)
        style.theme_use("clam")
        style.configure("TFrame", background="#10141c")
        style.configure("Panel.TFrame", background="#18202c")
        style.configure(
            "TLabel", background="#18202c", foreground="#dce6f2", font=("Avenir Next", 10)
        )
        style.configure(
            "Title.TLabel",
            background="#10141c",
            foreground="#62e6a7",
            font=("Avenir Next", 19, "bold"),
        )
        style.configure(
            "TButton",
            background="#27364a",
            foreground="#f2f6fa",
            borderwidth=0,
            padding=(10, 7),
        )
        style.map("TButton", background=[("active", "#35516e")])
        style.configure(
            "Accent.TButton", background="#17a673", foreground="#ffffff"
        )
        style.map("Accent.TButton", background=[("active", "#21c087")])
        style.configure(
            "TEntry", fieldbackground="#0f1520", foreground="#ffffff", insertcolor="#ffffff"
        )
        style.configure(
            "TCombobox", fieldbackground="#0f1520", foreground="#ffffff"
        )
        style.configure(
            "TNotebook",
            background="#18202c",
            borderwidth=0,
            tabmargins=(0, 0, 0, 8),
        )
        style.configure(
            "TNotebook.Tab",
            background="#202b3a",
            foreground="#9aabc0",
            padding=(13, 8),
            borderwidth=0,
        )
        style.map(
            "TNotebook.Tab",
            background=[("selected", "#17a673"), ("active", "#2d4055")],
            foreground=[("selected", "#ffffff"), ("active", "#ffffff")],
        )

    def _build_layout(self) -> None:
        header = ttk.Frame(self)
        header.pack(fill="x", padx=18, pady=(16, 10))
        ttk.Label(header, text="Pekalongan Memory Editor", style="Title.TLabel").pack(
            side="left"
        )
        ttk.Label(
            header,
            text="engine C++  |  Linux x64 / Windows x64",
            background="#10141c",
            foreground="#8494a8",
        ).pack(side="right", pady=7)

        body = ttk.Frame(self)
        body.pack(fill="both", expand=True, padx=18, pady=(0, 16))
        body.columnconfigure(1, weight=1)
        body.rowconfigure(0, weight=1)

        controls = ttk.Frame(body, style="Panel.TFrame", padding=10, width=350)
        controls.grid(row=0, column=0, sticky="ns", padx=(0, 12))
        controls.grid_propagate(False)
        console_frame = ttk.Frame(body, style="Panel.TFrame", padding=10)
        console_frame.grid(row=0, column=1, sticky="nsew")
        console_frame.columnconfigure(0, weight=1)
        console_frame.rowconfigure(0, weight=1)

        notebook = ttk.Notebook(controls)
        notebook.pack(fill="both", expand=True)
        scan_tab = ttk.Frame(notebook, style="Panel.TFrame", padding=10)
        tools_tab = ttk.Frame(notebook, style="Panel.TFrame", padding=10)
        notebook.add(scan_tab, text="SCAN")
        notebook.add(tools_tab, text="POINTER & TOOLS")

        self._build_target_controls(scan_tab)
        ttk.Separator(scan_tab).pack(fill="x", pady=12)
        self._build_scan_controls(scan_tab)

        self._build_pointer_controls(tools_tab)
        ttk.Separator(tools_tab).pack(fill="x", pady=12)
        self._build_memory_controls(tools_tab)
        ttk.Separator(tools_tab).pack(fill="x", pady=12)
        self._build_config_controls(tools_tab)

        self.console = tk.Text(
            console_frame,
            bg="#090d13",
            fg="#c9d7e6",
            insertbackground="#ffffff",
            selectbackground="#225b70",
            font=("Courier New", 10),
            relief="flat",
            wrap="word",
        )
        self.console.grid(row=0, column=0, sticky="nsew")
        scrollbar = ttk.Scrollbar(console_frame, command=self.console.yview)
        scrollbar.grid(row=0, column=1, sticky="ns")
        self.console.configure(yscrollcommand=scrollbar.set)

        command_row = ttk.Frame(console_frame, style="Panel.TFrame")
        command_row.grid(row=1, column=0, columnspan=2, sticky="ew", pady=(9, 0))
        command_row.columnconfigure(0, weight=1)
        self.command = ttk.Entry(command_row)
        self.command.grid(row=0, column=0, sticky="ew", padx=(0, 8))
        self.command.bind("<Return>", lambda _event: self._send_manual())
        ttk.Button(command_row, text="Kirim command", command=self._send_manual).grid(
            row=0, column=1
        )

    def _build_target_controls(self, parent: ttk.Frame) -> None:
        ttk.Label(parent, text="TARGET PROCESS", foreground="#62e6a7").pack(anchor="w")
        row = ttk.Frame(parent, style="Panel.TFrame")
        row.pack(fill="x", pady=(8, 5))
        self.pid = ttk.Entry(row, width=15)
        self.pid.pack(side="left", padx=(0, 6))
        ttk.Button(row, text="Attach PID", command=self._attach).pack(side="left")
        ttk.Button(parent, text="Refresh process list", command=lambda: self._send("ps")).pack(
            fill="x", pady=4
        )
        ttk.Button(parent, text="Cari point_blank", command=lambda: self._send("ps point_blank")).pack(
            fill="x", pady=4
        )
        ttk.Button(parent, text="Launch executable", command=self._launch).pack(
            fill="x", pady=4
        )

    def _build_scan_controls(self, parent: ttk.Frame) -> None:
        ttk.Label(parent, text="SCAN", foreground="#62e6a7").pack(anchor="w")
        row = ttk.Frame(parent, style="Panel.TFrame")
        row.pack(fill="x", pady=(8, 5))
        self.value_type = ttk.Combobox(
            row,
            values=("i8", "u8", "i16", "u16", "i32", "u32", "i64", "u64", "f32", "f64"),
            state="readonly",
            width=7,
        )
        self.value_type.set("i32")
        self.value_type.pack(side="left", padx=(0, 6))
        ttk.Button(row, text="Set type", command=self._set_type).pack(side="left")

        self.scan_value = ttk.Entry(parent)
        self.scan_value.pack(fill="x", pady=4)
        self.scan_value.insert(0, "100")
        ttk.Button(
            parent, text="First Scan exact", style="Accent.TButton", command=self._first_exact
        ).pack(fill="x", pady=4)
        ttk.Button(parent, text="First Scan unknown", command=lambda: self._send("scan unknown")).pack(
            fill="x", pady=4
        )
        ttk.Button(parent, text="Next Scan exact", command=self._next_exact).pack(
            fill="x", pady=4
        )

        next_row = ttk.Frame(parent, style="Panel.TFrame")
        next_row.pack(fill="x", pady=4)
        self.next_mode = ttk.Combobox(
            next_row,
            values=("changed", "unchanged", "increased", "decreased"),
            state="readonly",
            width=12,
        )
        self.next_mode.set("changed")
        self.next_mode.pack(side="left", padx=(0, 6))
        ttk.Button(next_row, text="Next", command=self._next_relative).pack(side="left")
        ttk.Button(parent, text="Show results", command=lambda: self._send("results 100")).pack(
            fill="x", pady=4
        )
        ttk.Button(parent, text="Reset scan", command=lambda: self._send("reset")).pack(
            fill="x", pady=4
        )

        ttk.Label(parent, text="THREADS / SIGNATURE", foreground="#62e6a7").pack(
            anchor="w", pady=(8, 0)
        )
        thread_row = ttk.Frame(parent, style="Panel.TFrame")
        thread_row.pack(fill="x", pady=(8, 4))
        self.thread_count = ttk.Combobox(
            thread_row,
            values=("auto", "1", "2", "4", "8"),
            state="readonly",
            width=10,
        )
        self.thread_count.set("auto")
        self.thread_count.pack(side="left", padx=(0, 6))
        ttk.Button(thread_row, text="Set Threads", command=self._set_threads).pack(side="left")
        ttk.Button(
            parent,
            text="Benchmark 1 thread vs auto",
            command=self._benchmark_exact,
        ).pack(fill="x", pady=4)

        self.signature_pattern = ttk.Entry(parent)
        self.signature_pattern.pack(fill="x", pady=4)
        self.signature_pattern.insert(0, "B2 57 ?? 02")
        ttk.Button(
            parent, text="Signature Scan", command=self._signature_scan
        ).pack(fill="x", pady=4)

    def _build_pointer_controls(self, parent: ttk.Frame) -> None:
        ttk.Label(parent, text="POINTER CHAIN", foreground="#62e6a7").pack(anchor="w")
        self.pointer_address = ttk.Entry(parent)
        self.pointer_address.pack(fill="x", pady=(8, 4))
        self.pointer_address.insert(0, "#0")

        pointer_row = ttk.Frame(parent, style="Panel.TFrame")
        pointer_row.pack(fill="x", pady=4)
        self.pointer_depth = ttk.Entry(pointer_row, width=6)
        self.pointer_depth.pack(side="left", padx=(0, 6))
        self.pointer_depth.insert(0, "3")
        self.pointer_offset = ttk.Entry(pointer_row, width=8)
        self.pointer_offset.pack(side="left", padx=(0, 6))
        self.pointer_offset.insert(0, "0x400")
        self.pointer_limit = ttk.Entry(pointer_row, width=6)
        self.pointer_limit.pack(side="left", padx=(0, 6))
        self.pointer_limit.insert(0, "100")
        self.pointer_index = ttk.Entry(pointer_row, width=6)
        self.pointer_index.pack(side="left", padx=(0, 6))
        self.pointer_index.insert(0, "0")
        ttk.Button(pointer_row, text="Scan", command=self._pointer_scan).pack(side="left")

        ttk.Button(parent, text="List Pointer Chains", command=self._show_pointers).pack(
            fill="x", pady=4
        )
        ttk.Button(parent, text="Follow Pointer", command=self._follow_pointer).pack(
            fill="x", pady=4
        )

    def _build_memory_controls(self, parent: ttk.Frame) -> None:
        ttk.Label(parent, text="READ / WRITE / FREEZE", foreground="#62e6a7").pack(
            anchor="w"
        )
        self.address = ttk.Entry(parent)
        self.address.pack(fill="x", pady=(8, 4))
        self.address.insert(0, "#0")
        self.edit_value = ttk.Entry(parent)
        self.edit_value.pack(fill="x", pady=4)
        self.edit_value.insert(0, "9999")

        row = ttk.Frame(parent, style="Panel.TFrame")
        row.pack(fill="x", pady=4)
        ttk.Button(row, text="Read", command=self._read).pack(side="left", expand=True, fill="x")
        ttk.Button(row, text="Write", command=self._write).pack(
            side="left", expand=True, fill="x", padx=5
        )
        ttk.Button(row, text="Freeze", command=self._freeze).pack(
            side="left", expand=True, fill="x"
        )
        ttk.Button(parent, text="Unfreeze", command=self._unfreeze).pack(fill="x", pady=4)
        ttk.Button(parent, text="Status", command=lambda: self._send("status")).pack(
            fill="x", pady=4
        )

    def _build_config_controls(self, parent: ttk.Frame) -> None:
        ttk.Label(parent, text="CONFIG", foreground="#62e6a7").pack(anchor="w")
        ttk.Button(parent, text="Save Config", command=self._save_config).pack(
            fill="x", pady=(8, 4)
        )
        ttk.Button(parent, text="Load Config", command=self._load_config).pack(
            fill="x", pady=4
        )

    def _send(self, command: str) -> None:
        try:
            self._engine.send(command)
        except RuntimeError as exc:
            messagebox.showerror("Command gagal", str(exc))

    def _send_manual(self) -> None:
        command = self.command.get().strip()
        if command:
            self._send(command)
            self.command.delete(0, "end")

    def _attach(self) -> None:
        if self.pid.get().strip():
            self._send(f"attach {self.pid.get().strip()}")

    def _launch(self) -> None:
        path = filedialog.askopenfilename(title="Pilih executable target")
        if path:
            self._send("launch " + shlex.quote(path))

    def _set_type(self) -> None:
        self._send("type " + self.value_type.get())

    def _set_threads(self) -> None:
        value = self.thread_count.get().strip()
        if value == "auto" or not value:
            self._send("threads auto")
        else:
            self._send("threads " + value)

    def _first_exact(self) -> None:
        self._send("scan exact " + self.scan_value.get().strip())

    def _benchmark_exact(self) -> None:
        value = self.scan_value.get().strip()
        if value:
            self._send("benchmark scan exact " + value)

    def _next_exact(self) -> None:
        self._send("next exact " + self.scan_value.get().strip())

    def _next_relative(self) -> None:
        self._send("next " + self.next_mode.get())

    def _signature_scan(self) -> None:
        pattern = self.signature_pattern.get().strip()
        if pattern:
            self._send("scan signature " + pattern)

    def _pointer_scan(self) -> None:
        address = self.pointer_address.get().strip()
        depth = self.pointer_depth.get().strip()
        max_offset = self.pointer_offset.get().strip()
        limit = self.pointer_limit.get().strip()
        if address:
            command = f"scan pointer {address}"
            if depth:
                command += f" {depth}"
            if max_offset:
                command += f" {max_offset}"
            if limit:
                command += f" {limit}"
            self._send(command)

    def _show_pointers(self) -> None:
        limit = self.pointer_limit.get().strip()
        self._send("pointers " + limit if limit else "pointers")

    def _follow_pointer(self) -> None:
        index = self.pointer_index.get().strip()
        if index:
            self._send("follow " + index)

    def _save_config(self) -> None:
        path = filedialog.asksaveasfilename(
            title="Simpan konfigurasi",
            defaultextension=".cfg",
            filetypes=(("Config files", "*.cfg *.txt"), ("All files", "*.*")),
        )
        if path:
            self._send("save config " + shlex.quote(path))

    def _load_config(self) -> None:
        path = filedialog.askopenfilename(
            title="Muat konfigurasi",
            filetypes=(("Config files", "*.cfg *.txt"), ("All files", "*.*")),
        )
        if path:
            self._send("load config " + shlex.quote(path))

    def _read(self) -> None:
        self._send(f"read {self.address.get().strip()} {self.value_type.get()}")

    def _write(self) -> None:
        self._send(
            f"write {self.address.get().strip()} {self.edit_value.get().strip()} "
            f"{self.value_type.get()}"
        )

    def _freeze(self) -> None:
        self._send(
            f"freeze {self.address.get().strip()} {self.edit_value.get().strip()} "
            f"{self.value_type.get()}"
        )

    def _unfreeze(self) -> None:
        self._send("unfreeze " + self.address.get().strip())

    def _drain_output(self) -> None:
        chunks: list[str] = []
        while True:
            try:
                chunks.append(self._queue.get_nowait())
            except queue.Empty:
                break
        if chunks:
            self.console.insert("end", "".join(chunks))
            self.console.see("end")
        self.after(30, self._drain_output)

    def _on_close(self) -> None:
        self._engine.close()
        self.destroy()


def find_default_engine() -> Path | None:
    root = Path(__file__).resolve().parents[1]
    names = (
        root / "build" / "memory_editor.exe",
        root / "build" / "Release" / "memory_editor.exe",
        root / "build" / "memory_editor",
        root / "build-linux" / "memory_editor",
        root / "build-windows" / "memory_editor.exe",
    )
    return next((path for path in names if path.is_file()), None)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--editor", type=Path, help="path ke binary memory_editor")
    arguments = parser.parse_args()
    executable = arguments.editor or find_default_engine()
    if executable is None or not executable.is_file():
        print(
            "Binary memory_editor tidak ditemukan. Build project atau gunakan "
            "--editor PATH.",
            file=sys.stderr,
        )
        return 1
    app = MemoryEditorGui(executable.resolve())
    app.mainloop()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
