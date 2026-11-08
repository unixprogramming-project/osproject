#!/bin/bash
# 2단계 보고서 캡처 (실제 명령 실행 결과). 사용: TEAM=조번호 scripts/capture_phase2.sh <출력폴더>
set -u
OUT=${1:?usage: capture_phase2.sh <out-dir>}
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
mkdir -p "$OUT"
cap() {
    local name=$1 cmd=$2
    (cd "$ROOT" && bash -c "$cmd") >"$OUT/$name.txt" 2>&1
    python3 "$ROOT/scripts/term2png.py" "$OUT/$name.png" "\$ $cmd" <"$OUT/$name.txt"
}
cap 11_make "make clean && make 2>&1 | grep -cE 'warning|error' ; make app_pipe app_shm && ls build/release | grep app_"
cap 12_demo_shm "TEAM=${TEAM:-0} scripts/demo_phase2.sh shm thread"
cap 13_demo_pipe "TEAM=${TEAM:-0} scripts/demo_phase2.sh pipe thread"
cap 14_sigint "build/release/app_shm_server --clients 2 --team ${TEAM:-0} & sleep 0.5; ls /dev/shm | grep osproject_; kill -INT \$!; wait \$!; echo \"server exit=\$? (128+SIGINT)\"; ls /dev/shm | grep -c osproject_"
cap 15_ipcs_after "ipcs; ls /dev/mqueue /dev/shm; ls /tmp | grep -c osproject_"
cap 16_summary_v1 "cat results/phase2/summary.txt | head -24"
cap 17_summary_v2 "cat results/phase2_v2/summary.txt | head -24"
ls "$OUT"
