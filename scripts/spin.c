/*
 * 원인 확인 실험용 부하. 낮은 우선순위로 CPU를 계속 돌려 코어가 유휴 상태로 내려가지 않게 한다.
 * 사용: spin <프로세스 수> (시그널로 종료)
 */
#include <stdlib.h>
#include <sys/resource.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    int n = argc > 1 ? atoi(argv[1]) : 1;
    for (int i = 1; i < n; i++) {
        if (fork() == 0)
            break;
    }
    setpriority(PRIO_PROCESS, 0, 19);
    for (volatile unsigned long x = 0;; x++) {
    }
    return 0;
}
