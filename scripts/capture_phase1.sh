#!/bin/bash
# 1단계 보고서 캡처를 만든다. 실제 명령을 다시 실행하거나 측정 때 남은 로그를 그대로 보여 준다.
# 사용: capture_phase1.sh <결과폴더>   (예: results/phase1) -> <결과폴더>/captures/*.png, *.txt
set -u
RES=${1:?usage: capture_phase1.sh <results-dir>}
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT=$RES/captures
mkdir -p "$OUT"
cap() { # cap 이름 "보여 줄 명령"  (명령을 실제로 실행해 출력을 저장)
    local name=$1 cmd=$2
    (cd "$ROOT" && bash -c "$cmd") >"$OUT/$name.txt" 2>&1
    python3 "$ROOT/scripts/term2png.py" "$OUT/$name.png" "\$ $cmd" <"$OUT/$name.txt"
}
cap 01_env "cat $RES/env.txt"
cap 02_verify "sed -n '1,/^runs:/p' official.log"
cap 03_done "tail -2 $RES/run.log; wc -l $RES/phase1.csv $RES/idle/busy.csv $RES/idle/same_cpu.csv; cat $RES/errors.log | wc -l"
cap 04_summary_latency "sed -n '/p50/,/p99/p' $RES/summary.txt | head -23"
cap 05_summary_throughput "sed -n '/MB\/s/,\$p' $RES/summary.txt"
cap 06_idle "sed -n '/== A/,/== B/p' $RES/sensitivity.txt"
cap 07_shm_slots "sed -n '/== B/,/== C/p' $RES/sensitivity.txt | grep 'MB/s'"
cap 08_mq_depth "sed -n '/== C/,\$p' $RES/sensitivity.txt"
for s in 1048576 64; do
    cap "09_strace_$s" "for i in pipe mq shm; do echo \"## \$i $s B, client 1, 500ms\"; sed -n '1,2p' $RES/strace/\${i}_$s.txt; grep -E 'read|writev|mq_timed|futex|total' $RES/strace/\${i}_$s.txt; done"
done
cap 10_ipcs_after "scripts/check_ipc.sh"
ls "$OUT"
