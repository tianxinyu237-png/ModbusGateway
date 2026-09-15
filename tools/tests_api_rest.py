#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""RESTful 接口测试（对着真实的 C 版 thttpd，不是模拟后端）
覆盖 README 里约定的 8 个接口 + token 鉴权 + 401 + 未知接口404
用法：先起 thttpd.out 8080（采集进程起不起都行，起的话实时数据更真实）
"""
import json, time, urllib.request, urllib.error

BASE = "http://127.0.0.1:8080"
PASS = FAIL = 0

def check(cond, msg):
    global PASS, FAIL
    if cond:
        print("  [PASS] " + msg); PASS += 1
    else:
        print("  [FAIL] " + msg); FAIL += 1

def call(path, method="GET", body=None, token=None):
    """返回 (http_status, headers, dict)"""
    url = BASE + path
    data = json.dumps(body).encode("utf-8") if body is not None else None
    req = urllib.request.Request(url, data=data, method=method)
    req.add_header("Content-Type", "application/json")
    if token:
        req.add_header("Authorization", "Bearer " + token)
    try:
        with urllib.request.urlopen(req, timeout=8) as r:
            raw = r.read().decode("utf-8", "replace")
            st, hdr = r.status, dict(r.headers)
    except urllib.error.HTTPError as e:
        raw = e.read().decode("utf-8", "replace")
        st, hdr = e.code, dict(e.headers)
    try:
        return st, hdr, json.loads(raw)
    except Exception:
        return st, hdr, {"_raw": raw[:200]}

uname = "api" + str(int(time.time()))[-8:]     # 每次跑用不同用户名，避免"已存在"
token = None

print("===== 1) 鉴权：没 token 必须 401 =====")
st, hdr, d = call("/api/realtime")
check(st == 401, "GET /api/realtime 无token -> HTTP 401（拿到 %s）" % st)
check(d.get("code") == 401, "401 响应体是统一格式 code=401")
check("application/json" in (hdr.get("Content-Type") or ""), "Content-Type: application/json")

print("===== 2) 注册 POST /api/register =====")
st, hdr, d = call("/api/register", "POST", {"username": uname, "password": "123456"})
check(st == 200 and d.get("code") == 0, "注册成功（code=0）")
st, hdr, d = call("/api/register", "POST", {"username": uname, "password": "123456"})
check(d.get("code") == 1002, "重复注册返回 1002 用户名已存在")
st, hdr, d = call("/api/register", "POST", {"username": "", "password": ""})
check(d.get("code") == 1003, "空用户名返回 1003")

print("===== 3) 登录 POST /api/login =====")
st, hdr, d = call("/api/login", "POST", {"username": uname, "password": "wrongpass"})
check(d.get("code") == 1001, "密码错误返回 1001")
st, hdr, d = call("/api/login", "POST", {"username": uname, "password": "123456"})
check(d.get("code") == 0, "登录成功 code=0")
data = d.get("data") or {}
token = data.get("token")
check(bool(token) and len(token) >= 16, "返回了 token（%s...）" % (token or "")[:12])
check(data.get("username") == uname, "返回用户名正确")

print("===== 4) 实时数据 GET /api/realtime =====")
st, hdr, d = call("/api/realtime", token=token)
check(st == 200 and d.get("code") == 0, "带token访问成功 code=0")
rt = d.get("data") or {}
check(("temp" in rt) and ("humi" in rt), "含 temp/humi 字段")
check(("tempMax" in rt) and ("tempMin" in rt) and ("count" in rt), "含 tempMax/tempMin/count 字段")
check("slaveId" in rt and "online" in rt, "含 slaveId/online 字段")
print("        实时数据: " + json.dumps(rt, ensure_ascii=False)[:150])

print("===== 5) 历史 GET /api/history?hours=24 =====")
st, hdr, d = call("/api/history?hours=24", token=token)
check(st == 200 and d.get("code") == 0, "查询成功 code=0")
hd = d.get("data") or {}
check(isinstance(hd.get("list"), list), "data.list 是数组（长度 %s）" % len(hd.get("list") or []))
check(len(hd.get("list") or []) > 0, "历史列表非空（JSON被截断过会在这里暴露）")
if hd.get("list"):
    item = hd["list"][0]
    check(all(k in item for k in ("time", "temp", "humi")), "每条含 time/temp/humi")
    print("        最新一条: " + json.dumps(item, ensure_ascii=False))

print("===== 6) 日志 GET /api/logs =====")
st, hdr, d = call("/api/logs", token=token)
check(st == 200 and d.get("code") == 0, "查询成功 code=0")
lg = d.get("data") or {}
check(isinstance(lg.get("list"), list), "data.list 是数组（长度 %s）" % len(lg.get("list") or []))
check(len(lg.get("list") or []) > 0, "日志列表非空")
if lg.get("list"):
    item = lg["list"][0]
    check(all(k in item for k in ("time", "level", "module", "message")), "每条含 time/level/module/message")
    print("        最新一条: " + json.dumps(item, ensure_ascii=False))

print("===== 7) 状态 GET /api/status =====")
st, hdr, d = call("/api/status", token=token)
check(st == 200 and d.get("code") == 0, "查询成功 code=0")
sd = d.get("data") or {}
check(all(k in sd for k in ("collectOnline", "webOnline", "modbusOnline", "period", "uptime")),
      "含 collectOnline/webOnline/modbusOnline/period/uptime")
print("        状态: " + json.dumps(sd, ensure_ascii=False))

print("===== 8) 指令下发 POST /api/command =====")
st, hdr, d = call("/api/command", "POST", {"action": "setPeriod", "period": 1000}, token=token)
check(d.get("code") == 0, "setPeriod -> code=0（%s）" % d.get("message"))
st, hdr, d = call("/api/command", "POST", {"action": "setThreshold", "threshold": 30}, token=token)
check(d.get("code") == 0, "setThreshold -> code=0（%s）" % d.get("message"))
st, hdr, d = call("/api/command", "POST", {"action": "readRegister", "addr": 0, "count": 2}, token=token)
check(d.get("code") == 0, "readRegister -> code=0（%s）" % d.get("message"))
print("        readRegister data: " + json.dumps(d.get("data") or {}, ensure_ascii=False))
st, hdr, d = call("/api/command", "POST", {"action": "重启"}, token=token)
check(d.get("code") == 1004, "未知指令 -> 1004")
st, hdr, d = call("/api/command", "POST", {"action": "setPeriod", "period": 999999}, token=token)
check(d.get("code") == 1004, "非法 period -> 1004")

print("===== 9) 未知接口 =====")
st, hdr, d = call("/api/notexist", token=token)
check(st == 404 and d.get("code") == 404, "GET /api/notexist -> HTTP 404 + code 404")

print("===== 10) 退出登录 POST /api/logout 后 token 失效 =====")
st, hdr, d = call("/api/logout", "POST", {}, token=token)
check(d.get("code") == 0, "退出成功 code=0")
st, hdr, d = call("/api/realtime", token=token)
check(st == 401, "退出后旧token访问 -> 401")

print("===== 11) 老接口没被破坏（realtime.html 还在用） =====")
st, hdr, d = call("/api?cmd=realtime")
check(st == 200 and (("temp" in d) or ("error" in d)), "GET /api?cmd=realtime 仍是裸JSON")
st, hdr, d = call("/api?cmd=history&limit=3")
check(st == 200 and isinstance(d, list), "GET /api?cmd=history 仍返回数组")

print("\n===== REST 汇总：PASS=%d  FAIL=%d =====" % (PASS, FAIL))
