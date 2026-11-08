#!/bin/bash
# 2단계 응용 1회 실행: 서버 1개 + 클라이언트 N개(각각 별도 프로세스)를 동시에 띄운다.
# 사용: run_app_one.sh <pipe|shm> <클라수> <적중률%> [csv] [rep] [추가 클라 옵션...]
# 환경 변수: TEAM(조 번호), APP_WORKERS(기본 4), APP_CACHE_MB(기본 64), APP_TIMEOUT(초, 기본 60)
set -u
IPC=$1 N=$2 HIT=$3 CSV=${4:-} REP=${5:-0}
shift 5 2>/dev/null || shift $#
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN=$ROOT/build/release
TEAM=${TEAM:-0}
TO=${APP_TIMEOUT:-60}
csvarg=()
[ -n "$CSV" ] && csvarg=(--csv "$CSV" --rep "$REP")
"$BIN/app_${IPC}_server" --cleanup-only --team "$TEAM"
timeout "$TO" "$BIN/app_${IPC}_server" --clients "$N" --workers "${APP_WORKERS:-4}" --cache-mb "${APP_CACHE_MB:-64}" \
    --team "$TEAM" --hit "$HIT" "${csvarg[@]}" &
srv=$!
pids=()
for ((i = 0; i < N; i++)); do
    timeout "$TO" "$BIN/app_${IPC}_client" --id "$i" --clients "$N" --hit "$HIT" --team "$TEAM" "${csvarg[@]}" "$@" &
    pids+=($!)
done
rc=0
for p in "${pids[@]}"; do wait "$p" || rc=1; done
wait "$srv" || rc=1
if [ $rc -ne 0 ]; then
    "$BIN/app_${IPC}_server" --cleanup-only --team "$TEAM"
fi
exit $rc
