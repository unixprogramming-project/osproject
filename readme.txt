UNIX시스템프로그래밍 설계과제: 내 응용에 맞는 IPC는 무엇인가

[현재 상태]
1단계 벤치마크 구현 완료. 2단계 응용은 1단계 본측정과 성능 예측 작성 후 구현한다.

[실행 환경]
Linux (Ubuntu 권장, WSL2 가능), gcc, make, python3

[컴파일]
make                 전체 컴파일
make bench_pipe      pipe 버전만
make bench_mq        message queue 버전만
make bench_shm       shared memory 버전만
make clean           빌드 결과 삭제
make tar TEAM=조번호  제출용 tar 생성

[직접 실행]
터미널 1: build/release/bench_shm_server --clients 2 --mode pingpong --team 조번호
터미널 2: build/release/bench_shm_client --clients 2 --size 1024 --mode pingpong --team 조번호
pipe, mq도 같은 옵션을 쓴다. 결과는 CSV 한 줄씩 출력된다. 서버는 측정이 끝나면 스스로 종료한다.
서버 --clients와 클라이언트 --clients는 같은 값이어야 한다.

[측정 재현]
scripts/run_tests.sh                   내용 검증, 자원 정리, sanitizer 검사
scripts/run_official.sh [출력폴더]      1단계 본측정 전체 (약 80분)
scripts/summarize.py <csv> [출력csv]    조건별 요약
scripts/exp_idle.sh <폴더> [반복]       코어 유휴 진입 영향 추가 실험
scripts/exp_sensitivity.sh <폴더> [반복] 버퍼 구성 추가 실험
scripts/exp_strace.sh <폴더>            시스템 콜 횟수 확인 (strace 필요)
scripts/check_ipc.sh                   종료 후 IPC 자원 확인

본측정 전 준비: 전원 연결, 다른 프로그램과 Docker Desktop 종료.
WSL2에서는 소스를 Linux 쪽(예: ~/osproject)에 두고 실행한다. /mnt/c에서는 run_official.sh가 실행을 거부한다.

[환경 변수: 서버 쪽에서 지정, 추가 실험용]
IPC_MQ_DEPTH (기본 5), IPC_MQ_MSGSIZE (기본 8192)
IPC_SHM_SLOTS (기본 64), IPC_SHM_SLOT_SIZE (기본 8192)

[IPC 이름]
POSIX: /osproject_조번호_c클라이언트번호_용도 (예: /osproject_11_c0_req)
FIFO:  /tmp/osproject_조번호_c클라이언트번호_req, _resp

[비정상 종료 후 정리]
build/release/bench_shm_server --cleanup-only --team 조번호
(pipe, mq도 같은 방법. 버전별로 실행한다)

[문서]
docs/setup-guide.md         팀 준비와 측정 절차 (먼저 읽는다)
docs/code-guide.md          코드 구조와 읽는 순서
docs/decisions.md           설계 결정과 근거
docs/requirements-check.md  과제 요구사항 점검표
docs/phase2-design.md       2단계 응용 설계안
