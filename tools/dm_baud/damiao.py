"""DAMIAO モーターのレジスタ操作 (CAN 速度の確認・変更)。

参考: enactic/openarm_can setup/cli/commands/change_motor_baudrate_commands.cpp
"""

from __future__ import annotations

import time
from dataclasses import dataclass

from slcan import Frame, Slcan

REG_ID = 0x7FF  # レジスタ操作用の CAN ID
OP_READ, OP_WRITE, OP_SAVE = 0x33, 0x55, 0xAA
RID_CAN_BR = 0x23  # CAN 速度コード (35)

# can_br の値 → (表示名, 通常部, データ部)
BAUD_CODES = {
    0: ("125k (CAN 2.0)", 125_000, None),
    1: ("200k (CAN 2.0)", 200_000, None),
    2: ("250k (CAN 2.0)", 250_000, None),
    3: ("500k (CAN 2.0)", 500_000, None),
    4: ("1M (CAN 2.0)", 1_000_000, None),
    5: ("2M (CAN FD)", 1_000_000, 2_000_000),
    6: ("2.5M (CAN FD)", 1_000_000, 2_500_000),
    7: ("3.2M (CAN FD)", 1_000_000, 3_200_000),
    8: ("4M (CAN FD)", 1_000_000, 4_000_000),
    9: ("5M (CAN FD)", 1_000_000, 5_000_000),
    10: ("8M (CAN FD)", 1_000_000, 8_000_000),
    11: ("10M (CAN FD)", 1_000_000, 10_000_000),
}
CODE_1M_CAN20 = 4
CODE_5M_CANFD = 9


def baud_label(code: int | None) -> str:
    if code is None:
        return "不明"
    return BAUD_CODES.get(code, (f"コード {code}",))[0]


@dataclass
class Motor:
    can_id: int  # ESC_ID
    master_id: int  # 応答フレームの CAN ID (MST_ID)
    baud_code: int | None
    frame_kind: str  # 応答の形式 (CAN 2.0 / CAN FD)


def _reg_frame(can_id: int, op: int, rid: int = 0, value: bytes = b"\0\0\0\0") -> bytes:
    return bytes([can_id & 0xFF, (can_id >> 8) & 0xFF, op, rid]) + value


def _is_reply(f: Frame, can_id: int | None, op: int, rid: int) -> bool:
    d = f.data
    if len(d) < 8 or d[2] != op or d[3] != rid:
        return False
    rid_id = d[0] | (d[1] << 8)
    return rid_id != 0 and (can_id is None or rid_id == can_id)


def scan(bus: Slcan, id_min: int, id_max: int, progress=None) -> list[Motor]:
    """全 ID に can_br の読み出しを送り、応答したモーターを集める。"""
    found: dict[int, Motor] = {}

    def collect(timeout: float) -> None:
        while (f := bus.recv(timeout)) is not None:
            if _is_reply(f, None, OP_READ, RID_CAN_BR):
                cid = f.data[0] | (f.data[1] << 8)
                found[cid] = Motor(cid, f.can_id, f.data[4], f.kind)
            timeout = 0

    bus.drain()
    total = id_max - id_min + 1
    for i, cid in enumerate(range(id_min, id_max + 1)):
        bus.send(REG_ID, _reg_frame(cid, OP_READ, RID_CAN_BR))
        time.sleep(0.0005)  # CANable の送信 FIFO があふれないように
        if i % 32 == 0:
            collect(0)
            if progress:
                progress(i / total)
    collect(0.3)
    if progress:
        progress(1.0)
    return sorted(found.values(), key=lambda m: m.can_id)


def read_baud(bus: Slcan, can_id: int, timeout: float = 0.3) -> Motor | None:
    bus.drain()
    bus.send(REG_ID, _reg_frame(can_id, OP_READ, RID_CAN_BR))
    end = time.monotonic() + timeout
    while (left := end - time.monotonic()) > 0:
        f = bus.recv(left)
        if f and _is_reply(f, can_id, OP_READ, RID_CAN_BR):
            return Motor(can_id, f.can_id, f.data[4], f.kind)
    return None


def change_baud(bus: Slcan, can_id: int, code: int, log) -> bool:
    """can_br を書き込み、フラッシュに保存する。反映にはモーターの電源再投入が必要。"""
    # 1. 無効化 (保存の前提条件。モーターはフリーになる)
    log(f"[1/3] モーター 0x{can_id:02X} を無効化")
    bus.send(can_id, bytes([0xFF] * 7 + [0xFD]))
    time.sleep(0.05)

    # 2. can_br を書き込み
    log(f"[2/3] CAN 速度を {baud_label(code)} (コード {code}) に設定")
    bus.drain()
    bus.send(REG_ID, _reg_frame(can_id, OP_WRITE, RID_CAN_BR, bytes([code, 0, 0, 0])))
    ok = False
    end = time.monotonic() + 0.3
    while (left := end - time.monotonic()) > 0:
        f = bus.recv(left)
        if f and _is_reply(f, can_id, OP_WRITE, RID_CAN_BR):
            ok = f.data[4] == code
            break
    if not ok:
        log("  書き込みの応答を確認できませんでした (保存は続行します)")
    else:
        log("  書き込み OK")

    # 3. フラッシュへ保存
    log("[3/3] フラッシュへ保存")
    bus.send(can_id, bytes([0xFF] * 7 + [0xFD]))
    time.sleep(0.05)
    bus.send(REG_ID, _reg_frame(can_id, OP_SAVE, 0x01))
    time.sleep(0.1)
    return ok
