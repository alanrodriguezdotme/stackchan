# Offline AI voice backend for StackChan

Run the whole voice agent (ASR → LLM → TTS) on your LAN, keep web search, and
point the robot at it instead of `xiaozhi.me`. This is milestone **W3** from
`docs/plan-offline-ai-and-dance.md`.

---

## The three machines (read this first)

This project spans three computers. Every step below is tagged with the machine
it runs on, so you're never guessing.

| Tag          | Machine                 | What it does                                                         |  Has the GPU?  | Has the robot? |
| ------------ | ----------------------- | -------------------------------------------------------------------- | :------------: | :------------: |
| **[DEVBOX]** | Cloud devbox            | Writes code, builds firmware                                         |       no       |       no       |
| **[MYPC]**   | Your computer           | Flashes the robot; remotes into DEVBOX + AIBOX; runs the test client |       no       |   yes (USB)    |
| **[AIBOX]**  | Geekom mini PC (garage) | Runs the entire voice server                                         | yes (RTX 3060) |       no       |

**MYPC, AIBOX, and the robot are all on the same home LAN.** DEVBOX is remote in
the cloud — it can't reach your LAN, so nothing on DEVBOX ever talks to the
robot or AIBOX directly.

**The plan is ordered to minimize hopping between machines:**

1. **[AIBOX]** — do _all_ server setup in one remote session (Ollama, model, config, `docker compose up`). Don't leave until the smoke test passes.
2. **[MYPC]** — run the test client against AIBOX and confirm the server works end-to-end.
3. **[MYPC]** — repoint the robot (captive-portal route = no rebuild, no DEVBOX needed).
4. **[DEVBOX] → [MYPC]** — _only if_ the captive-portal route fails: build firmware on DEVBOX, flash from MYPC.

---

## Runtime picture (once it's all set up)

```
  StackChan (K151)                 AIBOX (Geekom, Windows, in garage)
  ────────────────                 ──────────────────────────────────
  boot: POST OTA_URL ───────────►  :8003  xiaozhi-server (Docker, CPU)
  gets back ws url                        ├─ VAD  SileroVAD      (CPU)
  connect voice ws ─────────────►  :8000  ├─ ASR  SenseVoiceSmall(CPU)
                                          ├─ LLM  ──► Ollama (native, eGPU)  :11434
                                          └─ TTS  ──► Kokoro (Docker, CPU)   :8880
                                                 └─ tool: web_search ──► Tavily / SearXNG
```

**Why the LLM runs native and everything else is in Docker:** on Windows +
Docker Desktop, giving a container the GPU means WSL2 GPU passthrough, and an
eGPU over Thunderbolt inside WSL2 is the single flakiest thing in this stack.
So the one component that benefits most — the LLM — runs in **Ollama natively on
AIBOX**, where the eGPU already works. Everything containerized stays on CPU.
AIBOX's Ryzen 7 6800H handles ASR and TTS for short utterances fine, and the
RTX 3060's 12 GB is left entirely for the model.

---

## Step 1 — [AIBOX] Stand up the server

Remote-desktop from **MYPC** into **AIBOX** and do everything in this section
without leaving. All commands here run **on AIBOX** in PowerShell.

### 1a. Get these deploy files onto AIBOX

The code lives in the repo on DEVBOX/GitHub. Pull it down to AIBOX (or copy just
the `deploy\offline-ai` folder over the remote session — your call):

```powershell
# example: clone your repo somewhere on AIBOX
git clone https://github.com/alanrodriguezdotme/stackchan.git
cd stackchan\deploy\offline-ai
```

### 1b. Ollama — pull the model and expose it to Docker

```powershell
ollama pull qwen2.5:7b-instruct
```

Qwen2.5-7B is the floor for _reliable tool-calling_, which is what makes "search
the web when needed" actually fire. Smaller models flake on it.

By default Ollama listens only on `127.0.0.1`, which Docker containers can't
reach. Set it to listen on all interfaces:

1. `Win` → "environment variables" → **Edit the system environment variables** →
   **Environment Variables…**
2. Under **User variables** → **New…**: name `OLLAMA_HOST`, value `0.0.0.0:11434`
3. **Quit Ollama from the system tray and relaunch it** (it only reads the
   variable at startup). A reboot works too.

Verify (in a **new** terminal on AIBOX):

```powershell
Get-NetTCPConnection -LocalPort 11434 | Select-Object LocalAddress, State
```

`LocalAddress` should show `0.0.0.0` (or `::`), not `127.0.0.1`. If it still says
`127.0.0.1`, Ollama didn't pick up the variable — quit it from the tray
_completely_ and relaunch.

> Security note: `0.0.0.0` means anyone on your LAN can hit the Ollama API.
> Fine for home. Just don't port-forward `11434` to the internet.

> Prefer LM Studio? It's OpenAI-compatible on `:1234`. Tell me and I'll swap the
> `LLM` block. Ollama is the default here because it's headless and auto-starts.

### 1c. Download the SenseVoice ASR model

The compose file mounts one file: `models\SenseVoiceSmall\model.pt` (~900 MB).
From `deploy\offline-ai` on AIBOX:

```powershell
# Option A — Hugging Face
pip install -U "huggingface_hub[cli]"
huggingface-cli download FunAudioLLM/SenseVoiceSmall model.pt --local-dir models\SenseVoiceSmall

# Option B — ModelScope (often faster)
#   pip install -U modelscope
#   modelscope download --model iic/SenseVoiceSmall model.pt --local_dir models\SenseVoiceSmall
```

