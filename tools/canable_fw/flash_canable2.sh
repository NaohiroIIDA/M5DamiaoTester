#!/usr/bin/env bash
# CANable 2.0 に slcan ファームウェア (Nakakiyo092/canable2-fw v1.4.1) を書き込む。
#
# 公式版 (normaldotcom/canable2-fw) はデータ部 5Mbps の受信に余裕がなく
# (SJW 1/17 ビット、TDC なし)、5M (CAN FD) に設定した DAMIAO モーターの応答を受信できなかった。
# この派生版は 5M で SJW 6/32 ビット・サンプル点 78%・TDC ありで、コマンドは公式版と互換。
#
# ただし派生版は互換機 (Walfront) 向けにトランシーバの STBY (PA0) 制御を削除しており、
# Openlight Labs 製の CANable 2.0 ではトランシーバがスタンバイのまま (BIT0 エラーで BUS_OFF) になる。
# 0001-drive-transceiver-standby-low.patch で公式版と同じく PA0 を Low にしてから書き込む。
#
# 使い方:
#   1. CANable 2.0 の BOOT ボタンを押しながら (またはBOOTジャンパーをショートして) USB に挿す
#   2. ./flash_canable2.sh
#   3. 書き込み後、BOOT ジャンパーを戻して USB を挿し直す
#
# 必要なもの: git, arm-none-eabi-gcc, make, dfu-util
set -euo pipefail

FW_REPO="https://github.com/Nakakiyo092/canable2-fw.git"
FW_TAG="v1.4.1"
HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$HERE/build/canable2-fw-$FW_TAG"

command -v arm-none-eabi-gcc >/dev/null || { echo "arm-none-eabi-gcc が見つかりません (PATH を確認してください)"; exit 1; }
command -v dfu-util >/dev/null || { echo "dfu-util が見つかりません (brew install dfu-util)"; exit 1; }

# --- ソース取得 (タグに固定) ---
if [ ! -d "$SRC/.git" ]; then
    git clone --quiet --branch "$FW_TAG" "$FW_REPO" "$SRC"
fi
git -C "$SRC" checkout --quiet -- .
git -C "$SRC" apply "$HERE/0001-drive-transceiver-standby-low.patch"

# --- ビルド ---
make -C "$SRC" clean >/dev/null 2>&1 || true
make -C "$SRC" -j8
BIN="$(ls "$SRC"/build/canable2-*.bin | head -1)"
echo "ビルド完了: $BIN"
# パッチを当てているので版の表示は v1.4.1-dirty になる

# --- 書き込み (DFU) ---
echo "DFU モードの CANable 2.0 を待っています… (BOOT を押しながら USB を挿してください)"
dfu-util -w -d 0483:df11 -c 1 -i 0 -a 0 -s 0x08000000:leave -D "$BIN"
echo "書き込み完了。BOOT ジャンパーを戻して USB を挿し直してください。"
