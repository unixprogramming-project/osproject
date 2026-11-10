#!/usr/bin/env python3
"""보충 측정(exp_supp.sh) 결과를 실험별 표로 요약한다. 표준 라이브러리만 쓴다.

조건 라벨마다 CSV가 하나씩 있다. 회차별 값을 먼저 구한 뒤 회차 사이 중앙값을 내고,
두 조건의 차이는 Mann-Whitney U 검정(양측, 정규 근사, 동점 보정)의 p값을 함께 적는다.
사용: summarize_supp.py <exp_supp 출력폴더>
"""
import csv
import math
import os
import statistics as st
import sys
from collections import defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import summarize_phase2 as p2  # noqa: E402


def mwu(x, y):
    """양측 p값. 표본이 작으면(각 8 미만) nan."""
    n1, n2 = len(x), len(y)
    if n1 < 8 or n2 < 8:
        return float("nan")
    allv = sorted([(v, 0) for v in x] + [(v, 1) for v in y])
    ranks = [0.0] * len(allv)
    ties = 0.0
    i = 0
    while i < len(allv):
        j = i
        while j + 1 < len(allv) and allv[j + 1][0] == allv[i][0]:
            j += 1
        r = (i + j) / 2 + 1
        for k in range(i, j + 1):
            ranks[k] = r
        t = j - i + 1
        ties += t ** 3 - t
        i = j + 1
    r1 = sum(r for r, (_, g) in zip(ranks, allv) if g == 0)
    u1 = r1 - n1 * (n1 + 1) / 2
    n = n1 + n2
    mu = n1 * n2 / 2
    var = n1 * n2 / 12 * ((n + 1) - ties / (n * (n - 1)))
    if var <= 0:
        return 1.0
    z = (abs(u1 - mu) - 0.5) / math.sqrt(var)
    return math.erfc(max(z, 0) / math.sqrt(2))


def med(v):
    return st.median(v) if v else float("nan")


def pct(a, b):
    return (b / a - 1) * 100 if a else float("nan")


def pstr(p):
    return "   nan" if p != p else ("<0.001" if p < 0.001 else f"{p:6.3f}")


# ---------- 1단계 형식 ----------
def load_p1(path):
    """(mode, size, clients) -> 회차별 값 목록. pingpong은 p50(us), throughput은 MB/s. 클라 CS·sys도."""
    val, cs, sysus = defaultdict(list), defaultdict(list), defaultdict(list)
    if not os.path.exists(path):
        return val, cs, sysus
    with open(path, newline="") as f:
        for r in csv.DictReader(f):
            if r["role"] != "all" or r["verify"] != "0":
                continue
            k = (r["mode"], int(r["msg_size"]), int(r["clients"]))
            val[k].append(float(r["p50_ns"]) / 1e3 if r["mode"] == "pingpong" else float(r["mb_per_s"]))
            n = max(float(r["n"]), 1)
            cs[k].append(float(r["nvcsw"]) / n)
            sysus[k].append(float(r["sys_us"]) / n)
    return val, cs, sysus


def report_s1(d):
    labels = ["pipe", "shm512", "shm64"]
    data = {lb: load_p1(os.path.join(d, lb + ".csv")) for lb in labels}
    keys = sorted(set().union(*[set(data[lb][0]) for lb in labels]))
    if not keys:
        return
    print("\n# S1 버퍼 용량 (pipe 64KB / shm 링 512KB / shm 링 64KB)")
    for mode, title, better in (("pingpong", "왕복 지연 p50 (us), 배수 = pipe / shm", "low"),
                                ("throughput", "합계 처리량 중앙값 (MB/s), 배수 = shm / pipe", "high")):
        print(f"\n## {title}")
        print(f"{'size':>8} {'cli':>3} {'runs':>4} {'pipe':>10} {'shm512':>10} {'shm64':>10} "
              f"{'512배수':>7} {'64배수':>7} {'64/512':>7} {'p(64:512)':>9} {'p(64:pipe)':>10}")
        for k in keys:
            if k[0] != mode:
                continue
            v = {lb: data[lb][0].get(k, []) for lb in labels}
            m = {lb: med(v[lb]) for lb in labels}
            if better == "low":
                r512, r64 = m["pipe"] / m["shm512"], m["pipe"] / m["shm64"]
            else:
                r512, r64 = m["shm512"] / m["pipe"], m["shm64"] / m["pipe"]
            print(f"{k[1]:>8} {k[2]:>3} {min(len(x) for x in v.values()):>4} {m['pipe']:10.1f} {m['shm512']:10.1f} "
                  f"{m['shm64']:10.1f} {r512:7.2f} {r64:7.2f} {m['shm64'] / m['shm512']:7.2f} "
                  f"{pstr(mwu(v['shm64'], v['shm512'])):>9} {pstr(mwu(v['shm64'], v['pipe'])):>10}")
    print("\n## 클라이언트 자발적 CS (회/메시지), sys (us/메시지): pingpong")
    print(f"{'size':>8} {'cli':>3} " + " ".join(f"{lb + ' cs':>10} {lb + ' sys':>10}" for lb in labels))
    for k in keys:
        if k[0] != "pingpong":
            continue
        print(f"{k[1]:>8} {k[2]:>3} " + " ".join(
            f"{med(data[lb][1].get(k, [])):10.2f} {med(data[lb][2].get(k, [])):10.1f}" for lb in labels))


