"""DAMIAO モーター CAN 速度切り替えツール (CANable 2.0 + slcan)。

1M (CAN 2.0) と 5M (CAN FD) を切り替える。
CANable は 通常部 1Mbps / データ部 5Mbps の CAN FD で開くので、
どちらの設定のモーターとも通信できる。
"""

from __future__ import annotations

import queue
import threading
import tkinter as tk
from tkinter import messagebox, ttk

import damiao
from slcan import Slcan, find_ports

APP_TITLE = "DAMIAO CAN 速度切り替え"


class App(tk.Tk):
    def __init__(self) -> None:
        super().__init__()
        self.title(APP_TITLE)
        self.geometry("680x560")
        self.minsize(600, 480)

        self.bus = Slcan()
        self.motors: list[damiao.Motor] = []
        self.ui_queue: queue.Queue = queue.Queue()  # ワーカースレッド → GUI
        self.busy = False

        self._build()
        self.refresh_ports()
        self.after(50, self._poll_ui)
        self.protocol("WM_DELETE_WINDOW", self.on_close)

    # ------------------------------------------------------------
    #  画面
    # ------------------------------------------------------------
    def _build(self) -> None:
        pad = {"padx": 8, "pady": 4}

        # 接続
        f = ttk.LabelFrame(self, text="CANable 2.0")
        f.pack(fill="x", **pad)
        self.port_var = tk.StringVar()
        self.port_box = ttk.Combobox(f, textvariable=self.port_var, state="readonly", width=42)
        self.port_box.grid(row=0, column=0, **pad)
        ttk.Button(f, text="更新", command=self.refresh_ports).grid(row=0, column=1, **pad)
        self.conn_btn = ttk.Button(f, text="接続", command=self.toggle_connect)
        self.conn_btn.grid(row=0, column=2, **pad)
        self.conn_label = ttk.Label(f, text="未接続", foreground="gray")
        self.conn_label.grid(row=1, column=0, columnspan=3, sticky="w", **pad)

        # 検索
        f = ttk.LabelFrame(self, text="モーター検索")
        f.pack(fill="x", **pad)
        ttk.Label(f, text="CAN ID 範囲  0x").grid(row=0, column=0, **pad)
        self.id_min_var = tk.StringVar(value="01")
        self.id_max_var = tk.StringVar(value="FF")
        ttk.Entry(f, textvariable=self.id_min_var, width=5).grid(row=0, column=1)
        ttk.Label(f, text="〜 0x").grid(row=0, column=2)
        ttk.Entry(f, textvariable=self.id_max_var, width=5).grid(row=0, column=3)
        self.scan_btn = ttk.Button(f, text="検索", command=self.start_scan, state="disabled")
        self.scan_btn.grid(row=0, column=4, **pad)
        self.progress = ttk.Progressbar(f, length=180, mode="determinate")
        self.progress.grid(row=0, column=5, **pad)

        # 一覧
        f = ttk.LabelFrame(self, text="見つかったモーター")
        f.pack(fill="both", expand=True, **pad)
        cols = ("id", "mst", "baud", "kind")
        self.tree = ttk.Treeview(f, columns=cols, show="headings", height=5, selectmode="browse")
        for c, label, w in [
            ("id", "CAN ID", 90),
            ("mst", "Master ID", 90),
            ("baud", "現在の CAN 速度設定", 200),
            ("kind", "応答の形式", 140),
        ]:
            self.tree.heading(c, text=label)
            self.tree.column(c, width=w, anchor="center")
        self.tree.pack(fill="both", expand=True, padx=8, pady=4)
        self.tree.bind("<<TreeviewSelect>>", lambda e: self._update_buttons())

        # 切り替え
        f = ttk.Frame(self)
        f.pack(fill="x", **pad)
        self.btn_1m = ttk.Button(
            f, text="1M (CAN 2.0) に変更", command=lambda: self.start_change(damiao.CODE_1M_CAN20)
        )
        self.btn_5m = ttk.Button(
            f, text="5M (CAN FD) に変更", command=lambda: self.start_change(damiao.CODE_5M_CANFD)
        )
        self.btn_1m.pack(side="left", expand=True, fill="x", padx=4, ipady=6)
        self.btn_5m.pack(side="left", expand=True, fill="x", padx=4, ipady=6)

        # ログ
        f = ttk.LabelFrame(self, text="ログ")
        f.pack(fill="both", expand=True, **pad)
        self.log_text = tk.Text(f, height=8, state="disabled", wrap="word")
        sb = ttk.Scrollbar(f, command=self.log_text.yview)
        self.log_text.configure(yscrollcommand=sb.set)
        sb.pack(side="right", fill="y")
        self.log_text.pack(fill="both", expand=True, padx=4, pady=4)

        self._update_buttons()

    def log(self, msg: str) -> None:
        """どのスレッドからでも呼べる。"""
        self.ui_queue.put(("log", msg))

    def _append_log(self, msg: str) -> None:
        self.log_text.configure(state="normal")
        self.log_text.insert("end", msg + "\n")
        self.log_text.see("end")
        self.log_text.configure(state="disabled")

    def _update_buttons(self) -> None:
        connected = self.bus.is_open
        selected = bool(self.tree.selection())
        idle = not self.busy
        self.scan_btn.configure(state="normal" if connected and idle else "disabled")
        state = "normal" if connected and selected and idle else "disabled"
        self.btn_1m.configure(state=state)
        self.btn_5m.configure(state=state)
        self.conn_btn.configure(text="切断" if connected else "接続", state="normal" if idle else "disabled")

    def _show_motors(self) -> None:
        self.tree.delete(*self.tree.get_children())
        for m in self.motors:
            self.tree.insert(
                "",
                "end",
                iid=str(m.can_id),
                values=(f"0x{m.can_id:02X}", f"0x{m.master_id:02X}", damiao.baud_label(m.baud_code), m.frame_kind),
            )
        if self.motors:
            self.tree.selection_set(str(self.motors[0].can_id))
        self._update_buttons()

    def _poll_ui(self) -> None:
        while not self.ui_queue.empty():
            kind, payload = self.ui_queue.get_nowait()
            if kind == "log":
                self._append_log(payload)
            elif kind == "progress":
                self.progress["value"] = payload * 100
            elif kind == "motors":
                self.motors = payload
                self._show_motors()
            elif kind == "done":
                self.busy = False
                self._update_buttons()
                if payload:
                    messagebox.showinfo(APP_TITLE, payload)
        self.after(50, self._poll_ui)

    def _run_bg(self, target) -> None:
        self.busy = True
        self._update_buttons()

        def wrapper():
            msg = None
            try:
                msg = target()
            except Exception as e:  # noqa: BLE001 - GUI に表示する
                self.log(f"エラー: {e}")
            self.ui_queue.put(("done", msg))

        threading.Thread(target=wrapper, daemon=True).start()

    # ------------------------------------------------------------
    #  操作
    # ------------------------------------------------------------
    def refresh_ports(self) -> None:
        self.ports = find_ports()
        self.port_box["values"] = [f"{desc}  —  {dev}" for dev, desc in self.ports]
        if self.ports:
            self.port_box.current(0)

    def toggle_connect(self) -> None:
        if self.bus.is_open:
            self.bus.close()
            self.conn_label.configure(text="未接続", foreground="gray")
            self.log("切断しました")
            self._update_buttons()
            return
        idx = self.port_box.current()
        if idx < 0:
            messagebox.showwarning(APP_TITLE, "ポートを選んでください")
            return
        dev = self.ports[idx][0]
        try:
            version = self.bus.open(dev)
        except Exception as e:  # noqa: BLE001
            messagebox.showerror(APP_TITLE, f"接続できませんでした\n{e}")
            return
        self.conn_label.configure(text=f"接続中: 1Mbps / FD 5Mbps   FW: {version or '不明'}", foreground="green")
        self.log(f"{dev} に接続 (FW: {version or '不明'})")
        self._update_buttons()
        self.start_scan()

    def _parse_range(self) -> tuple[int, int] | None:
        try:
            lo, hi = int(self.id_min_var.get(), 16), int(self.id_max_var.get(), 16)
        except ValueError:
            messagebox.showwarning(APP_TITLE, "ID 範囲は 16 進数で入力してください")
            return None
        if not (1 <= lo <= hi <= 0x7FE):
            messagebox.showwarning(APP_TITLE, "ID 範囲は 0x01〜0x7FE で指定してください")
            return None
        return lo, hi

    def start_scan(self) -> None:
        rng = self._parse_range()
        if not rng or not self.bus.is_open:
            return

        def task():
            self.log(f"検索中… (0x{rng[0]:02X}〜0x{rng[1]:02X})")
            motors = damiao.scan(self.bus, *rng, progress=lambda p: self.ui_queue.put(("progress", p)))
            self.ui_queue.put(("motors", motors))
            if not motors:
                self.log("モーターが見つかりません。電源・配線・終端抵抗を確認してください")
            for m in motors:
                self.log(
                    f"  CAN ID 0x{m.can_id:02X} / Master 0x{m.master_id:02X} : "
                    f"{damiao.baud_label(m.baud_code)}  (応答 {m.frame_kind})"
                )
            return None

        self._run_bg(task)

    def start_change(self, code: int) -> None:
        sel = self.tree.selection()
        if not sel:
            return
        motor = next(m for m in self.motors if str(m.can_id) == sel[0])
        label = damiao.baud_label(code)
        if motor.baud_code == code:
            if not messagebox.askyesno(APP_TITLE, f"すでに {label} に設定されています。\n書き込みますか？"):
                return
        if not messagebox.askokcancel(
            APP_TITLE,
            f"モーター 0x{motor.can_id:02X} の CAN 速度を\n\n"
            f"  {damiao.baud_label(motor.baud_code)}  →  {label}\n\n"
            "に変更してフラッシュに保存します。\n"
            "・モーターは無効化 (フリー) されます\n"
            "・フラッシュの書き込み回数には上限があります\n"
            "・反映にはモーターの電源の入れ直しが必要です",
            icon="warning",
        ):
            return

        def task():
            ok = damiao.change_baud(self.bus, motor.can_id, code, self.log)
            self.log("完了。モーターの電源を入れ直してから「検索」で確認してください")
            note = "" if ok else "\n(書き込みの応答は確認できませんでした)"
            return f"{label} に設定して保存しました。{note}\n\nモーターの電源を入れ直してから「検索」で確認してください。"

        self._run_bg(task)

    def on_close(self) -> None:
        self.bus.close()
        self.destroy()


if __name__ == "__main__":
    App().mainloop()
