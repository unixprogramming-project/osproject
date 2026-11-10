#!/usr/bin/env python3
"""보충 측정(results/supp) 이상값 점검. 회차 이상치 기준은 check_outliers.py와 같다 (중앙값에서 MAD x3/0.6745 초과).
이상치마다 실행 계획(plan.txt) 순서로 추정한 실행 시각을 함께 적고, 2단계 형식은 이상치를 뺐을 때 중앙값 변화를 적는다.
1단계 형식(s1_buffer)은 |z|>6만 출력한다.
사용: check_outliers_supp.py <results/supp>
"""
import csv, statistics as st, sys, os, datetime as dt
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import summarize_supp as ss
R = sys.argv[1]
def plan_times(d):
    log = open(f"{R}/{d}/run.log").read().split("\n")
    start = dt.datetime.strptime(log[1].split(" ",1)[1], "%Y-%m-%d %H:%M:%S")
    end = dt.datetime.strptime([l for l in log if l.startswith("end")][0].split(" ",1)[1], "%Y-%m-%d %H:%M:%S")
    plan = [l.split() for l in open(f"{R}/{d}/plan.txt").read().strip().split("\n")]
    step = (end - start) / len(plan)
    return plan, start, step
tot = flagged = 0
DIRS = [(d, k) for d, k in (("s1_buffer", 1), ("s2_sender", 2), ("s3_workers", 2), ("s4_ring", 2), ("s5_size", 2))
        if os.path.isdir(f"{R}/{d}")]  # 있는 실험 폴더만 (S5는 results/supp_s5에 따로 있다)
for d, kind in DIRS:
    plan, start, step = plan_times(d)
    labels = sorted({p[0] for p in plan})
    for lb in labels:
        path = f"{R}/{d}/{lb}.csv"
        if kind == 1:
            rows = [r for r in csv.DictReader(open(path)) if r["role"] == "all" and r["verify"] == "0"]
            groups = {}
            for r in rows:
                k = (r["mode"], r["msg_size"], r["clients"])
                v = float(r["p50_ns"]) if r["mode"] == "pingpong" else float(r["mb_per_s"])
                groups.setdefault(k, []).append((int(r["rep"]), v))
        else:
            srv, cli = ss.p2.load(path); groups = {}
            for key, cs in cli.items():
                reps = {}
                for c in cs: reps.setdefault(int(c["rep"]), []).append(c["mbps"])
                groups[key] = [(r, sum(v)) for r, v in reps.items() if len(v) == key[1]]
        for k, vals in groups.items():
            xs = [v for _, v in vals]; m = st.median(xs); mad = st.median(abs(x - m) for x in xs) or 1e-9
            tot += len(xs)
            for rep, v in vals:
                z = 0.6745 * (v - m) / mad
                if abs(z) > 3:
                    flagged += 1
                    # 계획에서 위치 → 대략적 실행 시각
                    for i, p in enumerate(plan):
                        if p[0] == lb and int(p[-1]) == rep and all(str(x) in p for x in k if str(x) not in ("pingpong","throughput")) and (kind == 2 or p[3] == k[0]):
                            t = (start + step * i).strftime("%H:%M:%S"); break
                    else: t = "?"
                    if d != "s1_buffer" or abs(z) > 6:
                        print(f"{d:10} {lb:7} {str(k):32} rep={rep:2} {v:10.1f} med={m:10.1f} z={z:6.1f} ~{t}")
print("runs", tot, "flagged(MAD x3)", flagged)
# 이상치 제외 후 중앙값 변화 (2단계 형식)
for d in [d for d, k in DIRS if k == 2]:
    for lb in sorted({p[0] for p in plan_times(d)[0]}):
        srv, cli = ss.p2.load(f"{R}/{d}/{lb}.csv")
        for key, cs in cli.items():
            reps = {}
            for c in cs: reps.setdefault(int(c["rep"]), []).append(c["mbps"])
            xs = [sum(v) for v in reps.values() if len(v) == key[1]]; m = st.median(xs); mad = st.median(abs(x-m) for x in xs)
            ys = [x for x in xs if abs(0.6745*(x-m)/mad) <= 3]
            ch = (st.median(ys)/m - 1) * 100
            if abs(ch) > 0.3: print(f"  median shift {d} {lb} {key}: {ch:+.2f}% (removed {len(xs)-len(ys)})")
