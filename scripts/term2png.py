#!/usr/bin/env python3
"""명령 실행 결과(텍스트)를 터미널 화면 모양의 PNG로 만든다. 보고서 캡처용.

내용은 실제 명령 출력 그대로 쓴다. 편집하지 않는다.
사용: term2png.py <출력.png> <제목> < 텍스트
      명령 | term2png.py out.png "$ 명령"
"""
import sys

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
from matplotlib import font_manager  # noqa: E402

FONT = "/usr/share/fonts/truetype/nanum/NanumGothicCoding.ttf"


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    out, title = sys.argv[1], sys.argv[2]
    lines = [title] + sys.stdin.read().expandtabs(8).rstrip("\n").split("\n")
    try:
        fp = font_manager.FontProperties(fname=FONT, size=10)
    except Exception:
        fp = font_manager.FontProperties(family="monospace", size=10)
    width = max(len(s) + sum(1 for c in s if ord(c) > 0x1100) for s in lines)
    w_in = max(6.0, 0.075 * width + 0.6)
    h_in = 0.19 * len(lines) + 0.5
    fig = plt.figure(figsize=(w_in, h_in), facecolor="#1e1e1e")
    for i, s in enumerate(lines):
        color = "#9cdcfe" if i == 0 else "#d4d4d4"
        fig.text(0.02, 1 - (0.25 + 0.19 * i) / h_in, s, fontproperties=fp, color=color, va="top", ha="left", parse_math=False)
    fig.savefig(out, dpi=150, facecolor=fig.get_facecolor())
    plt.close(fig)
    return 0


if __name__ == "__main__":
    sys.exit(main())
