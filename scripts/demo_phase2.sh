#!/bin/bash
# 2단계 데모: 서버 1개 + 클라이언트 2개 동시 실행 -> 실행 중 스레드·IPC 자원 확인 -> 결과 -> 종료 후 자원 확인
# 사용: TEAM=조번호 scripts/demo_phase2.sh <pipe|shm> [sender=direct|thread]
set -u
IPC=${1:?usage: demo_phase2.sh <pipe|shm> [direct|thread]}
SENDER=${2:-thread}
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN=$ROOT/build/release
TEAM=${TEAM:-0}
CSV=$(mktemp /tmp/demo_XXXX.csv)
echo "## 1. 서버 실행: app_${IPC}_server --clients 2 --workers 4 --sender $SENDER --team $TEAM"
"$BIN/app_${IPC}_server" --clients 2 --workers 4 --sender "$SENDER" --team "$TEAM" --hit 80 --csv "$CSV" --rep 0 &
srv=$!
sleep 0.3
echo "## 2. 클라이언트 2개 동시 실행 (별도 프로세스, 적중 80%, 3초 측정, 응답 내용 검증)"
for i in 0 1; do
    "$BIN/app_${IPC}_client" --id $i --clients 2 --hit 80 --team "$TEAM" --duration-ms 3000 --warmup-ms 500 \
        --verify --window 1 --csv "$CSV" --rep 0 &
    pids[$i]=$!
done
sleep 1.5
echo "## 3. 실행 중 프로세스별 스레드 수 (/proc/PID/task 항목 수, main 포함)"
printf "%-8s %-22s %s\n" PID 이름 스레드수
for p in $srv ${pids[0]} ${pids[1]}; do
    printf "%-8s %-22s %s\n" "$p" "$(cat /proc/$p/comm)" "$(ls /proc/$p/task | wc -l)"
done
echo "## 4. 실행 중 IPC 자원"
if [ "$IPC" = shm ]; then ls -l /dev/shm | grep osproject_ || true; else ls -l /tmp | grep osproject_ || true; fi
wait "${pids[0]}" "${pids[1]}"
wait "$srv"
echo "server exit=$?"
echo "## 5. 결과 (클라이언트별: 3초 동안 끝낸 요청, 처리량, 지연, 잘못된 응답 수)"
awk -F, '$1=="client"{printf "client %s: 요청 %s건, %.0f 요청/s, %.1f MB/s, p50 %.1f us, p99 %.1f us, 잘못된 응답 %s\n",$6,$8,$11,$12,$14/1000,$15/1000,$22}
         $1=="server"{printf "server: 처리 %s건 (적중 %s, 미스 %s), 최대 RSS %.1f MB\n",$8,$9,$10,$23/1024}' "$CSV"
rm -f "$CSV"
echo "## 6. 종료 후 남은 IPC 자원 확인"
"$ROOT/scripts/check_ipc.sh"
