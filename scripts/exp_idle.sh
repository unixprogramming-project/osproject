#!/bin/bash
# 코어 유휴 진입의 영향을 확인하는 추가 측정. 전 메시지 크기, 클라이언트 1개.
#   busy: 낮은 우선순위 부하로 모든 코어를 깨워 둔다
#   same_cpu: 서버와 클라이언트를 한 코어에 고정한다 (코어 사이 깨우기가 없다)
# 사용: exp_idle.sh <출력 폴더> [반복=30]
set -u
OUT=${1:?usage: exp_idle.sh <out-dir> [reps]}
REPS=${2:-30}
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SIZES=${SIZES:-"64 1024 16384 262144 1048576"}
export DURATION_MS=${DURATION_MS:-500} WARMUP_MS=${WARMUP_MS:-100}
HEADER=role,ipc,mode,msg_size,clients,rep,client_id,n,elapsed_ns,mean_ns,p50_ns,p99_ns,max_ns,msgs_per_s,mb_per_s,user_us,sys_us,nvcsw,nivcsw,rss_before_kb,rss_after_kb,clock_overhead_ns,verify
mkdir -p "$OUT"
gcc -O1 -o "$OUT/spin" "$ROOT/scripts/spin.c" || exit 1
for v in busy same_cpu; do [ -s "$OUT/$v.csv" ] || echo "$HEADER" > "$OUT/$v.csv"; done

fail=0
run() { # 변형, 래퍼...
    v=$1; shift
    for r in $(seq 0 $((REPS - 1))); do for i in pipe mq shm; do for m in pingpong throughput; do for s in $SIZES; do
        "$@" "$ROOT/scripts/run_one.sh" "$i" "$m" "$s" 1 "$r" "$OUT/$v.csv" </dev/null 2>>"$OUT/errors.log" || fail=$((fail + 1))
    done; done; done; done
}

"$OUT/spin" "$(nproc)" & sp=$!
run busy env
pkill -P "$sp" 2>/dev/null; kill "$sp" 2>/dev/null; wait "$sp" 2>/dev/null
pkill -f "$OUT/spin" 2>/dev/null
run same_cpu taskset -c 2
echo "exp_idle failed: $fail"
[ "$fail" -eq 0 ]
