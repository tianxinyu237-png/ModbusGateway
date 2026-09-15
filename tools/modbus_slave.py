#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Modbus TCP 从机模拟器（纯标准库，不依赖 pymodbus）
监听 0.0.0.0:5020，实现功能码 03（读保持寄存器）

寄存器约定（与 modbus_collector.c 的注释一致）：
  寄存器0 = 温度 x10   寄存器1 = 湿度 x10
数值随时间缓慢漂移，方便观察采集曲线。
"""
import socket, struct, threading, time, math, sys

PORT = 5020
REG_COUNT = 16

start = time.time()

def regs():
    t = time.time() - start
    temp = 25.0 + 3.0 * math.sin(t / 10.0)        # 22.0 ~ 28.0 ℃
    humi = 60.0 + 8.0 * math.sin(t / 17.0 + 1.0)  # 52.0 ~ 68.0 %RH
    r = [0] * REG_COUNT
    r[0] = int(round(temp * 10)) & 0xFFFF
    r[1] = int(round(humi * 10)) & 0xFFFF
    return r

def handle(conn, addr):
    print(f"[slave] 客户端接入 {addr}", flush=True)
    buf = b""
    try:
        while True:
            data = conn.recv(1024)
            if not data:
                break
            buf += data
            while len(buf) >= 8:
                txid, proto, length, unit = struct.unpack(">HHHB", buf[:7])
                fc = buf[7]
                if fc == 3:
                    need = 12
                    if len(buf) < need:
                        break
                    addr_, qty = struct.unpack(">HH", buf[8:12])
                    buf = buf[need:]
                    r = regs()
                    if addr_ + qty > REG_COUNT:
                        qty = max(0, REG_COUNT - addr_)
                    print(f"[slave] FC03 addr={addr_} qty={qty} -> {r[addr_:addr_+qty]}", flush=True)
                    pdu = struct.pack(">BB", 3, qty * 2) + b"".join(
                        struct.pack(">H", v) for v in r[addr_:addr_ + qty])
                    mbap = struct.pack(">HHHB", txid, 0, len(pdu) + 1, unit)
                    conn.sendall(mbap + pdu)
                else:
                    # 不支持的功能码：返回异常响应（FC|0x80, 01）
                    print(f"[slave] 不支持的功能码 {fc}", flush=True)
                    buf = buf[7:]
                    pdu = struct.pack(">BB", fc | 0x80, 0x01)
                    conn.sendall(struct.pack(">HHHB", txid, 0, len(pdu) + 1, unit) + pdu)
    except Exception as e:
        print(f"[slave] 连接异常: {type(e).__name__}: {e}", flush=True)
    finally:
        conn.close()
        print(f"[slave] 客户端断开 {addr}", flush=True)

def main():
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("0.0.0.0", PORT))
    s.listen(5)
    print(f"[slave] Modbus TCP 从机已启动 0.0.0.0:{PORT}（Ctrl+C 退出）", flush=True)
    try:
        while True:
            conn, addr = s.accept()
            threading.Thread(target=handle, args=(conn, addr), daemon=True).start()
    except KeyboardInterrupt:
        pass
    finally:
        s.close()
        print("[slave] 已退出", flush=True)

if __name__ == "__main__":
    main()
