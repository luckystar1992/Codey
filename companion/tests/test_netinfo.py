"""netinfo.lan_ip 单测:monkeypatch socket 覆盖回环-only 与有 LAN IP 两种情形。"""
from codey import netinfo


def test_lan_ip_returns_lan_when_present(monkeypatch):
    monkeypatch.setattr(netinfo.socket, "gethostname", lambda: "host")
    monkeypatch.setattr(netinfo.socket, "gethostbyname_ex",
                        lambda h: ("host", [], ["127.0.0.1", "192.168.1.42"]))
    assert netinfo.lan_ip() == "192.168.1.42"


def test_lan_ip_loopback_only_falls_back(monkeypatch):
    monkeypatch.setattr(netinfo.socket, "gethostname", lambda: "host")
    monkeypatch.setattr(netinfo.socket, "gethostbyname_ex",
                        lambda h: ("host", [], ["127.0.0.1"]))
    assert netinfo.lan_ip() == "127.0.0.1"


def test_lan_ip_swallows_errors(monkeypatch):
    def boom(_):
        raise OSError("no dns")
    monkeypatch.setattr(netinfo.socket, "gethostname", lambda: "host")
    monkeypatch.setattr(netinfo.socket, "gethostbyname_ex", boom)
    assert netinfo.lan_ip() == "127.0.0.1"
