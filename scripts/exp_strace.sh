#!/bin/bash
# 시스템 콜 횟수로 원인을 확인한다. 시간 측정과 따로 실행한다 (strace가 실행을 크게 느리게 한다).
# 준비: sudo apt install strace
# 사용: exp_strace.sh <출력 폴더>
set -u
OUT=${1:?usage: exp_strace.sh <out-dir>}
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN=$ROOT/build/release
command -v strace >/dev/null || { echo "strace가 없습니다: sudo apt install strace" >&2; exit 1; }
mkdir -p "$OUT"
for ipc in pipe mq shm; do for size in 64 16384 1048576; do
    "$BIN/bench_${ipc}_server" --cleanup-only
    "$BIN/bench_${ipc}_server" --clients 1 --mode pingpong &
    srv=$!
    strace -f -c -o "$OUT/${ipc}_${size}.txt" \
        "$BIN/bench_${ipc}_client" --clients 1 --size "$size" --mode pingpong --duration-ms 500 --warmup-ms 0 >"$OUT/${ipc}_${size}.csv"
    wait "$srv"
done; done
echo "완료: $OUT"
