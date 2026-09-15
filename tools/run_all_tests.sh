#!/bin/bash
# ============================================================
# 一键验证脚本（thttpd + sqlite/cJSON + 共享内存读写锁 + POSIX消息队列
#              + modbus采集 + REST接口 + ModbusGateway前端静态托管）
# 用法：cd /home/hq/network/lianxi && bash tools/run_all_tests.sh
# ============================================================
P=$(cd "$(dirname "$0")/.." && pwd)
cd $P
T=$P/tools

cleanup() {
    ./collector.out --stop             >/dev/null 2>&1   # 采集守护进程（如果有）
    pkill -INT -f "collector.out"       2>/dev/null
    pkill    -f "thttpd.out 8080"       2>/dev/null
    pkill    -f "tools/modbus_slave.py" 2>/dev/null
    sleep 0.4
}
trap cleanup EXIT

echo "############ 1/8 编译 ############"
make clean >/dev/null 2>&1
rm -f collector.out
make 2>&1 | tail -3
make collector.out 2>&1 | tail -3
[ -x thttpd.out ] && [ -x collector.out ] || { echo "编译失败，终止"; exit 1; }
echo "编译通过：thttpd.out / collector.out"

echo
echo "############ 2/8 数据库层自测（SHA-256/注册/登录/历史/日志） ############"
BIN=$T/.db_selftest.bin
gcc -Wall -g -I. -o $BIN $T/db_selftest.c src/db/db.c -lsqlite3 -lcjson || exit 1
rm -f selftest.db
$BIN || echo "(有 FAIL，看上面输出)"
rm -f selftest.db $BIN

echo
echo "############ 3/8 起从机 + 采集进程 + web服务 ############"
python3 -c "
import sqlite3, os
if not os.path.exists('sensor.db'):
    con = sqlite3.connect('sensor.db')
    con.executescript(open('db/init.sql', encoding='utf-8').read())
    con.commit(); con.close()
    print('  已用 db/init.sql 初始化 sensor.db')
else:
    con = sqlite3.connect('sensor.db')
    con.execute('CREATE TABLE IF NOT EXISTS logs(id INTEGER PRIMARY KEY AUTOINCREMENT, time DATETIME DEFAULT (datetime(\'now\',\'localtime\')), level TEXT NOT NULL, module TEXT, message TEXT)')
    con.commit(); con.close()
    print('  sensor.db 已存在，跳过初始化（只补建 logs 表）')
"
nohup python3 $T/modbus_slave.py > /tmp/slave.log 2>&1 &
sleep 1.2
stdbuf -o0 ./collector.out > /tmp/collector.log 2>&1 &
sleep 2.5
stdbuf -o0 ./thttpd.out 8080 > /tmp/httpd_test.log 2>&1 &
sleep 1
pgrep -f "collector.out"   >/dev/null && echo "  collector.out 在跑" || echo "  !! collector.out 没起来"
pgrep -f "thttpd.out 8080" >/dev/null && echo "  thttpd.out 在跑"    || echo "  !! thttpd.out 没起来"

echo
echo "############ 4/8 RESTful 接口测试（前端用的 8 个接口 + token 鉴权） ############"
python3 $T/tests_api_rest.py

echo
echo "  ---- 会话持久化：重启 web 服务后 token 不失效 ----"
bash $T/test_session_persist.sh

echo
echo "############ 5/8 老接口集成（/api?cmd= 与 消息队列/共享内存链路） ############"
python3 $T/test_api.py

echo
echo "  ---- 采集进程日志（消息队列指令是否生效） ----"
grep -E "采集周期已修改|收到web下发指令" /tmp/collector.log | tail -3 || echo "  (没收到指令)"

echo
echo "############ 6/8 web 运行时测试（静态/登录/404/405/安全边界） ############"
python3 $T/tests_web.py
if pgrep -f "thttpd.out 8080" >/dev/null; then
    echo "  服务进程存活（未崩溃）"
else
    echo "  !! 服务进程不在了，说明崩过"
fi

echo
echo "############ 7/8 前端静态托管 + 数据库汇总 ############"
python3 - <<'PYEOF'
import urllib.request, urllib.error, sqlite3, json, re
def probe(p):
    try:
        with urllib.request.urlopen("http://127.0.0.1:8080"+p, timeout=5) as r:
            return r.status, r.read(200).decode("utf-8","replace")
    except urllib.error.HTTPError as e:
        return e.code, ""
    except Exception as e:
        return "EXC", str(e)
print("  前端页面（ModbusGateway）：")
for p in ["/app/index.html","/app/dashboard.html","/app/js/config.js","/app/js/echarts.min.js","/app/css/style.css"]:
    st, body = probe(p)
    print(f"    {p:28s} -> {st}")
st, body = probe("/app/js/config.js")
try:
    cfg = open("wwwroot/app/js/config.js", encoding="utf-8").read()
    m = re.search(r"MOCK:\s*(true|false)", cfg)
    if m:
        print("    config.js 里 MOCK =", m.group(1), "(false=直连自研C后端)")
except Exception as e:
    print("    读 config.js 失败:", e)
con = sqlite3.connect('sensor.db')
print("  sensor_data:", con.execute("SELECT COUNT(*) FROM sensor_data").fetchone()[0], "行")
print("  users      :", con.execute("SELECT COUNT(*) FROM users").fetchone()[0], "行（密码都是 salt$sha256 哈希）")
print("  logs       :", con.execute("SELECT COUNT(*) FROM logs").fetchone()[0], "行")
for r in con.execute("SELECT time,level,module,message FROM logs ORDER BY id DESC LIMIT 3"):
    print("    ", r)
con.close()
PYEOF

echo
echo "############ 8/8 采集守护进程测试（fork+setsid / 异常自愈 / pid文件） ############"
pkill -INT -f collector.out 2>/dev/null; sleep 1        # 先停掉前台采集，避免抢共享内存
bash $T/test_daemon.sh

cleanup
echo
echo "==================== 完 ===================="
