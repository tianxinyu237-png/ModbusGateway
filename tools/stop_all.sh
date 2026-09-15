#!/bin/bash
# ============================================================
# 停止全部服务（会自动处理"被 Ctrl+Z 挂起"和"用 sudo 起的"进程）
#   用法: bash tools/stop_all.sh
# ============================================================
P=$(cd "$(dirname "$0")/.." && pwd)
cd $P

PATTERN="thttpd.out|collector.out|tools/modbus_slave.py|mock_server.py"

echo "=========== 停止 ModbusGateway 全部服务 ==========="

# 0) 先处理"挂起"状态的进程：Ctrl+Z 只是暂停，SIGTERM 会一直挂着不生效，
#    必须先 SIGCONT 唤醒（或者直接 SIGKILL）
SUSPENDED=$(ps -eo pid,stat,cmd | awk -v p="$PATTERN" '$2 ~ /^T/ && $0 ~ p {print $1}')
if [ -n "$SUSPENDED" ]; then
    echo "  发现被挂起(Ctrl+Z)的进程: $(echo $SUSPENDED | tr '\n' ' ')"
    for pid in $SUSPENDED; do kill -CONT "$pid" 2>/dev/null; done
    echo "  已唤醒它们（唤醒后再正常终止）"
    sleep 0.5
fi

# 1) 采集守护进程（优先用它自己的 --stop，能优雅收尾并清掉共享内存/消息队列）
if [ -x ./collector.out ]; then
    ./collector.out --stop 2>/dev/null | sed 's/^/  /'
fi
pkill -INT -f "collector.out" 2>/dev/null && echo "  [OK]   前台采集进程已通知退出"

# 2) web 服务器
pkill -f "thttpd.out" 2>/dev/null && echo "  [OK]   web 服务已停止" || echo "  [跳过] web 服务没在运行"

# 3) Modbus 从机模拟器
pkill -f "tools/modbus_slave.py" 2>/dev/null && echo "  [OK]   Modbus 从机模拟器已停止" || echo "  [跳过] 从机模拟器没在运行"

# 4) 联调用的 Python 模拟后端（如果开着）
pkill -f "mock_server.py" 2>/dev/null

sleep 1

# 5) 检查残留：多半是之前用 sudo 起的（普通用户杀不掉）
LEFT=$(pgrep -f "$PATTERN" 2>/dev/null)
if [ -n "$LEFT" ]; then
    echo
    echo "  [注意] 还有进程没停掉："
    ps -eo pid,user,stat,cmd -p $(echo $LEFT | tr '\n' ',' | sed 's/,$//') 2>/dev/null | sed 's/^/    /'
    if sudo -n true 2>/dev/null; then
        echo "  检测到免密 sudo，自动清理…"
        sudo pkill -9 -f "thttpd.out"      2>/dev/null
        sudo pkill -9 -f "collector.out"   2>/dev/null
        sudo pkill -9 -f "tools/modbus_slave.py" 2>/dev/null
        sleep 1
    else
        echo "    这些多半是你之前用 sudo 起的（普通用户杀不掉），请执行："
        echo "      sudo pkill -9 -f thttpd.out"
        echo "    或者按 pid 杀： sudo kill -9 <pid>"
    fi
fi

echo
echo "  剩余相关进程："
ps -eo pid,user,cmd | grep -E "thttpd\.out|collector\.out|modbus_slave|mock_server" | grep -v grep | sed 's/^/    /'
echo "  （上面没有内容=全部停干净了）"
echo "  端口占用情况："
PORTS=$(ss -lntp 2>/dev/null | grep -E ":(80|8080|8081) ")
if [ -n "$PORTS" ]; then echo "$PORTS" | sed 's/^/    /'; else echo "    (80/8080/8081 都没人监听)"; fi
echo "=================================================="
