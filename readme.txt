UNIX시스템프로그래밍 설계과제 (3조): 내 응용에 맞는 IPC는 무엇인가
응용: 이미지 필터 캐시 서버 (2인 팀: 2단계는 pipe, shared memory 2가지 버전)

[현재 상태]
1단계 벤치마크(pipe, mq, shm)와 본측정 완료.
2단계 응용(pipe, shm) 구현, 본측정, 원인 확인 실험, 설계 개선(v2)과 전후 측정 완료.

[실행 환경]
Linux (Ubuntu 권장, WSL2 가능), gcc, make, python3 (그래프는 python3-matplotlib, 캡처는 fonts-nanum-coding)
측정 컴퓨터: Intel Core Ultra 5 245K, WSL2 Ubuntu 24.04.5, kernel 6.6.87.2, gcc 13.3.0

[컴파일]
make                 전체 컴파일 (1단계 벤치마크 + 검증 프로그램 + 2단계 응용)
make bench_pipe      1단계 pipe 버전만 (bench_mq, bench_shm 동일)
make app_pipe        2단계 응용 pipe 버전만
make app_shm         2단계 응용 shared memory 버전만
make tsan / asan     ThreadSanitizer / AddressSanitizer 빌드 (build/tsan, build/asan)
make clean           빌드 결과 삭제
make tar TEAM=조번호  제출용 tar 생성 (osproject_조번호.tar)

[1단계 직접 실행]
터미널 1: build/release/bench_shm_server --clients 2 --mode pingpong --team 3
터미널 2: build/release/bench_shm_client --clients 2 --size 1024 --mode pingpong --team 3
pipe, mq도 같은 옵션을 쓴다. 서버 --clients와 클라이언트 --clients는 같은 값이어야 한다.

[2단계 직접 실행] (클라이언트는 각각 별도 프로세스, 모두 접속하면 동시에 시작)
터미널 1: build/release/app_shm_server --clients 2 --team 3 [--workers 4] [--cache-mb 64] [--sender direct|thread]
터미널 2: build/release/app_shm_client --id 0 --clients 2 --hit 80 --team 3 [--verify --window 1]
터미널 3: build/release/app_shm_client --id 1 --clients 2 --hit 80 --team 3 [--verify --window 1]
  --sender direct  워커가 직접 전송 (v1, 처음 설계)
  --sender thread  클라이언트별 송신 스레드 (v2, 설계 개선)
  --verify         응답 내용을 기대 결과와 비교 (--window 1과 함께 쓴다, 측정에는 쓰지 않음)
  --csv 파일 --rep 번호  결과를 CSV 한 줄로 기록
데모 한 번에 실행: TEAM=3 scripts/demo_phase2.sh shm thread   (pipe도 같음)

[측정 재현]
1단계
  scripts/run_official.sh [출력폴더]        1단계 본측정 (약 35분) + 추가 실험 (합계 약 65분)
  scripts/exp_strace.sh <폴더>              시스템 콜 횟수 (strace 필요)
  scripts/plot_phase1.py <결과폴더>          그래프·표
2단계
  TEAM=3 scripts/run_phase2.sh <폴더> [30]                 본측정 (pipe·shm × 클라 2/4/8 × 적중 80/20%, 약 22분)
  APP_SENDER=thread TEAM=3 scripts/run_phase2.sh <폴더> 30  설계 개선 v2 본측정
  TEAM=3 scripts/exp_phase2.sh <폴더> [10]                 원인 확인 실험 (워커 수, 적중률, 창)
  APP_SENDER=thread APP_CACHE_MB=16 PHASE2_CLIENTS=4 TEAM=3 scripts/run_phase2.sh <폴더> 10   캐시 용량 실험
  scripts/plot_phase2.py <결과폴더> [--compare <비교폴더> 라벨A 라벨B]   그래프·표
공통
  scripts/check_ipc.sh                     종료 후 IPC 자원 확인 (ipcs, /dev/mqueue, /dev/shm, FIFO)
  scripts/capture_phase1.sh, capture_phase2.sh   보고서 캡처

측정 원자료: results/phase1, results/phase2 (v1), results/phase2_v2 (v2), results/phase2_exp, results/phase2_mem
본측정 전 준비: 전원 연결, 다른 프로그램과 Docker Desktop 종료.
WSL2에서는 소스를 Linux 쪽(예: ~/osproject)에 두고 실행한다. /mnt 아래에서는 측정 스크립트가 실행을 거부한다.

[환경 변수: 추가 실험용]
IPC_MQ_DEPTH (기본 5), IPC_MQ_MSGSIZE (기본 8192)          1단계 서버 쪽
IPC_SHM_SLOTS (기본 64), IPC_SHM_SLOT_SIZE (기본 8192)     서버 쪽
APP_WORKERS (기본 4), APP_CACHE_MB (기본 64), APP_SENDER (기본 direct)   2단계 스크립트

[IPC 이름]
POSIX shm: /osproject_조번호_c클라이언트번호_shm (예: /osproject_3_c0_shm)
POSIX mq:  /osproject_조번호_c클라이언트번호_req, _resp
FIFO:      /tmp/osproject_조번호_c클라이언트번호_req, _resp

[비정상 종료 후 정리]
build/release/app_shm_server --cleanup-only --team 3   (app_pipe_server, bench_*_server도 같은 방법)
서버는 SIGINT/SIGTERM을 받으면 시그널 스레드가 IPC 자원을 지우고 종료한다.

[문서]
docs/setup-guide.md         팀 준비와 측정 절차
docs/code-guide.md          코드 구조와 읽는 순서
docs/decisions.md           1단계 설계 결정과 근거
docs/requirements-check.md  과제 요구사항 점검표
docs/phase2-design.md       2단계 응용 설계안
docs/prediction.md          구현 전 성능 예측 (태그 prediction-v1, 구현 전 커밋)
