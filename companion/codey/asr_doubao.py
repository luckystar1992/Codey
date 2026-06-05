# companion/codey/asr_doubao.py
"""豆包 seed-asr 流式(sauc 2.0)客户端 + 文件回落。
二进制协议参考 meme/doubao_streaming.py(源自 xiaozhi-esp32-server)。"""
import gzip
import json


def build_header(message_type, flags=0):
    return bytearray([(0x1 << 4) | 0x1, (message_type << 4) | flags, (0x1 << 4) | 0x1, 0])


def build_frame(message_type, payload_bytes, last=False):
    flags = 0x2 if last else 0x0
    body = gzip.compress(payload_bytes)
    frame = build_header(message_type, flags)
    frame.extend(len(body).to_bytes(4, "big"))
    frame.extend(body)
    return bytes(frame)


def parse_response(data):
    if len(data) < 4:
        return {"error": f"short {len(data)}b"}
    message_type = data[1] >> 4
    if message_type == 0xF:
        code = int.from_bytes(data[4:8], "big")
        msg_len = int.from_bytes(data[8:12], "big")
        try:
            payload = json.loads(data[12:12 + msg_len].decode("utf-8"))
        except Exception:
            payload = {"raw": data[12:].hex()}
        return {"error": f"server_error code={code}", "payload": payload}
    if len(data) < 12:
        return {"error": "too short"}
    length = int.from_bytes(data[8:12], "big")
    body = data[12:12 + length] if 0 < length <= len(data) - 12 else data[8:]
    try:
        return {"payload": json.loads(body.decode("utf-8", errors="replace"))}
    except Exception as e:
        return {"error": f"parse json: {e}"}


class UtteranceMerger:
    """累積 definite 段為 final_text(去重),partial 段拼在尾巴上給 self.text。"""
    def __init__(self):
        self.final_text = ""
        self.text = ""
        self._seen = set()

    def feed(self, utterances):
        for u in utterances or []:
            txt = u.get("text")
            if u.get("definite") and txt:
                key = (u.get("start_time"), u.get("end_time"), txt)
                if key not in self._seen:
                    self._seen.add(key)
                    self.final_text += txt
        partials = [u["text"] for u in (utterances or []) if not u.get("definite") and u.get("text")]
        self.text = self.final_text + "".join(partials)
        return self.text
