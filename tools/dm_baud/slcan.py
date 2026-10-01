"""CANable 2.0 (normaldotcom/canable2-fw) 用の最小 slcan ドライバ。

CAN FD (データ部 2M/5M) の送受信に対応した非標準コマンドを使う。
受信フレームは t(CAN 2.0) / d(FD, BRSなし) / b(FD, BRSあり) で区別される。
"""

from __future__ import annotations

import queue
import threading
import time
from dataclasses import dataclass

import serial
from serial.tools import list_ports

CANABLE_VID_PID = (0x16D0, 0x117E)

# FD の DLC コード → バイト数
_FD_DLC_BYTES = {9: 12, 10: 16, 11: 20, 12: 24, 13: 32, 14: 48, 15: 64}


@dataclass
class Frame:
    can_id: int
    data: bytes
    fd: bool = False  # CAN FD フレーム
    brs: bool = False  # ビットレートスイッチあり
    extended: bool = False

    @property
    def kind(self) -> str:
        if not self.fd:
            return "CAN 2.0"
        return "CAN FD (BRS)" if self.brs else "CAN FD"


def find_ports() -> list[tuple[str, str]]:
    """(デバイス, 説明) の一覧。CANable を先頭に並べる。"""
    ports = []
    for p in list_ports.comports():
        is_canable = (p.vid, p.pid) == CANABLE_VID_PID
        desc = p.description or ""
        if is_canable:
            desc = f"CANable 2.0 ({p.serial_number})"
        ports.append((not is_canable, p.device, desc))
    ports.sort()
    return [(dev, desc) for _, dev, desc in ports]


class Slcan:
    def __init__(self) -> None:
        self._ser: serial.Serial | None = None
        self._rx: queue.Queue[Frame] = queue.Queue()
        self._thread: threading.Thread | None = None
        self._stop = threading.Event()
        self._lock = threading.Lock()

    @property
    def is_open(self) -> bool:
        return self._ser is not None

    def open(self, port: str, data_bitrate: int = 5_000_000) -> str:
        """1Mbps (通常部) + 指定データ部で CAN FD チャネルを開く。ファームウェア版を返す。"""
        self.close()
        ser = serial.Serial(port, 115200, timeout=0.2)
        ser.write(b"\r\r\r")
        time.sleep(0.05)
        ser.write(b"C\r")
        time.sleep(0.05)
        ser.reset_input_buffer()

        # 'v' は詳細版 (Nakakiyo092 版)、未対応なら BELL が返るので 'V' (公式版) を使う
        version = ""
        for cmd in (b"v\r", b"V\r"):
            ser.write(cmd)
            time.sleep(0.1)
            version = ser.read(ser.in_waiting or 1).decode(errors="replace").strip("\r\a\n ")
            if version:
                break

        # 通常部 1Mbps はサンプル点 75% にする (DAMIAO モーター・OpenArm と同じ)。
        # 既定の S8 (サンプル点 87.5%) では 5M (CAN FD) のモーターの応答を受信できなかった。
        # s/y はクロック 160MHz の Nakakiyo092 版の書式 (分周, seg1, seg2, SJW)。
        # 未対応のファームウェア (BELL が返る) では S8/Yn に戻す。
        timing = {
            # 1Mbps: 160MHz / 2 / (1+59+20) → SP 75%
            "nominal": (b"s023B1414\r", b"S8\r"),
            # 5Mbps: 160MHz / 1 / (1+23+8) → SP 75%、2Mbps: 160MHz / 2 / (1+29+10) → SP 75%
            "data": {
                5_000_000: (b"y01170808\r", b"Y5\r"),
                2_000_000: (b"y021D0A0A\r", b"Y2\r"),
            }[data_bitrate],
        }
        for precise, fallback in (timing["nominal"], timing["data"]):
            ser.reset_input_buffer()
            ser.write(precise)
            time.sleep(0.05)
            if b"\a" in ser.read(ser.in_waiting or 1):
                ser.write(fallback)
                time.sleep(0.02)
        ser.write(b"O\r")
        time.sleep(0.02)
        ser.reset_input_buffer()

        self._ser = ser
        self._stop.clear()
        self._rx = queue.Queue()
        self._thread = threading.Thread(target=self._reader, daemon=True)
        self._thread.start()
        return version

    def close(self) -> None:
        if not self._ser:
            return
        self._stop.set()
        if self._thread:
            self._thread.join(timeout=1)
        try:
            self._ser.write(b"C\r")
            self._ser.close()
        except serial.SerialException:
            pass
        self._ser = None

    def send(self, can_id: int, data: bytes) -> None:
        """CAN 2.0 (標準 ID) フレームを送る。"""
        if not self._ser:
            raise RuntimeError("CANable が開かれていません")
        line = f"t{can_id:03X}{len(data):X}{data.hex().upper()}\r".encode()
        with self._lock:
            self._ser.write(line)

    def recv(self, timeout: float) -> Frame | None:
        try:
            return self._rx.get(timeout=timeout)
        except queue.Empty:
            return None

    def drain(self) -> None:
        while not self._rx.empty():
            self._rx.get_nowait()

    # ------------------------------------------------------------
    def _reader(self) -> None:
        buf = b""
        while not self._stop.is_set():
            try:
                chunk = self._ser.read(self._ser.in_waiting or 1)
            except (serial.SerialException, OSError, TypeError):
                break
            if not chunk:
                continue
            buf += chunk
            while b"\r" in buf:
                line, buf = buf.split(b"\r", 1)
                frame = _parse(line.decode(errors="replace").strip("\a\n "))
                if frame:
                    self._rx.put(frame)


def _parse(line: str) -> Frame | None:
    if not line or line[0] not in "tTdDbB":
        return None
    kind = line[0]
    extended = kind.isupper()
    id_len = 8 if extended else 3
    try:
        can_id = int(line[1 : 1 + id_len], 16)
        dlc = int(line[1 + id_len], 16)
        n = _FD_DLC_BYTES.get(dlc, dlc) if kind in "dDbB" else min(dlc, 8)
        start = 2 + id_len
        data = bytes.fromhex(line[start : start + n * 2])
    except (ValueError, IndexError):
        return None
    return Frame(
        can_id=can_id,
        data=data,
        fd=kind in "dDbB",
        brs=kind in "bB",
        extended=extended,
    )
