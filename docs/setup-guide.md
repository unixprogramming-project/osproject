# 팀 준비와 측정 가이드

## 1. 팀 준비

### 할 일
| 순서 | 할 일 | 기한·기준 |
|---|---|---|
| 1 | 3인 팀 구성, 팀명·팀장 결정, e-class 등록 | **11/10(화)** |
| 2 | 조 번호 확인 | 이름 규칙과 tar 파일명에 쓴다 |
| 3 | 담당 IPC 결정 | pipe, message queue, shared memory를 1명씩 |
| 4 | 담당 코드 읽기 | [code-guide.md](code-guide.md)의 담당자 질문에 답할 수 있을 것 |
| 5 | 저장소 기여 | 각자 자기 계정으로 커밋 |

### 담당 기록표
| 이름 | 역할 | 담당 IPC | 구현 외 담당 |
|---|---|---|---|
| | 팀장 | | |
| | | | |
| | | | |

구현 외 담당 예시: 측정 실행, 그래프, 보고서 정리, 발표자료, 동영상. 보고서 7장에 그대로 쓰므로 실제 맡은 일을 적는다.

### 조 번호를 쓰는 곳
- 측정: `TEAM=조번호 scripts/run_official.sh`
- 직접 실행: `--team 조번호`
- 제출: `make tar TEAM=조번호`

POSIX IPC 이름은 자동으로 `/osproject_조번호_c0_req` 형식이 된다. **본측정 전에 조 번호를 정한다.**

### 커밋 메일
`git config user.email`을 GitHub noreply 주소로 설정한다. 저장소에 실제 메일이 남지 않게 하기 위해서다.

---

## 2. 측정 가이드

### 원칙
- **모든 측정은 측정용 컴퓨터 1대에서 한다.** 1단계와 2단계 3개 버전 모두 같은 컴퓨터다. 측정용 컴퓨터는 팀원 데스크탑으로 한다.
- 측정용 컴퓨터를 바꾸면 1단계부터 다시 잰다.
- 측정 뒤 전송 모듈이나 벤치마크 코드를 고치면 다시 잰다.
- 보고서에는 이 절차로 나온 CSV 값만 쓴다. `docs/decisions.md`의 수치는 다른 컴퓨터의 예비 측정이므로 쓰지 않는다.
- `--verify` 실행은 측정값으로 쓰지 않는다.

### 2.1 환경 준비 (처음 1회)
Linux 네이티브나 VM이면 3번부터 한다.

1. Windows면 PowerShell에서 WSL과 Ubuntu를 설치한다.
   ```powershell
   wsl --install -d Ubuntu
   ```
2. Ubuntu 터미널을 연다.
3. 도구를 설치한다.
   ```bash
   sudo apt update
   sudo apt install -y build-essential strace python3 git
   ```
4. **Linux 쪽 폴더**에 받는다. `/mnt/c` 아래에서는 측정이 거부된다.
   ```bash
   cd ~
   git clone https://github.com/unixprogramming-project/osproject.git
   cd osproject
   ```
   비공개 저장소라 로그인이 필요하다. 비밀번호 대신 GitHub 개인 액세스 토큰을 쓴다.

### 2.2 측정 전 확인
- [ ] 코드 최신 상태: `git pull`, `git status`에 변경 없음
- [ ] 조 번호 확인
- [ ] 브라우저, 메신저, 게임, Docker Desktop 등 다른 프로그램 종료
- [ ] Windows 절전 끔, 예약 업데이트나 백신 검사가 없는 시간
- [ ] 온도와 소음을 일정하게 둘 수 있는 곳 (측정 중 사용 금지)

### 2.3 본측정 (약 70분, 컴퓨터 속도에 따라 다름)
터미널이 닫혀도 계속 돌도록 `nohup`으로 실행한다. WSL은 터미널 창을 모두 닫으면 꺼질 수 있으니 **창은 열어 둔다.**

