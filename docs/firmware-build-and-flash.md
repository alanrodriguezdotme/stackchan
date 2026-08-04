# Firmware build and flash runbook (Windows)

Reference for building and flashing the StackChan firmware onto the CoreS3
(ESP32-S3). Reopen this every time you sit down at a different machine.

Verified against `firmware/README.md`, `firmware/CMakeLists.txt`,
`firmware/partitions.csv`, and a first-hand Windows 11 build write-up
(<https://zenn.dev/pinelibg/articles/stackchan-firmware-build>).

---

## Toolchain

- **ESP-IDF v5.5.4** exactly. Not 5.5.1, not 6.0. The build hard-fails on the
  wrong version (`project depends on idf (>=5.5.2)`), and building against a
  wrong *target* produces confusing unrelated errors deep in
  `lvgl_display.cc` / `V4L2_PIX_FMT_RGB565`.
- Windows, non-WSL is a proven path. Either the ESP-IDF VSCode extension or the
  standalone ESP-IDF Installation Manager (CLI-only) works.
- Target: `esp32s3`. After ever changing the target, run **Full Clean** —
  otherwise ninja fails with `loading 'build.ninja'`.

---

## ⚠️ Windows trap #1: the patch silently doesn't apply

`fetch_repos.py` applies `patches/xiaozhi-esp32.patch` via `git apply`. On Windows,
Git's CRLF conversion makes the patch fail — and the script **prints "skipped" and
exits 0**, so it looks like it worked. You then get baffling build errors.

Fix (already applied in this repo): both `git apply` invocations in
`clone_or_update_repo()` pass `--ignore-whitespace`:

```python
check_result = subprocess.run(
    ["git", "-C", path, "apply", "--ignore-whitespace", "--check", patch_full_path]
)
if check_result.returncode == 0:
    subprocess.run(["git", "-C", path, "apply", "--ignore-whitespace", patch_full_path], check=True)
```

(Alternative: `git config --global core.autocrlf false` and re-clone. The script
edit is more reliable and doesn't disturb global git config.)

After fetching, verify the log has **no** `patch does not apply` / `skipped` lines.

---

## ⚠️ Windows trap #2: OTA will silently overwrite your firmware

Every time AI.AGENT starts, the factory firmware checks for updates and OTA-flashes
a newer official build over yours. Disable this before flashing anything custom.
Two options — do at least one:

- Comment out **only** the `UpgradeFirmware(...)` call in
  `xiaozhi-esp32/main/application.cc` (`Application::CheckNewVersion`). Do **not**
  comment out the whole `CheckNewVersion` function — that's also what fetches the
  xiaozhi websocket endpoint from the OTA server, which the offline-AI work depends
  on.
- And/or set `PROJECT_VER` in `firmware/CMakeLists.txt` to something like `99.9.9`
  so the server never considers itself newer.

Note: `xiaozhi-esp32/` only exists **after** `fetch_repos.py` runs, so this step
happens post-fetch.

---

## Build

```powershell
cd C:\Users\rodriguezala\src\StackChan\firmware
python fetch_repos.py          # after the --ignore-whitespace fix
idf.py set-target esp32s3
idf.py build
```

---

## What gets built and where it goes

`idf.py flash` writes all four regions — you don't assemble anything by hand.
`firmware/main/CMakeLists.txt:507-531` registers the assets partition via
`esptool_py_flash_to_partition()`, so it's included automatically.

| Artifact (under `firmware/build/`) | Flash offset | Notes |
|---|---|---|
| `bootloader/bootloader.bin` | `0x0` | |
| `partition_table/partition-table.bin` | `0x8000` | from `partitions.csv` |
| `ota_data_initial.bin` | `0xd000` | selects the boot OTA slot; without it the device may boot the wrong/old slot |
| `stack-chan.bin` | `0x20000` | the app → `ota_0` |
| `generated_assets.bin` | `0xA00000` | ~2.3 MB; fonts, wake-word model, emoji, sounds |

`ota_1` is left holding the previous image — that's the recovery slot.

Flash settings baked in by the build (`flasher_args.json`): `--flash_mode dio
--flash_size 16MB --flash_freq 80m`, chip `esp32s3`.

---

## Flash

Put the CoreS3 into download mode first:
1. Connect USB-C (**data** cable — power-only cables are a classic waste of an hour).
2. Press and hold **RESET** for ~2–3 s until the internal green LED lights.
3. Release. Screen stays black with backlight on = download mode.
4. Confirm a new COM port appears.

```powershell
idf.py -p COM<N> flash monitor
```

Success looks like `Wrote 2298140 bytes ... at 0x00a00000 ... Hash of data verified.`
Note: the StackChan *body* has its own USB-C for power/data — make sure you're on
the port that actually enumerates a serial device.

---

## Fastest inner loop

`idf.py app-flash` reflashes only the app partition — much quicker than a full
flash, and correct whenever assets haven't changed (i.e. nearly every dance-tuning
iteration).

---

## Flashing from a different machine (build here, flash there)

When the build happens on a headless/cloud box but the device is on another
machine, you don't need ESP-IDF on the flashing machine — just `esptool` and the
five build artifacts.

1. Copy the flash bundle (see `dist/` produced by `scripts/make-flash-bundle.ps1`,
   or grab the five `.bin` files listed above plus `flasher_args.json` from
   `firmware/build/`).
2. On the flashing machine, install esptool: `pip install esptool` (or use the
   copy inside any ESP-IDF install).
3. Put the CoreS3 into download mode (below), note its COM port, and run the
   bundled `flash.ps1 -Port COM<N>` — or the raw command:

```powershell
python -m esptool --chip esp32s3 -b 460800 --before default_reset --after hard_reset `
  write_flash --flash_mode dio --flash_size 16MB --flash_freq 80m `
  0x0      bootloader.bin `
  0x8000   partition-table.bin `
  0xd000   ota_data_initial.bin `
  0x20000  stack-chan.bin `
  0xa00000 generated_assets.bin
```

(Offsets are authoritative from `firmware/build/flasher_args.json`. The bundle
flattens the `bootloader/` and `partition_table/` subfolders, so the filenames
above are bare.)

---

## Fastest inner loop (on the build machine with a device attached)

`idf.py app-flash` reflashes only the app partition — much quicker than a full
flash, and correct whenever assets haven't changed.

---

## Recovery if a flash goes bad

Best path is **M5Burner** (M5Stack's official flasher): pick
**`StackChan-UserDemo`** — that's the factory firmware. If a normal flash won't
take, use erase-flash mode, but note that wipes NVS too, so Wi-Fi and pairing must
be redone.

Dual OTA partitions mean the previous image lives in `ota_1` as a fallback, so a
single bad app-flash doesn't brick the device.
