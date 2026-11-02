#!/usr/bin/env python3
"""phase1.csv를 조건별로 요약한다. 표준 라이브러리만 쓴다.

회차(run)별 값을 먼저 구한 뒤 회차 사이의 통계를 낸다.
  latency: 회차별 mean/p50/p99/max -> 평균 mean, 중앙값 p50, 중앙값 p99, 최대 max
  throughput: 회차별 합계 처리량 -> 평균, 중앙값, 최소, 최대
사용: summarize.py <phase1.csv> [출력 csv]
"""
import csv
import statistics as st
import sys
from collections import defaultdict

IPC_ORDER = ["pipe", "mq", "shm"]


def fnum(row, key):
    v = row.get(key, "")
    return float(v) if v not in ("", None) else None


def load(path):
    with open(path, newline="") as f:
        return list(csv.DictReader(f))


def summarize(rows):
    groups = defaultdict(list)
    servers = defaultdict(list)
    for r in rows:
        if r["verify"] != "0":
            continue
        key = (r["mode"], int(r["msg_size"]), int(r["clients"]), r["ipc"])
        if r["role"] == "all":
            groups[key].append(r)
        elif r["role"] == "server":
            servers[key].append(r)
    out = []
    for key in sorted(groups):
        mode, size, clients, ipc = key
        g = groups[key]
        s = servers.get(key, [])
        d = {
            "mode": mode, "msg_size": size, "clients": clients, "ipc": ipc, "runs": len(g),
            "samples": int(sum(fnum(r, "n") for r in g)),
            "client_nvcsw_per_msg": st.median(fnum(r, "nvcsw") / max(fnum(r, "n"), 1) for r in g),
            "client_nivcsw_per_msg": st.median(fnum(r, "nivcsw") / max(fnum(r, "n"), 1) for r in g),
            "client_sys_us_per_msg": st.median(fnum(r, "sys_us") / max(fnum(r, "n"), 1) for r in g),
            "client_user_us_per_msg": st.median(fnum(r, "user_us") / max(fnum(r, "n"), 1) for r in g),
            "server_rss_kb": st.median(fnum(r, "rss_after_kb") for r in s) if s else "",
            "server_rss_delta_kb": st.median(fnum(r, "rss_after_kb") - fnum(r, "rss_before_kb") for r in s) if s else "",
            "client_rss_delta_kb": st.median(fnum(r, "rss_after_kb") - fnum(r, "rss_before_kb") for r in g),
        }
        if mode == "pingpong":
            d.update({
                "mean_us": st.mean(fnum(r, "mean_ns") for r in g) / 1e3,
                "p50_us": st.median(fnum(r, "p50_ns") for r in g) / 1e3,
                "p99_us": st.median(fnum(r, "p99_ns") for r in g) / 1e3,
                "max_us": max(fnum(r, "max_ns") for r in g) / 1e3,
                "p50_spread": spread([fnum(r, "p50_ns") for r in g]),
            })
        else:
            mb = [fnum(r, "mb_per_s") for r in g]
            mps = [fnum(r, "msgs_per_s") for r in g]
            d.update({
                "mb_per_s_mean": st.mean(mb), "mb_per_s_median": st.median(mb),
                "mb_per_s_min": min(mb), "mb_per_s_max": max(mb),
                "msgs_per_s_median": st.median(mps), "mb_spread": spread(mb),
            })
        out.append(d)
    return out


def spread(values):
    """회차 간 흔들림: (최대-최소)/중앙값"""
    m = st.median(values)
    return (max(values) - min(values)) / m if m else 0.0


def print_tables(summary):
    index = {(d["mode"], d["msg_size"], d["clients"], d["ipc"]): d for d in summary}
    sizes = sorted({d["msg_size"] for d in summary})
    clients = sorted({d["clients"] for d in summary})
    for mode, field, title in (("pingpong", "p50_us", "왕복 지연 p50 (us)"),
                               ("pingpong", "p99_us", "왕복 지연 p99 (us)"),
                               ("throughput", "mb_per_s_median", "합계 처리량 중앙값 (MB/s)")):
        print(f"\n## {title}")
        print(f"{'size':>9} {'cli':>3} " + " ".join(f"{i:>10}" for i in IPC_ORDER) + "  순위")
        for s in sizes:
            for c in clients:
                vals = {}
                for i in IPC_ORDER:
                    d = index.get((mode, s, c, i))
                    if d:
                        vals[i] = d[field]
                if not vals:
                    continue
                best_high = mode == "throughput"
                rank = sorted(vals, key=lambda k: vals[k], reverse=best_high)
                row = " ".join(f"{vals.get(i, float('nan')):10.1f}" for i in IPC_ORDER)
                print(f"{s:>9} {c:>3} {row}  {'>'.join(rank)}")


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    summary = summarize(load(sys.argv[1]))
    print_tables(summary)
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
