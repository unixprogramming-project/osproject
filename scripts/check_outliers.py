#!/usr/bin/env python3
"""측정 데이터 이상값 점검 (기준을 먼저 정하고 기계적으로 적용한다). 표준 라이브러리만 쓴다.

기준
  1. 진행 기록 간격: run.log의 진행 줄 사이 간격이 그 폴더 간격 중앙값의 1.5배를 넘으면 표시 (절전·중단 흔적)
  2. 측정 창: 1단계 elapsed_ns가 설정(500ms)보다 20% 이상 길거나, 2단계 window_ns가 3초와 1% 이상 다르면 표시
  3. 회차 이상치: 조건마다 회차별 값(1단계 pingpong p50·throughput MB/s, 2단계 회차 합계 처리량·평균 p50)이
     중앙값에서 MAD(중앙값 절대편차)×3/0.6745 를 넘게 벗어나면 표시 (원인 없이 통계적으로만 튀는 값은 제외하지 않는다)
  4. 실패·검증: run.log의 failed 수, errors.log 크기, 2단계 bad(잘못된 응답) 합
사용: check_outliers.py <results 폴더>
"""
import csv
import glob
import os
import re
import statistics as st
import sys
from collections import defaultdict

ROOT = sys.argv[1] if len(sys.argv) > 1 else "results"
P2CLIENT = ["role", "ipc", "clients", "hit", "rep", "id", "window", "n", "window_ns", "bytes", "rps", "mbps", "mean_ns",
            "p50_ns", "p99_ns", "max_ns", "user_us", "sys_us", "nvcsw", "nivcsw", "rss_kb", "bad", "verify"]


def sec(t):
    h, m, s = map(int, t.split(":"))
    return h * 3600 + m * 60 + s


def gaps(runlog):
    times = [sec(m.group(1)) for m in re.finditer(r"^(\d\d:\d\d:\d\d) \d+/\d+", open(runlog).read(), re.M)]
    d = [b - a for a, b in zip(times, times[1:])]
    if len(d) < 2:
        return []
    med = st.median(d)
    return [(i, x, med) for i, x in enumerate(d) if x > 1.5 * med]


def outliers(groups, label):
    out = []
    for key, vals in sorted(groups.items()):
        reps = sorted(vals)
        xs = [v for _, v in reps]
        if len(xs) < 5:
            continue
        med = st.median(xs)
        mad = st.median(abs(x - med) for x in xs) or 1e-12
        thr = 3 * mad / 0.6745
        bad = [(r, v) for r, v in reps if abs(v - med) > thr]
        if bad:
            out.append((label, key, med, bad))
    return out


def phase1(folder):
    rows = list(csv.DictReader(open(os.path.join(folder, "phase1.csv"))))
    win, pp, tp = [], defaultdict(list), defaultdict(list)
    for r in rows:
        if r["role"] != "all" or r["verify"] != "0":
            continue
        key = (r["ipc"], r["mode"], int(r["msg_size"]), int(r["clients"]))
        rep = int(r["rep"])
        if r["mode"] == "pingpong":
            pp[key].append((rep, float(r["p50_ns"]) / 1e3))
        else:
            tp[key].append((rep, float(r["mb_per_s"])))
            el = float(r["elapsed_ns"] or 0) / 1e6
            if el and el > 500 * 1.2:
                win.append((key, rep, el))
    return outliers(pp, "p50(us)") + outliers(tp, "MB/s"), win


def phase2(folder):
    f = os.path.join(folder, "phase2.csv")
    rep_tot, rep_p50, win, bad = defaultdict(float), defaultdict(list), [], 0
    for row in csv.reader(open(f)):
        if not row or row[0] != "client":
            continue
        d = dict(zip(P2CLIENT, row))
        key = (d["ipc"], int(d["clients"]), int(d["hit"]))
        rep = int(d["rep"])
        rep_tot[(key, rep)] += float(d["mbps"])
        rep_p50[(key, rep)].append(float(d["p50_ns"]) / 1e3)
        bad += int(d["bad"])
        w = float(d["window_ns"]) / 1e9
        if abs(w - 3.0) > 0.03:
            win.append((key, rep, w))
    g_tp, g_p50 = defaultdict(list), defaultdict(list)
    for (key, rep), v in rep_tot.items():
        g_tp[key].append((rep, v))
        g_p50[key].append((rep, st.mean(rep_p50[(key, rep)])))
    return outliers(g_tp, "MB/s") + outliers(g_p50, "p50(us)"), win, bad


folders = []
for runlog in sorted(glob.glob(os.path.join(ROOT, "**", "run.log"), recursive=True)):
    folders.append(os.path.dirname(runlog))

total_out = 0
for fd in folders:
    name = os.path.relpath(fd, ROOT)
    txt = open(os.path.join(fd, "run.log")).read()
    m = re.search(r"done: (\d+) failed: (\d+)", txt)
    err = os.path.getsize(os.path.join(fd, "errors.log")) if os.path.exists(os.path.join(fd, "errors.log")) else 0
    g = gaps(os.path.join(fd, "run.log"))
    if os.path.exists(os.path.join(fd, "phase1.csv")):
        outs, win = phase1(fd)
        badn = 0
    elif os.path.exists(os.path.join(fd, "phase2.csv")):
        outs, win, badn = phase2(fd)
    else:
        continue
    nrep = sum(len(b) for *_, b in outs)
    total_out += nrep
    print(f"\n## {name}: {m.group(0) if m else '?'} | errors.log {err}B | 잘못된 응답 {badn} | 진행 간격 이상 {len(g)} | 측정 창 이상 {len(win)} | 회차 이상치 {nrep}")
    for i, x, med in g:
        print(f"   간격: {i}번째 구간 {x}s (중앙값 {med}s)")
    for w in win[:5]:
        print(f"   측정 창: {w}")
    for label, key, med, b in outs:
        vals = ", ".join(f"rep{r}={v:.1f}" for r, v in b)
        print(f"   이상치 {label} {key} 중앙값 {med:.1f}: {vals}")
print(f"\n회차 이상치 합계 {total_out}")
