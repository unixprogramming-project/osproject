#!/bin/bash
# 보고서 2.1에 넣을 측정 환경을 기록한다.
echo "date: $(date '+%F %T %z')"
echo "cpu: $(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2 | sed 's/^ //')"
echo "logical_cpus: $(nproc)"
lscpu 2>/dev/null | grep -E '^(Thread|Core|Socket)\(s\)|Hypervisor vendor|Virtualization type'
echo "memory: $(free -h | awk 'NR==2{print $2}')"
if [ -r /etc/os-release ]; then . /etc/os-release; echo "os: $PRETTY_NAME"; fi
echo "kernel: $(uname -r)"
echo "gcc: $(gcc --version | head -1)"
echo "make: $(make --version | head -1)"
case "$(uname -r)" in
    *microsoft*|*WSL*) echo "virtualization: WSL2" ;;
    *) echo "virtualization: $(systemd-detect-virt 2>/dev/null || echo unknown)" ;;
esac
echo "clocksource: $(cat /sys/devices/system/clocksource/clocksource0/current_clocksource 2>/dev/null)"
echo "mq_msg_max: $(cat /proc/sys/fs/mqueue/msg_max)"
echo "mq_msgsize_max: $(cat /proc/sys/fs/mqueue/msgsize_max)"
echo "rlimit_msgqueue: $(ulimit -q)"
echo "PIPE_BUF: $(getconf PIPE_BUF /)"
echo "pipe_capacity: $(python3 -c 'import fcntl,os; r,w=os.pipe(); print(fcntl.fcntl(w,1032))' 2>/dev/null)"
echo "env IPC_MQ_DEPTH=${IPC_MQ_DEPTH:-default} IPC_MQ_MSGSIZE=${IPC_MQ_MSGSIZE:-default} IPC_SHM_SLOTS=${IPC_SHM_SLOTS:-default} IPC_SHM_SLOT_SIZE=${IPC_SHM_SLOT_SIZE:-default}"
