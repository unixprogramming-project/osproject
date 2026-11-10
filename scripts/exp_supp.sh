#!/bin/bash
# 보충 측정 (2026-11-10 계획). 각 실험은 요인 하나만 바꾸고, 비교할 조건들을 같은 시간대에 섞어서 잰다.
# 사용: TEAM=조번호 scripts/exp_supp.sh <출력폴더> [반복=30]
# 환경 변수: SUPP (기본 "S1 S2 S3 S4", 일부만 돌릴 때 지정)
#  S1 1단계 버퍼 용량: pipe(64KB) / shm 링 512KB(기본) / shm 링 64KB(8슬롯×8KB)
#     5크기 × 클라 1·8 × pingpong·throughput              -> 큰 메시지 차이 중 버퍼 용량 몫
#  S2 송신 방식: v1(--sender direct) / v2(--sender thread)
#     pipe·shm × 클라 2·4 × 적중 80/20%                    -> v1·v2를 같은 시간대에 비교
#  S3 워커 수: 8, 12, 14, 16 (shm, 클라 4, 적중 80%, v1)   -> 워커 16 처리량 하락 원인
#  S4 응용 shm 링: 64슬롯(기본) / 16슬롯 (v2, 클라 8, 적중 80/20%) -> IPC 쪽 메모리 절감
#  S5 응답 크기 고정 (2026-11-11 추가, 기본 목록에는 없음): 분포대로(mix) / 16KB / 256KB / 1MB
#     pipe·shm × 클라 8 × 적중 80%, v1 (APP_SIZE_CLASS)      -> 서버 CS·sys가 응답 크기에 따라 어떻게 바뀌는지 (P5·P6)
# 출력: <폴더>/<실험>/<조건라벨>.csv (조건마다 파일 하나, 형식은 본측정 CSV와 같다), plan.txt, run.log, errors.log, env.txt
set -u
OUT=${1:?usage: exp_supp.sh <out-dir> [reps]}
REPS=${2:-30}
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TEAM=${TEAM:-0}; export TEAM
export DURATION_MS=${DURATION_MS:-500} WARMUP_MS=${WARMUP_MS:-100}
case "$ROOT" in /mnt/*) echo "Linux 파일시스템으로 복사한 뒤 실행하세요 (/mnt 아래는 측정 불가)" >&2; exit 1 ;; esac
if [ -e "$OUT" ]; then echo "이미 있는 폴더입니다: $OUT (새 폴더를 지정하세요)" >&2; exit 1; fi
mkdir -p "$OUT"
make -s -C "$ROOT" || exit 1
P1HEADER=role,ipc,mode,msg_size,clients,rep,client_id,n,elapsed_ns,mean_ns,p50_ns,p99_ns,max_ns,msgs_per_s,mb_per_s,user_us,sys_us,nvcsw,nivcsw,rss_before_kb,rss_after_kb,clock_overhead_ns,verify

shuffle() { awk -v s="$1" 'BEGIN { srand(s) } { printf "%.12f\t%s\n", rand(), $0 }' | sort -n | cut -f2-; }

# 계획 한 줄: <라벨> <환경변수(쉼표로 구분, 없으면 -)> <나머지 인자...>
#   p1: ipc mode size clients rep      p2: ipc clients hit rep
run_plan() {
    local dir=$1 kind=$2 total done_n=0 failed=0 label envs a b c d e csv
    total=$(wc -l <"$dir/plan.txt")
    echo "runs: $total" | tee -a "$dir/run.log"
    echo "start $(date '+%F %T')" | tee -a "$dir/run.log"
    while read -r label envs a b c d e; do
        [ "$envs" = "-" ] && envs="" || envs=${envs//,/ }
        csv=$dir/$label.csv
        if [ "$kind" = p1 ]; then
            [ -s "$csv" ] || echo "$P1HEADER" >"$csv"
            # shellcheck disable=SC2086
            env $envs "$ROOT/scripts/run_one.sh" "$a" "$b" "$c" "$d" "$e" "$csv" 2>>"$dir/errors.log" </dev/null
        else
            # shellcheck disable=SC2086
            env $envs "$ROOT/scripts/run_app_one.sh" "$a" "$b" "$c" "$csv" "$d" 2>>"$dir/errors.log" </dev/null
        fi
        if [ $? -ne 0 ]; then
            echo "fail: $label $a $b $c $d $e" >>"$dir/errors.log"
            failed=$((failed + 1))
        fi
        done_n=$((done_n + 1))
        if [ $((done_n % 20)) -eq 0 ]; then echo "$(date +%T) $done_n/$total failed=$failed" >>"$dir/run.log"; fi
    done <"$dir/plan.txt"
    echo "end $(date '+%F %T')" >>"$dir/run.log"
    echo "done: $done_n failed: $failed" >>"$dir/run.log"
    echo "$(basename "$dir"): $(tail -1 "$dir/run.log")"
}

begin() { # begin <실험> <설명>
    local dir=$OUT/$1
    mkdir -p "$dir"
    "$ROOT/scripts/collect_env.sh" >"$dir/env.txt" 2>&1
    { echo "experiment: $2"; echo "reps: $REPS"; echo "commit: $(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null)"; } >>"$dir/env.txt"
}

# 내용 검증: 실패하면 그 실험은 측정하지 않는다
verify_p1() { # verify_p1 <dir> <라벨> <환경변수> <ipc>
    local envs=$3; [ "$envs" = "-" ] && envs="" || envs=${envs//,/ }
    for m in pingpong throughput; do for s in 64 65537 1048576; do for c in 1 8; do
        # shellcheck disable=SC2086
        env $envs DURATION_MS=60 WARMUP_MS=20 "$ROOT/scripts/run_one.sh" "$4" "$m" "$s" "$c" 0 "$1/verify.csv" --verify \
            2>>"$1/errors.log" </dev/null || { echo "검증 실패: $2 $m $s $c" >&2; return 1; }
    done; done; done
}
verify_p2() { # verify_p2 <dir> <라벨> <환경변수> <ipc> <클라수>
    local envs=$3; [ "$envs" = "-" ] && envs="" || envs=${envs//,/ }
    for hit in 80 20; do
        # shellcheck disable=SC2086
        env $envs APP_TIMEOUT=60 "$ROOT/scripts/run_app_one.sh" "$4" "$5" "$hit" "$1/verify.csv" 0 \
            --verify --window 1 --duration-ms 500 --warmup-ms 100 2>>"$1/errors.log" </dev/null \
            || { echo "검증 실패: $2 $4 hit=$hit" >&2; return 1; }
    done
}

s1() {
    local dir=$OUT/s1_buffer
    begin s1_buffer "1단계 버퍼 용량: pipe 64KB / shm 512KB(64x8KB) / shm 64KB(8x8KB)"
    local V="pipe:-:pipe shm512:-:shm shm64:IPC_SHM_SLOTS=8,IPC_SHM_SLOT_SIZE=8192:shm"
    for v in $V; do IFS=: read -r label envs ipc <<<"$v"; verify_p1 "$dir" "$label" "$envs" "$ipc" || return 1; done
    for r in $(seq 1 "$REPS"); do for v in $V; do IFS=: read -r label envs ipc <<<"$v"
        for m in pingpong throughput; do for s in 64 1024 16384 262144 1048576; do for c in 1 8; do
            echo "$label $envs $ipc $m $s $c $r"
        done; done; done
    done; done | shuffle 20261110 >"$dir/plan.txt"
    run_plan "$dir" p1
}

s2() {
    local dir=$OUT/s2_sender
    begin s2_sender "송신 방식: direct(v1) / thread(v2), 같은 시간대에 섞어서"
    for snd in direct thread; do for ipc in pipe shm; do
        verify_p2 "$dir" "$snd" "APP_SENDER=$snd" "$ipc" 2 || return 1
    done; done
    for r in $(seq 1 "$REPS"); do for snd in direct thread; do for ipc in pipe shm; do for n in 2 4; do for hit in 80 20; do
        echo "$snd APP_SENDER=$snd $ipc $n $hit $r"
    done; done; done; done; done | shuffle 20261111 >"$dir/plan.txt"
    run_plan "$dir" p2
}

s3() {
    local dir=$OUT/s3_workers
    begin s3_workers "워커 수 8/12/14/16 (shm, 클라 4, 적중 80%, sender direct)"
    for w in 8 12 14 16; do verify_p2 "$dir" "w$w" "APP_WORKERS=$w" shm 2 || return 1; done
    for r in $(seq 1 "$REPS"); do for w in 8 12 14 16; do
        echo "w$w APP_WORKERS=$w shm 4 80 $r"
    done; done | shuffle 20261112 >"$dir/plan.txt"
    run_plan "$dir" p2
}

s4() {
    local dir=$OUT/s4_ring
    begin s4_ring "응용 shm 링 64슬롯(512KB) / 16슬롯(128KB) (sender thread, 클라 8)"
    local V="slots64:APP_SENDER=thread slots16:APP_SENDER=thread,IPC_SHM_SLOTS=16"
    for v in $V; do IFS=: read -r label envs <<<"$v"; verify_p2 "$dir" "$label" "$envs" shm 8 || return 1; done
    for r in $(seq 1 "$REPS"); do for v in $V; do IFS=: read -r label envs <<<"$v"; for hit in 80 20; do
        echo "$label $envs shm 8 $hit $r"
    done; done; done | shuffle 20261113 >"$dir/plan.txt"
    run_plan "$dir" p2
}

s5() {
    local dir=$OUT/s5_size
    begin s5_size "응답 크기 고정: mix / 16KB / 256KB / 1MB (pipe·shm, 클라 8, 적중 80%, sender direct)"
    local V="mix:- sz16:APP_SIZE_CLASS=16 sz256:APP_SIZE_CLASS=256 sz1024:APP_SIZE_CLASS=1024"
    for v in $V; do IFS=: read -r label envs <<<"$v"; for ipc in pipe shm; do
        verify_p2 "$dir" "$label" "$envs" "$ipc" 2 || return 1
    done; done
    for r in $(seq 1 "$REPS"); do for v in $V; do IFS=: read -r label envs <<<"$v"; for ipc in pipe shm; do
        echo "$label $envs $ipc 8 80 $r"
    done; done; done | shuffle 20261114 >"$dir/plan.txt"
    run_plan "$dir" p2
}

for e in ${SUPP:-S1 S2 S3 S4}; do
    case $e in
        S1) s1 ;; S2) s2 ;; S3) s3 ;; S4) s4 ;; S5) s5 ;;
        *) echo "모르는 실험: $e" >&2 ;;
    esac
done
python3 "$ROOT/scripts/summarize_supp.py" "$OUT" | tee "$OUT/summary.txt"
echo "완료: $OUT"
