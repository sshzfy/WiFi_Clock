# -*- coding: utf-8 -*-
"""离线验证 OTA_ParseTag / Ver_Parse / OTA_BuildAssetUrl 的逻辑。

对应 User/Application/Src/OTA_Update.c 里的三个静态函数，
用 Python 复刻同样的边界行为，不需要硬件即可回归。
运行: py Documents/Tools/test_tag_parse.py
"""

OWNER = "ssh_zfy"
REPO  = "WiFi_Clock"
KEY   = "/releases/tag/"


def parse_tag(loc, tag_len=32):
    """复刻 OTA_ParseTag：取 /releases/tag/ 之后到 ? # / 或结尾的一段"""
    if loc is None or tag_len < 3:
        return None
    i = loc.find(KEY)
    if i < 0:
        return None
    p = i + len(KEY)
    out = []
    while p < len(loc) and loc[p] not in "?#/" and len(out) < tag_len - 1:
        out.append(loc[p])
        p += 1
    tag = "".join(out)
    return tag if len(tag) > 1 else None


def ver_parse(s):
    """复刻 Ver_Parse：sscanf("v%u.%u.%u") 语义，允许后缀，每段 <=255"""
    import re
    m = re.match(r"[vV]?(\d+)\.(\d+)\.(\d+)", s)
    if not m:
        return None, None
    a, b, c = (int(x) for x in m.groups())
    if a > 255 or b > 255 or c > 255:
        return None, None
    return (a << 16) | (b << 8) | c, "%d.%d.%d" % (a, b, c)


def build_url(tag, ver):
    """复刻 OTA_BuildAssetUrl"""
    return ("https://gitee.com/%s/%s/releases/download/%s/fw_%s.ota"
            % (OWNER, REPO, tag, ver))


CASES = [
    # (说明, Location, 期望 tag, 期望 ver, 期望 url)
    ("真实 Gitee 302",
     "https://gitee.com/ssh_zfy/WiFi_Clock/releases/tag/v1.0.6",
     "v1.0.6", 0x010006, "https://gitee.com/ssh_zfy/WiFi_Clock/releases/download/v1.0.6/fw_1.0.6.ota"),
    ("带查询参数",
     "https://gitee.com/o/r/releases/tag/v1.0.7?foo=1",
     "v1.0.7", 0x010007, "https://gitee.com/ssh_zfy/WiFi_Clock/releases/download/v1.0.7/fw_1.0.7.ota"),
    ("带锚点",
     "https://gitee.com/o/r/releases/tag/v2.3.4#notes",
     "v2.3.4", 0x020304, "https://gitee.com/ssh_zfy/WiFi_Clock/releases/download/v2.3.4/fw_2.3.4.ota"),
    ("没有 tag 路径", "https://gitee.com/o/r/releases", None, None, None),
    ("空 Location", "", None, None, None),
    ("两段版本(非法)", "https://gitee.com/o/r/releases/tag/v1.0", "v1.0", None, None),
    ("段值越界", "https://gitee.com/o/r/releases/tag/v1.0.300", "v1.0.300", None, None),
    ("带后缀 tag", "https://gitee.com/o/r/releases/tag/v1.1.0-beta", "v1.1.0-beta", 0x010100,
     "https://gitee.com/ssh_zfy/WiFi_Clock/releases/download/v1.1.0-beta/fw_1.1.0.ota"),
]


def main():
    failed = 0
    for desc, loc, want_tag, want_ver, want_url in CASES:
        tag = parse_tag(loc)
        ver, ver_str = (None, None)
        url = None
        if tag is not None:
            ver, ver_str = ver_parse(tag)
            if ver is not None:
                url = build_url(tag, ver_str)
        ok = (tag == want_tag and ver == want_ver and url == want_url)
        if not ok:
            failed += 1
        print("[%s] %-14s tag=%s ver=%s" % ("PASS" if ok else "FAIL", desc,
                                            tag, ("0x%06X" % ver) if ver else None))
        if not ok:
            print("      期望 tag=%s ver=%s url=%s" % (want_tag, want_ver, want_url))
            print("      实际 tag=%s ver=%s url=%s" % (tag, ver, url))

    # 版本比较语义：1.0.7 > 1.0.6，1.0.10 > 1.0.9（按编码比大小）
    v = lambda s: ver_parse(s)[0]
    checks = [
        ("1.0.7 比 1.0.6 新", v("v1.0.7") > v("v1.0.6"), True),
        ("1.0.10 比 1.0.9 新", v("v1.0.10") > v("v1.0.9"), True),
        ("1.0.6 不比 1.0.6 新", v("v1.0.6") > v("v1.0.6"), False),
    ]
    print("")
    for desc, got, want in checks:
        ok = got == want
        if not ok:
            failed += 1
        print("[%s] %s" % ("PASS" if ok else "FAIL", desc))

    print("\n结果: %s" % ("全部通过" if failed == 0 else "%d 项失败" % failed))
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())