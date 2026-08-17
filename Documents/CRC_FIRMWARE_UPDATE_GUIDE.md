# CRC32 Firmware Update Guide

This document describes the CRC32 firmware-update flow implemented in the
STM32H750 bootloader. The bootloader receives a firmware image over UART,
writes it to an external W25Q64 QSPI flash, and verifies integrity using
**CRC-32/ISO-HDLC** (compatible with Python's `zlib.crc32()`).

CRC32 detects accidental corruption during UART transfer and external-flash
programming. It does **not** authenticate firmware; secure boot requires a
digital signature (e.g., ECDSA or Ed25519).

---

## Table of Contents

1. [Why CRC Is Needed](#1-why-crc-is-needed)
2. [System Overview](#2-system-overview)
3. [CRC32 Implementation](#3-crc32-implementation)
4. [Firmware Metadata Header](#4-firmware-metadata-header)
5. [Update Protocol](#5-update-protocol)
6. [Bootloader Update Flow](#6-bootloader-update-flow)
7. [Memory-Mapped Mode Handling](#7-memory-mapped-mode-handling)
8. [Jump to Application](#8-jump-to-application)
9. [Python Upload Script](#9-python-upload-script)
10. [CRC Parameters](#10-crc-parameters)
11. [Configuration Macros](#11-configuration-macros)
12. [File Reference](#12-file-reference)

---

## 1. Why CRC Is Needed

A firmware update passes through several stages before the CPU executes it:

```text
Firmware file on host → UART → bootloader RAM → QSPI program → W25Q64
```

An error at any stage can change one or more bytes. Without an integrity check,
the bootloader only knows how many bytes it received and wrote; it cannot know
whether they are identical to the original firmware.

CRC32 helps detect:

- UART noise, framing errors, dropped bytes, or duplicated data.
- A transfer that ends early but is mistakenly accepted as complete.
- Incorrect chunk ordering, length handling, or page-boundary logic.
- QSPI programming failures or corruption in external flash.
- The wrong `.bin` file or a file changed after metadata was generated.
- Stale or incorrect data returned during flash read-back.

The host calculates CRC32 over the complete firmware before transmission. The
bootloader independently calculates it over received bytes and over the image
read back from external flash. All three values must match:

```text
Host CRC == UART receive CRC == QSPI read-back CRC
```

If they differ, at least one byte is different and the application must not be
booted. This prevents a corrupted vector table or instruction stream from
causing a HardFault, unpredictable behavior, or unreliable startup.

CRC32 is deterministic, inexpensive, streamable, and stronger than a simple
sum or XOR checksum. It can be updated one chunk at a time, so the bootloader
does not need to keep the complete firmware image in RAM.

CRC32 is **not** a security mechanism. An attacker can modify the firmware and
calculate a new matching CRC. Use a cryptographic signature when the bootloader
must reject unauthorized firmware.

---

## 2. System Overview

### Hardware

| Component | Details |
|---|---|
| MCU | STM32H750VBT6 (Cortex-M7, 480 MHz HSI) |
| External Flash | Winbond W25Q64 (64 Mbit / 8 MB, QSPI) |
| QSPI base address | `0x90000000` (memory-mapped XIP) |
| UART | USART1 @ 115200 baud |

### Memory Map

| Region | Address | Size |
|---|---|---|
| QSPI memory-mapped base | `0x90000000` | 8 MB |
| Application region | `0x00000000` (flash offset) | 8 MB (`QSPI_APP_MAX_SIZE`) |

### Key Source Files

| File | Role |
|---|---|
| `Core/Src/main.c` | Bootloader entry point, UPDATE command detection |
| `Core/Src/bootloader.c` | Firmware receive, write, verify, jump-to-app |
| `Core/Inc/bootloader.h` | Bootloader API and configuration macros |
| `Core/Src/crc32.c` | Software CRC-32/ISO-HDLC implementation |
| `Core/Inc/crc32.h` | CRC32 API |
| `Core/Src/external_flash.c` | W25Q64 QSPI driver (indirect + memory-mapped) |
| `Core/Inc/external_flash.h` | Flash geometry, commands, API |
| `Loader/upload_firmware.py` | Host-side Python upload script |

---

## 3. CRC32 Implementation

The bootloader uses a software bitwise CRC-32/ISO-HDLC implementation. No
lookup table is needed, which keeps the code small and avoids the risk of an
incomplete or corrupted table.

### `Core/Inc/crc32.h`

```c
#ifndef INC_CRC32_H_
#define INC_CRC32_H_

#include <stdint.h>
#include <stddef.h>

#define CRC_TEST (0) /* 1 -> Enable ; 0 -> Disable  */

uint32_t CRC32_Init(void);
uint32_t CRC32_Update(uint32_t crc, const uint8_t *data, uint32_t length);
uint32_t CRC32_Finalize(uint32_t crc);
uint32_t CRC32_Calculate(const uint8_t *data, uint32_t length);

#endif /* INC_CRC32_H_ */
```

### `Core/Src/crc32.c`

```c
#include "crc32.h"

#define CRC32_POLYNOMIAL  0xEDB88320U

uint32_t CRC32_Init(void)
{
    return 0xFFFFFFFFU;
}

uint32_t CRC32_Update(uint32_t crc, const uint8_t *data, uint32_t length)
{
    if (data == NULL)
        return crc;

    for (uint32_t i = 0; i < length; i++)
    {
        crc ^= data[i];

        for(uint32_t bit = 0; bit < 8U; bit++)
        {
            if ((crc & 1U) != 0U)
                crc = (crc >> 1U) ^ CRC32_POLYNOMIAL;
            else
                crc >>= 1U;
        }
    }

    return crc;
}

uint32_t CRC32_Finalize(uint32_t crc)
{
    return crc ^ 0xFFFFFFFFU;
}

uint32_t CRC32_Calculate(const uint8_t *data, uint32_t length)
{
    uint32_t crc = CRC32_Init();
    crc = CRC32_Update(crc, data, length);
    return CRC32_Finalize(crc);
}
```

### API Usage

| Function | Purpose |
|---|---|
| `CRC32_Init()` | Returns the initial CRC value (`0xFFFFFFFF`). |
| `CRC32_Update(crc, data, len)` | Feeds `len` bytes into the running CRC state. Called once per chunk. |
| `CRC32_Finalize(crc)` | Applies the final XOR (`0xFFFFFFFF`) to produce the result. |
| `CRC32_Calculate(data, len)` | Convenience wrapper: init + update + finalize in one call. |

### Self-Test

When `CRC_TEST` is set to `1` in `crc32.h`, `main.c` calculates the CRC of the
standard test vector `"123456789"` and prints it over UART. The expected result
is `0xCBF43926`.

```c
/* In main.c, guarded by #if (CRC_TEST == 1) */
static const uint8_t test[] = "123456789";
uint32_t crc = CRC32_Calculate(test, 9);
/* Expected: 0xCBF43926 */
```

---

## 4. Firmware Metadata Header

Before any firmware chunks are sent, the host transmits a 12-byte metadata
header:

| Offset | Size | Field |
|---:|---:|---|
| 0 | 4 | ASCII magic: `FWUP` |
| 4 | 4 | Firmware size, little-endian uint32 |
| 8 | 4 | Firmware CRC32, little-endian uint32 |

This is defined in `bootloader.h`:

```c
#define FW_HEADER_SIZE  12U

typedef struct
{
    uint32_t size;
    uint32_t crc32;
} FirmwareInfo;
```

The bootloader receives the header into a raw byte array and decodes the
integers explicitly using `ReadLE32()`, rather than receiving directly into a
C struct. This avoids alignment and endianness issues:

```c
static uint32_t ReadLE32(const uint8_t *data)
{
    return ((uint32_t)data[0])       |
           ((uint32_t)data[1] << 8)  |
           ((uint32_t)data[2] << 16) |
           ((uint32_t)data[3] << 24);
}
```

The header is validated before any flash erase occurs. If the magic bytes do
not match `FWUP`, or the size is zero or exceeds `QSPI_APP_MAX_SIZE` (8 MB),
the update is rejected immediately.

---

## 5. Update Protocol

The complete UART protocol is:

```text
Host → STM32:  "UPDATE"                          (6 bytes ASCII)

STM32 → Host:  "=== FIRMWARE UPDATE MODE ==="    (confirmation)

Host → STM32:  "FWUP" + size_le32 + crc32_le32   (12-byte metadata header)

STM32 erases the 8 MB flash region (64 KB block erase, progress printed per 1 MB)

STM32 → Host:  "Waiting for firmware upload..."
STM32 → Host:  "Send binary data (max 8 MB)"

Host → STM32:  [chunk_len_le16][chunk_data]       (repeated)
STM32 → Host:  "ACK"                             (after each chunk)

Host → STM32:  [0x00 0x00]                       (end marker)

STM32 verifies UART CRC, then flash read-back CRC
STM32 → Host:  "[OK] UART firmware CRC verified"
STM32 → Host:  "[OK] External flash CRC verified"
STM32 → Host:  "Update successful, rebooting..."
STM32 reboots (NVIC_SystemReset)
```

### Chunk Format

Each chunk is preceded by a 2-byte little-endian length:

```text
[chunk_len_lo][chunk_len_hi][chunk_data ...]
```

- `chunk_len` is at most `CHUNK_SIZE` (1024 bytes).
- A `chunk_len` of `0x0000` is the end marker — it is only valid after all
  `expected_size` bytes have been received. If it arrives early, the bootloader
  reports `"ERROR: Firmware ended prematurely"` and aborts.
- After writing each chunk to flash, the bootloader sends `"ACK"` so the host
  knows it can safely transmit the next chunk.

### Chunk Validation

The bootloader validates each chunk before accepting it:

- `chunk_len` must not exceed `CHUNK_SIZE` (1024).
- `chunk_len` must not exceed the remaining bytes
  (`expected_size - fw_received`), preventing overflow past the declared size.
- A zero-length chunk before all bytes are received is treated as a premature
  end and rejected.

---

## 6. Bootloader Update Flow

### `main.c` — Entry Point and UPDATE Detection

After peripheral initialization and W25Q64 identification, `main.c` waits
**3 seconds** for an `"UPDATE"` command on USART1:

```c
uint32_t boot_timeout = HAL_GetTick() + 3000;
uint8_t update_requested = 0;
uint8_t update_cmd[16];

while (HAL_GetTick() < boot_timeout)
{
    if (HAL_UART_Receive(&huart1, update_cmd, 6, 100) == HAL_OK) {
        if (strncmp((char *)update_cmd, "UPDATE", 6) == 0) {
            update_requested = 1;
            break;
        }
    }
    HAL_Delay(100);
}
```

- If `"UPDATE"` is received → `Bootloader_UpdateFirmware()` is called.
  - On success: prints `"Update successful, rebooting..."` and calls
    `NVIC_SystemReset()`.
  - On failure: prints `"Update failed, returning to bootloader"` and falls
    through to an infinite loop.
- If no `"UPDATE"` is received within 3 seconds → enters memory-mapped mode
  and calls `Bootloader_JumpToApplication()`.

### `Bootloader_UpdateFirmware()` — Full Sequence

This function in `bootloader.c` orchestrates the entire update:

```c
int8_t Bootloader_UpdateFirmware(void)
{
    FirmwareInfo firmware;
    uint32_t received_crc;
    uint32_t flash_crc;

    UART_SendLine(&huart1, "=== FIRMWARE UPDATE MODE ===");

    /* Step 0: Receive and validate metadata (before erasing). */
    if (Bootloader_ReceiveFirmwareInfo(&firmware) != 0)
        return -1;

    /* Step 1: Erase the full 8 MB flash region in 64 KB blocks. */
    if (Bootloader_EraseExternalFlash() != 0)
        return -1;

    /* Step 2: Receive chunks, calculate streaming CRC, write to flash. */
    if (Bootloader_ReceiveAndWrite(firmware.size, &received_crc) != 0)
        return -1;

    /* Step 3: Verify UART receive CRC against header CRC. */
    if (received_crc != firmware.crc32)
    {
        /* Prints expected vs. received CRC, then returns -1. */
        return -1;
    }
    UART_SendLine(&huart1, "[OK] UART firmware CRC verified");

    /* Step 4: Enter memory-mapped mode and verify flash read-back CRC. */
    if (QSPI_Flash_EnterMemoryMappedMode() != 0)
        return -1;

    flash_crc = CRC32_Calculate((const uint8_t *)QSPI_BASE_ADDR, firmware.size);

    if (flash_crc != firmware.crc32)
    {
        UART_SendLine(&huart1, "ERROR: Flash read-back CRC mismatch");
        return -1;
    }
    UART_SendLine(&huart1, "[OK] External flash CRC verified");

    /* Return to main.c, which prints the success message and reboots. */
    return 0;
}
```

> **Note:** `Bootloader_UpdateFirmware()` does **not** call `NVIC_SystemReset()`
> itself. It returns `0` on success, and `main.c` prints the confirmation
> message and triggers the reboot. This lets the host script detect the
> `"Update successful, rebooting..."` line.

### `Bootloader_ReceiveAndWrite()` — Streaming CRC and Flash Write

This function receives chunks in a loop, updates the running CRC, and writes
each chunk to external flash:

```c
static int8_t Bootloader_ReceiveAndWrite(uint32_t expected_size, uint32_t *received_crc)
{
    /* ... parameter validation ... */

    uint32_t running_crc = CRC32_Init();

    fw_total_size = 0;
    fw_received   = 0;
    fw_write_addr = QSPI_APP_ADDR;

    while (fw_received < expected_size)
    {
        uint8_t  chunk_header[2];
        uint16_t chunk_len;

        /* Receive 2-byte LE chunk length. */
        if (HAL_UART_Receive(&huart1, chunk_header, sizeof(chunk_header), 5000) != HAL_OK)
            return -1;

        chunk_len = (uint16_t)chunk_header[0] | ((uint16_t)chunk_header[1] << 8);

        if (chunk_len == 0U)          /* premature end */
            return -1;

        if (chunk_len > CHUNK_SIZE ||
            chunk_len > (expected_size - fw_received))
            return -1;

        /* Receive chunk data. */
        if (HAL_UART_Receive(&huart1, fw_chunk, chunk_len, 5000) != HAL_OK)
            return -1;

        /* Update streaming CRC. */
        running_crc = CRC32_Update(running_crc, fw_chunk, chunk_len);

        /* Write to external flash (handles page-boundary splitting). */
        if (Bootloader_WriteExternalFlash(fw_write_addr, fw_chunk, chunk_len) != 0)
            return -1;

        fw_write_addr += chunk_len;
        fw_received   += chunk_len;

        UART_SendLine(&huart1, "ACK");   /* host waits for this before next chunk */
    }

    /* Receive and validate the end marker (0x00 0x00). */
    /* ... */

    fw_total_size = fw_received;
    *received_crc = CRC32_Finalize(running_crc);

    return 0;
}
```

### Flash Erase

`Bootloader_EraseExternalFlash()` erases the entire 8 MB application region in
64 KB blocks using the W25Q64 block-erase command (`0xD8`). Progress is printed
every 16 blocks (1 MB):

```text
Erasing external Flash (8 MB)...
  Erased 1 MB
  Erased 2 MB
  ...
Erase complete!
```

### Flash Write

`Bootloader_WriteExternalFlash()` writes data to the W25Q64, automatically
splitting writes at 256-byte page boundaries. It uses quad-input page program
(`0x32`) when `WRITE_DATA_OPTIONS` is set to `WRITE_DATA_QUAD_LINE`.

---

## 7. Memory-Mapped Mode Handling

The QSPI peripheral can only be in one mode at a time:

- **Indirect mode** → erase / program (used during firmware update)
- **Memory-mapped mode** → XIP read (used to boot the application and to
  read back flash for CRC verification)

If memory-mapped mode is entered before erase/program, those operations will
silently fail because the peripheral is locked. The bootloader therefore
**defers** memory-mapped mode:

```text
Initialize QSPI and enable QE
             |
             v
      Wait 3 s for UPDATE
        |                  |
        | UPDATE           | No update (timeout)
        v                  v
Receive metadata       Enter memory-mapped mode
Erase 8 MB             Jump to application
Program chunks
Verify UART CRC
Enter memory-mapped mode
Verify flash CRC
Return to main → reboot
```

Memory-mapped mode is entered in two places only:

1. **Normal boot path** (`main.c`, no update requested): immediately before
   calling `Bootloader_JumpToApplication()`.
2. **Update path** (`Bootloader_UpdateFirmware()`): after programming is
   complete, before the flash read-back CRC calculation.

### Exiting Memory-Mapped Mode

`external_flash.c` provides `QSPI_Flash_ExitMemoryMappedMode()`, which calls
`HAL_QSPI_Abort()` to return the peripheral to indirect mode. This is available
if the bootloader needs to re-enter indirect mode after memory-mapped mode
(e.g., for a subsequent update without reboot).

### D-Cache Consideration

If D-cache is enabled and QSPI was read before programming, the relevant cache
lines must be invalidated before calculating the flash CRC. Otherwise the CPU
may read stale cached data. The current project does **not** enable D-cache, so
no invalidation is needed. This must be revisited if cache support is added.

---

## 8. Jump to Application

`Bootloader_JumpToApplication()` performs extensive validation before handing
control to the application:

1. **Read vector table** from `QSPI_BASE_ADDR` (`0x90000000`):
   - SP at offset `0x00`
   - Reset handler at offset `0x04`

2. **Check for empty flash**: SP or reset handler equal to `0x00000000` or
   `0xFFFFFFFF` → abort.

3. **Stack alignment**: SP must be 8-byte aligned (AAPCS requirement).

4. **Stack pointer validity**: SP must point to a valid RAM region:
   - DTCM: `0x20000000`–`0x20020000`
   - AXI SRAM: `0x24000000`–`0x24080000`
   - D2 SRAM: `0x30000000`–`0x30048000`
   - D3 SRAM: `0x38000000`–`0x38010000`

5. **Thumb bit**: Reset handler address must have bit 0 set (Cortex-M executes
   Thumb only).

6. **Address range**: Reset handler (with Thumb bit cleared) must be within
   `QSPI_BASE_ADDR` to `QSPI_BASE_ADDR + QSPI_FLASH_MAX_SIZE`.

If all checks pass, the bootloader:
- Deinitializes UART
- Disables interrupts
- Stops SysTick and clears pending system exceptions
- Disables all NVIC interrupts and clears pending requests
- Sets `SCB->VTOR = QSPI_BASE_ADDR`
- Sets MSP to the application SP
- Re-enables interrupts
- Branches to the application's reset handler via inline assembly

```c
__asm volatile (
    "msr msp, %0  \n"
    "cpsie i      \n"
    "bx %1        \n"
    :
    : "r" (app_sp),
      "r" (app_reset_handler)
    : "memory"
);
```

---

## 9. Python Upload Script

The host-side script `Loader/upload_firmware.py` automates the entire update
process. It:

1. Reads the `.bin` firmware file.
2. Calculates `zlib.crc32()` over the firmware.
3. Builds the 12-byte `FWUP` metadata header.
4. Waits for the bootloader banner (`"[OK] W25Q64 detected"`).
5. Sends the `"UPDATE"` command.
6. Sends the metadata header.
7. Waits for the erase to complete (`"Waiting for firmware upload"`).
8. Sends firmware in 1024-byte chunks, waiting for `"ACK"` after each chunk.
9. Sends the end marker (`0x00 0x00`).
10. Waits for CRC verification messages.
11. Waits for `"Update successful, rebooting..."`.

### Key Configuration

```python
PORT = "COM8"                        # Serial port
BAUD = 115200                        # Baud rate
BIN_FILE = r"...application.bin"     # Firmware binary path
CHUNK_SIZE = 1024                    # Must match bootloader CHUNK_SIZE
MAX_FIRMWARE_SIZE = 8 * 1024 * 1024  # W25Q64 capacity
WAIT_FOR_CHUNK_ACK = True            # Wait for ACK after each chunk
CHUNK_DELAY_SECONDS = 0.02           # Fallback delay if ACK disabled
```

### Header Construction

```python
def build_firmware_header(firmware):
    firmware_crc = zlib.crc32(firmware) & 0xFFFFFFFF
    header = b"FWUP" + struct.pack("<I", len(firmware))
    header += struct.pack("<I", firmware_crc)
    return header, firmware_crc
```

### Chunk Transmission

```python
for i in range(0, len(fw_data), CHUNK_SIZE):
    chunk = fw_data[i:i + CHUNK_SIZE]
    chunk_len = len(chunk)

    packet = struct.pack('<H', chunk_len) + chunk
    send_bytes(ser, packet)

    if WAIT_FOR_CHUNK_ACK:
        wait_for_message(ser, "ACK", 10)
    else:
        time.sleep(CHUNK_DELAY_SECONDS)
```

### End Marker

```python
send_bytes(ser, struct.pack('<H', 0))   # 0x00 0x00
```

---

## 10. CRC Parameters

The STM32 and host must use identical CRC parameters:

| Parameter | Value |
|---|---|
| Algorithm | CRC-32/ISO-HDLC |
| Normal polynomial | `0x04C11DB7` |
| Reflected polynomial | `0xEDB88320` |
| Initial value | `0xFFFFFFFF` |
| Input reflected | Yes |
| Output reflected | Yes |
| Final XOR | `0xFFFFFFFF` |
| `123456789` result | `0xCBF43926` |

This matches Python's `zlib.crc32()` and the standard `crc32` command-line
tool.

---

## 11. Configuration Macros

### `bootloader.h`

| Macro | Value | Purpose |
|---|---|---|
| `FW_HEADER_SIZE` | `12U` | Size of the `FWUP` metadata header in bytes |
| `QSPI_FW_ERASE_GRANULARITY` | `QSPI_FLASH_BLOCK_SIZE` (64 KB) | Erase block size |
| `QSPI_APP_ADDR` | `0x00000000` | Flash offset where the application starts |
| `QSPI_APP_MAX_SIZE` | `8 * 1024 * 1024` | Maximum application size (full W25Q64) |
| `CHUNK_SIZE` | `1024` | UART receive buffer and max chunk size |
| `BOOT_TEST` | `0` | Enable verbose debug messages in bootloader |

### `crc32.h`

| Macro | Value | Purpose |
|---|---|---|
| `CRC_TEST` | `0` | Enable CRC self-test with `"123456789"` vector |

### `external_flash.h`

| Macro | Value | Purpose |
|---|---|---|
| `QSPI_FLASH_MAX_SIZE` | `8 * 1024 * 1024` | W25Q64 total capacity |
| `QSPI_FLASH_BLOCK_SIZE` | `64 * 1024` | 64 KB erase block |
| `QSPI_FLASH_SECTOR_SIZE` | `4 * 1024` | 4 KB erase sector |
| `QSPI_FLASH_PAGE_SIZE` | `256` | Program page size |
| `QSPI_BASE_ADDR` | `0x90000000` | QSPI memory-mapped base address |
| `WRITE_DATA_OPTIONS` | `WRITE_DATA_QUAD_LINE` | Use quad-input page program |

---

## 12. File Reference

### `Core/Inc/crc32.h`

CRC32 API declarations and the `CRC_TEST` toggle.

### `Core/Src/crc32.c`

Software bitwise CRC-32/ISO-HDLC implementation. Provides streaming
(`Init`/`Update`/`Finalize`) and one-shot (`Calculate`) interfaces.

### `Core/Inc/bootloader.h`

Bootloader API, `FirmwareInfo` struct, and configuration macros
(`FW_HEADER_SIZE`, `QSPI_APP_ADDR`, `QSPI_APP_MAX_SIZE`, `CHUNK_SIZE`,
`BOOT_TEST`).

### `Core/Src/bootloader.c`

Main bootloader logic:

- `Bootloader_UpdateFirmware()` — orchestrates the full update sequence.
- `Bootloader_ReceiveFirmwareInfo()` — receives and validates the 12-byte
  `FWUP` header.
- `Bootloader_ReceiveAndWrite()` — receives chunks, updates streaming CRC,
  writes to flash, sends ACK per chunk.
- `Bootloader_EraseExternalFlash()` — erases 8 MB in 64 KB blocks.
- `Bootloader_WriteExternalFlash()` — writes data, splitting at page
  boundaries.
- `Bootloader_JumpToApplication()` — validates vector table and jumps to app.
- `UART_SendString()` / `UART_SendLine()` — UART transmit helpers.

### `Core/Inc/external_flash.h`

W25Q64 geometry constants, QSPI command definitions, and flash driver API.

### `Core/Src/external_flash.c`

W25Q64 QSPI driver:

- `QSPI_Flash_Init()` — verifies JEDEC ID and enables quad mode.
- `QSPI_Flash_ReadID()` — reads 3-byte JEDEC ID (`0xEF 0x40 0x17`).
- `QSPI_Flash_EraseBlock()` — erases a 64 KB block (`0xD8`).
- `QSPI_Flash_WritePage()` — programs up to 256 bytes (`0x32` quad or `0x02`
  single).
- `QSPI_Flash_EnterMemoryMappedMode()` — enters XIP mode with quad fast read
  (`0xEB`).
- `QSPI_Flash_ExitMemoryMappedMode()` — aborts memory-mapped mode, returns to
  indirect mode.
- `QSPI_Flash_EnableQuadMode()` — sets the QE bit in status register 2.
- `QSPI_Flash_IsBusy()` / `QSPI_Flash_WaitReady()` — poll the BUSY bit.

### `Core/Src/main.c`

Bootloader entry point. Initializes peripherals, identifies the W25Q64, waits
3 seconds for an `"UPDATE"` command, then either performs a firmware update or
boots the application from QSPI.

### `Loader/upload_firmware.py`

Host-side Python script. Reads a `.bin` file, calculates CRC32, sends the
`FWUP` header, transmits firmware in 1024-byte chunks with ACK handshaking,
and waits for CRC verification and reboot confirmation.