```bash
cd ~/osproject
chmod +x scripts/*.sh scripts/summarize.py
TEAM=조번호 nohup scripts/run_official.sh results/phase1 > official.log 2>&1 &
tail -f official.log
```

`tail`은 Ctrl+C로 빠져나와도 측정은 계속된다. **측정 중에는 다른 작업을 하지 않는다.**

실행 내용
1. 검증 전체 (실패하면 측정하지 않고 멈춘다)
2. 1단계: 3 IPC × 2 모드 × 크기 5 × 클라이언트 4 × 30회 = 3600회
3. 추가 실험: 클라이언트 1개에서 코어 유휴 차단, 한 코어 고정 각 900회
4. 요약 파일 생성

### 2.4 끝난 뒤 결과 확인
```bash
tail -3 official.log
cat results/phase1/run.log | tail -2
wc -l results/phase1/phase1.csv results/phase1/idle/busy.csv results/phase1/idle/same_cpu.csv
cat results/phase1/errors.log results/phase1/idle/errors.log 2>/dev/null | head
scripts/check_ipc.sh
```

| 확인 항목 | 정상 값 |
|---|---|
| `run.log` 마지막 줄 | `done: 3600 failed: 0` |
| `phase1.csv` 줄 수 | 20701 |
| `busy.csv`, `same_cpu.csv` 줄 수 | 각 2701 |
| `errors.log` | 비어 있음 |
| `check_ipc.sh` | `osproject_` 항목 없음 |

**줄 수 계산:** 조건별로 클라이언트 행, 합계 행, 서버 행이 나오므로 1회 반복에 690행이다. 30회면 20700행, 머리글까지 20701행이다.

실패가 있으면 오류 내용을 확인하고 원인을 고친 뒤 **새 폴더에 전체를 다시 측정한다.** 실패한 조건만 골라 다시 재서 합치지 않는다.

### 2.5 시스템 콜 근거 수집 (시간 측정과 별도)
`strace`는 실행을 크게 느리게 하므로 횟수 근거로만 쓰고, 여기서 나온 시간은 쓰지 않는다.
```bash
scripts/exp_strace.sh results/phase1/strace
```

### 2.6 결과 해석과 공유
```bash
cat results/phase1/summary.txt
cat results/phase1/idle/busy_summary.txt
cat results/phase1/idle/same_cpu_summary.txt
```

확인할 내용
- 조건별 순위와 순위가 바뀌는 지점
- 회차 사이 흔들림: `summary.csv`의 `p50_spread`, `mb_spread`. 값이 크면 측정 중 간섭을 의심한다
- 클라이언트 1개의 기본 결과와 `busy`, `same_cpu` 결과 차이
- 이 컴퓨터의 제한값: `results/phase1/env.txt`의 `mq_msgsize_max`, `rlimit_msgqueue`

공유
```bash
git add results/phase1 official.log
git commit -m "1단계 본측정 결과 추가"
git push
```

### 2.7 측정 다음 순서
1. 결과를 팀이 함께 보고 해석한다.
2. `docs/phase2-design.md`의 워크로드를 확정한다.
3. **응용 구현 전에** 성능 예측을 `docs/prediction.md`에 쓰고 커밋한다.
   ```bash
   git add docs/prediction.md
   git commit -m "구현 전 성능 예측 작성"
   git tag prediction-v1
   git push origin main --tags
   ```
4. 그다음 응용을 구현한다.

### 문제 해결
| 증상 | 조치 |
|---|---|
| `Linux 파일시스템으로 복사한 뒤 실행하세요` | `~/osproject`에서 실행한다 |
| `Too many open files` (mq) | `ulimit -q` 값을 확인한다. 기본 819200에서 큐 깊이 5로 클라이언트 8개가 동작한다 |
| 실행이 멈춘 뒤 자원이 남음 | `build/release/bench_mq_server --cleanup-only --team 조번호` (pipe, shm도 각각) |
| `Permission denied` | `chmod +x scripts/*.sh` |
| sanitizer 검사만 실패 | 출력 내용을 팀에 공유한다. 측정은 진행하지 않는다 |
