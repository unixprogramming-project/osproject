#!/usr/bin/env python3
"""1단계 요약(summary.csv)으로 보고서용 그래프와 표를 만든다.

사용: plot_phase1.py <결과폴더> [출력폴더]
  <결과폴더>/summary.csv            (run_official.sh가 만든 요약) 필수
  <결과폴더>/idle/busy_summary.csv  (있으면 유휴 진입 실험 그래프 추가)
  <결과폴더>/idle/same_cpu_summary.csv
출력 (기본 <결과폴더>/figures):
  fig1_latency_vs_size.png    클라이언트 수별, 메시지 크기에 따른 왕복 지연 (p50 실선, p99 점선)
  fig2_throughput_vs_size.png 클라이언트 수별, 메시지 크기에 따른 합계 처리량 (중앙값, 막대는 최소~최대)
  fig3_vs_clients.png         대표 크기(64B, 16KB, 1MB)에서 클라이언트 수에 따른 지연·처리량
  fig4_idle.png               클라이언트 1개에서 기본 / 코어 깨움(busy) / 한 코어 고정(same_cpu) 지연 비교
  tables.md                   보고서에 붙일 표 (평균, 중앙값, p99, 최대 / 처리량 중앙값, 최소, 최대)
그래프의 세 IPC는 항상 한 그래프에 함께 그린다. 값은 summary.csv에 있는 측정값만 쓴다.
"""
import csv
import os
import sys

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402

IPC = ["pipe", "mq", "shm"]
LABEL = {"pipe": "pipe (FIFO)", "mq": "POSIX message queue", "shm": "POSIX shared memory"}
COLOR = {"pipe": "#1f77b4", "mq": "#d62728", "shm": "#2ca02c"}
MARK = {"pipe": "o", "mq": "s", "shm": "^"}


def size_label(b):
    if b >= 1 << 20:
        return f"{b >> 20}MB"
    if b >= 1 << 10:
        return f"{b >> 10}KB"
    return f"{b}B"


def load(path):
    with open(path, newline="") as f:
        rows = list(csv.DictReader(f))
    out = {}
    for r in rows:
        key = (r["mode"], int(r["msg_size"]), int(r["clients"]), r["ipc"])
        out[key] = {k: (float(v) if v not in ("", None) and k not in ("mode", "ipc") else v) for k, v in r.items()}
    return out


def axes_sizes(ax, sizes):
    ax.set_xscale("log", base=2)
    ax.set_xticks(sizes)
    ax.set_xticklabels([size_label(s) for s in sizes])
    ax.minorticks_off()
    ax.grid(True, which="major", alpha=0.3)


def fig_latency(d, sizes, clients, out):
    fig, axs = plt.subplots(2, 2, figsize=(11, 8), sharex=True)
    for ax, c in zip(axs.flat, clients):
        for i in IPC:
            xs = [s for s in sizes if ("pingpong", s, c, i) in d]
            if not xs:
                continue
            p50 = [d[("pingpong", s, c, i)]["p50_us"] for s in xs]
            p99 = [d[("pingpong", s, c, i)]["p99_us"] for s in xs]
            ax.plot(xs, p50, marker=MARK[i], color=COLOR[i], label=f"{LABEL[i]} p50")
            ax.plot(xs, p99, ls="--", marker=MARK[i], mfc="none", color=COLOR[i], alpha=0.7, label=f"{LABEL[i]} p99")
        axes_sizes(ax, sizes)
        ax.set_yscale("log")
        ax.set_title(f"clients = {c}")
        ax.set_xlabel("message size")
        ax.set_ylabel("round-trip latency (µs, log)")
    axs.flat[0].legend(fontsize=8)
    fig.suptitle("Phase 1: ping-pong round-trip latency by message size")
    fig.tight_layout()
    fig.savefig(out, dpi=150)
    plt.close(fig)


def fig_throughput(d, sizes, clients, out):
    fig, axs = plt.subplots(2, 2, figsize=(11, 8), sharex=True)
    for ax, c in zip(axs.flat, clients):
        for i in IPC:
            xs = [s for s in sizes if ("throughput", s, c, i) in d]
            if not xs:
                continue
            med = [d[("throughput", s, c, i)]["mb_per_s_median"] for s in xs]
            lo = [m - d[("throughput", s, c, i)]["mb_per_s_min"] for s, m in zip(xs, med)]
            hi = [d[("throughput", s, c, i)]["mb_per_s_max"] - m for s, m in zip(xs, med)]
            ax.errorbar(xs, med, yerr=[lo, hi], marker=MARK[i], color=COLOR[i], capsize=3, label=LABEL[i])
        axes_sizes(ax, sizes)
        ax.set_yscale("log")
        ax.set_title(f"clients = {c}")
        ax.set_xlabel("message size")
        ax.set_ylabel("total throughput (MB/s, log)")
    axs.flat[0].legend(fontsize=8)
    fig.suptitle("Phase 1: total throughput by message size (median, bar = min..max of runs)")
    fig.tight_layout()
    fig.savefig(out, dpi=150)
    plt.close(fig)


