"""独立 Kindle 入口守卫:import codey_kindle 不得拉入重依赖(numpy/sherpa/websockets/asr_stream)。"""
import subprocess
import sys
import os

COMPANION_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def test_import_stays_lightweight():
    # 子进程干净 import,检查重依赖均未被加载(锁「零第三方依赖」不变量)
    code = (
        "import codey_kindle, sys; "
        "heavy=[m for m in ('numpy','sherpa_onnx','websockets','asr_stream') if m in sys.modules]; "
        "print(','.join(heavy)); "
        "sys.exit(1 if heavy else 0)"
    )
    r = subprocess.run([sys.executable, "-c", code], cwd=COMPANION_DIR,
                       capture_output=True, text=True)
    assert r.returncode == 0, f"重依赖被加载: {r.stdout.strip()} / stderr={r.stderr.strip()}"


def test_exposes_port_and_main():
    code = ("import codey_kindle; "
            "assert isinstance(codey_kindle.PORT, int); "
            "assert callable(codey_kindle.main)")
    r = subprocess.run([sys.executable, "-c", code], cwd=COMPANION_DIR,
                       capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
