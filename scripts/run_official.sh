#!/bin/bash
# 1단계 본측정 전체. 약 70분 걸린다. 측정 중에는 컴퓨터를 쓰지 않는다.
# 전원 연결, 다른 프로그램 종료, Docker Desktop 종료 후 실행한다.
# 소스가 Windows 쪽(/mnt/c)에 있으면 먼저 Linux 쪽으로 복사해서 실행한다.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT" || exit 1
case "$ROOT" in
    /mnt/*) echo "error: Linux 파일시스템으로 복사한 뒤 실행하세요 (예: ~/osproject)" >&2; exit 1 ;;
esac
OUT=${1:-results/phase1_$(date +%Y%m%d_%H%M)}
scripts/run_tests.sh || { echo "검증 실패: 측정하지 않습니다" >&2; exit 1; }
scripts/run_phase1.sh "$OUT" 30 || echo "일부 실행 실패: $OUT/errors.log 확인" >&2
scripts/exp_idle.sh "$OUT/idle" 30 || echo "유휴 실험 일부 실패: $OUT/idle/errors.log 확인" >&2
python3 scripts/summarize.py "$OUT/phase1.csv" "$OUT/summary.csv" | tee "$OUT/summary.txt"
for v in busy same_cpu; do
    python3 scripts/summarize.py "$OUT/idle/$v.csv" "$OUT/idle/${v}_summary.csv" > "$OUT/idle/${v}_summary.txt"
done
echo "완료: $OUT"
