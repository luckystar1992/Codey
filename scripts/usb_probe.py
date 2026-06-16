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
    print(f"[probe] {port} 打开,{secs:.0f}s 自检中…(设备 CDC 一连即应开始发 HELLO 探针)")
    dec = uf.FrameDecoder()
    hello_before_ack = 0          # 回 ACK 前收到的 HELLO 数(应 ≥1)
    hello_after_ack = 0           # 回 ACK 后收到的 HELLO 数(应 =0 → 设备已置在线)
    acked = False
    got_state = False
    t0 = time.time()
    ack_t = None
    while time.time() - t0 < secs:
        data = ser.read(4096)
        if data:
            frames, logs = dec.feed(data)
            if logs:
                sys.stdout.write("[dev] " + logs.decode("utf-8", "replace"))
                sys.stdout.flush()
            for ftype, payload in frames:
                if ftype == uf.HELLO:
                    if not acked:
                        hello_before_ack += 1
                        ser.write(uf.encode(uf.HELLO_ACK, b'{"type":"hello","transport":"usb"}'))
                        acked = True
                        ack_t = time.time()
                        print(f"\n[probe] 收到 HELLO #{hello_before_ack} → 已回 HELLO_ACK")
                    else:
                        hello_after_ack += 1
                elif ftype == uf.STATE:
                    got_state = True
                    print(f"[probe] 收到 STATE 帧 {len(payload)}B(设备侧请求触发?)")
                else:
                    print(f"[probe] 收到帧 type=0x{ftype:02X} len={len(payload)}")
        # ACK 后给设备 ~2s 看它是否停发 HELLO
        if ack_t and time.time() - ack_t > 2.0:
            break
    ser.close()

    print("\n==== 自检结论 ====")
    print(f"ACK 前 HELLO 数 : {hello_before_ack}  ({'OK 固件→本机解码互通' if hello_before_ack else 'FAIL 没收到任何 HELLO,检查端口/固件/有无别的程序占用'})")
    if acked:
        print(f"ACK 后 HELLO 数 : {hello_after_ack}  ({'OK 设备已收 ACK 并置在线(停发探针)' if hello_after_ack == 0 else 'WARN 设备仍在发 HELLO,可能没解出 ACK(检查 CRC/帧格式)'})")
    ok = hello_before_ack > 0 and acked and hello_after_ack == 0
    print(f"双向握手        : {'✅ 通过(真机编解码互通)' if ok else '❌ 未通过'}")
    return 0 if ok else 2


if __name__ == "__main__":
    sys.exit(main())
