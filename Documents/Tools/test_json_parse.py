# -*- coding: utf-8 -*-
"""复刻 OTA_Update.c 的 JSON 解析逻辑做离线验证"""
import re


def json_get_str(json_s, key):
    """复刻 Json_GetStr: key 不含冒号引号, 匹配后跳过 : 空白 " 再取值"""
    p = json_s.find(key)
    if p < 0:
        return None
    v = json_s[p + len(key):]
    while v and v[0] in " \t":
        v = v[1:]
    if v and v[0] == ":":
        v = v[1:]
    while v and v[0] in " \t":
        v = v[1:]
    if v and v[0] == '"':
        v = v[1:]
    out = []
    for c in v:
        if c == '"':
            break
        out.append(c)
    s = "".join(out)
    return s if s else None


def ver_parse(s):
    """复刻 Ver_Parse"""
    m = re.match(r"^[vV]?(\d+)\.(\d+)\.(\d+)$", s)
    if not m:
        return None
    a, b, c = (int(x) for x in m.groups())
    if a > 255 or b > 255 or c > 255:
        return None
    return (a << 16) | (b << 8) | c, "%d.%d.%d" % (a, b, c)


def _skip_to_value(v):
    """复刻 name/size/url 字段的 : 空白 " 跳过逻辑"""
    while v and v[0] in " \t":
        v = v[1:]
    if v and v[0] == ":":
        v = v[1:]
    while v and v[0] in " \t":
        v = v[1:]
    if v and v[0] == '"':
        v = v[1:]
    return v


def json_find_ota_asset(json_s):
    """复刻 Json_FindOtaAsset: 找第一个 .ota 资产"""
    pos = 0
    while True:
        p = json_s.find('"name"', pos)
        if p < 0:
            return None
        v = _skip_to_value(json_s[p + 6:])
        name = []
        for c in v:
            if c == '"':
                break
            name.append(c)
        name = "".join(name)
        if len(name) >= 4 and name[-4:].lower() == ".ota":
            tail = json_s[p + 6 + len(name):]
            # size
            s = tail.find('"size"')
            size = None
            if s >= 0:
                sv = _skip_to_value(tail[s + 6:])
                m = re.match(r"\s*(\d+)", sv)
                if m:
                    size = int(m.group(1))
            # url
            u = tail.find('"browser_download_url"')
            if u >= 0:
                uv = _skip_to_value(tail[u + 22:])
                url = []
                for c in uv:
                    if c == '"':
                        break
                    url.append(c)
                return ("".join(url), size)
            return None
        pos = p + 6


# GitHub API 真实响应是紧凑 JSON（无空格）
SAMPLE = '{"tag_name":"v1.0.6","name":"v1.0.6","draft":false,"prerelease":false,"body":"release notes","assets":[{"url":"https://api.github.com/repos/sshzfy/WiFi_Clock/releases/assets/1","id":1,"name":"fw_106.ota","label":"","content_type":"application/octet-stream","state":"uploaded","size":159484,"download_count":3,"browser_download_url":"https://github.com/sshzfy/WiFi_Clock/releases/download/v1.0.6/fw_106.ota"},{"url":"...","id":2,"name":"fw_106.hex","size":400000,"browser_download_url":"https://github.com/sshzfy/WiFi_Clock/releases/download/v1.0.6/fw_106.hex"}]}'

# 带空格变体（防御测试）
SAMPLE_SPACED = SAMPLE.replace('":"', '": "').replace('",', '", ').replace('{', '{ ').replace('}', ' }')

tag = json_get_str(SAMPLE, '"tag_name"')
print("compact tag:", tag)
v = ver_parse(tag)
print("version: 0x%06X str=%s" % (v[0], v[1]))
assert v[0] == 0x010006

url, size = json_find_ota_asset(SAMPLE)
print("asset url:", url)
print("asset size:", size)
assert url == "https://github.com/sshzfy/WiFi_Clock/releases/download/v1.0.6/fw_106.ota"
assert size == 159484

# 带空格变体
tag2 = json_get_str(SAMPLE_SPACED, '"tag_name"')
assert tag2 == "v1.0.6", tag2
u2, s2 = json_find_ota_asset(SAMPLE_SPACED)
assert u2 is not None and s2 == 159484
print("spaced variant pass")

# 边界: tag 无 v 前缀
assert ver_parse("1.0.6")[0] == 0x010006
# 边界: 大小写后缀
S2 = SAMPLE.replace('fw_106.ota', 'FW_106.OTA')
u3, _ = json_find_ota_asset(S2)
assert u3 is not None, "case-insensitive suffix"
# 边界: 无 ota 资产
S3 = SAMPLE.replace('fw_106.ota', 'fw_106.bin')
assert json_find_ota_asset(S3) is None
# 边界: tag 是 2 段 (v1.6) 不合法
assert ver_parse("v1.6") is None

print("ALL PASS")
