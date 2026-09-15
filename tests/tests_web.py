#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""thttpd.out 完整运行时验证
覆盖：静态资源 / 带参数的静态页 / POST登录 / POST求和 / 404 / 404兜底 / 405 /
      路径穿越拦截 / 超大Content-Length / API(realtime,interval,history)
"""
import socket, time, os, json

HOST, PORT = "127.0.0.1", 8080
PASS = FAIL = 0

def check(cond, msg):
    global PASS, FAIL
    if cond:
        print(f"  [PASS] {msg}"); PASS += 1
    else:
        print(f"  [FAIL] {msg}"); FAIL += 1

def raw(payload, wait=0.8, binary=False):
    s = socket.socket(); s.settimeout(3)
    s.connect((HOST, PORT))
    s.sendall(payload)
    time.sleep(wait)
    data = b""; rst = False
    try:
        while True:
            d = s.recv(4096)
            if not d: break
            data += d
    except socket.timeout:
        pass
    except ConnectionResetError:
        rst = True
    s.close()
    if binary:
        return data, rst
    return data.decode("utf-8", "replace"), rst

def get(path, wait=0.8):
    return raw(f"GET {path} HTTP/1.1\r\nHost: x\r\n\r\n".encode(), wait)

def post(path, body):
    return raw((f"POST {path} HTTP/1.1\r\nHost: x\r\nContent-Type: application/x-www-form-urlencoded\r\n"
                f"Content-Length: {len(body)}\r\n\r\n{body}").encode())

print("===== 静态资源 =====")
body, rst = get("/index.html")
check("HTTP/1.0 200 OK" in body, "GET /index.html -> 200")
check("<!DOCTYPE html>" in body, "返回的是HTML内容")
check(not rst, "GET 无RST复位")

print("===== 静态页带?参数（修复前会被当成业务请求返回JSON） =====")
body, _ = get("/index.html?v=1")
check("200 OK" in body and "<!DOCTYPE html>" in body, "GET /index.html?v=1 -> 仍是HTML页面")
body, _ = get("/post.html?utm_source=test")
check("200 OK" in body and "<!DOCTYPE html>" in body, "GET /post.html?utm_source=test -> 仍是HTML页面")

print("===== POST 登录（修复前必然404） =====")
body, rst = post("/login", "username=admin&password=admin")
check("200 OK" in body, "POST /login -> 200")
check("localStorage.setItem" in body and "index.html" in body, "登录成功返回跳转JS")
check(not rst, "POST 无RST复位")
body, _ = post("/login", "username=admin&password=xxx")
check("alert" in body, "错误密码返回alert提示")

print("===== POST 求和 =====")
body, _ = raw(b'POST /add HTTP/1.1\r\nHost: x\r\nContent-Length: 15\r\n\r\n"data1=3data2=4"')
check(body.rstrip().endswith("7"), "POST /add 返回 7")
# 含 data1=/data2= 但格式不对（少了外层双引号）-> 必须给明确错误，不能返回随机数
body, _ = post("/add", "data1=3data2=4")
check("参数格式错误" in body, "非法求和参数给出明确错误（不再返回随机数）")

print("===== 404 与 405 =====")
body, _ = get("/nope.html")
check("404" in body and "<h1>404 Not Found</h1>" in body, "GET /nope.html -> 404页面")
body, rst = raw(b"PUT /index.html HTTP/1.0\r\n\r\n")
check("405 Method Not Allowed" in body and "Content-Length" in body, "PUT -> 405 且带头部")
body, rst = raw(b"DELETE / HTTP/1.1\r\nHost: x\r\nX-Pad: " + b"A"*200 + b"\r\n\r\n")
check("405" in body and not rst, "DELETE 带长头部 -> 405 且无RST")

print("===== 路径穿越拦截（修复前 200 且把数据库/源码发出去） =====")
body, _ = get("/../Makefile")
check("403 Forbidden" in body, "GET /../Makefile -> 403 已拦截")
body, _ = get("/../sensor.db")
check("403 Forbidden" in body and "CFLAGS" not in body, "GET /../sensor.db -> 403 已拦截")
body, _ = get("/../src/db/db.c")
check("403 Forbidden" in body, "GET /../src/db/db.c -> 403 已拦截")
body, _ = get("/../../etc/passwd")
check("403 Forbidden" in body, "GET /../../etc/passwd -> 403 已拦截")

print("===== 404 页面缺失时的兜底（原来会崩） =====")
os.rename("wwwroot/404.html", "wwwroot/404.html.bak")
try:
    time.sleep(0.2)
    body, _ = get("/still_missing.html")
    check("404" in body and "404 Not Found" in body, "404.html 被删后仍返回404响应(兜底生效)")
    alive, _ = get("/index.html")
    check("200 OK" in alive, "兜底之后服务进程没崩，还能继续服务")
finally:
    os.rename("wwwroot/404.html.bak", "wwwroot/404.html")

print("===== 超大 Content-Length（原来会栈溢出） =====")
raw(b"POST /login HTTP/1.1\r\nHost: x\r\nContent-Length: 999999999\r\n\r\n" + b"A"*100)
alive, _ = get("/index.html")
check("200 OK" in alive, "Content-Length=999999999 后服务进程仍存活")

print("===== 长URL（原来 get_line 会越界写1字节） =====")
raw(("GET /" + "A"*5000 + " HTTP/1.0\r\n\r\n").encode())
alive, _ = get("/index.html")
check("200 OK" in alive, "5000字节URL 后服务进程仍存活")

print("===== 实时监控页 =====")
body, _ = get("/realtime.html")
check("200 OK" in body and "实时监控" in body, "GET /realtime.html -> 200")

print("===== API: cmd=realtime =====")
body, _ = get("/api?cmd=realtime")
try:
    d = json.loads(body.split("\r\n\r\n", 1)[1])
    check(("temp" in d) or ("error" in d), "返回合法JSON，含temp或error: " + json.dumps(d, ensure_ascii=False)[:70])
except Exception as e:
    check(False, f"解析JSON失败: {e} | body={body[:120]}")

print("===== API: 参数错误 / 未知cmd 都要有明确JSON而不是空白 =====")
body, _ = get("/api?cmd=interval&sec=abc")
check("error" in body, "sec非法 -> 返回error JSON")
body, _ = get("/api?cmd=whatever")
check("未知的cmd" in body, "未知cmd -> 返回error JSON")
body, _ = get("/api?cmd=history&limit=5")
check(body.split("\r\n\r\n",1)[1].strip().startswith(("[", "{")), "history -> 返回JSON")

print(f"\n===== 汇总：PASS={PASS}  FAIL={FAIL} =====")
