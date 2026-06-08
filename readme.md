![Codey dashboard](asserts/result.png)

# Codey

Codey is a usage dashboard for Claude Code and Codex, built for the M5Stack StopWatch
(SKU C152, ESP32-S3). The watch shows live session and weekly usage, reset countdowns,
device status, and a small animated interface designed for the round 466 x 466 AMOLED
screen.

中文文档见 [readme_zh.md](readme_zh.md).

## What It Does

- Displays Claude Code and Codex usage on the M5 StopWatch.
- Tracks 5-hour/session and weekly usage percentages with reset countdowns.
- Shows Claude Code model information when available from the Claude statusline.
- Reads Codex usage from the local CodexBar history cache.
- Provides a round-screen dashboard with Claude, Codex, and analog watch pages.
- Supports Wi-Fi provisioning through the `Codey-Setup` captive portal.
- Polls a LAN companion service every 30 seconds for normalized usage data.
- Uses real StopWatch battery, RTC/time, buttons, microphone, and IMU data.
- Includes a settings page for Wi-Fi setup, brightness, and volume.
- Streams microphone audio to a local/cloud ASR engine over WebSocket, with an on-device
  recognition animation (active mascot + sonar rings + streaming transcript).

## Project Status

Implemented:

- Arduino CLI workflow for M5Stack StopWatch build, flash, and serial monitor.
- `hello_stopwatch` hardware/toolchain smoke-test sketch.
- `stopwatch` standalone stopwatch sketch with laps and a round progress ring.
- `codey_dash` main dashboard sketch with animated Claude/Codex pages.
- Unified Python companion (one process: state API + ASR WebSocket + web admin), zero-network
  for usage data.
- Claude Code statusline capture through `companion/codey-statusline.sh`.
- Codex usage reader for CodexBar's local history cache.
- Lunar date, zodiac, and watch-face display data from the companion service.
- Streaming ASR proof of concept with `companion/asr_stream.py`.

In progress / rough edges:

- `pending_reviews` is currently a placeholder value from the companion response.
- The watch firmware still has a hardcoded Mac hostname and fallback IP in
  `sketches/codey_dash/codey_dash.ino`.
- Voice input currently focuses on transcription; command execution is not wired in.
- Codex usage depends on CodexBar's macOS cache path. Without that app/cache, Codex
  usage displays as zero.
- Usage data is read fully offline (statusline file + CodexBar cache); only the optional
  Doubao ASR engine reaches the internet.

## Repository Layout

```text
Codey/
├── arduino-cli.yaml             # Project-local Arduino board manager config
├── asserts/
│   └── result.png               # Current dashboard preview
├── companion/
│   ├── codey_companion.py       # Unified entry: HTTP state API + ASR WS + web admin
│   ├── asr_stream.py            # Streaming ASR WebSocket server (:8788)
│   ├── codey/                   # Python package (state, quota, collect, asr, paste, ...)
│   ├── web/                     # Web admin UI (device mirror + ASR history)
│   ├── deploy.sh                # One-command launcher (preflight + start/stop/status)
│   ├── codey-statusline.sh      # Claude Code statusline usage capture
│   ├── models/                  # Whisper / sherpa-onnx model files
│   └── data/                    # ASR history JSONL (gitignored)
├── docs/
│   └── 开发的基础方法.md          # StopWatch development notes
├── libs/
│   └── WebSockets/              # Vendored Arduino WebSockets library
├── scripts/
│   ├── build.sh                 # Compile a sketch
│   ├── flash.sh                 # Compile and upload a sketch
│   └── monitor.sh               # Serial monitor at 115200 baud
└── sketches/
    ├── hello_stopwatch/         # Minimal StopWatch validation sketch
    ├── stopwatch/               # Standalone stopwatch app
    └── codey_dash/              # Main Codey dashboard firmware
```

## Architecture

```text
Claude Code statusline ─┐
                        ├─ companion (codey_companion.py) ─── HTTP /codey/state ── M5 StopWatch
CodexBar history ───────┤
                        ├─ WebSocket (ASR :8788) ───────────── M5 microphone PCM
                        └─ Web admin (http://localhost:8787) ── device mirror + history
```

The watch connects to a single companion process on the Mac. The companion normalizes all usage sources
into one JSON payload, handles voice transcription over WebSocket, and provides a web admin UI for
monitoring and reviewing ASR history.

## Requirements

Hardware:

- M5Stack StopWatch, SKU C152, ESP32-S3.
- USB-C data cable.
- Mac and StopWatch on the same LAN.

Firmware toolchain:

- `arduino-cli`
- M5Stack ESP32 board package
- Arduino libraries: `M5Unified`, `M5GFX`, `WiFiManager`, `ArduinoJson`
- Vendored `libs/WebSockets` library from this repo

Companion service:

- Python 3 (standard library only to serve usage state + web admin; no pip packages required)
- Optional: `claude-hud` for preserving the normal Claude statusline render
- Optional: CodexBar for the Codex usage cache
- `companion/deploy.sh` runs a preflight check and launches everything

ASR experiments:

