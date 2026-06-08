from codey.ngrok_api import parse_tunnels, public_urls

SAMPLE = {
    "tunnels": [
        {"name": "codey-state", "public_url": "https://your-name.ngrok-free.app", "config": {"addr": "http://localhost:8787"}},
        {"name": "codey-asr",   "public_url": "https://ab12cd.ngrok-free.app",     "config": {"addr": "http://localhost:8788"}},
    ]
}


def test_parse_maps_by_local_port():
    m = parse_tunnels(SAMPLE)
    assert m[8787] == "https://your-name.ngrok-free.app"
    assert m[8788] == "https://ab12cd.ngrok-free.app"


def test_public_urls_picks_state_and_asr():
    urls = public_urls(SAMPLE, state_port=8787, asr_port=8788)
    assert urls == {"state_url": "https://your-name.ngrok-free.app",
                    "asr_url": "wss://ab12cd.ngrok-free.app"}


def test_public_urls_tolerates_missing():
    assert public_urls({"tunnels": []}, 8787, 8788) == {"state_url": "", "asr_url": ""}
