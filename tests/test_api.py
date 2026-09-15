#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""API 集成测试：验证 网页 <-> 采集进程 这条链路
前提：modbus_slave.py 和 collector.out 都在跑，thttpd.out 在 8080 端口
"""
import json, time, urllib.request

PASS = FAIL = 0
def check(cond, msg):
    global PASS, FAIL
    if cond: print(f"  [PASS] {msg}"); PASS += 1
    else:    print(f"  [FAIL] {msg}"); FAIL += 1

def api(qs, timeout=6):
    with urllib.request.urlopen("http://127.0.0.1:8080/api?" + qs, timeout=timeout) as r:
        return r.read().decode("utf-8", "replace")

print("===== 1) 共享内存实时值 =====")
try:
    d1 = json.loads(api("cmd=realtime"))
    print("     第1次:", json.dumps(d1, ensure_ascii=False))
    check("temp" in d1 and "humi" in d1, "能读到实时温湿度（共享内存链路通）")
    check(d1.get("status") == 1, "设备状态为在线(status=1)")

    time.sleep(2.5)
    d2 = json.loads(api("cmd=realtime"))
    print("     第2次:", json.dumps(d2, ensure_ascii=False))
    check("temp" in d2, "第二次读取也正常")
    check(d1.get("temp") != d2.get("temp"),
          f"数值在变化（{d1.get('temp')} -> {d2.get('temp')}），说明是活数据不是脏内存")
except Exception as e:
    check(False, f"realtime 接口异常: {type(e).__name__}: {e}")

print("===== 2) 消息队列下发采集周期 =====")
try:
    d = json.loads(api("cmd=interval&sec=1"))
    print("     下发返回:", json.dumps(d, ensure_ascii=False))
    check(d.get("result") == "ok", "接口返回 ok")
    check(d.get("interval_ms") == 1000, "interval_ms = 1000")
except Exception as e:
    check(False, f"interval 接口异常: {type(e).__name__}: {e}")

print("   等待4秒让采集进程按新周期采集…")
time.sleep(4)

print("===== 3) 历史数据（sqlite + cJSON） =====")
try:
    raw = api("cmd=history&limit=5")
    rows = json.loads(raw)
    check(isinstance(rows, list) and len(rows) > 0, f"查到 {len(rows) if isinstance(rows,list) else '?'} 条历史")
    if isinstance(rows, list) and rows:
        r = rows[0]
        check(all(k in r for k in ("temp", "humi", "status", "time")), "每条记录含 temp/humi/status/time 字段")
        print("     最新一条:", json.dumps(r, ensure_ascii=False))
except Exception as e:
    check(False, f"history 接口异常: {type(e).__name__}: {e} | raw={raw[:120] if 'raw' in dir() else ''}")

print(f"\n===== API 汇总：PASS={PASS}  FAIL={FAIL} =====")
