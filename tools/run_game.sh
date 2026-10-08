#!/bin/sh
# Run the native build headlessly-ish with scripted keys and screenshots.
# usage: tools/run_game.sh <run-name> <exit-after-ms> "<keys>" [png frame numbers...]
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
NAME="$1"; MS="$2"; KEYS="$3"; shift 3
OUT="$ROOT/build/native/runs/$NAME"
mkdir -p "$OUT"
cd "$ROOT/build/native" || exit 1
timeout $(( MS / 1000 + 30 )) ./IRC.exe $IRC_FLAGS --shots "$OUT" --keys "$KEYS" --exit-after "$MS" 2>/dev/null
echo "exit=$?"
cp irc_runtime.log "$OUT/runtime.log"
grep -v "unral\|FAIL" "$OUT/runtime.log" | tail -20
ls "$OUT"/*.bmp 2>/dev/null | wc -l
for n in "$@"; do
  f=$(ls "$OUT"/*.bmp | sed -n "${n}p")
  [ -n "$f" ] && "$ROOT/.venv/Scripts/python" -I "$ROOT/tools/bmp2png.py" "$f"
done
