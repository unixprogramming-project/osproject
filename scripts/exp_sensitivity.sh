#!/bin/bash
# 예비 측정에서 나온 이상 현상의 원인을 요인 하나씩 바꿔 확인한다.
#  A. 코어 유휴 진입: 기본 / 저우선순위 부하로 유휴 차단 / 같은 코어 고정 / 다른 코어 고정
#  B. shared memory 슬롯 구성: 같은 총용량에서 슬롯 크기와 개수만 변경
#  C. message queue 깊이
# 사용: exp_sensitivity.sh <출력 폴더> [반복=7]
set -u
OUT=${1:?usage: exp_sensitivity.sh <out-dir> [reps]}
REPS=${2:-7}
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN=$ROOT/build/release
export DURATION_MS=${DURATION_MS:-300} WARMUP_MS=${WARMUP_MS:-100}
mkdir -p "$OUT"
HEADER=role,ipc,mode,msg_size,clients,rep,client_id,n,elapsed_ns,mean_ns,p50_ns,p99_ns,max_ns,msgs_per_s,mb_per_s,user_us,sys_us,nvcsw,nivcsw,rss_before_kb,rss_after_kb,clock_overhead_ns,verify
gcc -O1 -o "$OUT/spin" "$ROOT/scripts/spin.c" || exit 1
NCPU=$(nproc)

med() { sort -n | awk '{a[NR]=$1} END {if (NR) print a[int((NR+1)/2)]; else print "nan"}'; }

# run_case <라벨> <래퍼 명령 또는 ""> <ipc> <mode> <size> <clients>
run_case() {
    label=$1; wrap=$2; ipc=$3; mode=$4; size=$5; clients=$6
    csv="$OUT/$label.csv"
    [ -s "$csv" ] || echo "$HEADER" > "$csv"
    for r in $(seq 1 "$REPS"); do
        if [ -n "$wrap" ]; then
            $wrap "$ROOT/scripts/run_one.sh" "$ipc" "$mode" "$size" "$clients" "$r" "$csv" </dev/null 2>>"$OUT/errors.log"
        else
            "$ROOT/scripts/run_one.sh" "$ipc" "$mode" "$size" "$clients" "$r" "$csv" </dev/null 2>>"$OUT/errors.log"
        fi || echo "FAILED $label $ipc $mode $size $clients" >&2
    done
    col() { awk -F, -v i="$ipc" -v m="$mode" -v s="$size" -v c="$clients" \
        '$1=="all" && $2==i && $3==m && $4==s && $5==c {print '"$1"'}' "$csv" | med; }
    if [ "$mode" = pingpong ]; then
        printf '%-14s %-5s %8s cli=%s p50=%9.1fus p99=%9.1fus vcsw/msg=%s\n' "$label" "$ipc" "$size" "$clients" \
            "$(col '$11/1000')" "$(col '$12/1000')" "$(col '$18/$8')"
    else
        printf '%-14s %-5s %8s cli=%s %10.1f MB/s vcsw/msg=%s\n' "$label" "$ipc" "$size" "$clients" \
            "$(col '$15')" "$(col '$18/$8')"
    fi
}

echo "== A. 코어 유휴 진입 (왕복 지연)"
for ipc in pipe mq shm; do
    for s in 64 16384; do
        run_case base "" $ipc pingpong $s 1
        run_case same_cpu "taskset -c 2" $ipc pingpong $s 1
        "$OUT/spin" "$NCPU" & sp=$!
        run_case busy_cpus "" $ipc pingpong $s 1
        pkill -P $sp 2>/dev/null; kill $sp 2>/dev/null; wait $sp 2>/dev/null
    done
done
pkill -f "$OUT/spin" 2>/dev/null

echo "== B. shared memory 슬롯 구성 (총용량 약 512KB)"
for cfg in "8 65536" "64 8192" "512 1024" "2048 256"; do
    set -- $cfg
    label="shm_${1}x${2}"
    for s in 64 1024 16384 1048576; do
        IPC_SHM_SLOTS=$1 IPC_SHM_SLOT_SIZE=$2 run_case "$label" "" shm throughput $s 1
        IPC_SHM_SLOTS=$1 IPC_SHM_SLOT_SIZE=$2 run_case "$label" "" shm pingpong $s 1
    done
done

echo "== C. message queue 깊이 (클라이언트 1개)"
for d in 1 5 10; do
    for s in 64 1048576; do
        IPC_MQ_DEPTH=$d run_case "mq_depth$d" "" mq throughput $s 1
    done
done
