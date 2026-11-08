#!/usr/bin/env python3
"""2단계 요약(summary.csv)으로 보고서용 그래프와 표를 만든다.

사용: plot_phase2.py <결과폴더> [출력폴더] [--compare <다른결과폴더> <라벨A> <라벨B>]
  <결과폴더>/summary.csv 필수 (run_phase2.sh가 만든 요약)
출력 (기본 <결과폴더>/figures):
  fig5_throughput.png  클라 수별 처리량 (적중 80/20%, pipe·shm 한 그래프)
  fig6_latency.png     클라 수별 요청 지연 p50 실선 / p99 점선
  fig7_breakdown.png   서버 요청당 시간 분해 (큐 대기, 송신 잠금 대기, 캐시 잠금 대기)와 MB당 CPU
  fig8_memory.png      서버 최대 RSS
  fig9_compare.png     (--compare) 개선 전후 처리량·지연 비교
  tables.md            보고서용 표
"""
import csv
import os
import sys

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402

IPC = ["pipe", "shm"]
LABEL = {"pipe": "pipe (FIFO)", "shm": "POSIX shared memory"}
COLOR = {"pipe": "#1f77b4", "shm": "#2ca02c"}
MARK = {"pipe": "o", "shm": "^"}


def load(path):
    out = {}
    with open(path, newline="") as f:
        for r in csv.DictReader(f):
            key = (r["ipc"], int(r["clients"]), int(r["hit"]))
            out[key] = {k: (float(v) if v not in ("", None) and k != "ipc" else v) for k, v in r.items()}
    return out


def clients_axis(ax, clients):
    ax.set_xscale("log", base=2)
    ax.set_xticks(clients)
    ax.set_xticklabels([str(c) for c in clients])
    ax.minorticks_off()
    ax.grid(True, alpha=0.3)
    ax.set_xlabel("concurrent clients")


def fig_throughput(d, clients, hits, out):
    fig, axs = plt.subplots(1, len(hits), figsize=(6 * len(hits), 4.5))
    for ax, h in zip(axs, hits):
        for i in IPC:
            cs = [c for c in clients if (i, c, h) in d]
            med = [d[(i, c, h)]["mbps_median"] for c in cs]
            lo = [m - d[(i, c, h)]["mbps_min"] for c, m in zip(cs, med)]
            hi = [d[(i, c, h)]["mbps_max"] - m for c, m in zip(cs, med)]
            ax.errorbar(cs, med, yerr=[lo, hi], marker=MARK[i], color=COLOR[i], capsize=3, label=LABEL[i])
        clients_axis(ax, clients)
        ax.set_ylim(bottom=0)
        ax.set_ylabel("total throughput (MB/s)")
        ax.set_title(f"cache hit {h}%")
    axs[0].legend()
    fig.suptitle("Phase 2: image filter cache server throughput (median, bar = min..max of runs)")
    fig.tight_layout()
    fig.savefig(out, dpi=150)
    plt.close(fig)


def fig_latency(d, clients, hits, out):
    fig, axs = plt.subplots(1, len(hits), figsize=(6 * len(hits), 4.5))
    for ax, h in zip(axs, hits):
        for i in IPC:
            cs = [c for c in clients if (i, c, h) in d]
            ax.plot(cs, [d[(i, c, h)]["p50_us"] / 1e3 for c in cs], marker=MARK[i], color=COLOR[i], label=f"{LABEL[i]} p50")
            ax.plot(cs, [d[(i, c, h)]["p99_us"] / 1e3 for c in cs], ls="--", marker=MARK[i], mfc="none", color=COLOR[i],
                    label=f"{LABEL[i]} p99")
        clients_axis(ax, clients)
        ax.set_ylim(bottom=0)
        ax.set_ylabel("request latency (ms)")
        ax.set_title(f"cache hit {h}%")
    axs[0].legend(fontsize=8)
    fig.suptitle("Phase 2: request latency (send -> full response received)")
    fig.tight_layout()
    fig.savefig(out, dpi=150)
    plt.close(fig)


