#!/bin/bash
# ============================================================
# 一键启动全部服务（后台运行，终端不会被占住）
#   用法: bash tools/start_all.sh [web端口]     默认 8080
#   停止: bash tools/stop_all.sh
# ============================================================
P=$(cd "$(dirname "$0")/.." && pwd)
cd $P
PORT=${1:-8080}
mkdir -p logs

echo "=========== 启动 ModbusGateway 全部服务 ==========="
echo "  端口: web=$PORT  modbus从机=5020"

# 0) 先看看 web 端口是不是被别人占着（占着的话 bind 一定失败）
if ss -lnt 2>/dev/null | grep -q ":$PORT "; then
    OWNER=$(ss -lntp 2>/dev/null | grep ":$PORT " | head -1)
    echo "  [注意] 端口 $PORT 已经被占用："
    echo "         $OWNER"
    echo "         可能上一次的服务还在跑（包括在终端里 Ctrl+Z 挂起的）。
         处理： bash tools/stop_all.sh   或  sudo pkill -9 -f thttpd.out
         换端口： bash tools/start_all.sh 8081"
    exit 1
fi

# 1) Modbus 从机模拟器
if pgrep -f "tools/modbus_slave.py" >/dev/null; then
    echo "  [跳过] Modbus 从机模拟器已在运行"
else
    nohup python3 tools/modbus_slave.py > logs/slave.log 2>&1 &
    sleep 1
    pgrep -f "tools/modbus_slave.py" >/dev/null \
        && echo "  [OK]   Modbus 从机模拟器已启动 (0.0.0.0:5020)" \
        || echo "  [失败] 从机模拟器没起来，看 logs/slave.log"
fi

# 2) 采集进程（守护模式，后台 + 异常自动拉起）
if ./collector.out --status 2>/dev/null | grep -q "运行中"; then
    echo "  [跳过] 采集守护进程已在运行"
else
    ./collector.out --daemon
fi
sleep 2

# 3) web 服务器
if pgrep -f "thttpd.out $PORT" >/dev/null; then
    echo "  [跳过] web 服务已在运行 (端口 $PORT)"
else
    nohup ./thttpd.out $PORT > logs/web.log 2>&1 &
    sleep 1.2
    if pgrep -f "thttpd.out $PORT" >/dev/null; then
        echo "  [OK]   web 服务已启动 (端口 $PORT)"
    else
        echo "  [失败] web 服务没起来，看 logs/web.log："
        tail -12 logs/web.log | sed 's/^/         /'
    fi
fi

IP=$(hostname -I 2>/dev/null | awk '{print $1}')
echo
echo "  浏览器打开："
echo "    登录/注册   http://${IP}:$PORT/app/index.html    演示账号 admin / 123456"
echo "    数据看板    http://${IP}:$PORT/app/dashboard.html"
echo "    早期监控页  http://${IP}:$PORT/realtime.html"
echo "    教学首页    http://${IP}:$PORT/index.html"
echo
echo "  查看采集状态： ./collector.out --status"
echo "  查看日志    ： tail -f logs/collector.log logs/web.log   （Ctrl+C 只退出tail，不影响服务）"
echo "  全部停止    ： bash tools/stop_all.sh"
echo "=================================================="
