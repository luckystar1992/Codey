# companion/codey/asr_punct.py
"""可选:本地 sherpa-onnx 标点(models/ 下有 *punct* 模型才启用,否则 no-op)。"""
import glob
import os

MODELS_DIR = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "models")


def _find_punct_model():
    for d in sorted(glob.glob(os.path.join(MODELS_DIR, "*punct*"))):
        hits = glob.glob(os.path.join(d, "*.onnx"))
        if hits:
            return hits[0]
    return None


def _load_engine():
    model = _find_punct_model()
    if not model:
        return None
    try:
        import sherpa_onnx
        cfg = sherpa_onnx.OfflinePunctuationConfig(
            model=sherpa_onnx.OfflinePunctuationModelConfig(ct_transformer=model))
        return sherpa_onnx.OfflinePunctuation(cfg)
    except Exception:
        return None


class Punctuator:
    def __init__(self, engine="auto"):
        self._engine = _load_engine() if engine == "auto" else engine

    @property
    def available(self):
        return self._engine is not None

    def add(self, text):
        text = (text or "").strip()
        if not text or not self._engine:
            return text
        try:
            return self._engine.add_punctuation(text)
        except Exception:
            return text
