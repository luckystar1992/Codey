"""网络信息小工具:本机 LAN IP(仅主机名解析,不对外发包)。"""
import socket


def lan_ip():
    """本机 LAN IP;仅解析主机名,失败回 "127.0.0.1"。"""
    try:
        for ip in socket.gethostbyname_ex(socket.gethostname())[2]:
            if not ip.startswith("127."):
                return ip
    except Exception:
        pass
    return "127.0.0.1"
