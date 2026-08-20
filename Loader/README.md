# Loader Scripts — Usage Guide

The `Loader/` directory contains Python scripts for signing firmware and
uploading it to the STM32 over UART.

## Overview

| File | Purpose | When to use |
|---|---|---|
| `rsa_sign_demo.py` | RSA sign/verify demo from scratch. Generates key pair, signs firmware, tests tamper detection, prints public key as C array | First-time setup, or when changing RSA keys |
| `upload_firmware.py` | Upload firmware (CRC32 only, no RSA) | When RSA is disabled (`RSA_SECURE_BOOT_ENABLE = 0`) |
| `upload_firmware_rsa.py` | Upload firmware with RSA signature + magic marker | **Default — use every time you update firmware** |

## Prerequisites

```bash
pip install pyserial
```

No external crypto library needed — RSA and SHA-256 use the Python standard
library only.

## Workflow

### First-time setup (once only)

```
Step 1: Generate RSA key pair
  ┌──────────────────────────────────────────┐
  │ cd Loader                                │
  │ python rsa_sign_demo.py --bits 32        │
  └──────────────────────────────────────────┘
  → Creates rsa_keys_32.json (contains n, e, d)
  → Prints public key as C array

Step 2: Copy public key into bootloader
  ┌──────────────────────────────────────────────────────┐
  │ Open Core/Inc/bootloader.h                           │
  │ Replace rsa_public_n[] with values from demo output  │
  │ Verify RSA_PUBLIC_E = 65537 (must match demo)        │
  └──────────────────────────────────────────────────────┘

Step 3: Build + Flash bootloader
  ┌──────────────────────────────────────────┐
  │ build.bat                                │
  │ Flash Debug\STM32H750_Bootloader.elf     │
  └──────────────────────────────────────────┘
```

### Every firmware update (.bin changed)

```
  ┌──────────────────────────────────────────┐
  │ 1. Build application (.bin)              │
  │ 2. cd Loader                             │
  │ 3. python upload_firmware_rsa.py          │
  │ 4. Press RESET when prompted              │
  └──────────────────────────────────────────┘
```

**Just one command!** `upload_firmware_rsa.py` automatically:
- Loads RSA keys from `rsa_keys_32.json`
- Signs firmware with the private key (d)
- Builds payload: `[firmware] [signature] [fw_size footer] [magic marker 0x55667788]`
- Computes CRC32 over the entire payload
- Uploads via UART

---

## Script Details

### 1. `rsa_sign_demo.py` — RSA Demo & Key Generation

Run **once** to generate the RSA key pair and verify the sign/verify logic.

```bash
# Generate RSA-32 keys (default)
python rsa_sign_demo.py

# Reuse existing keys (skip key generation)
python rsa_sign_demo.py --skip-keygen

# RSA-2048 (production security)
python rsa_sign_demo.py --bits 2048
```

**Output:**
- `rsa_keys_32.json` — key pair file (n, e, d)
- `firmware_signed.bin` — signed firmware (for testing)
- Public key printed as C array → copy into `bootloader.h`

**What the demo does:**
1. Generate RSA key pair (p, q → n, e, d)
2. Read firmware `.bin`
3. Sign: `signature = SHA256(fw)[:4] ^ d mod n`
4. Verify: `recovered = signature^e mod n == digest?`
5. Tamper test: flip 1 byte → verify FAIL
6. Build `firmware_signed.bin`
7. Print public key as C array

> **⚠️ RSA-32 is educational only!** Use `--bits 2048` for production.

---

### 2. `upload_firmware_rsa.py` — Upload Firmware (with RSA)

**Main upload script.** Automatically signs firmware before sending.

```bash
python upload_firmware_rsa.py
```

**Configuration (edit in file):**
```python
PORT = "COM8"        # ← Change to your COM port
BIN_FILE = r"...\STM32H750_Application\Debug\STM32H750_Application.bin"
RSA_KEY_FILE = "rsa_keys_32.json"
```

**Payload sent over UART:**
```
[FWUP header (12B)] [firmware] [RSA signature (4B)] [fw_size footer (4B)] [magic marker 0x55667788 (4B)]
```

**Expected UART output:**
```
[OK] UART firmware CRC verified
[OK] External flash CRC verified
[*] Verifying RSA signature...
[OK] RSA signature verified during update
Update successful, rebooting...

(After reboot:)
[*] Verifying RSA signature...
[OK] RSA signature verified — firmware is authentic
[OK] Jumping to application...
(Application runs)
```

---

### 3. `upload_firmware.py` — Upload Firmware (CRC32 only, no RSA)

Use when RSA is disabled (`RSA_SECURE_BOOT_ENABLE = 0` in `bootloader.h`).

```bash
python upload_firmware.py
```

Sends raw firmware binary — no signature, footer, or marker.

---

## Generated Files

| File | Created by | Purpose |
|---|---|---|
| `rsa_keys_32.json` | `rsa_sign_demo.py` | RSA key pair (n, e, d) — d is SECRET |
| `rsa_private.pem` | `rsa_sign_demo.py` | Private key in PEM format (for RSA-2048) |
| `rsa_public.pem` | `rsa_sign_demo.py` | Public key in PEM format |
| `firmware_signed.bin` | `rsa_sign_demo.py` | Signed firmware (test file) |

> **⚠️ Do NOT commit `rsa_keys_32.json` to git!** It contains the private key (d).

## Troubleshooting

### RSA verification FAIL

| Cause | Fix |
|---|---|
| Public key in `bootloader.h` ≠ keys in `rsa_keys_32.json` | Run `rsa_sign_demo.py`, copy new public key into `bootloader.h`, rebuild |
| Firmware changed but old signature used | Cannot happen — `upload_firmware_rsa.py` signs every time |
| `rsa_keys_32.json` missing | Run `rsa_sign_demo.py --bits 32` to generate |

### Magic marker not found

| Cause | Fix |
|---|---|
| Flash not erased before write | Bootloader auto-erases — ensure `Bootloader_EraseExternalFlash()` runs |
| Using `upload_firmware.py` instead of `upload_firmware_rsa.py` | Use `upload_firmware_rsa.py` to include the marker |

### Upload timeout

| Cause | Fix |
|---|---|
| Wrong COM port | Check Device Manager, update `PORT` in script |
| Firmware too large | Verify `MAX_FIRMWARE_SIZE = 8MB` |
| Baud rate mismatch | Ensure `BAUD = 115200` matches bootloader |

## Security Notes

- **RSA-32** = educational only (factorable in milliseconds)
- **RSA-2048** = production security (use `--bits 2048`)
- **Private key (d)** is NEVER stored on the STM32 — only the public key (n, e)
- **Magic marker `0x55667788`** marks end-of-payload — more reliable than
  scanning for `0xFF` (firmware binary can contain `0xFF` bytes)
- To lock the device: enable RDP Level 2 (irreversible!) — see guide Step 10.4
