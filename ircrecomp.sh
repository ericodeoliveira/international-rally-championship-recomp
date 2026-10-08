#!/bin/sh
# One-click: recompile International Rally Championship from a CD image (.cue) on Linux/macOS.
set -e
ROOT="$(cd "$(dirname "$0")" && pwd)"
[ -n "$1" ] || { echo "usage: $0 path/to/game.cue [output dir]"; exit 1; }
OUT="${2:-$ROOT/dist/IRC}"
if [ ! -x "$ROOT/.venv/bin/python" ]; then
  python3 -m venv "$ROOT/.venv"
  "$ROOT/.venv/bin/python" -m pip install -q -r "$ROOT/requirements.txt"
fi
cd "$ROOT" && "$ROOT/.venv/bin/python" -m ircrecomp build "$1" --out "$OUT"
