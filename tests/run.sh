#!/bin/sh
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN=$ROOT/autogonki
cd "$ROOT"

echo "=== test help ==="
$BIN --help >/tmp/ag_help.txt
grep -q "autogonki" /tmp/ag_help.txt

echo "=== test default race ==="
$BIN --seed 1 --laps 2 --max-rounds 80 --delay 0 --log /tmp/ag_default.log >/tmp/ag_default.out
grep -q "итоговая классификация" /tmp/ag_default.out
grep -q "командный зачёт" /tmp/ag_default.log

echo "=== test random short ==="
$BIN --strategy random --seed 7 --laps 1 --sectors 12 --teams 2 --cars 2 \
     --max-rounds 40 --bonus 0 --log /tmp/ag_rnd.log >/tmp/ag_rnd.out
grep -q "судья: гонка остановлена" /tmp/ag_rnd.out

echo "=== test bad args ==="
if $BIN --lanes 1 >/tmp/ag_bad.out 2>/tmp/ag_bad.err; then
    echo "expected failure"; exit 1
fi
grep -q "некорректные" /tmp/ag_bad.err

echo "OK"
