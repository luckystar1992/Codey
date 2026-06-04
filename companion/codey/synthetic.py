"""合成 user 行识别(不代表用户在等模型回复)。纯函数。"""
import re

CMD_PREFIX = re.compile(
    r"^\s*<(command-name|command-message|command-args|"
    r"bash-input|bash-stdout|bash-stderr|local-command-[a-z]+)>"
)


def text_of(content):
    """从 message.content 取首个 text 块的文本(或字符串本身)。"""
    if isinstance(content, str):
        return content
    if isinstance(content, list):
        for b in content:
            if isinstance(b, dict) and b.get("type") == "text":
                return str(b.get("text") or "")
    return ""


def is_synthetic_user_message(entry):
    if not isinstance(entry, dict) or entry.get("type") != "user":
        return False
    if entry.get("isMeta") is True:
        return True
    content = (entry.get("message") or {}).get("content")
    if (
        isinstance(content, list)
        and len(content) > 0
        and all(isinstance(b, dict) and b.get("type") == "tool_result" for b in content)
    ):
        return True
    if CMD_PREFIX.search(text_of(content)):
        return True
    return False