# ---------- 2단계 형식 ----------
def load_p2(path):
    """(ipc, clients, hit) -> 지표 이름 -> 회차별 값 목록"""
    out = defaultdict(lambda: defaultdict(list))
    if not os.path.exists(path):
        return out
    srv, cli = p2.load(path)
    for key, rows in cli.items():
        reps = defaultdict(list)
        for c in rows:
            reps[int(c["rep"])].append(c)
        for cs in reps.values():
            if len(cs) != key[1]:
                continue
            out[key]["mbps"].append(sum(c["mbps"] for c in cs))
            out[key]["p50"].append(st.mean(c["p50_ns"] for c in cs) / 1e3)
            out[key]["p99"].append(st.mean(c["p99_ns"] for c in cs) / 1e3)
            out[key]["bad"].append(sum(c["bad"] for c in cs))
    for key, rows in srv.items():
        for r in rows:
            jobs = max(r["jobs"], 1)
            out[key]["rss_peak_mb"].append(r["rss_peak_kb"] / 1024)
            out[key]["nvcsw"].append(r["nvcsw"] / jobs)
            out[key]["nivcsw"].append(r["nivcsw"] / jobs)
            out[key]["queue_us"].append(r["queue_wait_ns"] / jobs / 1e3)
            out[key]["send_us"].append(r["send_wait_ns"] / jobs / 1e3)
            out[key]["lock_us"].append(r["lock_wait_ns"] / jobs / 1e3)
            out[key]["hit_ratio"].append(r["hits"] / jobs)
            out[key]["cpu_us_per_mb"].append((r["user_us"] + r["sys_us"]) / max(r["bytes"] / 1e6, 1e-9))
    return out


def pair_table(title, a, b, la, lb, metrics):
    keys = sorted(set(a) | set(b))
    if not keys:
        return
    print(f"\n## {title}")
    for name, label, fmt in metrics:
        print(f"\n{label}")
        print(f"{'ipc':>5} {'cli':>3} {'hit':>3} {'runs':>5} {la:>10} {lb:>10} {'변화%':>7} {'p':>6}   범위({la}) / 범위({lb})")
        for k in keys:
            x, y = a[k][name], b[k][name]
            if not x or not y:
                continue
            print(f"{k[0]:>5} {k[1]:>3} {k[2]:>3} {len(x):>2}/{len(y):<2} {fmt.format(med(x)):>10} {fmt.format(med(y)):>10} "
                  f"{pct(med(x), med(y)):+7.1f} {pstr(mwu(x, y)):>6}   {fmt.format(min(x))}~{fmt.format(max(x))} / "
                  f"{fmt.format(min(y))}~{fmt.format(max(y))}")


def report_s2(d):
    a, b = load_p2(os.path.join(d, "direct.csv")), load_p2(os.path.join(d, "thread.csv"))
    if not a and not b:
        return
    print("\n# S2 송신 방식 (v1 direct / v2 thread, 같은 시간대)")
    pair_table("v1 → v2", a, b, "direct", "thread",
               [("mbps", "처리량 (MB/s)", "{:.0f}"), ("p50", "지연 p50 (us)", "{:.0f}"),
                ("queue_us", "요청당 작업 큐 대기 (us)", "{:.1f}"), ("nvcsw", "서버 자발적 CS (회/요청)", "{:.2f}")])


def report_s3(d):
    ws = [8, 12, 14, 16]
    data = {w: load_p2(os.path.join(d, f"w{w}.csv")) for w in ws}
    data = {w: v for w, v in data.items() if v}
    if not data:
        return
    print("\n# S3 워커 수 (shm, 클라 4, 적중 80%, direct)")
    print(f"{'워커':>4} {'runs':>4} {'MB/s':>8} {'최소':>8} {'최대':>8} {'w8대비%':>8} {'p(w8)':>6} {'p(앞)':>6} "
          f"{'p50us':>7} {'자발CS':>7} {'비자발CS':>8} {'큐대기us':>8} {'잠금us':>7} {'CPUus/MB':>9}")
    base = prev = None
    for w in ws:
        if w not in data:
            continue
        v = next(iter(data[w].values()))
        x = v["mbps"]
        if base is None:
            base = x
        print(f"{w:>4} {len(x):>4} {med(x):8.0f} {min(x):8.0f} {max(x):8.0f} {pct(med(base), med(x)):+8.1f} "
              f"{pstr(mwu(base, x)) if x is not base else '     -':>6} {pstr(mwu(prev, x)) if prev else '     -':>6} "
              f"{med(v['p50']):7.0f} {med(v['nvcsw']):7.2f} {med(v['nivcsw']):8.3f} {med(v['queue_us']):8.1f} "
              f"{med(v['lock_us']):7.2f} {med(v['cpu_us_per_mb']):9.1f}")
        prev = x


def report_s4(d):
    a, b = load_p2(os.path.join(d, "slots64.csv")), load_p2(os.path.join(d, "slots16.csv"))
    if not a and not b:
        return
    print("\n# S4 응용 shm 링 (64슬롯 512KB / 16슬롯 128KB, v2, 클라 8)")
    pair_table("64슬롯 → 16슬롯", a, b, "slots64", "slots16",
               [("rss_peak_mb", "서버 최대 RSS (MB)", "{:.1f}"), ("mbps", "처리량 (MB/s)", "{:.0f}"),
                ("p50", "지연 p50 (us)", "{:.0f}"), ("p99", "지연 p99 (us)", "{:.0f}"),
                ("nvcsw", "서버 자발적 CS (회/요청)", "{:.2f}"), ("hit_ratio", "실제 적중률", "{:.3f}")])


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    root = sys.argv[1]
    for sub, fn in (("s1_buffer", report_s1), ("s2_sender", report_s2), ("s3_workers", report_s3), ("s4_ring", report_s4)):
        d = os.path.join(root, sub)
        if os.path.isdir(d):
            fn(d)
            log = os.path.join(d, "run.log")
            if os.path.exists(log):
                lines = open(log).read().strip().split("\n")
                print(f"\n({sub}: {lines[1] if len(lines) > 1 else ''} ~ {' / '.join(lines[-2:])})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
