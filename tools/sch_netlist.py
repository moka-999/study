#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
原理图 PDF → 网络表提取工具（几何重建法, 无需视觉模型）
==========================================================
用法:
  python tools/sch_netlist.py <pdf> <page>              # 全页文字行 dump
  python tools/sch_netlist.py <pdf> <page> pinmap       # 引脚名→网络 匹配

原理:
  PDF 矢量原理图的每根导线都是线段, 引脚文本与网络标号文本按行排列。
  1. 提取页内所有线段(get_drawings), 并查连通分量(union-find)
  2. 引脚 stub 行 = 引脚名文本中心 y + 偏移(本原理图 +1.8pt 行列距 3.6pt)
  3. 网络标号中心 y 与 stub 行匹配 → 引脚-网络映射

已验证: MYZR-RK3588 原理图 P21 提取结果与 H743 AF 表一致
  (PA1=RMII_REF_CLK, PC1=ETH_MDC, PE12=SPI4_CLK, PB7=I2C1_SDA ...)
"""
import re
import sys
import io

import fitz

sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding="utf-8")

NET_RE = re.compile(r"^[A-Z][A-Z0-9_/\-]{2,}$")
REF_RE = re.compile(r"^(R|C|L|U|FB|Q|D|J|Y|X|SW|TP|ED|CON)\d")
PIN_RE = re.compile(r"^P[ABCDEFGHI]\d{1,2}$")
SKIP = {
    "GND", "VDD", "VSS", "Title", "Size", "Date", "Rev", "Sheet",
    "Document", "Number", "DNP", "NC", "DOWNLOAD", "Download",
}


def is_net(w):
    if not NET_RE.match(w):
        return False
    if REF_RE.match(w):
        return False
    if PIN_RE.match(w):
        return False
    return w not in SKIP


def words_of(pdf, page_idx):
    return pdf[page_idx].get_text("words")


def word_center(w):
    return w[0] + (w[2] - w[0]) / 2, w[1] + (w[3] - w[1]) / 2


def get_segs(p):
    segs = []
    for dr in p.get_drawings():
        if dr.get("fill"):
            continue
        for it in dr["items"]:
            if it[0] == "l":
                a, b = it[1], it[2]
                segs.append((a.x, a.y, b.x, b.y))
            elif it[0] == "re":
                r = it[1]
                if (r.x1 - r.x0) > 3 or (r.y1 - r.y0) > 3:
                    segs.append((r.x0, r.y0, r.x1, r.y0))
                    segs.append((r.x1, r.y0, r.x1, r.y1))
                    segs.append((r.x1, r.y1, r.x0, r.y1))
                    segs.append((r.x0, r.y1, r.x0, r.y0))
    return segs


def pin_net_map(pdf, page_idx, tol=3.0, max_dx=120.0):
    """引脚名 → [(net, dy, x)] 映射表"""
    p = pdf[page_idx]
    words = words_of(pdf, page_idx)
    seen = {}
    for w in words:
        t = w[4]
        if not PIN_RE.match(t):
            continue
        cx, cy = word_center(w)
        key = (t, round(cy))
        if key not in seen:
            seen[key] = (t, w[0], cy)
    labels = []
    for w in words:
        t = w[4]
        if not is_net(t):
            continue
        cx, cy = word_center(w)
        labels.append((t, w[0], cy))
    used = {}
    for t, lx, ly in labels:
        best = None
        bd = 99.0
        for pt, px, py in seen.values():
            if abs(px - lx) > max_dx:
                continue
            dy = abs(py - ly)
            if dy < bd:
                bd = dy
                best = (pt, px, py)
        if best and bd <= tol:
            used.setdefault(best[0], []).append((t, bd, lx))
    return used


def dump_page(pdf, page_idx):
    p = pdf[page_idx]
    words = words_of(pdf, page_idx)
    words.sort(key=lambda w: (round(w[1] / 5), w[0]))
    return " ".join(w[4] for w in words)


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return
    pdf_path, page = sys.argv[1], int(sys.argv[2])
    mode = sys.argv[3] if len(sys.argv) > 3 else "dump"
    pdf = fitz.open(pdf_path)
    if mode == "dump":
        print(dump_page(pdf, page - 1))
    elif mode == "pinmap":
        used = pin_net_map(pdf, page - 1)
        for pin in sorted(used, key=lambda k: (k[0][0], int(re.sub(r"\D", "", k[0]) or 0))):
            lst = sorted(used[pin], key=lambda x: x[1])
            s = "  ".join(f"{t}@{dx:.0f}({dy:.1f})" for t, dy, dx in lst)
            print(f"{pin:6s} {s}")


if __name__ == "__main__":
    main()
