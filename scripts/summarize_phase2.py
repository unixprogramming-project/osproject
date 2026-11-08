#!/usr/bin/env python3
"""phase2.csv를 조건(ipc, 클라 수, 적중률)별로 요약한다. 표준 라이브러리만 쓴다.

회차별로 먼저 합친 뒤 회차 사이 통계를 낸다.
  처리량: 회차별 클라이언트 합계 (요청/s, MB/s) -> 중앙값, 최소, 최대
  지연: 회차별 클라이언트 p50·p99의 평균 -> 회차 사이 중앙값. 최대는 전체 최대. 평균은 클라 평균의 평균
  서버: 요청당 잠금 대기, 큐 대기, 송신 잠금 대기, 미스당 필터 시간, MB당 CPU, 요청당 컨텍스트 스위치, 최대 RSS
사용: summarize_phase2.py <phase2.csv> [출력 csv]
"""
import csv
import statistics as st
import sys
from collections import defaultdict

SERVER = ["role", "ipc", "clients", "hit", "rep", "workers", "cache_mb", "jobs", "hits", "misses", "evictions", "bytes",
          "lock_wait_ns", "queue_wait_ns", "send_wait_ns", "filter_ns", "user_us", "sys_us", "nvcsw", "nivcsw",
          "rss_before_kb", "rss_after_kb", "rss_peak_kb"]
CLIENT = ["role", "ipc", "clients", "hit", "rep", "id", "window", "n", "window_ns", "bytes", "rps", "mbps", "mean_ns",
          "p50_ns", "p99_ns", "max_ns", "user_us", "sys_us", "nvcsw", "nivcsw", "rss_kb", "bad", "verify"]


def load(path):
    srv, cli = defaultdict(list), defaultdict(list)
    with open(path, newline="") as f:
        for row in csv.reader(f):
            if not row:
                continue
            cols = SERVER if row[0] == "server" else CLIENT
            d = dict(zip(cols, row))
            for k, v in d.items():
                if k not in ("role", "ipc"):
                    d[k] = float(v)
            key = (d["ipc"], int(d["clients"]), int(d["hit"]))
            (srv if row[0] == "server" else cli)[key].append(d)
    return srv, cli


def summarize(srv, cli):
    out = []
    for key in sorted(cli):
        ipc, n, hit = key
        reps = defaultdict(list)
        for c in cli[key]:
            reps[int(c["rep"])].append(c)
        full = {r: cs for r, cs in reps.items() if len(cs) == n}
        rps = [sum(c["rps"] for c in cs) for cs in full.values()]
        mbps = [sum(c["mbps"] for c in cs) for cs in full.values()]
        p50 = [st.mean(c["p50_ns"] for c in cs) / 1e3 for cs in full.values()]
        p99 = [st.mean(c["p99_ns"] for c in cs) / 1e3 for cs in full.values()]
        mean = [st.mean(c["mean_ns"] for c in cs) / 1e3 for cs in full.values()]
        mx = max(c["max_ns"] for cs in full.values() for c in cs) / 1e3
        nreq = [sum(c["n"] for c in cs) for cs in full.values()]
        cli_cpu = [sum(c["user_us"] + c["sys_us"] for c in cs) / max(sum(c["n"] for c in cs), 1) for cs in full.values()]
        d = {"ipc": ipc, "clients": n, "hit": hit, "runs": len(full),
             "rps_median": st.median(rps), "rps_min": min(rps), "rps_max": max(rps),
             "mbps_median": st.median(mbps), "mbps_min": min(mbps), "mbps_max": max(mbps),
             "mean_us": st.mean(mean), "p50_us": st.median(p50), "p99_us": st.median(p99), "max_us": mx,
             "p50_spread": (max(p50) - min(p50)) / st.median(p50) if p50 else 0,
             "client_cpu_us_per_req": st.median(cli_cpu), "req_per_run": st.median(nreq)}
        s = srv.get(key, [])
        if s:
            def per(field, den):
                return st.median(r[field] / max(r[den], 1) for r in s)
            d.update({
                "hit_ratio": st.median(r["hits"] / max(r["jobs"], 1) for r in s),
                "lock_wait_us_per_req": per("lock_wait_ns", "jobs") / 1e3,
                "queue_wait_us_per_req": per("queue_wait_ns", "jobs") / 1e3,
                "send_wait_us_per_req": per("send_wait_ns", "jobs") / 1e3,
                "filter_us_per_miss": per("filter_ns", "misses") / 1e3,
                "srv_sys_us_per_mb": st.median(r["sys_us"] / max(r["bytes"] / 1e6, 1e-9) for r in s),
                "srv_user_us_per_mb": st.median(r["user_us"] / max(r["bytes"] / 1e6, 1e-9) for r in s),
                "srv_nvcsw_per_req": per("nvcsw", "jobs"),
                "srv_nivcsw_per_req": per("nivcsw", "jobs"),
                "srv_rss_peak_kb": st.median(r["rss_peak_kb"] for r in s),
                "srv_rss_after_kb": st.median(r["rss_after_kb"] for r in s),
            })
        out.append(d)
    return out


