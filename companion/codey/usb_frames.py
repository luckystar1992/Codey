"""USB-CDC 帧编解码:C0 DE | type(1) | len(2 LE) | payload | crc16(2 LE)。
非帧字节当设备日志透传。纯函数 + 增量解码器,无 IO,便于单测。"""

MAGIC = b"\xc0\xde"

# 解码上限:本解码器解 设备→companion 帧,最大是 300ms PCM(4800 样本×2 = 9600B),故须 ≥9600。
# 取 16384 与固件 USB_MAX_PAYLOAD 对齐;损坏的巨长 len 最多缓冲 16KB 再丢弃重同步(防 head-of-line 阻塞)。
MAX_PAYLOAD = 16384

# 固件 g_rxBuf / USB_MAX_PAYLOAD(codey_usb.h)大小:companion→设备 帧(整份 STATE)不得超过它,否则设备 RX 丢弃。两处保持同步。
STATE_MAX = 16384

# frame types(与固件 codey_usb.h 的枚举一一对应)
HELLO = 0x01
STATE_REQ = 0x10
LISTEN = 0x20
PCM = 0x21
FOCUS = 0x30
HELLO_ACK = 0x81
STATE = 0x90
STT = 0xA0


def crc16(data: bytes) -> int:
    """CRC-16/CCITT-FALSE: poly=0x1021, init=0xFFFF, no reflect, no xorout。"""
    crc = 0xFFFF
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if (crc & 0x8000) else (crc << 1) & 0xFFFF
    return crc


def encode(ftype: int, payload: bytes = b"") -> bytes:
    n = len(payload)
    if n > 0xFFFF:
        raise ValueError("payload too large")
    body = bytes([ftype, n & 0xFF, (n >> 8) & 0xFF]) + payload   # crc 覆盖 type+len+payload
    c = crc16(body)
    return MAGIC + body + bytes([c & 0xFF, (c >> 8) & 0xFF])


class FrameDecoder:
    """增量喂字节,产出 (frames, logs)。frames=[(type, payload)];logs=非帧字节。"""

    def __init__(self):
        self._buf = bytearray()

    def feed(self, data: bytes) -> "tuple[list[tuple[int, bytes]], bytes]":
        self._buf.extend(data)
        frames = []
        logs = bytearray()
        while True:
            i = self._buf.find(MAGIC)
            if i < 0:                                  # 无完整魔数
                keep = 1 if self._buf[-1:] == MAGIC[:1] else 0   # 末尾孤立 0xC0 可能是魔数起点,暂留
                if len(self._buf) > keep:
                    logs.extend(self._buf[: len(self._buf) - keep])
                    del self._buf[: len(self._buf) - keep]
                break
            if i > 0:                                  # 魔数前的字节是日志
                logs.extend(self._buf[:i])
                del self._buf[:i]
            if len(self._buf) < 5:                     # 魔数(2)+type(1)+len(2) 还不够
                break
            n = self._buf[3] | (self._buf[4] << 8)
            if n > MAX_PAYLOAD:                        # 损坏的巨长 len:吞魔数首字节,重同步
                logs.extend(self._buf[:1])
                del self._buf[:1]
                continue
            total = 5 + n + 2
            if len(self._buf) < total:                 # 半帧,等更多字节
                break
            body = bytes(self._buf[2 : 5 + n])
            crc_rx = self._buf[5 + n] | (self._buf[6 + n] << 8)
            if crc16(body) == crc_rx:
                frames.append((self._buf[2], bytes(self._buf[5 : 5 + n])))
                del self._buf[:total]
            else:                                      # 坏帧:吞掉魔数首字节当日志,重同步
                logs.extend(self._buf[:1])
                del self._buf[:1]
        return frames, bytes(logs)
