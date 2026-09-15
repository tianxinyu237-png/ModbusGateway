#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
signal_gen.py —— "现场传感器信号发生器"

作用：把采集目标（真实设备 / Modbus Slave 模拟器）里的保持寄存器周期性写入
      随时间变化的正弦漂移值，让网页上的实时曲线真正动起来。

为什么需要它：
  Modbus Slave 里的寄存器是静态的（你不改它就不变），而采集进程每 2 秒读一次，
  读到的一直是同一个数 -> 网页曲线就是一条直线。要么在 Modbus Slave 界面上开
  "Auto increment"，要么用本脚本从外部往里写值（本脚本走的是标准 Modbus 主机
  FC16 写多个保持寄存器，任何合规从机都接受）。

用法：
  python3 tools/signal_gen.py                      # 默认写 192.168.142.1:502 从机1，寄存器0/1
  python3 tools/signal_gen.py --host 127.0.0.1 --port 5020        # 写虚拟机里那种本机模拟器
  python3 tools/signal_gen.py --period 1.0 --tbase 25 --tamp 5 --hbase 60 --hamp 15
  python3 tools/signal_gen.py --once 350 700       # 只写一组固定值然后退出

参数说明（都是"真实值"，脚本内部会 × scale 转成寄存器原始值）：
  --host/--port/--slave   目标设备
  --reg-temp/--reg-humi   寄存器偏移（默认 0 / 1，和 collector.conf 一致）
  --scale                 原始值 = 真实值 × scale（默认 10）
  --tbase/--tamp           温度基准值 / 波动幅度（℃，默认 25 / 5）
  --hbase/--hamp           湿度基准值 / 波动幅度（%RH，默认 60 / 15）
  --period                 写入周期秒（默认 1.0）
  --count                  写多少次后退出（默认一直写，Ctrl+C 停）

按 Ctrl+C 优雅退出。
"""
import argparse
import math
import socket
import struct
import sys
import time


def write_regs(host, port, unit, addr, values, timeout=3.0):
    """FC16 写多个保持寄存器。返回 (ok, 说明)"""
    qty = len(values)
    pdu = struct.pack(">BHHB", 0x10, addr, qty, qty * 2)
    for v in values:
        pdu += struct.pack(">H", v & 0xFFFF)
    req = struct.pack(">HHHB", 0x0001, 0x0000, len(pdu) + 1, unit) + pdu
    try:
        s = socket.create_connection((host, port), timeout=timeout)
        s.sendall(req)
        resp = s.recv(260)
        s.close()
    except Exception as e:
        return False, "连接/发送失败: %s" % e
    if len(resp) < 9:
        return False, "响应太短: %s" % resp.hex()
    fc = resp[7]
    if fc & 0x80:
        return False, "从机返回异常码 0x%02X" % resp[8]
    if fc != 0x10:
        return False, "功能码不符: 0x%02X" % fc
    return True, "ok"


def main():
    ap = argparse.ArgumentParser(add_help=True)
    ap.add_argument("--host", default="192.168.142.1")
    ap.add_argument("--port", type=int, default=502)
    ap.add_argument("--slave", type=int, default=1)
    ap.add_argument("--reg-temp", type=int, default=0)
    ap.add_argument("--reg-humi", type=int, default=1)
    ap.add_argument("--scale", type=float, default=10.0)
    ap.add_argument("--tbase", type=float, default=25.0)
    ap.add_argument("--tamp", type=float, default=5.0)
    ap.add_argument("--hbase", type=float, default=60.0)
    ap.add_argument("--hamp", type=float, default=15.0)
    ap.add_argument("--period", type=float, default=1.0)
    ap.add_argument("--count", type=int, default=0)
    ap.add_argument("--once", nargs=2, type=float, metavar=("TEMP", "HUMI"))
    args = ap.parse_args()

    print("[信号发生器] 目标 tcp %s:%d 从机%d  寄存器 %d(温度)/%d(湿度)  scale=%g"
          % (args.host, args.port, args.slave, args.reg_temp, args.reg_humi, args.scale))

    if args.once:
        t, h = args.once
        tv = int(round(t * args.scale))
        hv = int(round(h * args.scale))
        ok, msg = write_regs(args.host, args.port, args.slave, args.reg_temp, [tv, hv])
        print("  写入 温度 %g℃(原始 %d) 湿度 %g%%RH(原始 %d) -> %s %s" % (t, tv, h, hv, "成功" if ok else "失败", msg if not ok else ""))
        return 0 if ok else 1

    i = 0
    nfail = 0
    print("  按 Ctrl+C 停止\n")
    try:
        while True:
            # 两个不同相位/频率的正弦，看起来像真实的温湿度漂移
            ph = i / 20.0
            t = args.tbase + args.tamp * math.sin(ph)
            h = args.hbase + args.hamp * math.sin(ph * 0.7 + 1.1)
            tv = int(round(t * args.scale))
            hv = int(round(h * args.scale))
            ok, msg = write_regs(args.host, args.port, args.slave, args.reg_temp, [tv, hv])
            stamp = time.strftime("%H:%M:%S")
            if ok:
                nfail = 0
                print("[%s] 写入 温度 %5.1f℃ 湿度 %5.1f%%RH   (原始 %d / %d)"
                      % (stamp, t, h, tv, hv))
            else:
                nfail += 1
                print("[%s] 写入失败(%d次): %s" % (stamp, nfail, msg))
                if nfail >= 5:
                    print("  连续失败 5 次，先退出。检查：目标设备是否在跑？IP/端口/站号对不对？")
                    return 2
            i += 1
            if args.count and i >= args.count:
                break
            time.sleep(args.period)
    except KeyboardInterrupt:
        print("\n  已停止（共写 %d 次）" % i)
    return 0


if __name__ == "__main__":
    sys.exit(main())