- Python 3
- `numpy`, `websockets`, `sherpa-onnx`
- Streaming Zipformer model files under `companion/models/`
- Optional `whisper.cpp` tools (`whisper-server` / `whisper-cli`) for the batch
  `POST /codey/asr` endpoint

## Setup

Install the Arduino platform and libraries:

```bash
brew install arduino-cli
arduino-cli config init
arduino-cli config add board_manager.additional_urls https://static-cdn.m5stack.com/resource/arduino/package_m5stack_index.json
arduino-cli core update-index
arduino-cli core install m5stack:esp32
arduino-cli lib install M5Unified M5GFX WiFiManager ArduinoJson
```

Install the ASR dependencies (only needed for the voice features):

```bash
cd companion
python3 -m pip install numpy websockets sherpa-onnx
```

The state API and web admin need no third-party packages. `./deploy.sh` checks all of this
for you before starting.

### ASR models (voice only)

Voice transcription needs a local **sherpa-onnx streaming Zipformer** model. The repo tracks
the folder and `tokens.txt`, but the `.onnx` weights are gitignored — download them once into
the existing folder so it looks like:

```text
companion/models/sherpa-onnx-streaming-zipformer-bilingual-zh-en-2023-02-20/
├── encoder-epoch-99-avg-1.int8.onnx
├── decoder-epoch-99-avg-1.onnx
├── joiner-epoch-99-avg-1.int8.onnx
└── tokens.txt            # already in the repo
```