No Python on AIBOX? Download `model.pt` straight from the browser at
https://huggingface.co/FunAudioLLM/SenseVoiceSmall (Files tab) and drop it in
the folder. Verify the final path:

```powershell
Get-Item models\SenseVoiceSmall\model.pt | Select-Object FullName, @{n='SizeMB';e={[math]::Round($_.Length/1MB)}}
```

### 1d. Fill in your config

```powershell
Copy-Item xiaozhi-config.override.example.yaml data\.config.yaml
Copy-Item .env.example .env
ipconfig   # note AIBOX's LAN IPv4, e.g. 192.168.1.50
```

Edit `data\.config.yaml`:

- `server.websocket` and `server.vision_explain` → **AIBOX's LAN IP**. This is
  the address the robot will be told to connect to, so it must be AIBOX's real
  LAN IP — not `localhost`, not `0.0.0.0`.
- `plugins.web_search.api_key` → your Tavily key (`tvly-...`, free tier at
  https://tavily.com). Or tell me and we'll do fully-offline SearXNG instead.

### 1e. Bring it up (still on AIBOX)

```powershell
docker compose up -d
docker compose logs -f xiaozhi-server
```

Healthy startup logs the loaded modules (`SileroVAD`, `FunASR`, `OllamaLLM`,
`KokoroTTS`) and starts the websocket on `:8000`. First boot is slow — it loads
the VAD/ASR models. Smoke-test the OTA endpoint locally:

```powershell
curl http://localhost:8003/xiaozhi/ota/
```

Don't move on until this section is solid. Everything after this assumes AIBOX
is serving.

---

## Step 2 — [MYPC] Test the server before touching the robot

Run the desktop test client **on MYPC** (same LAN as AIBOX). This proves the
_server_ works, so if something's off later you know it's the robot, not AIBOX.
The robot is never the thing under test here.

```bash
git clone https://github.com/huangjunsen0406/py-xiaozhi
cd py-xiaozhi
pip install -r requirements.txt
# point it at ws://<AIBOX-LAN-IP>:8000/xiaozhi/v1/  (see its config/README)
python main.py
```

Talk to it. You want: speech recognized (ASR) → sensible reply (LLM) → spoken
back (TTS). Ask something current ("what's the weather in Tokyo right now") to
confirm the web-search tool fires. Watch `docker compose logs` on AIBOX to see
the requests land. Only once this loop is solid do you repoint the robot.

---

## Step 3 — [MYPC] Repoint the robot (try this first, no rebuild)

On first boot with no Wi-Fi, or after a reset, the robot serves a Wi-Fi setup
portal. Connect MYPC to its AP and look for an **Advanced / OTA URL** field. Set:

```
http://<AIBOX-LAN-IP>:8003/xiaozhi/ota/
```

Save, reconnect. Done — no flashing, DEVBOX not involved. The device re-reads the
OTA URL on every boot, so this sticks in NVS. Trigger the AI agent, say "Hi
StackChan," and watch AIBOX's `docker compose logs` to confirm it's talking to
your box.

If the portal has no OTA-URL field, fall through to Step 4.

---

## Step 4 — [DEVBOX] → [MYPC] Repoint by rebuild (only if Step 3 fails)

**On DEVBOX** (has ESP-IDF; no robot attached):

```powershell
cd firmware
Copy-Item sdkconfig.defaults.local.example sdkconfig.defaults.local
# edit sdkconfig.defaults.local: set CONFIG_OTA_URL to AIBOX's LAN IP
. C:\Users\rodriguezala\eim\activate-idf.ps1
idf.py build
# regenerate the portable flash bundle
..\scripts\make-flash-bundle.ps1
```

`sdkconfig.defaults.local` is git-ignored (it holds AIBOX's LAN IP), and
`firmware/CMakeLists.txt` auto-layers it over the committed defaults.

**Then on MYPC** (has the robot on USB): copy `dist\stackchan-flash.zip` from
DEVBOX to MYPC over your remote session, unzip, and run the flash script inside
it against the robot's COM port. (DEVBOX can't flash — the robot is plugged into
MYPC, not the cloud.)

After the robot reconnects, trigger the agent and confirm via AIBOX logs.

---

## When AIBOX / the GPU is off

Honest state: this ecosystem has **no built-in failover**. On connect failure the
robot just shows an error and idles — no retry, no fallback URL. Building graceful
degradation (a CPU-only backend + a health-checking reverse proxy that
`CONFIG_OTA_URL` points at) is milestone **`w3-fallback`**, scheduled last on
purpose. For now: AIBOX up = agent works; AIBOX down = agent unavailable. The
on-device wake word and the (separate) dance mode work regardless.

---

## Ports reference (all on AIBOX)

| Port  | Service                 | Who talks to it                     |
| ----- | ----------------------- | ----------------------------------- |
| 8000  | xiaozhi voice websocket | robot, py-xiaozhi (on MYPC)         |
| 8003  | OTA + vision HTTP       | robot (`CONFIG_OTA_URL`)            |
| 11434 | Ollama (native)         | xiaozhi-server container            |
| 8880  | Kokoro TTS              | xiaozhi-server container (internal) |

---

## What's deliberately not done yet

- `w3-websearch`: fully self-hosted SearXNG + MCP (currently Tavily, needs internet).
- `w3-goserver`: self-hosting M5Stack's Go backend + patching its hardcoded
  `https://xiaozhi.me/` calls, for the phone-app / dances / binding features.
- `w3-fallback`: CPU-only degraded tier + failover proxy.
- `w4-*`: migrating off this borrowed Python stack to a minimal self-owned server.

See `docs/plan-offline-ai-and-dance.md` for the full roadmap and rationale.
