#!/bin/bash
# 측정 전에 돌리는 검증. 내용 확인, 경쟁 상태, 자원 정리, 비정상 종료를 확인한다.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT" || exit 1
TEAM=${TEAM:-0}; export TEAM
BIN=$ROOT/build/release
pass=0; fail=0
check() { if [ "$2" -eq 0 ]; then pass=$((pass + 1)); echo "ok   $1"; else fail=$((fail + 1)); echo "FAIL $1"; fi; }
residual() { ls /dev/mqueue /dev/shm /tmp 2>/dev/null | grep -c "^osproject_${TEAM}_"; }

make -j8 >/dev/null || exit 1

echo "== 1. 양방향 경계값 전송"
for i in pipe mq shm; do
    timeout 180 "$BIN/duplex_$i" 3 >/dev/null; check "duplex $i" $?
done

echo "== 2. 내용 검증 벤치마크 (크기 경계, 클라이언트 1/2/8)"
TMP=$(mktemp -d)
for i in pipe mq shm; do for m in pingpong throughput; do for s in 8 4096 8188 8189 8192 65536 65537 1048576; do for c in 1 2 8; do
    DURATION_MS=60 WARMUP_MS=20 "$ROOT/scripts/run_one.sh" "$i" "$m" "$s" "$c" 0 "$TMP/verify.csv" --verify
    rc=$?
    if [ $rc -ne 0 ]; then check "verify $i $m size=$s clients=$c" $rc; fi
done; done; done; done
check "verify 전체 실행 후 잔여 자원 0" "$(residual)"

echo "== 3. 측정 중 서버에 SIGINT/SIGTERM → 자원 정리"
for i in pipe mq shm; do for sig in INT TERM; do
    "$BIN/bench_${i}_server" --clients 2 --team "$TEAM" &
    srv=$!
    timeout 3 "$BIN/bench_${i}_client" --clients 2 --size 4096 --team "$TEAM" --duration-ms 5000 --warmup-ms 1 >/dev/null 2>&1 &
    cl=$!
    sleep 0.3
    kill -"$sig" "$srv"
    wait "$srv" 2>/dev/null
    wait "$cl" 2>/dev/null
    check "$i SIG$sig 후 잔여 자원 0" "$(residual)"
    sleep 0.1
    check "$i SIG$sig 후 남은 프로세스 0" "$(pgrep -c '^bench_' || true)"
done; done

echo "== 4. 서버 SIGKILL 뒤 --cleanup-only로 복구"
for i in pipe mq shm; do
    "$BIN/bench_${i}_server" --clients 2 --team "$TEAM" &
    srv=$!
    sleep 0.2
    kill -KILL "$srv"; wait "$srv" 2>/dev/null
    "$BIN/bench_${i}_server" --cleanup-only --team "$TEAM"
    check "$i SIGKILL 복구" "$(residual)"
done

echo "== 5. 메시지 큐 최대 클라이언트 자원 한도"
"$BIN/bench_mq_server" --clients 8 --team "$TEAM" &
srv=$!; sleep 0.2
kill -0 "$srv" 2>/dev/null; check "mq 클라이언트 8개(큐 16개) 생성" $?
kill -TERM "$srv" 2>/dev/null; wait "$srv" 2>/dev/null

echo "== 6. Sanitizer"
make -j8 tsan >/dev/null 2>&1; check "tsan 빌드" $?
make -j8 asan >/dev/null 2>&1; check "asan 빌드" $?
for i in pipe mq shm; do
    timeout 300 setarch "$(uname -m)" -R build/tsan/duplex_$i 1 >/dev/null 2>"$TMP/tsan_$i.log"; rc=$?
    [ $rc -ne 0 ] && head -30 "$TMP/tsan_$i.log"
    check "tsan duplex $i" $rc
    timeout 300 build/asan/duplex_$i 1 >/dev/null 2>"$TMP/asan_$i.log"; rc=$?
    [ $rc -ne 0 ] && head -30 "$TMP/asan_$i.log"
    check "asan duplex $i" $rc
done
rm -rf "$TMP"
echo "pass=$pass fail=$fail"
[ "$fail" -eq 0 ]