Download the model named `sherpa-onnx-streaming-zipformer-bilingual-zh-en-2023-02-20` from the
sherpa-onnx pretrained models (e.g. its
[Hugging Face repo](https://huggingface.co/csukuangfj/sherpa-onnx-streaming-zipformer-bilingual-zh-en-2023-02-20);
use the `https://hf-mirror.com` mirror if Hugging Face is slow). Any folder under
`companion/models/` matching `*streaming-zipformer*` and containing `tokens.txt` is
auto-detected.

Optional model extras:

- **Cloud engine (Doubao)** — no model download; just set credentials in `.env` (see below).
- **Local punctuation** — drop a sherpa `*punct*` model folder under `companion/models/` to
  punctuate local-sherpa output (absent → no-op).
- **Whisper batch endpoint** — for the optional `POST /codey/asr`, `brew install whisper-cpp`
  and put `ggml-small.bin` under `companion/models/`.

Configure Claude Code to use the statusline wrapper if you want exact live Claude rate-limit
percentages:

```bash
chmod +x companion/codey-statusline.sh
```

Then point your Claude Code statusline command to:

```text
/Users/zyc/code/Codey/companion/codey-statusline.sh
```

The wrapper writes the latest Claude usage to:

```text
~/.claude/codey-usage.json
```

## Run

Start the companion (all-in-one):

```bash
cd companion
./deploy.sh            # preflight + start (foreground); add `start --bg` to background it
# or directly: python3 codey_companion.py
```

Check the state API:

```bash
curl http://127.0.0.1:8787/codey/state
```

Open the web admin:

```bash
open http://127.0.0.1:8787/
```

Build the main dashboard firmware:

```bash
./scripts/build.sh sketches/codey_dash
```

Flash it to the StopWatch:

```bash
./scripts/flash.sh sketches/codey_dash
```

Open the serial monitor:

```bash
./scripts/monitor.sh
```

On first boot, the watch opens a Wi-Fi setup hotspot named `Codey-Setup`. Join it from a
phone or laptop, open `192.168.4.1`, and select the LAN used by the Mac running the companion.

## Companion Service

`deploy.sh` is the recommended launcher — it preflights Python + ASR deps + the sherpa model,
auto-creates `.env` from the example, and refuses to start if a port is already in use:

```bash
cd companion
./deploy.sh             # preflight + start in foreground (Ctrl-C to stop)
./deploy.sh start --bg  # background (logs: data/companion.log, PID: data/companion.pid)
./deploy.sh status      # show :8787 / :8788 status
./deploy.sh restart     # restart in background
./deploy.sh stop        # stop
```

Override ports/interpreter with `CODEY_PORT`, `CODEY_ASR_PORT`, `PYTHON`. To run the entry
point directly instead: `python3 codey_companion.py`.

The single process serves:
- HTTP state API on `:8787` (`GET /codey/state`)
- ASR WebSocket on `:8788` (receives 16 kHz mono PCM)
- Web admin UI at `http://<mac>:8787/` (device mirror + recognition history)

The web admin shows:
- **Device mirror** (`/sim`): live watch state via polling `/codey/state`
- **ASR history** (`/codey/history`): recognition history from `companion/data/asr_history.jsonl`

ASR history is stored as a line-delimited JSON file (one record per line, with ISO timestamp):

```bash
cat companion/data/asr_history.jsonl
```

For development, `asr_stream.py` can still run standalone on `:8788`:

```bash
cd companion
python3 asr_stream.py
```

### Voice input bridge

`asr_stream.py` is the streaming ASR server on `:8788` the watch connects to. On start it
prints the selected engine and auto-loads `companion/.env` (copy `companion/.env.example`
and fill in credentials).

The voice input bridge and the on-device recognition animation are adapted from
[meme by EthanM2025](https://github.com/EthanM2025/meme) — see [Acknowledgments](#acknowledgments).

**Configure** — copy the example env file (auto-loaded on start) and edit it:

```bash
cp companion/.env.example companion/.env
```

| Variable | Default | Purpose |
|---|---|---|
| `CODEY_ASR_ENGINE` | `auto` | `sherpa` (local/offline), `doubao` (cloud), or `auto` |
| `CODEY_PASTE` | `1` | paste the final transcript into the focused macOS window |
| `CODEY_PASTE_AUTO_ENTER` | `0` | press Return after pasting (auto-submit) |
| `DOUBAO_API_KEY` / `DOUBAO_APP_ID` | — | required only for the `doubao` engine |

**Engine** — controlled by `CODEY_ASR_ENGINE` (default `auto`):

- `sherpa` — local sherpa-onnx Zipformer; works offline.
- `doubao` — 火山引擎 streaming ASR (Chinese-accurate, built-in punctuation/ITN); requires
  `DOUBAO_API_KEY` + `DOUBAO_APP_ID`; falls back to file-mode ASR if the stream drops.
- `auto` — uses doubao if `DOUBAO_API_KEY` is set, otherwise sherpa.

**Paste** — on recording release the final transcript is auto-pasted into the focused macOS
window (`pbcopy` + `osascript` Cmd+V). **First run requires Accessibility permission**
(System Settings → Privacy & Security → Accessibility → enable the terminal running Python),
otherwise Cmd+V is silently dropped. Toggle with `CODEY_PASTE=0`; add auto-Enter after paste
with `CODEY_PASTE_AUTO_ENTER=1`.

**submit / clear** — the firmware (BtnB multi-press) sends `{"type":"submit"}` (Enter) or
`{"type":"clear"}` (Cmd+A + delete); the companion handles both.

**chime** — `/codey/state` carries `chime: {agent, seq}` when a Claude/Codex turn completes
(`seq` increments per completion, `null` otherwise). The firmware plays a tone on seq change.

**On-device animation** — while recording, the watch shows the provider mascot in an active
state that breathes with the mic level, surrounded by sonar "listening" rings, with the live
transcript streaming above and a `● LISTENING…` / `RECOGNIZING…` status line below.

**Optional local punctuation** — drop a sherpa `*punct*` model directory under
`companion/models/` to punctuate local-sherpa output; absent → no-op.

## Device Controls

- Left button: switch page.
- Right button: start or stop voice transcription.
- Hold both buttons: open or close settings.
- In settings: left button moves selection, right button confirms.
- Shake gesture: switch page.
- Idle for about 20 seconds: dim screen.

## Companion API

`GET /codey/state` returns normalized watch state:

```json
{
  "ts": 1760000000,
  "stale": false,
  "battery": { "pct": 0, "charging": false },
  "providers": [
    {
      "id": "claude",
      "name": "Claude Code",
      "session": { "used_pct": 42, "reset_epoch": 1760003600 },
      "weekly": { "used_pct": 18, "reset_epoch": 1760300000 },
      "pending_reviews": 0,
      "model": "Claude Sonnet",
      "_src": "statusline"
    }
  ],
  "lunar": {
    "date": "四月十五",
    "ganzhi": "丙午",
    "zodiac": "马",
    "jieqi": ""
  }
}
```

## Development Notes

- Main firmware target: `m5stack:esp32:m5stack_stopwatch`.
- The helper scripts pass `--libraries libs`, so the vendored WebSockets library is used.
- The main dashboard renders to a PSRAM canvas for flicker-free animation.
- See [docs/开发的基础方法.md](docs/开发的基础方法.md) for the StopWatch development workflow.

## Acknowledgments

Codey's voice features stand on the excellent work of
[**meme** by EthanM2025](https://github.com/EthanM2025/meme) (MIT-licensed), a sister project
for the same M5Stack StopWatch. Adapted from meme:

- **Voice input bridge** — the streaming-ASR-to-clipboard flow: auto-pasting the final
  transcript into the focused macOS window, Doubao (火山引擎) streaming ASR with
  punctuation/ITN, the xiaozhi-style WebSocket protocol, submit/clear control, and the
  completion chime.
- **On-device recognition animation & interaction** — the "speak → live transcript +
  animated mascot" experience on the round screen. Codey re-implements meme's idea on its own
  M5GFX renderer: the provider mascot enters an active state that breathes with the mic level,
  surrounded by sonar "listening" rings, with the streaming transcript above.

Thanks to EthanM2025 for sharing meme as open source. See the
[meme repository](https://github.com/EthanM2025/meme) for the original implementation.

## Roadmap

- Move Mac hostname, fallback IP, and service ports into a device-side settings flow.
- Replace placeholder review counts with real Claude/Codex pending-review data.
- Convert voice transcripts into actionable watch commands.
- Add a launchd service file for the companion process (`deploy.sh` already does start/stop).
- Add a small simulator or snapshot test harness for dashboard layout regressions.
