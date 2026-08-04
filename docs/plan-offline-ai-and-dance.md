# StackChan: offline AI agent + reactive dance

> Working plan for a personal fork of `m5stack/StackChan`. Lives in the repo so it
> travels across machines. The build/flash mechanics are split into
> [`firmware-build-and-flash.md`](./firmware-build-and-flash.md).

## Problem

Two changes to the M5Stack StackChan (K151, CoreS3/ESP32-S3), based on the
`m5stack/StackChan` repo:

1. **AI agent works offline.** Today ASR/LLM/TTS all go to `xiaozhi.me`. Target:
   run the whole voice pipeline on a LAN GPU box, still able to search the web on
   demand, and degrade gracefully (slower, worse TTS) when the GPU box is off.
2. **Replace dance mode.** Today it only works with M5Stack's music and is
   unreliable. Target: (a) dance to whatever music the onboard mics hear, and
   (b) manual joystick head control from the phone app.

Priority: beat-reactive dance first, then joystick, with the offline AI work
running alongside (it's mostly deployment, not firmware).

---

## What the recon actually found

### The AI agent is xiaozhi-esp32

`firmware/repos.json` clones `78/xiaozhi-esp32` @ `v2.2.4` and applies
`firmware/patches/xiaozhi-esp32.patch`. This is enormously good news — the device
speaks the standard xiaozhi protocol, so it will talk to any xiaozhi-compatible
server.

- Wake word is **already fully on-device** — `CONFIG_SR_WN_WN9_HISTACKCHAN_TTS3=y`
  (ESP-SR WakeNet, "Hi StackChan"), and `CONFIG_SEND_WAKE_WORD_DATA=n`. No cloud.
- The websocket server address is **not compiled in**. On every boot the device
  POSTs to `CONFIG_OTA_URL` and the response JSON's `websocket.url` is saved to
  NVS. Change the OTA URL → change the entire AI backend.
- The M5Stack patch only: silences activation-code speech, disables M5Stack's
  closed `EmoteStrategy` display, adds a non-fatal I2C read helper.

**Two separate server dependencies, often confused:**

| Kconfig | Default | What it's for |
|---|---|---|
| `CONFIG_OTA_URL` | `https://api.tenclass.net/xiaozhi/ota/` | Voice AI: hands back the ASR/LLM/TTS websocket URL |
| `CONFIG_STACKCHAN_SERVER_URL` | `http://47.113.125.164:12800` | M5Stack Go backend: device binding, app store, dances, phone-app WS relay |

`firmware/CMakeLists.txt:11-20` auto-loads a git-ignored `sdkconfig.defaults.local`
overlay, with a comment explicitly saying it exists so self-hosted deployments can
pin these URLs. M5Stack anticipated this.

Caveat: the repo's own Go server still phones home —
`server/internal/xiaozhi/xiaozhi.go` hardcodes `baseUrl = "https://xiaozhi.me/"`
for agent/device management. Full airgap needs that patched out.

### Dance mode is broken by design

`firmware/main/apps/app_dance/app_dance.cpp` is a **dumb BLE puppet**. It starts a
BLE server and applies avatar/motion/RGB JSON the instant it arrives. There is no
timeline, no music, no choreography on the device.

The *phone* owns everything: `app/lib/view/home/dance.dart` plays the music
locally via `just_audio`, then walks the keyframe list sending one frame at a time
with `await Future.delayed(durationMs)` (BLE path adds a hardcoded `+ 70` ms fudge).
That's why it drifts and "often doesn't work" — no clock sync, BLE jitter accumulates,
and the audio is coming out of your phone, not the robot.

**But the firmware already has the right machinery, unused:**
- `stackchan/animation/animation.h` — `Keyframe`, `KeyframeSequence`, `Timeline`
  (start/stop/pause/resume/update, loop support) running fully on-device.
- `stackchan/modifiers/dance.h` — a `DanceModifier` with built-in `Happy`, `Robot`,
  `Panic`, `LookAround` sequences driven by that `Timeline`.

So on-device choreography is a wiring job, not a from-scratch build.

### Motion API is already joystick-shaped

`stackchan/motion/motion.h:129` —
`lookAtNormalized(float x, float y, int speed)`, documented as *"ideal for visual
tracking or joystick-based control"*, maps `[-1,1]²` onto the servo limits.
`servo.h` has spring/damping easing, torque control, stall protection, and
`setAutoAngleSyncEnabled()` (which `app_dance.cpp:143` already toggles based on
command recency — exactly the "am I being puppeted right now?" heuristic).

On the app side `app/lib/view/util/grid_coordinate_joystick.dart` already exists,
already has an `onImmediatelyRelease` real-time callback, and is already wired into
`app/lib/view/popup/motion.dart`. The joystick feature is ~60% built.

### Mic is available and full-duplex

`firmware/main/hal/board/config.h`: ES7210 dual-channel mic ADC + AW88298 speaker
amp on a shared full-duplex I2S bus. 24 kHz, 16-bit, 2 channels
(**ch0 = mic, ch1 = speaker reference** — a hardware loopback for AEC, so we can
subtract the robot's own audio).

`hal/audio.cpp:117` `getMicWaveformFrame()` is the existing hook, but it's a
blocking pull that grabs 768 frames (~32 ms) on demand. Beat detection needs a
proper streaming FreeRTOS task instead.

Nothing currently does FFT. ESP-DSP (`dsps_fft2r_fc32`) is available in ESP-IDF and
is the right tool — hand-rolling or pulling in GPL'd `arduinoFFT` is unnecessary.

### Safety net

`firmware/partitions.csv` has dual OTA slots (`ota_0`/`ota_1`, 0x4f0000 each) on
16 MB flash + a `coredump` partition. Bad flashes are recoverable over USB.
`firmware/tests/` has a host-side CMake/ctest harness for the motion math — we can
TDD the beat→motion mapping with zero hardware in the loop.

---

## Ecosystem survey (the "what else exists" ask)

**Core stack-chan**
- `stack-chan/stack-chan` — ⭐1.6k, very active. The canonical community firmware
  (Moddable JS/TS on ESP-IDF). Browser flasher, block editor, WASM simulator, MOD
  gallery. Different lineage from your K151 firmware, but the best idea mine.
  https://github.com/stack-chan/stack-chan
- `m5stack/StackChan-BSP` — ⭐43, new (Apr 2026). Official Arduino BSP for *your*
  K151 body (feedback servos, 12 RGB LEDs, IR, NFC, touch zones).
  https://github.com/m5stack/StackChan-BSP
- `stack-chan/m5stack-avatar` — ⭐230. The de-facto kawaii face lib for Arduino
  builds, incl. lip-sync API. https://github.com/stack-chan/m5stack-avatar
- `mongonta0716/stack-chan-tester` — ⭐43, active. Servo calibration utility.
  https://github.com/mongonta0716/stack-chan-tester
- `mongonta0716/stackchan-bluetooth-simple` — ⭐22, pushed 2026-07-31. Minimal BLE
  servo/expression/TTS control. Good small reference.
  https://github.com/mongonta0716/stackchan-bluetooth-simple

**LLM voice firmwares**
- `robo8080/M5Unified_StackChan_ChatGPT` — ⭐152 but stale since 2023. Historically
  important (established the ASR-text → LLM → TTS → face pattern), Japanese docs.
  https://github.com/robo8080/M5Unified_StackChan_ChatGPT

**Xiaozhi ecosystem (directly relevant — this IS your firmware)**
- `78/xiaozhi-esp32` — ⭐28.6k, pushed daily. Upstream of your firmware.
  English README + good `docs/websocket.md`. https://github.com/78/xiaozhi-esp32
- `xinnan-tech/xiaozhi-esp32-server` — ⭐10.2k, active. **The** mature self-hosted
  server. Python/FastAPI/Docker, modular ASR/LLM/TTS/VAD, web console.
  https://github.com/xinnan-tech/xiaozhi-esp32-server
- `hackers365/xiaozhi-esp32-server-golang` — Go, FunASR + Ollama + CosyVoice, web
  console, Docker AIO. https://github.com/hackers365/xiaozhi-esp32-server-golang
- `AnimeAIChat/xiaozhi-server-go` — ⭐469, Go. **Cloud-only ASR/TTS (Doubao/Edge)**
  — does not meet the offline goal. https://github.com/AnimeAIChat/xiaozhi-server-go
- `joey-zhou/xiaozhi-esp32-server-java` — ⭐1.3k, Spring Boot + MySQL + Redis.
  Heavy. https://github.com/joey-zhou/xiaozhi-esp32-server-java
- `huangjunsen0406/py-xiaozhi` — ⭐3.4k. Python desktop client speaking the same
  protocol — **excellent for testing your server without touching the robot**.
  https://github.com/huangjunsen0406/py-xiaozhi

**Local voice building blocks**
- `espressif/esp-sr` — ⭐1.5k. WakeNet/MultiNet/AEC/AFE. Already in your firmware.
  https://github.com/espressif/esp-sr
- `k2-fsa/sherpa-onnx` — ⭐13.9k, daily. Offline ASR+TTS+VAD, CPU-friendly ONNX.
  https://github.com/k2-fsa/sherpa-onnx
- `ggml-org/whisper.cpp` — ⭐52.5k. https://github.com/ggml-org/whisper.cpp
- `remsky/Kokoro-FastAPI` — ⭐5.3k. OpenAI-compatible local TTS server.
  https://github.com/remsky/Kokoro-FastAPI
- ⚠️ `rhasspy/piper` is **archived** as of ~Aug 2025. Use sherpa-onnx TTS (which
  reads Piper ONNX models) instead. https://github.com/rhasspy/piper

**Audio-reactive prior art**
- `wled/WLED` — ⭐18.5k. Its `usermods/audioreactive/` is the most production-tested
  FFT + beat detection + AGC on ESP32. Best reference for the dance work.
  https://github.com/wled/WLED
- `kosme/arduinoFFT` — ⭐657, **GPL-3.0** (licence-incompatible with this MIT repo —
  avoid; use ESP-DSP). https://github.com/kosme/arduinoFFT

---

## Decision: offline AI architecture

Nobody has publicly documented self-hosting for the K151 specifically. The generic
xiaozhi guides apply, but deployment docs are largely Chinese-only.

### Option A — Self-host `xinnan-tech/xiaozhi-esp32-server`, keep the Go server for device/app/dance
Point `CONFIG_OTA_URL` at your box. Go server stays for the phone app, dances, app store.

- ✅ Least new code. Mature, ~10k stars, actively developed.
- ✅ Only stack with a documented fully-local combo (SherpaASR/FunASR + Ollama + PaddleSpeech/IndexTTS + SileroVAD).
- ✅ Built-in `function_call` intent mode + MCP sidecar → web search is config, not code.
- ✅ Reversible: flip the OTA URL back to go cloud again.
- ❌ Chinese-only deployment docs; Python/Docker stack to babysit.
- ❌ Still need to patch `server/internal/xiaozhi/xiaozhi.go` for a true airgap.
- ❌ Docker images x86-only from v0.8.2+.

### Option B — Build ASR/LLM/TTS into the repo's Go server
One server to rule them all.

- ✅ Single service, one language, full control, no Chinese docs.
- ❌ You reimplement the xiaozhi websocket protocol, Opus framing, VAD gating,
  streaming TTS chunking, and the OTA handshake — weeks of work for parity.
- ❌ Go's local-inference story is weak; you'd be shelling out to Python anyway.
- ❌ You own every protocol break when xiaozhi-esp32 moves.

### Option C — New minimal xiaozhi-protocol server from scratch
- ✅ Small, comprehensible, exactly your pipeline.
- ✅ `docs/websocket.md` upstream is decent English, and `py-xiaozhi` is a working
  reference client to test against.
- ❌ Same protocol-chasing burden as B, minus the existing Go plumbing.
- ❌ No web console, no provider ecosystem — you write every ASR/TTS adapter.

### DECIDED: A now, C later

Start on A to get a working end-to-end system on borrowed infrastructure, then
migrate to a minimal self-owned server (C) once the protocol is understood. B is
ruled out — bolting inference onto the Go server gets the worst of both.

This ordering has a concrete engineering consequence: **treat phase A as a
protocol-capture exercise, not just a deployment.** Specifically —

- Run the xinnan-tech server with verbose websocket logging from day one and keep
  the transcripts. Those become the spec for the phase-C rewrite.
- Build the phase-C server against `py-xiaozhi` as a conformance client before
  ever pointing the robot at it, so the robot is never the thing under test.
- Keep a captured golden session (hello handshake → Opus frames up → JSON events
  → Opus TTS frames down) as a regression fixture.
- Deliberately don't over-invest in phase-A customisation — anything written into
  the Python server's provider layer is throwaway. Config, yes; forked code, no.
- The one exception: the SearXNG MCP wrapper (`w3-websearch`) is worth writing
  properly, since MCP is transport-agnostic and survives the migration intact.

### Degradation when the GPU box is off — honest assessment

**This is not a solved problem in this ecosystem.** Verified from
`78/xiaozhi-esp32` sources: on connect failure the device fires a network-error
callback, shows an alert, returns to idle. No retry loop, no fallback URL, no
on-device TTS. Nobody has published a dual-server failover setup.

Realistic ladder:

| Tier | Stack | End-to-end latency |
|---|---|---|
| GPU box up | FunASR (GPU) + Ollama Qwen2.5-14B + IndexTTS | ~1–2 s |
| Always-on CPU box | SherpaASR (ONNX int8, CPU) + Ollama 1.5B–3B + PaddleSpeech | ~5–15 s |
| Everything down | ESP-SR MultiNet on-device command words only | instant, ~no vocabulary |

Note: all local ASR options are **batch, not streaming** — the full utterance
length adds to latency. That's the main felt difference vs cloud, more than raw
model speed. Also: EdgeTTS is "free" but needs internet — it does not count as local.

Failover approach: put nginx (or Caddy) with health checks in front of both
backends, point `CONFIG_OTA_URL` at the proxy. The device re-reads the OTA response
on every boot, so the proxy picks the live backend at connect time. This is DIY —
budget real time for it.

### Web search while offline

"Offline" here means no cloud *AI*; web search is an explicit tool call. Two paths:

- **Fast:** the server's built-in `web_search` plugin (Tavily / Metaso). Config
  only, but external API + key.
- **Self-hosted:** local SearXNG + an MCP wrapper behind `mcp-endpoint-server`.
  Architecturally supported, **not documented anywhere as a complete recipe** —
  we'd write the MCP wrapper.

Either way the LLM must tool-call reliably: Qwen2.5-7B/14B or Qwen3-8B minimum.
1.5B/3B models are not dependable for this, so the CPU fallback tier realistically
loses web search.

---

## Git / remote setup

This is a personal fork. Current state:

- The M5Stack upstream `origin` remote has been **removed**.
- The single remote is **`personal`** → `https://github.com/alanrodriguezdotme/stackchan.git`.
- Work happens on a feature branch off `main` (`feature/offline-ai-and-dance`),
  never directly on `main`.

If you ever want to pull M5Stack's upstream changes again, re-add it read-only:

```powershell
git remote add upstream https://github.com/m5stack/StackChan.git
git fetch upstream
```

---

## Execution scope

**Branch:** `feature/offline-ai-and-dance` off `main`. Nothing lands on `main`
directly. Commits are local; pushing to `personal` is done by the repo owner once
auth is sorted.

**Stop point: end of W1.** W1 produces something testable on real hardware —
a standalone on-device dance mode that reacts to music through the mics with no
phone and no server involved. That's the natural place to pause and get real-world
feedback, because everything downstream (choreography feel, servo amplitude,
false-positive rate, CPU headroom) can only be judged on the device.

In scope now: **W0 + W1.**
Deferred until after hardware testing: **W2, W3, W4.**

### Hand-off checklist at the W1 stop point
- Factory flash backup verified restorable (from `w0-backup`).
- Host-side ctest suite green for the beat detector against WAV fixtures.
- Beat-detection tuning parameters (threshold, refractory period, band edges,
  AGC rate) exposed somewhere adjustable without a full rebuild cycle.
- A way to observe what the detector thinks it's hearing on-device — on-screen
  band meter / beat flash — so "it didn't dance" is diagnosable as either
  *didn't hear it* or *heard it and moved badly*.
- Known-limitations note covering what's deliberately unfinished.

---

## Todos

### W0 — Toolchain and a flash you can undo
- `w0-branch` — Create a new local branch off `main`. All work lands there.
- `w0-remote` — Remote `personal` → your repo; `origin` (M5Stack) removed. Commit locally; push is done by the owner (`git push -u personal <branch>`) once auth is sorted.
- `w0-plan-doc` — Commit this plan to `docs/plan-offline-ai-and-dance.md`.
- `w0-flash-doc` — Commit the build/flash runbook to `docs/firmware-build-and-flash.md`.
- `w0-patch-fix` — Add `--ignore-whitespace` to both `git apply` calls in `fetch_repos.py`, then verify the xiaozhi patch actually applied. Silent failure otherwise.
- `w0-toolchain` — ESP-IDF v5.5.4 on Windows, `python fetch_repos.py`, `idf.py set-target esp32s3`, clean `idf.py build`.
- `w0-no-ota` — Disable auto-update before flashing custom firmware: comment out only the `UpgradeFirmware(...)` call in `application.cc` (keep the rest of `CheckNewVersion`), and/or bump `PROJECT_VER` to `99.9.9`.
- `w0-backup` — Full 16 MB flash dump + NVS backup over USB. Belt-and-braces; M5Burner `StackChan-UserDemo` is the primary recovery path.
- `w0-baseline-flash` — Flash unmodified self-built firmware, confirm parity with factory.
- `w0-host-tests` — Get `firmware/tests/` ctest harness green as the TDD target.

### W1 — Beat-reactive dance (first priority)
- `w1-audio-tap` — Streaming mic task: dedicated FreeRTOS task pulling ch0 from `audio_codec->InputData()`, ring-buffered, decoupled from the blocking `getMicWaveformFrame()` path.
- `w1-fft` — ESP-DSP `dsps_fft2r_fc32` over a Hann-windowed frame, split into bass/mid/treble bands, with AGC. Port the approach from WLED's audioreactive usermod (do not vendor GPL code).
- `w1-beat` — Onset/beat detector: rolling energy average, adaptive threshold, refractory period; running BPM estimate. Pure function, unit-tested on host with synthetic + recorded WAV fixtures.
- `w1-choreo` — Beat→motion mapping: generative moves driven by `Timeline` + `lookAtNormalized`, with an amplitude→expression and →RGB mapping. Must respect servo limits, stall protection, and never fight `setAutoAngleSyncEnabled`.
- `w1-app-mode` — New on-device mode (either rework `AppDance` or add a sibling app) that runs entirely standalone: no phone, no server, no BLE required. This is the core fix.
- `w1-aec` — Use ch1 (speaker reference) to avoid the robot's own speaker output triggering beats.
- `w1-tuning` — Expose beat-detector parameters adjustably without a full rebuild, plus an on-screen band/beat debug view.

<!-- STOP HERE for on-device testing. W2/W3/W4 below are deferred. -->

### W2 — Joystick / puppet mode
- `w2-transport` — Low-latency control path. BLE direct (~20–50 ms) beats the server round-trip (~200–400 ms). Decide BLE vs a direct-LAN UDP path.
- `w2-firmware` — Continuous streaming control handler using `lookAtNormalized`, rate-limited (~20–30 Hz), with a watchdog that re-centres/releases torque on signal loss.
- `w2-app-ui` — Promote `GridCoordinateJoystick` to a first-class control screen; throttle `onImmediatelyRelease`; add expression + RGB controls.
- `w2-record` — Optional: record a puppet session into an on-device `KeyframeSequence` for replay.

### W3 — Offline AI
- `w3-server-deploy` — Stand up the chosen server on the GPU box (Ollama + ASR + TTS + VAD).
- `w3-test-client` — Validate with `py-xiaozhi` from a laptop **before** repointing the robot.
- `w3-repoint` — `firmware/sdkconfig.defaults.local` with `CONFIG_OTA_URL` + `CONFIG_STACKCHAN_SERVER_URL`. Try the runtime captive-portal "Advanced Options" route first — no rebuild.
- `w3-goserver` — Self-host the repo's Go server; patch out the hardcoded `https://xiaozhi.me/` calls in `server/internal/xiaozhi/xiaozhi.go`.
- `w3-websearch` — Wire up web search (Tavily first, then SearXNG+MCP for fully self-hosted).
- `w3-fallback` — Second CPU-only backend + health-checking reverse proxy. Explicitly DIY; schedule it last.

### W4 — Migration to a self-owned server (phase C)
- `w4-capture` — Capture golden protocol transcripts from the phase-A server as regression fixtures. Do this *during* W3.
- `w4-server` — Minimal xiaozhi-protocol server owning only what you need.
- `w4-conform` — Validate against `py-xiaozhi` and the golden transcripts before the robot ever points at it.

---

## Notes and risks

- **Mic ownership conflict.** Xiaozhi's audio pipeline owns the codec when the AI
  agent runs. Beat detection can't just grab the mic concurrently. Simplest v1:
  dance is its own mode. `app_dance.cpp:140` already calls `requestWarmReboot(5)`
  on close, so mode switches are already heavyweight.
- **CPU budget.** FFT + LVGL rendering + servo updates on a 240 MHz dual-core.
  Pin the audio task to the core not doing LVGL. Start at 512-point FFT over
  24 kHz and only go bigger if bass resolution is inadequate.
- **Licensing.** Repo is MIT. `arduinoFFT` and `ServoEasing` are GPL-3.0 — do not
  vendor them. ESP-DSP (Apache-2.0) and the existing spring easing in `servo.h`
  are fine.
- **Servo wear.** Beat-reactive mode moves constantly. Enforce amplitude limits, a
  minimum inter-move interval, and an idle timeout.
- **Don't fight the OTA slot.** Keep the factory image in the other slot for as
  long as possible.
- **Upstream drift.** `xiaozhi-esp32` is pinned at v2.2.4 and moves fast (ESP-IDF
  6.0 upstream vs 5.5.4 here). Do not chase upstream mid-project.
- **Order matters.** W0 gates everything. W3 is mostly deployment and can proceed
  in parallel with W1/W2 since it barely touches firmware.
