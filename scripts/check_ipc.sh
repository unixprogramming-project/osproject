#!/bin/bash
# 실행 종료 후 IPC 자원이 남았는지 확인한다. 보고서 5.3 캡처용.
# ipcs는 System V 자원만 보여 주므로 POSIX 자원과 FIFO 위치도 함께 확인한다.
echo "== ipcs"
ipcs
echo "== POSIX message queue (/dev/mqueue)"
ls -la /dev/mqueue
echo "== POSIX shared memory (/dev/shm)"
ls -la /dev/shm
echo "== FIFO (/tmp/osproject_*)"
ls -la /tmp/osproject_* 2>/dev/null || echo "없음"
