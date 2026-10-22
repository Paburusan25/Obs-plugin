#!/usr/bin/env bash
# Compila y ejecuta las pruebas del motor de notas IA (solo necesita Qt5 Core+Network y python3).
# Uso: tests/run_notes_tests.sh
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$HERE/../src/notes"
WORK="$(mktemp -d)"
trap 'kill ${SP:-0} 2>/dev/null || true; rm -rf "$WORK"' EXIT

QT_INC="$(pkg-config --variable=includedir Qt5Core)"
MOC="$(pkg-config --variable=host_bins Qt5Core)/moc"
INC="-I$QT_INC -I$QT_INC/QtCore -I$QT_INC/QtNetwork"

"$MOC" $INC "$SRC/MarkdownNotesEngine.h" -o "$WORK/moc_engine.cpp"
g++ -std=c++17 -fPIC -Wall -Wextra $INC "$HERE/notes_engine_test.cpp" "$SRC/MarkdownNotesEngine.cpp" \
    "$WORK/moc_engine.cpp" $(pkg-config --libs Qt5Network Qt5Core) -o "$WORK/notes_test"

echo "== funciones puras =="
"$WORK/notes_test" 0 /tmp pure

for mode in ok fence error500 truncated short; do
  PORT=$((20000 + RANDOM % 5000))
  MOCK_MODE=$([ "$mode" = short ] && echo ok || echo "$mode") python3 -I "$HERE/mock_llm_server.py" "$PORT" &
  SP=$!
  sleep 1
  echo "== modo $mode =="
  mkdir -p "$WORK/out_$mode"
  "$WORK/notes_test" "$PORT" "$WORK/out_$mode" "$mode" | grep -E "^(ok|FALLO|TIMEOUT)"
  kill $SP; wait $SP 2>/dev/null || true
done
echo "TODO OK"
