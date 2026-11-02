#!/bin/bash
# 1단계 전 조건 측정. 시간에 따른 편향을 막기 위해 실행 순서를 고정 시드로 섞는다.
# 사용: run_phase1.sh <출력 폴더> [반복 횟수=30]
# 환경 변수: SIZES, CLIENTS, IPCS, MODES, DURATION_MS, WARMUP_MS, TEAM, SEED
set -u
OUT=${1:?usage: run_phase1.sh <out-dir> [reps]}
REPS=${2:-30}
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SIZES=${SIZES:-"64 1024 16384 262144 1048576"}
CLIENTS=${CLIENTS:-"1 2 4 8"}
IPCS=${IPCS:-"pipe mq shm"}
MODES=${MODES:-"pingpong throughput"}
SEED=${SEED:-20261102}
export DURATION_MS=${DURATION_MS:-500} WARMUP_MS=${WARMUP_MS:-100}

mkdir -p "$OUT"
CSV=$OUT/phase1.csv
HEADER=role,ipc,mode,msg_size,clients,rep,client_id,n,elapsed_ns,mean_ns,p50_ns,p99_ns,max_ns,msgs_per_s,mb_per_s,user_us,sys_us,nvcsw,nivcsw,rss_before_kb,rss_after_kb,clock_overhead_ns,verify
[ -s "$CSV" ] || echo "$HEADER" > "$CSV"
"$ROOT/scripts/collect_env.sh" > "$OUT/env.txt"

PLAN=$(mktemp)
for r in $(seq 0 $((REPS - 1))); do for i in $IPCS; do for m in $MODES; do for s in $SIZES; do for c in $CLIENTS; do
    echo "$i $m $s $c $r"
done; done; done; done; done > "$PLAN"
total=$(wc -l < "$PLAN")
# shuf 구현 차이에 기대지 않도록 POSIX awk와 sort로 섞는다 (시드가 같으면 순서도 같다)
awk -v s="$SEED" 'BEGIN { srand(s) } { printf "%.12f\t%s\n", rand(), $0 }' "$PLAN" | sort -n | cut -f2- > "$PLAN.shuf"
mv "$PLAN.shuf" "$PLAN"
if [ "$(wc -l < "$PLAN")" -ne "$total" ]; then
    echo "error: shuffled plan has wrong size" >&2
    exit 1
fi
echo "runs: $total  (실행당 약 $(((DURATION_MS + WARMUP_MS) / 100))00ms 이상)" | tee "$OUT/run.log"
date '+start %F %T' | tee -a "$OUT/run.log"

done_n=0; fail=0
while read -r i m s c r; do
    if ! "$ROOT/scripts/run_one.sh" "$i" "$m" "$s" "$c" "$r" "$CSV" 2>>"$OUT/errors.log" </dev/null; then
        fail=$((fail + 1))
    fi
    done_n=$((done_n + 1))
    if [ $((done_n % 100)) -eq 0 ]; then
        echo "$(date '+%T') $done_n/$total failed=$fail" | tee -a "$OUT/run.log"
    fi
done < "$PLAN"
rm -f "$PLAN"
date '+end %F %T' | tee -a "$OUT/run.log"
echo "done: $done_n failed: $fail" | tee -a "$OUT/run.log"
[ "$fail" -eq 0 ]