def fig_breakdown(d, clients, hits, out):
    parts = [("queue_wait_us_per_req", "job queue wait"), ("send_wait_us_per_req", "send lock wait"),
             ("lock_wait_us_per_req", "cache lock wait")]
    fig, axs = plt.subplots(1, 2, figsize=(13, 4.5))
    labels, x = [], 0
    for h in hits:
        for c in clients:
            for i in IPC:
                r = d.get((i, c, h))
                if not r:
                    continue
                bottom = 0
                for k, (f, name) in enumerate(parts):
                    v = r.get(f) or 0
                    axs[0].bar(x, v, bottom=bottom, color=["#9467bd", "#ff7f0e", "#8c564b"][k],
                               edgecolor=COLOR[i], linewidth=1.5, label=name if x == 0 else None)
                    bottom += v
                labels.append(f"{i}\n{c}c/{h}%")
                x += 1
    axs[0].set_xticks(range(len(labels)))
    axs[0].set_xticklabels(labels, fontsize=7)
    axs[0].set_ylabel("server wait time per request (µs)")
    axs[0].set_title("where requests wait inside the server")
    axs[0].legend(fontsize=8)
    w = 0.35
    keys = [(c, h) for h in hits for c in clients]
    for k, i in enumerate(IPC):
        xs = [j + (k - 0.5) * w for j in range(len(keys))]
        sysv = [d.get((i, c, h), {}).get("srv_sys_us_per_mb") or 0 for c, h in keys]
        usrv = [d.get((i, c, h), {}).get("srv_user_us_per_mb") or 0 for c, h in keys]
        axs[1].bar(xs, sysv, w, color=COLOR[i], label=f"{LABEL[i]} sys")
        axs[1].bar(xs, usrv, w, bottom=sysv, color=COLOR[i], alpha=0.4, label=f"{LABEL[i]} user")
    axs[1].set_xticks(range(len(keys)))
    axs[1].set_xticklabels([f"{c}c/{h}%" for c, h in keys], fontsize=8)
    axs[1].set_ylabel("server CPU time per MB sent (µs/MB)")
    axs[1].set_title("server CPU cost")
    axs[1].legend(fontsize=8)
    fig.tight_layout()
    fig.savefig(out, dpi=150)
    plt.close(fig)


def fig_memory(d, clients, hits, out):
    fig, ax = plt.subplots(figsize=(6.5, 4.5))
    for i in IPC:
        for h, ls in zip(hits, ["-", "--"]):
            cs = [c for c in clients if (i, c, h) in d]
            ax.plot(cs, [d[(i, c, h)]["srv_rss_peak_kb"] / 1024 for c in cs], ls=ls, marker=MARK[i], color=COLOR[i],
                    label=f"{LABEL[i]}, hit {h}%")
    clients_axis(ax, clients)
    ax.set_ylabel("server peak RSS (MB, VmHWM)")
    ax.set_title("Phase 2: server memory")
    ax.legend(fontsize=8)
    fig.tight_layout()
    fig.savefig(out, dpi=150)
    plt.close(fig)


def fig_compare(a, b, la, lb, clients, hits, out):
    fig, axs = plt.subplots(1, 2, figsize=(13, 4.5))
    for i in IPC:
        for h, ls in zip(hits, ["-", "--"]):
            for src, lab, alpha in ((a, la, 0.45), (b, lb, 1.0)):
                cs = [c for c in clients if (i, c, h) in src]
                if not cs:
                    continue
                axs[0].plot(cs, [src[(i, c, h)]["mbps_median"] for c in cs], ls=ls, marker=MARK[i], color=COLOR[i],
                            alpha=alpha, label=f"{i} {h}% {lab}")
                axs[1].plot(cs, [src[(i, c, h)]["p50_us"] / 1e3 for c in cs], ls=ls, marker=MARK[i], color=COLOR[i],
                            alpha=alpha, label=f"{i} {h}% {lab}")
    for ax in axs:
        clients_axis(ax, clients)
        ax.set_ylim(bottom=0)
    axs[0].set_ylabel("total throughput (MB/s)")
    axs[1].set_ylabel("request latency p50 (ms)")
    axs[0].legend(fontsize=7)
    fig.suptitle(f"Phase 2 design improvement: {la} (faded) vs {lb}")
    fig.tight_layout()
    fig.savefig(out, dpi=150)
    plt.close(fig)


