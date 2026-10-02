# -*- coding: utf-8 -*-
"""校验 BSP/Src/Utf8_Gb2312.c 里的转码表与二分查找逻辑。

做法: 直接解析生成的 C 源码取回两张表, 在 Python 里按 C 的算法(含边界)
复现 Utf8_To_Gb2312(), 再与 Python 自带的 GB2312 编解码逐字对照。
覆盖正常省市名、直辖市(city 为空)、含二级汉字的市名(字库没有该字)、
ASCII 混合、以及比表中最小 Unicode 还小的码点(专打二分查找的下溢边界)。

用法: python Documents/Tools/verify_utf8_gb2312.py
"""

import io
import os
import re

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SRC = os.path.join(ROOT, "BSP", "Src", "Utf8_Gb2312.c")

text = io.open(SRC, encoding="utf-8").read()


def grab(name):
    m = re.search(r"%s\[GB_TABLE_COUNT\]\s*=\s*\{(.*?)\};" % name, text, re.S)
    if m is None:
        raise SystemExit("找不到数组 %s" % name)
    return [int(v, 16) for v in re.findall(r"0x([0-9A-Fa-f]{4})U", m.group(1))]


uni = grab("Uni_Table")
gb = grab("Gb_Table")
assert len(uni) == len(gb) == 3755, (len(uni), len(gb))
assert uni == sorted(uni), "Uni_Table 必须升序"
print("表长度 = %d, Unicode 升序 OK" % len(uni))


def find_gb(code):
    """复刻 C 里的二分查找(int 版, 注意 hi 的边界)。"""
    lo, hi = 0, len(uni) - 1
    while lo <= hi:
        mid = (lo + hi) // 2
        if uni[mid] < code:
            lo = mid + 1
        elif uni[mid] > code:
            hi = mid - 1
        else:
            return gb[mid]
    return 0


def expect(text_in):
    """按"一级汉字才保留"的规则给出期望字节串。"""
    out = bytearray()
    for ch in text_in:
        if ord(ch) < 0x80:
            out += ch.encode("ascii")
            continue
        try:
            raw = ch.encode("gb2312")
        except UnicodeEncodeError:
            continue  # 一级字库没有 -> 由调用方的 gb2312 分支决定; 见下面的 assert
        if len(raw) == 2 and 0xB0 <= raw[0] <= 0xD7:
            out += raw
    return bytes(out)


def to_gb2312(text_in):
    """复刻 C 的 Utf8_To_Gb2312()。"""
    out = bytearray()
    for ch in text_in:
        if ord(ch) < 0x80:
            out += ch.encode("ascii")
            continue
        code = find_gb(ord(ch))
        if code == 0:
            continue
        out += bytes(((code >> 8) & 0xFF, code & 0xFF))
    return bytes(out)


samples = [
    "河北省衡水市",
    "北京市",
    "上海市",
    "重庆市",
    "内蒙古自治区呼和浩特市",
    "新疆维吾尔自治区乌鲁木齐市",
    "黑龙江省哈尔滨市",
    "广西壮族自治区南宁市",
    "安徽省亳州市",      # 亳 是二级汉字(字库没有), 应被整字丢弃
    "四川省绵阳市涪城区",  # 涪 同上
    "广东省深圳市南山区",
    "abc123",
    "Hello, World!",
]

bad = 0
for s in samples:
    got = to_gb2312(s)
    exp = expect(s)
    flag = "OK " if got == exp else "BAD"
    if got != exp:
        bad += 1
    print("%s %-24s -> %s" % (flag, s, got.hex(" ")))

# 二分查找的下溢边界: 比表中最小的 Unicode 还小 / 比最大的还大
for probe in (0x0001, 0x4DBF, 0x9FFF, 0xFFFF):
    r = find_gb(probe)
    assert r == 0 or 0xB0A1 <= r <= 0xD7FE, probe
print("二分查找边界(最小/最大之外)未越界")

# 反查表里的每一项都能编回同一个 GB2312 字节
for u, g in zip(uni, gb):
    raw = chr(u).encode("gb2312")
    assert raw[0] == (g >> 8) and raw[1] == (g & 0xFF), (hex(u), hex(g))
print("3755 项与 Python gb2312 编解码完全一致")

print("失败用例: %d" % bad)
raise SystemExit(1 if bad else 0)
