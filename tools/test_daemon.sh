#!/bin/bash
# ============================================================
# 采集守护进程测试：启动 → 状态 → 杀死子进程看是否自动拉起 → 停止
# 用法： cd ~/network/lianxi && bash tools/test_daemon.sh
# ============================================================
P=$(cd "$(dirname "$0")/.." && pwd)
cd $P
PASS=0; FAIL=0
ok()   { echo "  [PASS] $1"; PASS=$((PASS+1)); }
bad()  { echo "  [FAIL] $1"; FAIL=$((FAIL+1)); }

echo "===== 0) 清理环境 ====="
./collector.out --stop >/dev/null 2>&1
pkill -f "collector.out --daemon" 2>/dev/null
pkill -INT -f collector.out 2>/dev/null
sleep 1
rm -f run/collector.pid
echo "  环境已清理"

echo "===== 1) 未启动时 --status 应显示未运行 ====="
./collector.out --status | grep -q "未运行" && ok "--status 正确报告未运行" || bad "--status 状态不对"

echo "===== 2) --daemon 后台启动 ====="
./collector.out --daemon
sleep 2.5
SUP=$(cat run/collector.pid 2>/dev/null)
[ -n "$SUP" ] && ok "pid 文件已写入: run/collector.pid = $SUP" || bad "pid 文件没写"
ps -p "$SUP" -o pid= >/dev/null 2>&1 && ok "守护进程(父进程)存活" || bad "守护进程没起来"

echo "===== 3) 子进程(worker)是否被拉起 ====="
WORKER=$(pgrep -P "$SUP" | head -1)
[ -n "$WORKER" ] && ok "采集子进程已拉起 pid=$WORKER" || bad "没有子进程"

echo "===== 4) 子进程是否在真干活（共享内存有数据） ====="
sleep 2.5
./collector.out --status | tee /tmp/daemon_status.txt | grep -q "实时数据" && ok "共享内存有实时数据" || bad "共享内存没数据"
grep -q "已采集 [1-9]" /tmp/daemon_status.txt && ok "已采集点数 > 0（真的在采集）" || bad "采集点数为0"
echo "  ---- --status 输出 ----"
sed 's/^/  /' /tmp/daemon_status.txt

echo "===== 5) 异常自愈：kill -9 子进程，看父进程是否重新拉起 ====="
OLD=$WORKER
kill -9 "$OLD" 2>/dev/null
echo "  已强杀子进程 pid=$OLD，等待父进程自动拉起…"
sleep 6
NEW=$(pgrep -P "$SUP" | head -1)
if [ -n "$NEW" ] && [ "$NEW" != "$OLD" ]; then
    ok "子进程已被自动拉起（旧 $OLD -> 新 $NEW）"
else
    bad "子进程没有被拉起（父进程监管失效）"
fi
grep -q "重新拉起" logs/collector.log 2>/dev/null && ok "日志里记录了'重新拉起'" || bad "日志里没有重启记录"
echo "  ---- 守护进程日志尾部 ----"
tail -6 logs/collector.log 2>/dev/null | sed 's/^/  /'
echo "  ---- syslog 里有没有 ----"
(grep -h "modbus-collector" /var/log/syslog 2>/dev/null | tail -3 | sed 's/^/  /') || echo "  (没有syslog权限，忽略)"

echo "===== 6) --stop 停止守护进程 ====="
./collector.out --stop
sleep 1
if ps -p "$SUP" -o pid= >/dev/null 2>&1; then bad "守护进程还在"; else ok "守护进程已退出"; fi
pgrep -f "collector.out --daemon" >/dev/null && bad "还有残留进程" || ok "没有残留进程"
[ -f run/collector.pid ] && bad "pid 文件没清掉" || ok "pid 文件已清理"

echo
echo "===== 守护进程测试汇总：PASS=$PASS  FAIL=$FAIL ====="
[ "$FAIL" -eq 0 ]