def tables(d, clients, hits, out):
    L = ["# 2단계 측정 표 (summary.csv에서 생성)", ""]
    L += ["| 적중 | 클라 | IPC | 처리량 MB/s 중앙값 (최소~최대) | 요청/s | 지연 평균 / p50 / p99 / 최대 (us) | 적중률 |",
          "|---|---|---|---|---|---|---|"]
    for h in hits:
        for c in clients:
            for i in IPC:
                r = d.get((i, c, h))
                if r:
                    L.append(f"| {h}% | {c} | {i} | {r['mbps_median']:.0f} ({r['mbps_min']:.0f}~{r['mbps_max']:.0f}) | "
                             f"{r['rps_median']:.0f} | {r['mean_us']:.0f} / {r['p50_us']:.0f} / {r['p99_us']:.0f} / {r['max_us']:.0f} | "
                             f"{r.get('hit_ratio', 0):.3f} |")
    L += ["", "| 적중 | 클라 | IPC | 서버 sys / user (us/MB) | 서버 CS/요청 | 큐 / 송신잠금 / 캐시잠금 대기 (us/요청) | 필터 (us/미스) | 서버 최대 RSS (MB) | 클라 CPU (us/요청) |",
          "|---|---|---|---|---|---|---|---|---|"]
    for h in hits:
        for c in clients:
            for i in IPC:
                r = d.get((i, c, h))
                if r:
                    L.append(f"| {h}% | {c} | {i} | {r['srv_sys_us_per_mb']:.0f} / {r['srv_user_us_per_mb']:.0f} | "
                             f"{r['srv_nvcsw_per_req']:.2f} | {r['queue_wait_us_per_req']:.0f} / {r['send_wait_us_per_req']:.1f} / "
                             f"{r['lock_wait_us_per_req']:.2f} | {r['filter_us_per_miss']:.0f} | {r['srv_rss_peak_kb'] / 1024:.1f} | "
                             f"{r['client_cpu_us_per_req']:.1f} |")
    with open(out, "w", encoding="utf-8") as f:
        f.write("\n".join(L) + "\n")


def main():
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        return 2
    cmp = None
    if "--compare" in args:
        k = args.index("--compare")
        cmp = args[k + 1:k + 4]
        args = args[:k]
    res = args[0]
    out = args[1] if len(args) > 1 else os.path.join(res, "figures")
    os.makedirs(out, exist_ok=True)
    d = load(os.path.join(res, "summary.csv"))
    clients = sorted({k[1] for k in d})
    hits = sorted({k[2] for k in d}, reverse=True)
    fig_throughput(d, clients, hits, os.path.join(out, "fig5_throughput.png"))
    fig_latency(d, clients, hits, os.path.join(out, "fig6_latency.png"))
    fig_breakdown(d, clients, hits, os.path.join(out, "fig7_breakdown.png"))
    fig_memory(d, clients, hits, os.path.join(out, "fig8_memory.png"))
    tables(d, clients, hits, os.path.join(out, "tables.md"))
    if cmp:
        other = load(os.path.join(cmp[0], "summary.csv"))
        fig_compare(other, d, cmp[1], cmp[2], clients, hits, os.path.join(out, "fig9_compare.png"))
    print(f"written to {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
