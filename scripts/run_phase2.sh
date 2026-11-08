#!/bin/bash
# 2단계 응용 본측정: pipe, shm × 클라 2/4/8 × 적중 80/20% × 반복 횟수
# 사용: TEAM=조번호 scripts/run_phase2.sh <출력폴더> [반복=30]
# 출력: phase2.csv (서버·클라 행, 열은 summarize_phase2.py 참고), run.log, errors.log, env.txt
# 추가 실험용 환경 변수: APP_WORKERS, APP_CACHE_MB, IPC_SHM_SLOTS, IPC_SHM_SLOT_SIZE (서버·클라 모두 같은 값)
#                       PHASE2_IPCS(기본 "pipe shm"), PHASE2_CLIENTS(기본 "2 4 8"), PHASE2_HITS(기본 "80 20")
set -u
OUT=${1:?usage: run_phase2.sh <out-dir> [reps]}
REPS=${2:-30}
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TEAM=${TEAM:-0}
case "$ROOT" in /mnt/*) echo "Linux 파일시스템으로 복사한 뒤 실행하세요 (/mnt 아래는 측정 불가)" >&2; exit 1 ;; esac
if [ -e "$OUT" ]; then echo "이미 있는 폴더입니다: $OUT (새 폴더를 지정하세요)" >&2; exit 1; fi
mkdir -p "$OUT"
CSV=$OUT/phase2.csv
make -s -C "$ROOT" apps || exit 1
"$ROOT/scripts/collect_env.sh" >"$OUT/env.txt" 2>&1
echo "APP_SENDER=${APP_SENDER:-direct} APP_WORKERS=${APP_WORKERS:-4} APP_CACHE_MB=${APP_CACHE_MB:-64} IPC_SHM_SLOTS=${IPC_SHM_SLOTS:-default} IPC_SHM_SLOT_SIZE=${IPC_SHM_SLOT_SIZE:-default}" >>"$OUT/env.txt"
git -C "$ROOT" rev-parse --short HEAD >>"$OUT/env.txt" 2>/dev/null

# 1. 내용 검증 (실패하면 측정하지 않는다)
for ipc in ${PHASE2_IPCS:-pipe shm}; do for hit in 80 20; do
    if ! TEAM=$TEAM APP_TIMEOUT=60 "$ROOT/scripts/run_app_one.sh" "$ipc" 2 "$hit" "$OUT/verify.csv" 0 \
        --verify --window 1 --duration-ms 500 --warmup-ms 100 2>>"$OUT/errors.log"; then
        echo "검증 실패: $ipc hit=$hit (errors.log 확인)" >&2
        exit 1
    fi
done; done
echo "verify ok"

# 2. 실행 계획: 조건 × 반복을 고정 시드로 섞는다
PLAN=$OUT/plan.txt
for r in $(seq 1 "$REPS"); do
    for ipc in ${PHASE2_IPCS:-pipe shm}; do for n in ${PHASE2_CLIENTS:-2 4 8}; do for hit in ${PHASE2_HITS:-80 20}; do
        echo "$ipc $n $hit $r"
    done; done; done
done | awk 'BEGIN{srand(2026)} {printf "%.10f %s\n", rand(), $0}' | sort -k1,1 | cut -d" " -f2- >"$PLAN"
total=$(wc -l <"$PLAN")
echo "runs: $total" | tee -a "$OUT/run.log"
echo "start $(date '+%F %T')" | tee -a "$OUT/run.log"
done_n=0 failed=0
while read -r ipc n hit r; do
    # PHASE2_CLIENT_ARGS: 추가 실험용 클라 옵션 (예: "--window 1"). 읽기 루프의 표준 입력을 뺏지 않게 </dev/null
    # shellcheck disable=SC2086
    if ! TEAM=$TEAM "$ROOT/scripts/run_app_one.sh" "$ipc" "$n" "$hit" "$CSV" "$r" ${PHASE2_CLIENT_ARGS:-} \
        2>>"$OUT/errors.log" </dev/null; then
        echo "fail: $ipc $n $hit $r" >>"$OUT/errors.log"
        failed=$((failed + 1))
    fi
    done_n=$((done_n + 1))
    if [ $((done_n % 20)) -eq 0 ]; then echo "$(date +%T) $done_n/$total failed=$failed" >>"$OUT/run.log"; fi
done <"$PLAN"
echo "end $(date '+%F %T')" >>"$OUT/run.log"
echo "done: $done_n failed: $failed" >>"$OUT/run.log"
tail -1 "$OUT/run.log"
python3 "$ROOT/scripts/summarize_phase2.py" "$CSV" "$OUT/summary.csv" | tee "$OUT/summary.txt"