def fig_clients(d, sizes, clients, out):
    pick = [s for s in (64, 16384, 1 << 20) if s in sizes] or sizes[:3]
    fig, axs = plt.subplots(2, len(pick), figsize=(4 * len(pick) + 1, 8))
    if len(pick) == 1:
        axs = [[axs[0]], [axs[1]]]
    for col, s in enumerate(pick):
        a1, a2 = axs[0][col], axs[1][col]
        for i in IPC:
            cs = [c for c in clients if ("pingpong", s, c, i) in d]
            a1.plot(cs, [d[("pingpong", s, c, i)]["p50_us"] for c in cs], marker=MARK[i], color=COLOR[i], label=LABEL[i])
            cs = [c for c in clients if ("throughput", s, c, i) in d]
            a2.plot(cs, [d[("throughput", s, c, i)]["mb_per_s_median"] for c in cs], marker=MARK[i], color=COLOR[i], label=LABEL[i])
        for a in (a1, a2):
            a.set_xscale("log", base=2)
            a.set_xticks(clients)
            a.set_xticklabels([str(c) for c in clients])
            a.minorticks_off()
            a.grid(True, alpha=0.3)
            a.set_xlabel("concurrent clients")
        a1.set_title(f"latency p50, {size_label(s)}")
        a1.set_ylabel("round-trip latency p50 (µs)")
        a2.set_title(f"throughput, {size_label(s)}")
        a2.set_ylabel("total throughput (MB/s)")
    axs[0][0].legend(fontsize=8)
    fig.suptitle("Phase 1: effect of concurrent clients")
    fig.tight_layout()
    fig.savefig(out, dpi=150)
    plt.close(fig)


def fig_idle(d, idle, sizes, out):
    fig, axs = plt.subplots(1, 3, figsize=(15, 4.5), sharey=True)
    styles = [("default", d, "-"), ("busy (cores kept awake)", idle.get("busy"), "--"), ("same CPU (pinned)", idle.get("same_cpu"), ":")]
    for ax, i in zip(axs, IPC):
        for name, src, ls in styles:
            if not src:
                continue
            xs = [s for s in sizes if ("pingpong", s, 1, i) in src]
            if xs:
                ax.plot(xs, [src[("pingpong", s, 1, i)]["p50_us"] for s in xs], ls=ls, marker=MARK[i], color=COLOR[i], label=name)
        axes_sizes(ax, sizes)
        ax.set_yscale("log")
        ax.set_title(LABEL[i])
        ax.set_xlabel("message size")
    axs[0].set_ylabel("round-trip latency p50 (µs, log), clients = 1")
    axs[0].legend(fontsize=8)
    fig.suptitle("Phase 1 extra: CPU idle-state effect on ping-pong latency")
    fig.tight_layout()
    fig.savefig(out, dpi=150)
    plt.close(fig)


def tables(d, sizes, clients, out):
    lines = ["# 1단계 측정 표 (summary.csv에서 생성)", ""]
    lines += ["## 왕복 지연 (µs): 평균 / 중앙값 / p99 / 최대", "",
              "| 크기 | 클라이언트 | " + " | ".join(IPC) + " |", "|---|---|" + "---|" * len(IPC)]
    for s in sizes:
        for c in clients:
            cells = []
            for i in IPC:
                r = d.get(("pingpong", s, c, i))
                cells.append(f"{r['mean_us']:.1f} / {r['p50_us']:.1f} / {r['p99_us']:.1f} / {r['max_us']:.0f}" if r else "-")
            lines.append(f"| {size_label(s)} | {c} | " + " | ".join(cells) + " |")
    lines += ["", "## 합계 처리량 (MB/s): 중앙값 (최소~최대), 초당 메시지 중앙값", "",
              "| 크기 | 클라이언트 | " + " | ".join(IPC) + " |", "|---|---|" + "---|" * len(IPC)]
    for s in sizes:
        for c in clients:
            cells = []
            for i in IPC:
                r = d.get(("throughput", s, c, i))
                cells.append(f"{r['mb_per_s_median']:.1f} ({r['mb_per_s_min']:.1f}~{r['mb_per_s_max']:.1f}), {r['msgs_per_s_median']:.0f}/s" if r else "-")
            lines.append(f"| {size_label(s)} | {c} | " + " | ".join(cells) + " |")
    lines += ["", "## 자원 사용 (지연 모드, 메시지당 중앙값): 자발적 / 비자발적 컨텍스트 스위치, 시스템 CPU µs", "",
              "| 크기 | 클라이언트 | " + " | ".join(IPC) + " |", "|---|---|" + "---|" * len(IPC)]
    for s in sizes:
        for c in clients:
            cells = []
            for i in IPC:
                r = d.get(("pingpong", s, c, i))
                cells.append(f"{r['client_nvcsw_per_msg']:.2f} / {r['client_nivcsw_per_msg']:.3f} / {r['client_sys_us_per_msg']:.2f}" if r else "-")
            lines.append(f"| {size_label(s)} | {c} | " + " | ".join(cells) + " |")
    with open(out, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    res = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(res, "figures")
    os.makedirs(out, exist_ok=True)
    d = load(os.path.join(res, "summary.csv"))
    sizes = sorted({k[1] for k in d})
    clients = sorted({k[2] for k in d})
    fig_latency(d, sizes, clients, os.path.join(out, "fig1_latency_vs_size.png"))
    fig_throughput(d, sizes, clients, os.path.join(out, "fig2_throughput_vs_size.png"))
    fig_clients(d, sizes, clients, os.path.join(out, "fig3_vs_clients.png"))
    idle = {}
    for v in ("busy", "same_cpu"):
        p = os.path.join(res, "idle", f"{v}_summary.csv")
        if os.path.exists(p):
            idle[v] = load(p)
    if idle:
        fig_idle(d, idle, sizes, os.path.join(out, "fig4_idle.png"))
    tables(d, sizes, clients, os.path.join(out, "tables.md"))
    print(f"written to {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
