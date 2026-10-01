#!/usr/bin/env bash
# 初回は仮想環境を作って pyserial を入れてから GUI を起動する。
# tkinter が使える Python を指定したい場合: PYTHON=/path/to/python3 ./run.sh
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
VENV="$HERE/.venv"

if [ ! -x "$VENV/bin/python" ]; then
    PY="${PYTHON:-}"
    if [ -z "$PY" ]; then
        for c in python3.13 python3.12 python3.11 python3; do
            if command -v "$c" >/dev/null && "$c" -c "import tkinter" 2>/dev/null; then PY="$c"; break; fi
        done
    fi
    [ -n "$PY" ] || { echo "tkinter が使える Python が見つかりません (brew install python-tk@3.12 など)"; exit 1; }
    "$PY" -m venv "$VENV"
    "$VENV/bin/pip" install -q -r "$HERE/requirements.txt"
fi
exec "$VENV/bin/python" "$HERE/dm_baud_gui.py"
