#!/usr/bin/env python3
"""USB 有线兜底链路 — 真机握手自检(不依赖完整 companion / ASR 栈)。

打开设备 USB-CDC 串口,用与 companion 同一套帧编解码(codey.usb_frames):
  · 解码设备发来的 HELLO 探针(验固件 TX 编码 + 本机解码 互通)
  · 回 HELLO_ACK(验固件 RX 解码:设备收到后应置 g_usbActive=true 并停止再发 HELLO)
  · 透传打印设备的非帧调试日志([dev] ...)
若 ACK 后设备停发 HELLO,则双向编解码 + 握手在真机上验证通过。

用法:  python3 scripts/usb_probe.py [/dev/cu.usbmodemXXXX] [秒数=8]
       (省略端口则自动取第一个 /dev/cu.usbmodem*)
前置:  pip install pyserial;且当前没有别的程序占用该串口(关掉 monitor / 另一个 companion)。
"""
import glob
import sys
import time

# 允许从仓库根目录直接运行:把 companion 加入 import path
import os
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "companion"))
from codey import usb_frames as uf  # noqa: E402


def find_port():
    hits = sorted(glob.glob("/dev/cu.usbmodem*"))
    return hits[0] if hits else None


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else find_port()
    secs = float(sys.argv[2]) if len(sys.argv) > 2 else 8.0
    if not port:
        print("找不到 /dev/cu.usbmodem* — 设备没插?")
        return 1
    try:
        import serial
    except ImportError:
        print("缺 pyserial:pip install pyserial")
        return 1

    ser = serial.Serial(port, 115200, timeout=0.1)
    print(f"[probe] {port} 打开,{secs:.0f}s 自检中…(每收一个 HELLO 即回一个 HELLO_ACK)")
    dec = uf.FrameDecoder()
    hello_total = 0               # 收到的 HELLO 探针总数(应 ≥1 → 固件 TX/本机解码互通)
    acks_sent = 0
    got_state = False
    t0 = time.time()
    last_hello_t = None
    quiet_after_ack = False       # ACK 发出后设备静默 >1.5s → 已置在线(吃下了 ACK)
    while time.time() - t0 < secs:
        data = ser.read(4096)
        now = time.time()
        if data:
            frames, logs = dec.feed(data)
            if logs:
                sys.stdout.write("[dev] " + logs.decode("utf-8", "replace"))
                sys.stdout.flush()
            for ftype, payload in frames:
                if ftype == uf.HELLO:
                    hello_total += 1
                    last_hello_t = now
                    ser.write(uf.encode(uf.HELLO_ACK, b'{"type":"hello","transport":"usb"}'))
                    acks_sent += 1
                    print(f"[probe] +{now-t0:4.1f}s 收到 HELLO #{hello_total} → 回 HELLO_ACK")
                elif ftype == uf.STATE:
                    got_state = True
                    print(f"[probe] +{now-t0:4.1f}s 收到 STATE 帧 {len(payload)}B")
                elif ftype == uf.STT:
                    print(f"[probe] +{now-t0:4.1f}s 收到 STT 帧 {len(payload)}B: {payload[:60]!r}")
                else:
                    print(f"[probe] +{now-t0:4.1f}s 收到帧 type=0x{ftype:02X} len={len(payload)}")
        # 已发过 ACK、且距上个 HELLO 静默 >1.5s → 设备已置在线、停发探针
        if acks_sent > 0 and last_hello_t and now - last_hello_t > 1.5:
            quiet_after_ack = True
            break
    ser.close()

    print("\n==== 自检结论 ====")
    print(f"收到 HELLO 总数 : {hello_total}  ({'OK 固件 TX→本机解码互通(codec 真机验证)' if hello_total else 'FAIL 没收到任何 HELLO,检查端口/固件/占用'})")
    print(f"回 HELLO_ACK 数 : {acks_sent}")
    print(f"ACK 后设备静默  : {'是 → 设备解出 ACK 并置 g_usbActive=true(停发探针)' if quiet_after_ack else '否 → 设备仍在发 HELLO,RX 侧未吃下 ACK'}")
    ok = hello_total > 0 and quiet_after_ack
    print(f"双向握手        : {'✅ 通过(真机双向编解码互通)' if ok else '❌ 未通过'}")
    return 0 if ok else 2


if __name__ == "__main__":
    sys.exit(main())
