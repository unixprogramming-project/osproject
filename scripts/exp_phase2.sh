#!/bin/bash
# 2단계 원인 확인 실험. 각 실험은 요인 하나만 바꾼다. 나머지는 본측정과 같다.
# 사용: TEAM=조번호 scripts/exp_phase2.sh <출력폴더> [반복=10]
#  E1 workers : 워커 수 1,2,4,8,16  (클라 4, 적중 80%)   -> 워커가 전송에 묶이는지
#  E2 hit     : 적중률 0,20,50,80,100 (클라 4, 워커 4)   -> 요청당 워커 시간 = 전송 + 미스율 x 필터 인지
#  E3 window  : 창 1, 4             (클라 2, 적중 80%)   -> 큐 대기를 없앴을 때 지연 차이
set -u
OUT=${1:?usage: exp_phase2.sh <out-dir> [reps]}
REPS=${2:-10}
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
if [ -e "$OUT" ]; then echo "이미 있는 폴더입니다: $OUT" >&2; exit 1; fi
mkdir -p "$OUT"
run() { # run <하위폴더> [환경 변수...]
    local sub=$1; shift
    env "$@" PHASE2_IPCS="pipe shm" "$ROOT/scripts/run_phase2.sh" "$OUT/$sub" "$REPS" >"$OUT/$sub.log" 2>&1
    echo "$sub: $(tail -1 "$OUT/$sub/run.log" 2>/dev/null)"
}
for w in 1 2 4 8 16; do run "workers_$w" APP_WORKERS=$w PHASE2_CLIENTS=4 PHASE2_HITS=80; done
for h in 0 20 50 80 100; do run "hit_$h" PHASE2_CLIENTS=4 PHASE2_HITS=$h; done
for win in 1 4; do run "window_$win" PHASE2_CLIENTS=2 PHASE2_HITS=80 PHASE2_CLIENT_ARGS="--window $win"; done
echo "완료: $OUT"
