# Bootloader Roadmap — STM32H750VBT6

> **Actual architecture:** Bootloader 128KB (Internal Flash `0x08000000`) + Application 8MB (QSPI XIP `0x90000000`, W25Q64).

---

## Table of Contents

1. [Overview](#1-overview)
2. [Memory Map](#2-memory-map)
3. [Hardware Configuration (CubeMX)](#3-hardware-configuration-cubemx)
4. [Implemented Source Code](#4-implemented-source-code)
5. [Bootloader Flow](#5-bootloader-flow)
6. [QSPI Flash Driver (W25Q64)](#6-qspi-flash-driver-w25q64)
7. [Firmware Update Protocol (UART)](#7-firmware-update-protocol-uart)
8. [Jump to Application (QSPI XIP)](#8-jump-to-application-qspi-xip)
9. [Application Project (Linker Script + VTOR)](#9-application-project-linker-script--vtor)
10. [Python Upload Tool](#10-python-upload-tool)
11. [Troubleshooting](#11-troubleshooting)
12. [Advanced Topics (not yet implemented)](#12-advanced-topics-not-yet-implemented)
13. [References](#13-references)

---

## 1. Overview

```
┌─────────────────────────────────────────────────────┐
│                    POWER ON / RESET                 │
│                        │                            │
│                        ▼                            │
│  ┌──────────────────────────────────┐               │
│  │   BOOTLOADER (0x08000000, 128KB) │               │
│  │  - Init UART1, QSPI, Clock       │               │
│  │  - Wait 3s for "UPDATE" command  │               │
│  │  - If UPDATE: receive FW → write │               │
│  │    to QSPI → reset               │               │
│  │  - If timeout: jump to app       │               │
│  └──────────────┬───────────────────┘               │
│                 ▼                                   │
│  ┌──────────────────────────────────┐               │
│  │  APPLICATION (0x90000000, 8MB)   │               │
│  │  QSPI XIP — executes directly    │               │
│  └──────────────────────────────────┘               │
└─────────────────────────────────────────────────────┘
```

**STM32H750VBT6 specifics:**

| Feature | Value | Impact |
|---|---|---|
| Flash | 128 KB (1 sector) | Cannot split dual bank → use external flash |
| RAM | 1056 KB | Enough buffer for firmware receive |
| Cortex-M7 | 480 MHz | Fast, watch out for cache when jumping |
| External Flash | W25Q64 8MB via QSPI | App executes directly (XIP) |

> **Why external flash:** STM32H750 has only 1 sector of 128KB — cannot split into bootloader + app. Bootloader occupies the entire internal flash; the app lives in QSPI external flash.

---

## 2. Memory Map

### Internal Memory (MCU)

```
0x08000000 ┌──────────────────────┐
           │  FLASH (128 KB)      │  ← Bootloader (never erased)
0x08020000 └──────────────────────┘

0x20000000 ┌──────────────────────┐
           │  DTCMRAM (128 KB)    │
0x20020000 └──────────────────────┘
0x24000000 ┌──────────────────────┐
           │  RAM_D1 (512 KB)     │  ← Stack & Heap (default)
0x24080000 └──────────────────────┘
0x30000000 ┌──────────────────────┐
           │  RAM_D2 (288 KB)     │
0x30048000 └──────────────────────┘
0x38000000 ┌──────────────────────┐
           │  RAM_D3 (64 KB)      │
0x38010000 └──────────────────────┘
0x00000000 ┌──────────────────────┐
           │  ITCMRAM (64 KB)     │
0x00010000 └──────────────────────┘
```

### External Flash (W25Q64 via QSPI)

```
Logical (W25Q64):   0x00000000 - 0x007FFFFF (8MB)  ← Application firmware
QSPI Mapped (CPU):  0x90000000 - 0x907FFFFF        ← XIP execution
```

### W25Q64 Geometry

```
Chip (8 MB)
 └─ 128 Blocks × 64 KB    ← erase 0xD8 (used for firmware update)
     └─ 16 Sectors × 4 KB  ← erase 0x20 (smallest erase unit)
         └─ 16 Pages × 256 B ← program (write) unit
```

| Parameter | Value |
|---|---|
| Capacity | 64 Mbit = 8 MB |
| Block Size | 64 KB (cmd `0xD8`) |
| Sector Size | 4 KB (cmd `0x20`) |
| Page Size | 256 bytes (cmd `0x02` / `0x32`) |
| JEDEC ID | `0xEF 0x40 0x17` |
| Max SPI Clock | 104 MHz |

> **NOR Flash rule:** Cannot erase smaller than 4KB. Write only flips bits 1→0; only erase returns them to 1.

---

## 3. Hardware Configuration (CubeMX)

### UART1

| Parameter | Value |
|---|---|
| TX / RX | PA9 / PA10 |
| Baud Rate | 115200 |
| Format | 8N1 |
| Flow Control | None |

### Clock (HSI → 480 MHz)

```
HSI 64 MHz → PLLM=4 → PLLN=60 → PLLP=2 → SYSCLK = 480 MHz
HPRE=/2 → HCLK3 = 240 MHz → QSPI kernel clock = 240 MHz
APB1=120, APB2=120, APB3=120, APB4=120 MHz
```

### QSPI (for W25Q64)

| Pin | Signal | Function |
|---|---|---|
| PB2 | QSPI_SCK | Clock |
| PD11 | QSPI_IO0 | Data 0 |
| PD12 | QSPI_IO1 | Data 1 |
| PE2 | QSPI_IO2 | Data 2 |
| PD13 | QSPI_IO3 | Data 3 |
| PB6 | QSPI_CS | Chip Select |

| QSPI Parameter | Value | Notes |
|---|---|---|
| ClockPrescaler | 2 | 240/(2+1) = **80 MHz** |
| Flash Size (FSIZE) | **22** | 8MB = 2^23 → FSIZE = 23-1 = 22 |
| Sample Shift | Half Cycle | Required at 80 MHz |
| CS High Time | 2 cycles | |

> ⚠️ **FSIZE = 22, not 26.** Formula: `bytes = 2^(FSIZE+1)`. W25Q64 = 8MB = 2^23 bytes → FSIZE = 22. Wrong value → memory-mapped read wrap-around, very hard to debug.

> 📖 Details: [QSPI_EXPLAINED.md](QSPI_EXPLAINED.md)

### Linker Script (Bootloader)

File: `STM32H750VBTX_FLASH.ld`

```ld
MEMORY
{
  FLASH (rx)     : ORIGIN = 0x08000000, LENGTH = 128K
  DTCMRAM (xrw)  : ORIGIN = 0x20000000, LENGTH = 128K
  RAM_D1 (xrw)   : ORIGIN = 0x24000000, LENGTH = 512K
  RAM_D2 (xrw)   : ORIGIN = 0x30000000, LENGTH = 288K
  RAM_D3 (xrw)   : ORIGIN = 0x38000000, LENGTH = 64K
  ITCMRAM (xrw)  : ORIGIN = 0x00000000, LENGTH = 64K
}

ENTRY(Reset_Handler)
_estack = ORIGIN(RAM_D1) + LENGTH(RAM_D1);  /* 0x24080000 */
```

- `.isr_vector` at `0x08000000` (mandatory)
- `_estack = 0x24080000` (top of RAM_D1)
- Bootloader uses the entire 128KB internal flash

---

## 4. Implemented Source Code

```
STM32H750_Bootloader/
├── Core/
│   ├── Inc/
│   │   ├── bootloader.h          ← Bootloader API + constants
│   │   └── external_flash.h      ← QSPI W25Q64 driver API + commands
│   ├── Src/
│   │   ├── main.c                ← Entry point: init → wait UPDATE → jump/update
│   │   ├── bootloader.c          ← Erase, Write, Receive, JumpToApplication
│   │   └── external_flash.c      ← QSPI: ReadID, Erase, Write, MemoryMapped
│   └── Startup/
│       └── startup_stm32h730xx.s
├── STM32H750VBTX_FLASH.ld        ← Bootloader linker script (128KB @ 0x08000000)
├── Loader/
│   └── upload_firmware.py        ← Python tool: send .bin via UART
└── STM32H750_Bootloader.ioc      ← CubeMX config
```

### Function Map

| File | Function | Purpose |
|---|---|---|
| `main.c` | `main()` | Init peripherals → QSPI init → memory-mapped → wait UPDATE → jump/update |
| `bootloader.c` | `Bootloader_EraseExternalFlash()` | Erase 8MB W25Q64 (128 blocks × 64KB) |
| | `Bootloader_WriteExternalFlash()` | Write firmware page-by-page (256B max) |
| | `Bootloader_ReceiveAndWrite()` | Receive chunks via UART → write to QSPI |
| | `Bootloader_UpdateFirmware()` | Complete update: erase → receive → reset |
| | `Bootloader_JumpToApplication()` | Jump to app @ 0x90000000 (XIP) |
| | `Bootloader_IsValidStackPointer()` | Validate SP within RAM range |
| `external_flash.c` | `QSPI_Flash_Init()` | Verify W25Q64 + enable Quad mode |
| | `QSPI_Flash_ReadID()` | Read JEDEC ID (0x9F) |
| | `QSPI_Flash_EraseBlock()` | Erase 64KB block (0xD8) |
| | `QSPI_Flash_WritePage()` | Write 256 bytes (0x02 or 0x32) |
| | `QSPI_Flash_IsBusy()` | Check status register (0x05) |
| | `QSPI_Flash_EnableQuadMode()` | Set QE bit in Status Register-2 |
| | `QSPI_Flash_EnterMemoryMappedMode()` | Enable XIP @ 0x90000000 |

---

## 5. Bootloader Flow

```
main()
  │
  ├─ HAL_Init()
  ├─ SystemClock_Config()          → 480 MHz HSI
  ├─ MX_GPIO_Init()
  ├─ MX_USART1_UART_Init()         → 115200, 8N1
  ├─ MX_QUADSPI_Init()             → 80 MHz, FSIZE=22
  │
  ├─ QSPI_Flash_Init()             → Verify W25Q64 (JEDEC ID: EF 40 17)
  ├─ QSPI_Flash_EnterMemoryMappedMode()  → XIP @ 0x90000000
  │
  ├─ Wait 3s for "UPDATE" command
  │   │
  │   ├─ If "UPDATE" received:
  │   │   └─ Bootloader_UpdateFirmware()
  │   │       ├─ Erase 8MB external flash (128 × 64KB blocks)
  │   │       ├─ Receive firmware chunks via UART → write to QSPI
  │   │       └─ NVIC_SystemReset()  → reboot → load new app
  │   │
  │   └─ If timeout:
  │       └─ Bootloader_JumpToApplication()  → jump to 0x90000000
  │
  └─ Fallback: infinite loop
```

---

## 6. QSPI Flash Driver (W25Q64)

File: `Core/Src/external_flash.c` / `Core/Inc/external_flash.h`

### Key Constants

```c
#define QSPI_FLASH_MAX_SIZE     (8 * 1024 * 1024)   /* 8 MB  */
#define QSPI_FLASH_BLOCK_SIZE   (64 * 1024)         /* 64 KB */
#define QSPI_FLASH_PAGE_SIZE    256                  /* 256 B  */
#define QSPI_BASE_ADDR          0x90000000           /* XIP mapped */
```

### W25Q64 Commands

| Command | Opcode | Purpose |
|---|---|---|
| Read ID | `0x9F` | Read JEDEC ID (EF 40 17) |
| Read Status | `0x05` | Check BUSY bit |
| Write Enable | `0x06` | Required before erase/write |
| Block Erase 64K | `0xD8` | Erase 64KB |
| Page Program | `0x02` | Write 1-line (standard) |
| Quad Input Page Program | `0x32` | Write 4-line (quad) |
| Fast Quad Read | `0xEB` | Read 4-line (for memory-mapped) |
| Read Status Reg 2 | `0x35` | Read QE bit |
| Write Status Reg 2 | `0x31` | Set QE bit |

### Quad Enable (QE bit) vs QPI Mode

> ⚠️ **QE bit ≠ QPI mode.** This is a very common mistake.

| | QE bit (what you want) | QPI mode (`0x38`) |
|---|---|---|
| How to enable | Set bit 1 of SR2 via `0x31` | Send command `0x38` |
| Opcode travels on | 1 line | 4 lines |
| Use it? | ✅ Standard, use with `0xEB` | ⚠️ Only if you need QPI |

Code implementation: `QSPI_Flash_EnableQuadMode()` in `external_flash.c` — reads SR2, if QE not set then write enable + write SR2 with QE bit.

### Memory-Mapped Mode (XIP)

`QSPI_Flash_EnterMemoryMappedMode()` configures QSPI in memory-mapped mode:
- Command: `0xEB` (Fast Quad Read)
- Address: 4 lines, 24-bit
- Data: 4 lines
- Dummy cycles: 6
- After calling: CPU accesses `0x90000000+` like RAM, no explicit read commands needed

---

## 7. Firmware Update Protocol (UART)

### Protocol

```
┌──────────────────────────────────────────────────────────┐
│  FIRMWARE UPLOAD PROTOCOL                                │
├──────────────────────────────────────────────────────────┤
│                                                          │
│ 1. PC sends: "UPDATE" (6 bytes)                          │
│    Bootloader: enters update mode                        │
│                                                          │
│ 2. Bootloader erases 8MB external flash (128 × 64KB)     │
│                                                          │
│ 3. PC sends firmware in chunks:                          │
│    [2-byte length LE][chunk data (max 1024 bytes)]       │
│    Bootloader: writes page-by-page to QSPI               │
│                                                          │
│ 4. PC sends end marker: [0x00 0x00]                      │
│                                                          │
│ 5. Bootloader: resets → loads new app                    │
│                                                          │
└──────────────────────────────────────────────────────────┘
```

### Chunk Format

```
┌──────────────┬─────────────────────────┐
│ Length (2B)  │ Data (up to 1024B)     │
│ Little-endian│                         │
└──────────────┴─────────────────────────┘
```

- `chunk_len = 0` → end of transfer
- `chunk_len > 1024` → error
- Progress printed every 256KB

### Code Flow (`bootloader.c`)

```
Bootloader_UpdateFirmware()
  ├─ Bootloader_EraseExternalFlash()     → 128 blocks × 64KB
  ├─ Bootloader_ReceiveAndWrite()
  │   ├─ Loop: receive 2-byte header → receive chunk → write to QSPI
  │   ├─ End when chunk_len == 0
  │   └─ Progress print every 256KB
  └─ NVIC_SystemReset()
```

---

## 8. Jump to Application (QSPI XIP)

File: `Core/Src/bootloader.c` — `Bootloader_JumpToApplication()`

### Jump Sequence

```
1. Read vector table from QSPI (0x90000000)
   ├─ app_sp            = *(uint32_t*)(0x90000000 + 0x00)
   └─ app_reset_handler = *(uint32_t*)(0x90000000 + 0x04)

2. Validate:
   ├─ SP != 0x00000000 / 0xFFFFFFFF
   ├─ SP 8-byte aligned
   ├─ SP within RAM range (DTCM/RAM_D1/RAM_D2/RAM_D3)
   └─ Reset_Handler has Thumb bit (bit 0 = 1)
   └─ Reset_Handler within QSPI range [0x90000000, 0x907FFFFF]

3. HAL_UART_DeInit(&huart1)     ← Disable UART
   ⚠️ DO NOT call HAL_QSPI_DeInit()  ← QSPI must stay active for XIP!

4. __disable_irq()

5. Stop SysTick:
   ├─ SysTick->CTRL = 0
   ├─ SysTick->LOAD = 0
   ├─ SysTick->VAL  = 0
   └─ SCB->ICSR = PENDSTCLR | PENDSVCLR

6. Disable all NVIC interrupts:
   ├─ NVIC->ICER[i] = 0xFFFFFFFF  (disable)
   └─ NVIC->ICPR[i] = 0xFFFFFFFF  (clear pending)

7. __set_CONTROL(0)  ← privileged, MSP
   __ISB()

8. SCB->VTOR = 0x90000000  ← App vector table
   __DSB()
   __ISB()

9. Inline asm:
   ├─ msr msp, app_sp        ← Set stack pointer
   ├─ cpsie i                 ← Re-enable interrupts
   └─ bx app_reset_handler    ← Jump (never returns)
```

> ⚠️ **Top 2 causes of "jump succeeds but app is completely silent":**
> 1. **`HAL_QSPI_DeInit()` before jump** — tears down XIP mode, CPU fetches garbage → HardFault
> 2. **Forgetting `__enable_irq()` (cpsie i)** — PRIMASK=1, SysTick interrupt never fires, `HAL_Delay()` hangs forever

> **Why inline asm instead of `__set_MSP()` + `__enable_irq()` + function pointer:**
> After `__set_MSP()`, no C function calls are safe (stack has changed). Inline asm ensures MSP + cpsie + bx execute atomically without interruption.

---

## 9. Application Project (Linker Script + VTOR)

The application is a separate project (`STM32H750_Application`), built into a `.bin` and flashed to QSPI via the bootloader.

### Application Linker Script

```ld
MEMORY
{
    FLASH (rx)  : ORIGIN = 0x90000000, LENGTH = 8M    /* QSPI XIP */
    RAM_D1 (xrw): ORIGIN = 0x24000000, LENGTH = 512K
    DTCMRAM (xrw): ORIGIN = 0x20000000, LENGTH = 128K
    RAM_D2 (xrw): ORIGIN = 0x30000000, LENGTH = 288K
    RAM_D3 (xrw): ORIGIN = 0x38000000, LENGTH = 64K
}

ENTRY(Reset_Handler)
_estack = ORIGIN(RAM_D1) + LENGTH(RAM_D1);  /* 0x24080000 */

SECTIONS
{
    .isr_vector 0x90000000 : {     /* Vector table at QSPI base */
        KEEP(*(.isr_vector))
    } > FLASH

    .text : {
        *(.text*)
        *(.rodata*)
    } > FLASH

    .data : {
        PROVIDE(_sdata = .);
        *(.data*)
        PROVIDE(_edata = .);
    } > RAM_D1 AT > FLASH

    .bss : {
        PROVIDE(_sbss = .);
        *(.bss*)
        *(COMMON)
        PROVIDE(_ebss = .);
    } > RAM_D1
}
```

### Application SystemInit

```c
void SystemInit(void)
{
    SCB->VTOR = 0x90000000;  /* Point to app vector table in QSPI */
}
```

### Build & Flash

1. Build application → `.elf`
2. Post-build: `arm-none-eabi-objcopy -O binary app.elf app.bin`
3. Flash `app.bin` to QSPI:
   - **Option A:** STM32CubeProgrammer + external loader (fast, ~35s)
   - **Option B:** Bootloader UART (production, use `upload_firmware.py`)

---

## 10. Python Upload Tool

File: `Loader/upload_firmware.py`

```python
PORT = "COM8"
BAUD = 115200
CHUNK_SIZE = 256

# Flow:
# 1. Open .bin file
# 2. Connect UART
# 3. Wait 3.5s (bootloader startup)
# 4. Send "UPDATE"
# 5. Loop: send [2-byte len LE][chunk data] for each 256 bytes
# 6. Send end marker [0x00 0x00]
# 7. Read response
```

**Usage:**
```bash
python upload_firmware.py
```

> Update `PORT` and `BIN_FILE` in the script before running.

---

## 11. Troubleshooting

| Symptom | Cause | Fix |
|---|---|---|
| No UART output | Wrong pin/baud | Verify PA9/PA10, 115200, 8N1 |
| QSPI init fail, ID = 00 00 00 | QSPI not initialized or chip not responding | Check QSPI pins, clock config |
| QSPI ID = FF FF FF | W25Q64 not connected | Check soldering, power, pins |
| Jump succeeds but app silent | `HAL_QSPI_DeInit()` before jump | **Remove `HAL_QSPI_DeInit()`** — QSPI must stay active for XIP |
| Jump succeeds but app hangs | Missing `__enable_irq()` | Add `cpsie i` before `bx` (already fixed in code) |
| App crashes right after jump | Wrong VTOR | `SCB->VTOR = 0x90000000` in app SystemInit |
| SysTick interrupt fires after jump | SysTick not stopped | `SysTick->CTRL=0; SCB->ICSR=PENDSTCLR` |
| Flash write fail | Address not page-aligned | Write max 256B/page, don't cross page boundary |
| CRC mismatch | Endianness mismatch | Verify little-endian on both PC and MCU |
| Bootloader size > 128KB | Code too large | `-Os`, remove unused HAL modules |

### Debug Tips

1. **UART debug logging** — Print every decision: "Entering bootloader", "Starting update", "Jump to app"
2. **STM32CubeProgrammer** — Read memory at `0x90000000` to verify app vector table
3. **LED status** — Green = ready, Red = error
4. **Hex dump** — `STM32_Programmer_CLI -c port=SWD -r 0x90000000 32`

---

## 12. Advanced Topics (not yet implemented)

### 12.1. Secure Boot (HMAC-SHA256)

```
Firmware Structure:
┌──────────────────────────┐
│ Header (36 bytes)         │
│  - Size (4 bytes)         │
│  - HMAC-SHA256 (32 bytes) │
├──────────────────────────┤
│ Application Binary        │
└──────────────────────────┘
```

Bootloader verifies HMAC before jumping. Key stored in bootloader (production: use OTP/Option Bytes).

### 12.2. Firmware Encryption

- AES-256 (STM32H7 has CRYP hardware accelerator)
- ECDSA/RSA signature verification

### 12.3. OTA via other interfaces

| Interface | Speed | Difficulty | Notes |
|---|---|---|---|
| USB DFU | 12-480 Mbps | Medium | Industry standard, `dfu-util` |
| SD Card | 50 Mbps | Medium | FatFS, field updates |
| Ethernet | 100+ Mbps | Hard | TFTP/HTTP |
| CAN Bus | 1 Mbps | Medium | FDCAN |

### 12.4. Watchdog

- IWDG in bootloader — if update hangs → reset → return to bootloader
- App must feed watchdog periodically

### 12.5. A/B Bank Switching

```
External Flash:
├─ Bank A (0x00000000) — Active app
├─ Bank B (0x00400000) — Backup app
└─ Boot flag in Backup SRAM selects bank
```

---

## 13. References

### Datasheet & Reference Manual

| Code | Name | Role |
|---|---|---|
| DS12559 | STM32H750 Datasheet | Hardware specs |
| **RM0433** | STM32H7 Reference Manual | FLASH, RCC, QSPI, SCB |
| PM0253 | STM32H7 Programming Manual | Cortex-M7 core, NVIC, vector table |
| AN2606 | STM32 Boot Mode | Boot pins, system bootloader |

### Related Documents

| File | Content |
|---|---|
| [QSPI_EXPLAINED.md](QSPI_EXPLAINED.md) | QSPI config details, FSIZE, QE vs QPI |
| [17_SECURE_BOOT_GUIDE.md](17_SECURE_BOOT_GUIDE.md) | Secure boot implementation |
| [18_EXTERNAL_FLASH_ARCHITECTURE.md](18_EXTERNAL_FLASH_ARCHITECTURE.md) | External flash architecture |

### Open Source Bootloaders

- [MCUboot](https://github.com/mcu-tools/mcuboot) — Professional bootloader (A/B, crypto)
- [STM32-OTA](https://github.com/fboris/STM32_OTA) — OTA for STM32

---

## Quick Reference

### Key Addresses

```
Bootloader:  0x08000000 (128KB internal flash)
App (XIP):   0x90000000 (8MB QSPI mapped)
QSPI Clock:  80 MHz (240/(2+1))
UART:        115200, 8N1, PA9/PA10
W25Q64 ID:   EF 40 17
```

### Key Registers

| Register | Operation |
|---|---|
| `SCB->VTOR` | `= 0x90000000` — set app vector table |
| `SCB->ICSR` | `= PENDSTCLR \| PENDSVCLR` — clear pending |
| `SysTick->CTRL` | `= 0` — disable SysTick |
| `NVIC->ICER[i]` | `= 0xFFFFFFFF` — disable all IRQs |
| `NVIC->ICPR[i]` | `= 0xFFFFFFFF` — clear pending IRQs |

### Key Formulas

```
QSPI Clock = HCLK3 / (ClockPrescaler + 1) = 240 / 3 = 80 MHz
FSIZE = log2(capacity in BYTES) - 1 = log2(8MB) - 1 = 23 - 1 = 22
JEDEC ID: EF (Winbond) 40 (W25Q64JV) 17 (8MB = 2^23)
```

---

**Version:** 2.0 | **For:** STM32H750VBT6 Cortex-M7 + W25Q64 QSPI XIP
