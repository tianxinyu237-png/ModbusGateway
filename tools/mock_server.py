# -*- coding: utf-8 -*-
"""
mock_server.py —— 前端联调用的模拟后端（Python 版）

作用：在 C 语言 webserver 完成之前，用 Python 起一个同接口的临时服务，
      方便把前端 js/config.js 的 MOCK 关掉，走真实 HTTP 链路验证接口格式。

用法：
    python tools/mock_server.py 8080
    浏览器访问 http://localhost:8080/index.html

注意：这只是脚手架，正式项目仍应使用自研 C webserver（epoll reactor）。
"""

import json
import math
import os
import random
import socketserver
import sys
import time
from datetime import datetime, timedelta
from http.server import SimpleHTTPRequestHandler, HTTPServer

# 注意：http.server.ThreadingHTTPServer 是 Python 3.7 才有的，
# 这台机器（Ubuntu 18.04 自带 Python 3.6.9）import 会直接报
# ImportError: cannot import name 'ThreadingHTTPServer'
# 所以这里用 socketserver.ThreadingMixIn + HTTPServer 自己拼一个等价的多线程服务器
class ThreadingHTTPServer(socketserver.ThreadingMixIn, HTTPServer):
    daemon_threads = True
    allow_reuse_address = True

WEB_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'wwwroot', 'app')
WEB_DIR = os.path.abspath(WEB_DIR)

USERS = {'admin': '123456'}
SESSIONS = set()

STATE = {
    'temp': 24.5,
    'humi': 55.0,
    'temp_max': 24.5,
    'temp_min': 24.5,
    'count': 0,
    'period': 2000,
    'threshold': 32,
    'start': time.time(),
}
LOGS = []


def now_str():
    return datetime.now().strftime('%Y-%m-%d %H:%M:%S')


def log(level, module, message):
    LOGS.insert(0, {'time': now_str(), 'level': level, 'module': module, 'message': message})
    del LOGS[200:]


log('INFO', 'modbus', 'connected to slave 0x01 @ 192.168.1.100:502')
log('INFO', 'collect', '采集进程启动完成 (pid=2041, daemon)')
log('INFO', 'shm', 'shared memory attach success')
log('INFO', 'mq', 'message queue opened: /mq_modbus_cmd')
log('INFO', 'webserver', 'listen on 0.0.0.0:8080, epoll ET mode')


def walk(base, step, low, high):
    v = base + (random.random() - 0.5) * step
    return max(low, min(high, v))


def gen_history(hours):
    step_ms = 5 * 60
    n = max(2, int(hours * 60 / step_ms))
    t, h = 24.5, 55.0
    out = []
    for i in range(n, -1, -1):
        ts = datetime.now() - timedelta(minutes=i * step_ms / 60.0)
        wave = math.sin((ts.hour - 6) / 24 * math.pi * 2) * 3.2
        t = walk(t, 0.9, 12, 34)
        h = walk(h, 1.6, 30, 88)
        out.append({'time': ts.strftime('%Y-%m-%d %H:%M:%S'),
                    'temp': round(t + wave, 2), 'humi': round(h, 2)})
    return out


