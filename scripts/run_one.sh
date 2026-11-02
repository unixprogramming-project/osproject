#!/bin/bash
# 조건 하나를 1회 실행한다. 서버와 클라이언트를 새로 띄우고 끝나면 IPC 자원이 남았는지 확인한다.
# 사용: run_one.sh <pipe|mq|shm> <pingpong|throughput> <크기> <클라이언트 수> <회차> <csv> [추가 클라이언트 옵션...]
# 환경 변수: TEAM(기본 0), BIN(빌드 폴더), DURATION_MS, WARMUP_MS, RUN_TIMEOUT
set -u
ipc=$1; mode=$2; size=$3; clients=$4; rep=$5; csv=$6; shift 6
TEAM=${TEAM:-0}
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN=${BIN:-$ROOT/build/release}
DURATION_MS=${DURATION_MS:-500}
WARMUP_MS=${WARMUP_MS:-100}
RUN_TIMEOUT=${RUN_TIMEOUT:-60}
server=$BIN/bench_${ipc}_server
client=$BIN/bench_${ipc}_client

verify=""
for a in "$@"; do [ "$a" = "--verify" ] && verify="--verify"; done

"$server" --cleanup-only --team "$TEAM"
timeout "$RUN_TIMEOUT" "$server" --clients "$clients" --mode "$mode" --team "$TEAM" --csv "$csv" \
    --size "$size" --rep "$rep" $verify &
srv=$!
timeout "$RUN_TIMEOUT" "$client" --clients "$clients" --size "$size" --mode "$mode" --team "$TEAM" \
    --duration-ms "$DURATION_MS" --warmup-ms "$WARMUP_MS" --csv "$csv" --rep "$rep" "$@"
c_status=$?
if [ $c_status -ne 0 ]; then
    kill -TERM "$srv" 2>/dev/null
fi
wait "$srv"
s_status=$?

left=$(ls /dev/mqueue /dev/shm /tmp 2>/dev/null | grep -c "^osproject_${TEAM}_")
if [ "$left" -ne 0 ]; then
    echo "LEFTOVER IPC: $ipc $mode $size $clients" >&2
    "$server" --cleanup-only --team "$TEAM"
    exit 3
fi
if [ $c_status -ne 0 ] || [ $s_status -ne 0 ]; then
    echo "FAILED: $ipc $mode size=$size clients=$clients rep=$rep client=$c_status server=$s_status" >&2
    exit 1
fi
exit 0