def table(summary, title, field, fmt="{:10.1f}"):
    idx = {(d["ipc"], d["clients"], d["hit"]): d for d in summary}
    ipcs = sorted({d["ipc"] for d in summary})
    print(f"\n## {title}")
    print(f"{'hit':>4} {'cli':>3} " + " ".join(f"{i:>10}" for i in ipcs) + "   shm/pipe")
    for hit in sorted({d["hit"] for d in summary}, reverse=True):
        for n in sorted({d["clients"] for d in summary}):
            vals = [idx.get((i, n, hit), {}).get(field) for i in ipcs]
            cells = " ".join(fmt.format(v) if v is not None else f"{'-':>10}" for v in vals)
            ratio = ""
            if "pipe" in ipcs and "shm" in ipcs:
                a, b = idx.get(("pipe", n, hit), {}).get(field), idx.get(("shm", n, hit), {}).get(field)
                if a and b:
                    ratio = f"{b / a:8.2f}"
            print(f"{hit:>4} {n:>3} {cells}   {ratio}")


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    srv, cli = load(sys.argv[1])
    summary = summarize(srv, cli)
    table(summary, "처리량 중앙값 (MB/s)", "mbps_median")
    table(summary, "처리량 중앙값 (요청/s)", "rps_median", "{:10.0f}")
    table(summary, "요청 지연 p50 (us)", "p50_us")
    table(summary, "요청 지연 p99 (us)", "p99_us")
    table(summary, "캐시 적중률", "hit_ratio", "{:10.3f}")
    table(summary, "요청당 캐시 잠금 대기 (us)", "lock_wait_us_per_req", "{:10.2f}")
    table(summary, "요청당 작업 큐 대기 (us)", "queue_wait_us_per_req")
    table(summary, "요청당 송신 잠금 대기 (us)", "send_wait_us_per_req")
    table(summary, "미스당 필터 연산 (us)", "filter_us_per_miss")
    table(summary, "서버 sys CPU (us/MB)", "srv_sys_us_per_mb")
    table(summary, "서버 user CPU (us/MB)", "srv_user_us_per_mb")
    table(summary, "서버 자발적 CS (회/요청)", "srv_nvcsw_per_req", "{:10.2f}")
    table(summary, "서버 최대 RSS (KB)", "srv_rss_peak_kb", "{:10.0f}")
    table(summary, "클라 CPU (us/요청)", "client_cpu_us_per_req")
    if len(sys.argv) > 2:
        fields = []
        for d in summary:
            for k in d:
                if k not in fields:
                    fields.append(k)
        with open(sys.argv[2], "w", newline="") as f:
            w = csv.DictWriter(f, fieldnames=fields)
            w.writeheader()
            w.writerows(summary)
    return 0


if __name__ == "__main__":
    sys.exit(main())