class Handler(SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        # 注意：不要给父类传 directory=WEB_DIR，
        # 那个参数是 Python 3.7 才加的，3.6 上传了会 TypeError:
        #   __init__() got an unexpected keyword argument 'directory'
        # 静态目录改为在 __main__ 里用 os.chdir(WEB_DIR) 定位
        super().__init__(*args, **kwargs)

    def log_message(self, fmt, *args):  # 静音访问日志
        pass

    def _json(self, obj, status=200):
        body = json.dumps(obj, ensure_ascii=False).encode('utf-8')
        self.send_response(status)
        self.send_header('Content-Type', 'application/json; charset=utf-8')
        self.send_header('Content-Length', str(len(body)))
        self.send_header('Access-Control-Allow-Origin', '*')
        self.end_headers()
        self.wfile.write(body)

    def _body(self):
        n = int(self.headers.get('Content-Length') or 0)
        if not n:
            return {}
        try:
            return json.loads(self.rfile.read(n).decode('utf-8'))
        except Exception:
            return {}

    def _authed(self):
        auth = self.headers.get('Authorization') or ''
        token = auth.replace('Bearer ', '').strip()
        return (not SESSIONS) or token in SESSIONS

    # ---------------- GET ----------------
    def do_GET(self):
        path = self.path.split('?')[0]
        query = {}
        if '?' in self.path:
            for kv in self.path.split('?', 1)[1].split('&'):
                if '=' in kv:
                    k, v = kv.split('=', 1)
                    query[k] = v

        if path == '/api/realtime':
            s = STATE
            s['temp'] = round(walk(s['temp'], 0.35, 12, 34), 2)
            s['humi'] = round(walk(s['humi'], 0.8, 30, 88), 2)
            s['count'] += 1
            s['temp_max'] = max(s['temp_max'], s['temp'])
            s['temp_min'] = min(s['temp_min'], s['temp'])
            return self._json({'code': 0, 'data': {
                'temp': s['temp'], 'humi': s['humi'],
                'tempMax': round(s['temp_max'], 2), 'tempMin': round(s['temp_min'], 2),
                'count': s['count'], 'slaveId': 1, 'online': True,
                'collectTime': now_str()}})

        if path == '/api/history':
            hours = float(query.get('hours', 24))
            return self._json({'code': 0, 'data': {'list': gen_history(hours)}})

        if path == '/api/logs':
            return self._json({'code': 0, 'data': {'list': LOGS[:60]}})

        if path == '/api/status':
            return self._json({'code': 0, 'data': {
                'collectOnline': True, 'webOnline': True, 'modbusOnline': True,
                'period': STATE['period'],
                'uptime': int(time.time() - STATE['start'])}})

        return super().do_GET()

    # ---------------- POST ----------------
    def do_POST(self):
        path = self.path.split('?')[0]
        data = self._body()

        if path == '/api/register':
            u = (data.get('username') or '').strip()
            p = data.get('password') or ''
            if not u or not p:
                return self._json({'code': 1003, 'message': '用户名或密码不能为空'})
            if u in USERS:
                return self._json({'code': 1002, 'message': '用户名已存在'})
            USERS[u] = p
            log('INFO', 'webserver', '新用户注册: %s → 写入用户信息表' % u)
            return self._json({'code': 0, 'message': '注册成功'})

        if path == '/api/login':
            u = data.get('username')
            p = data.get('password')
            if USERS.get(u) != p:
                log('WARN', 'webserver', '登录失败: %s' % u)
                return self._json({'code': 1001, 'message': '用户名或密码错误'})
            token = 'tk-%d' % int(time.time() * 1000)
            SESSIONS.add(token)
            log('INFO', 'webserver', '用户登录成功: %s' % u)
            return self._json({'code': 0, 'message': '登录成功',
                               'data': {'username': u, 'role': 'admin', 'token': token}})

        if path == '/api/logout':
            SESSIONS.clear()
            return self._json({'code': 0, 'message': '已退出登录'})

        if path == '/api/command':
            action = data.get('action')
            if action == 'setPeriod':
                STATE['period'] = int(data.get('period', 2000))
                log('INFO', 'mq', 'mq_send(cmd=SET_PERIOD, period=%d)' % STATE['period'])
                return self._json({'code': 0, 'message': '已下发采集周期指令，period=%dms' % STATE['period']})
            if action == 'setThreshold':
                STATE['threshold'] = float(data.get('threshold', 32))
                log('INFO', 'mq', 'mq_send(cmd=SET_THRESHOLD, value=%s)' % STATE['threshold'])
                return self._json({'code': 0, 'message': '已下发告警阈值指令，threshold=%s℃' % STATE['threshold']})
            if action == 'readRegister':
                addr = int(data.get('addr', 0))
                count = int(data.get('count', 2))
                log('INFO', 'modbus', 'read_holding_registers(addr=%d, count=%d)' % (addr, count))
                return self._json({'code': 0, 'message': '已下发读取寄存器指令 addr=%d count=%d' % (addr, count)})
            if action == 'restart':
                log('WARN', 'collect', '收到重启指令，采集进程准备退出并重新拉起')
                return self._json({'code': 0, 'message': '已下发重启采集进程指令'})
            return self._json({'code': 1004, 'message': '未知指令: %s' % action})

        return self._json({'code': 404, 'message': 'not found'}, 404)

    def do_OPTIONS(self):
        self.send_response(204)
        self.send_header('Access-Control-Allow-Origin', '*')
        self.send_header('Access-Control-Allow-Headers', 'Content-Type, Authorization')
        self.send_header('Access-Control-Allow-Methods', 'GET, POST, OPTIONS')
        self.end_headers()


if __name__ == '__main__':
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8080
    os.chdir(WEB_DIR)          # 切到前端目录，SimpleHTTPRequestHandler 就以它为静态根
    print('静态目录: %s' % WEB_DIR)
    print('模拟后端已启动: http://localhost:%d/index.html  (演示账号 admin / 123456)' % port)
    ThreadingHTTPServer(('0.0.0.0', port), Handler).serve_forever()
