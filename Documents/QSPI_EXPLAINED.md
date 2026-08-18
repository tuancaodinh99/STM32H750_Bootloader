# QSPI (Quad SPI) — Detailed Explanation

## What is QSPI?

QSPI = **Quad Serial Peripheral Interface** — an extension of SPI that uses **4 data lines** instead of 1, making it **4x faster**.

---

## SPI vs QSPI Comparison

### Traditional SPI (Single-line SPI)

```
One data line at a time:

Master (STM32H750)        Slave (Flash chip)
    MOSI ────────────────→ DI
    ←──────────────────── DO  MISO
    SCK  ────────────────→ SCK
    CS   ────────────────→ /CS

Data transmission (1 bit per clock):
Clock:  ─┐ ┌─┐ ┌─┐ ┌─┐ ┌─┐ ┌─┐ ┌─┐ ┌─┐ ┌─
        └─┘ └─┘ └─┘ └─┘ └─┘ └─┘ └─┘ └─┘

MOSI:   ─┤D7┤D6┤D5┤D4┤D3┤D2┤D1┤D0┤─
        
Result: 8 clocks needed for 1 byte
Speed:  ~30 MHz SPI clock → ~3.75 MB/s
```

### QSPI (Quad-line SPI)

```
Four data lines simultaneously:

Master (STM32H750)         Slave (Flash chip)
    IO0 (DQ0) ──────────── DQ0
    IO1 (DQ1) ──────────── DQ1
    IO2 (DQ2) ──────────── DQ2
    IO3 (DQ3) ──────────── DQ3
    SCK ──────────────────→ SCK
    CS ───────────────────→ /CS

Data transmission (4 bits per clock):
Clock:  ─┐ ┌─┐ ┌─┐ ┌─┐ ┌──
        └─┘ └─┘ └─┘ └─┘

IO0-3:  ┌───────────────────────┐
        │D7 D6 D5 D4 D3 D2 D1 D0│  (all 4 bits in parallel)
        └───────────────────────┘

Result: 2 clocks needed for 1 byte (4x faster!)
Speed:  ~80 MHz QSPI clock → ~10 MB/s (or higher)
```

---

## Why 4x Faster?

### SPI Transmission (8 clocks per byte)

```
Byte: 0xA5 = 1010 0101

Clock 1: Send bit D7=1  (MOSI=1)
Clock 2: Send bit D6=0  (MOSI=0)
Clock 3: Send bit D5=1  (MOSI=1)
Clock 4: Send bit D4=0  (MOSI=0)
Clock 5: Send bit D3=0  (MOSI=0)
Clock 6: Send bit D2=1  (MOSI=1)
Clock 7: Send bit D1=0  (MOSI=0)
Clock 8: Send bit D0=1  (MOSI=1)

Time: 8 clocks
```

### QSPI Transmission (2 clocks per byte)

```
Byte: 0xA5 = 1010 0101 = nibble(A=1010) + nibble(5=0101)

Clock 1: Send 4 bits (1,0,1,0) on IO3, IO2, IO1, IO0 simultaneously
         IO3=1, IO2=0, IO1=1, IO0=0 → Nibble 0xA

Clock 2: Send 4 bits (0,1,0,1) on IO3, IO2, IO1, IO0 simultaneously
         IO3=0, IO2=1, IO1=0, IO0=1 → Nibble 0x5

Time: 2 clocks (4x reduction!)
```

---

## QSPI Operation Modes

### Mode 1: Standard SPI (1 line)
- Used for issuing commands
- Example: Send READ command (0x03)
- 1 data line

### Mode 2: Dual SPI (2 lines)
- Some fast operations
- Less common for bootloader

### Mode 4: Quad SPI (4 lines) ← **RECOMMENDED FOR BOOTLOADER**
- **Fast read** operations
- All 4 lines used simultaneously
- Best performance

---

## QSPI on STM32H750 — XIP Mapping

### The Magic: Memory-Mapped Address Space

QSPI is special because it can be **memory-mapped** into the CPU's address space:

```
W25Q64 Flash Chip (logical address space):
├─ 0x00000000 - 0x007FFFFF
│  (8 MiB of data)
│
└─ Is visible in the CPU address space at:
   0x90000000 - 0x9FFFFFFF (256 MiB STM32H750 QUADSPI window)

For this 8 MiB W25Q64, the usable XIP range is:
   0x90000000 - 0x907FFFFF

When bootloader does:
    data = *(uint32_t *)0x90000000;

The STM32H750's QSPI controller automatically:
1. Issues QSPI read command to flash chip
2. Retrieves data from flash address 0x00000000
3. Returns data to CPU at address 0x90000000

Result: Flash acts like RAM! (Execute-In-Place)
```

### Address Capacity Limits

The QUADSPI controller has two different capacity limits. Do not confuse them:

| Access mode | Maximum addressable capacity | Meaning |
|---|---:|---|
| Indirect mode | **4 GiB** | The controller can issue up to 32 address bits, provided that the flash device and command set support them. |
| Memory-mapped / XIP | **256 MiB** | The STM32H750 maps QUADSPI into the fixed CPU window `0x90000000`–`0x9FFFFFFF`. |

For a W25Q64, configure `FlashSize = 22`. Its physical 8 MiB capacity occupies `0x90000000`–`0x907FFFFF`; the rest of the 256 MiB CPU window does **not** represent additional flash storage.

A flash larger than 16 MiB normally requires 4-byte addressing or dedicated 4-byte read/program commands. A flash larger than 256 MiB can still be used through indirect commands, but it cannot be exposed as one contiguous XIP region on this MCU.

### Why This is Important for Bootloader

```
Traditional approach (copy-to-RAM):
┌─────────────────────────────────────────┐
│ Bootloader (Internal Flash)             │
│ • 128 KB size                           │
│ • Fast execution                        │
└────────┬────────────────────────────────┘
         │
         ↓ Jump to app → Copy app to RAM first
    ┌────────────────────────────────┐
    │ Application in W25Q64          │
    │ 8 MB size                      │
    └────────┬───────────────────────┘
             │
             ↓ Copy (slow, uses time & RAM)
    ┌────────────────────────────────┐
    │ RAM_D1 (512 KB total)          │
    │ • App code: < 512 KB           │
    │ • Must copy before jumping     │
    │ • Slow process (seconds)       │
    └────────────────────────────────┘
             │
             ↓ Jump to app in RAM
    ┌────────────────────────────────┐
    │ Application runs in RAM        │
    │ (Full 512 KB RAM used)         │
    └────────────────────────────────┘

Cons:
- Time: Copy takes 1-5 seconds
- RAM: Entire app must fit in RAM
- Complexity: Extra copy step
```

**QSPI XIP approach (execute directly):**

```
┌─────────────────────────────────────┐
│ Bootloader (Internal Flash)         │
│ • 128 KB size                       │
└────────┬────────────────────────────┘
         │
         ↓ Jump to app → Execute directly from QSPI
    ┌─────────────────────────────────┐
    │ Application in W25Q64           │
    │ 8 MB size                       │
    │ Mapped to 0x90000000            │
    └────────┬────────────────────────┘
             │
             ↓ QSPI memory-mapped access
             │ (No copy needed!)
             │
    ┌────────────────────────────────┐
    │ Application executes from QSPI │
    │ • CPU fetches instructions     │
    │  from 0x90000000+              │
    │ • Flash acts like RAM          │
    │ • Full 512 KB RAM free!        │
    └────────────────────────────────┘

Pros:
- Speed: No copy (instant boot)
- RAM: Full 512 KB available
- Simplicity: Direct execution
```

---

## How QSPI XIP Works on STM32H750

### Step 1: CPU Issues Read Request

```
CPU wants to read from address 0x90000001:
    data = *(uint8_t *)0x90000001;

CPU's memory controller recognizes:
    0x90000000 - 0x93FFFFFF is QSPI range
    └→ Route to QSPI controller
```

### Step 2: QSPI Controller Translates Address

```
CPU address:  0x90000001
QSPI maps:    0x90000000 → 0x00000000 (in flash)

Calculated flash address: 0x00000001
```

### Step 3: QSPI Issues Read Command to Flash

```
QSPI Controller sends to W25Q64:
1. CS = 0 (chip select active)
2. Command: 0xEB (Fast Quad Read)
3. Address: 0x00000001 (3 bytes)
4. Dummy cycles: 2 clocks (required by W25Q64)
5. Data read: 4 bytes at a time (quad mode)
6. CS = 1 (chip select inactive)

All done in hardware (transparent to CPU!)
```

### Step 4: Data Returns to CPU

```
QSPI returns data to CPU:
    data = 0x??  (from flash address 0x00000001)

CPU thinks it read from RAM (but actually from QSPI)
```

---

## QSPI vs SPI6 for W25Q64

| Feature | QSPI | SPI6 |
|---------|------|------|
| **Data lines** | 4 (IO0-IO3) | 1 (MOSI/MISO) |
| **Speed** | ~80 MHz → 10 MB/s | ~30 MHz → 3.75 MB/s |
| **Execute-In-Place** | ✅ YES (0x90000000) | ❌ NO |
| **Memory-mapped** | ✅ YES | ❌ NO |
| **Copy to RAM needed** | ❌ NO | ✅ YES |
| **Application size** | Full 8 MB | Limited by RAM (512 KB) |
| **Boot time** | Fast (instant) | Slow (copy takes seconds) |
| **Bootloader complexity** | Low | High (copy logic) |
| **Production ready** | ✅ YES (industry standard) | ⚠️ Limited use |

---

## QSPI XIP Boot Flow (STM32H750)

```
Power ON / Reset
    ↓
STM32H750 boots from internal Flash (0x08000000)
    ↓
Bootloader runs (HSI 240 MHz, internal Flash)
    ↓
Initialize QSPI interface
    ├─ Enable QSPI controller
    ├─ Set pins (PB2, PD11-13, PE2, PB6)
    ├─ Configure memory-mapped mode
    └─ Verify W25Q64 present (read JEDEC ID)
    ↓
Wait 3 seconds for UPDATE command (UART)
    ├─ If no command → Boot to app
    └─ If UPDATE → Receive new firmware
    ↓
Boot to application at 0x90000000
    ↓
CPU fetches first instruction from QSPI:
    PC = 0x90000000
    Instruction = *(uint32_t *)0x90000000
    ↓
    (QSPI controller transparently reads from flash)
    ↓
Application's Reset_Handler executes
    ├─ Initialize clocks (HSE 480 MHz)
    ├─ Initialize peripherals
    └─ Call main()
    ↓
Application runs at full performance
```

---

## W25Q64 with QSPI

### Flash Specifications

```
Model: W25Q64
Capacity: 64 Mbits = 8 MB
JEDEC ID: 0xEF 0x40 0x17

Memory hierarchy (Winbond's official terminology):
  Blocks:  128   × 64 KB   (large erase unit    — cmd 0xD8)
  Sectors: 2048  × 4 KB    (SMALLEST erase unit — cmd 0x20)
  Pages:   32768 × 256 B   (program/write unit  — cmd 0x02 / 0x32)

  1 block = 16 sectors = 256 pages

Frequently used commands:
- 0x9F: Read JEDEC ID
- 0xEB: Fast Read Quad I/O (requires dummy cycles)
- 0x32: Quad Input Page Program (max 256 bytes per command)
- 0x20: Sector Erase (4 KB)
- 0x52: Block Erase (32 KB)
- 0xD8: Block Erase (64 KB)
- 0xC7 / 0x60: Chip Erase (entire 8 MB)
```

> **Erase vs Program:** NOR Flash can only write bits from 1→0. Returning a bit to 1
> requires an erase, and the smallest erasable unit is **one 4 KB sector** — you cannot
> erase a single byte or a single page.
>
> **Don't confuse "sector" with "block".** In Winbond's datasheet a *sector* is 4 KB and
> a *block* is 64 KB. Many tutorials incorrectly call the 64 KB unit a "sector", which
> leads to erase-alignment bugs.

---

## Where Do These Command Bytes Come From?

Command opcodes such as `0xD8` are **defined by Winbond, not by ST**. The STM32 HAL only
drives the QSPI *controller*; the instruction set belongs to the external flash chip. You
will not find any of these values anywhere in `Drivers/` — searching for `0xD8` in the
CMSIS headers only turns up an unrelated RCC register offset comment.

### Primary source: the W25Q64 datasheet

Download it from [winbond.com](https://www.winbond.com) and look for the section titled
**"Instruction Set Table"** (typically section 8), which is split into *Standard SPI
Instructions* and *Dual/Quad SPI Instructions*.

⚠️ **Check the marking on your physical chip first.** W25Q64**JV**, **FV**, **DW** and **BV**
are different generations. The basic erase/program opcodes are identical, but details such
as the QE bit location, dummy-cycle counts, and supported quad modes differ between them.

### Instruction set reference (W25Q64)

| Opcode | Name | Notes |
|---|---|---|
| `0x9F` | Read JEDEC ID | Returns 3 bytes; use to identify the chip |
| `0x06` | Write Enable | **Required before every erase/program command** |
| `0x04` | Write Disable | |
| `0x05` | Read Status Register-1 | Bit 0 = BUSY, bit 1 = WEL |
| `0x35` | Read Status Register-2 | Bit 1 = QE (Quad Enable) |
| `0x31` | Write Status Register-2 | Use this to set the QE bit |
| `0x03` | Read Data | Low speed, no dummy cycles |
| `0x0B` | Fast Read | 8 dummy cycles |
| `0x02` | Page Program | Max 256 bytes |
| `0x20` | Sector Erase | 4 KB |
| `0x52` | Block Erase | 32 KB |
| **`0xD8`** | **Block Erase** | **64 KB** |
| `0xC7` / `0x60` | Chip Erase | Entire 8 MB |
| `0x6B` | Fast Read Quad Output | Data on 4 lines |
| `0xEB` | Fast Read Quad I/O | Address + data on 4 lines |
| `0x32` | Quad Input Page Program | |
| `0x5A` | Read SFDP | See below |
| `0x38` | Enter QPI Mode | ⚠️ Not the same as "enable quad read" |
| `0xFF` | Exit QPI Mode | |
| `0x66` / `0x99` | Enable Reset / Reset Device | Must be sent as a pair, in that order |

### ⚠️ QE bit vs QPI mode

`0x38` is **Enter QPI Mode**, which is *not* how you normally enable quad reads. There are
two distinct mechanisms:

| | **QE bit** (what you normally want) | **QPI mode** (`0x38`) |
|---|---|---|
| How to enable | Set bit 1 of Status Register-2 via `0x31` | Send command `0x38` |
| Opcode travels on | **1 line** | **4 lines** |
| Address / data on | 4 lines | 4 lines |
| Exit | Clear the QE bit | Send `0xFF` |

```c
/* QE bit set, still in standard SPI mode — the usual choice */
cmd.InstructionMode = QSPI_INSTRUCTION_1_LINE;
cmd.AddressMode     = QSPI_ADDRESS_4_LINES;
cmd.DataMode        = QSPI_DATA_4_LINES;

/* After Enter QPI Mode (0x38) — the opcode itself goes on 4 lines */
cmd.InstructionMode = QSPI_INSTRUCTION_4_LINES;
```

Send `0x38` but leave `InstructionMode = QSPI_INSTRUCTION_1_LINE`, and the flash will be
listening for 4-line opcodes while the STM32 keeps sending 1-line ones. The chip then stops
recognising *every* command — including the reset commands — and goes completely silent.
There is no error flag; reads simply return `0xFF` or `0x00`. Recovery normally requires a
power cycle.

### Discovering the opcodes at runtime: SFDP

Instead of hard-coding values from the datasheet, you can ask the chip directly using
**SFDP** (Serial Flash Discoverable Parameters, JEDEC standard JESD216) via command `0x5A`.
The Basic Flash Parameter Table contains four "erase type" entries, each giving an **opcode**
and a **size expressed as 2^N bytes**.

On a W25Q64 this reports exactly `0x20`/4 KB, `0x52`/32 KB and `0xD8`/64 KB — straight from
the chip. This lets one bootloader support several different flash parts without code
changes. The trade-off is that you must write an SFDP parser, so for a learning project
hard-coding from the datasheet is more practical; SFDP earns its keep when you need to
support multiple chips.

### Cross-check using the JEDEC ID

Command `0x9F` returns `0xEF 0x40 0x17` for a W25Q64. The third byte is not an arbitrary
part number — it is the **base-2 logarithm of the capacity in bytes**:

```
0x17 = 23     →  2^23 = 8,388,608 bytes = 8 MB   ✓
```

Which connects directly back to the FSIZE discussion:

```
FSIZE = JEDEC_ID[2] - 1 = 0x17 - 1 = 22
```

Verify against other family members:

| Chip | JEDEC ID | Capacity byte | Capacity | FSIZE |
|---|---|---|---|---|
| W25Q32 | `EF 40 16` | 0x16 = 22 | 2^22 = 4 MB | 21 |
| **W25Q64** | `EF 40 17` | 0x17 = 23 | 2^23 = 8 MB | **22** |
| W25Q128 | `EF 40 18` | 0x18 = 24 | 2^24 = 16 MB | 23 |

This means your `ReadID()` function can derive the real capacity at runtime instead of
trusting a compile-time constant — useful for catching the case where someone populates the
board with a different flash part than expected.

### Memory Organization

```
W25Q64 Logical Address Space:
┌──────────────────────────────────────┐
│ 0x00000000 - 0x007FFFFF (8 MB)       │
├──────────────────────────────────────┤
│ Bootloader data        (0-4 KB)      │
│ App code               (0-7 MB)      │
│ Data/Config            (7-8 MB)      │
└──────────────────────────────────────┘

When mapped to CPU (with FlashSize = 22, the CORRECT setting):
┌──────────────────────────────────────┐
│ 0x90000000 - 0x907FFFFF (8 MB view)  │
├──────────────────────────────────────┤
│ W25Q64 content (8 MB)                │
│ 1:1 mapping, no repetition           │
│ 0x90000000 + N  ⟷  flash offset N   │
└──────────────────────────────────────┘
```

> **The size of this window is determined by `FlashSize` — it is not fixed.** If you
> declare a `FlashSize` larger than the actual capacity, the window becomes wider than
> the chip and the flash contents **repeat** across the surplus range — reading
> `0x90800000` silently returns the data at `0x90000000`. See the next section.

---

## Configuring Flash Size (FSIZE) — The Most Common Mistake

### The name "Flash Size" is a trap

The `hqspi.Init.FlashSize` field (register `QUADSPI_DCR.FSIZE`) is **not the capacity in
MB or Mbit.** ST's own comment in `stm32h7xx_hal_qspi.h` spells it out:

> *"FlashSize+1 is effectively the **number of address bits** required to address the flash memory."*

So the number you enter is the **address bit count, minus one**.

An analogy: you tell the postal service *"addresses in this neighborhood use 5 digits"* —
not *"this neighborhood has 90,000 houses"*. Both describe the same place, but the value
you declare is the **number of digits**, not the number of houses.

### Calculating it for the W25Q64, step by step

```
Step 1 — Convert bits to bytes   ← this is where most people go wrong
   64 Mbit = 64 × 1,048,576 bits = 67,108,864 bits
           ÷ 8                   =  8,388,608 bytes = 8 MB

Step 2 — 8 MB is 2 to the power of what?
   8,388,608 = 2^23     →  requires 23 address bits

Step 3 — Apply ST's formula
   FlashSize + 1 = 23   →  FlashSize = 22   ✓
```

You can cross-check this from the address range without computing any powers. The last
byte of a W25Q64 sits at `0x7FFFFF`:

```
0x7FFFFF  =  0 111 1111 1111 1111 1111 1111
             ↑
             bit 23 is always 0 (unused by the chip)
               └───── 23 significant bits (bit 22 → bit 0) ─────┘
```

Count the significant bits: 23 → `FlashSize = 22`.

### Why the "+1"?

`FSIZE` is only **5 bits wide** in `QUADSPI_DCR`, so it can hold values 0–31.

- If the definition were `size = 2^FSIZE`, the maximum would be 2^31 = 2 GB
- With `size = 2^(FSIZE+1)`, the maximum is 2^32 = **4 GiB** — the full 32-bit address space in **indirect mode**

The "+1" is purely a trick to fit 4 GiB of range into 5 bits. It carries no physical
meaning, so don't look for deeper logic in it.

### Lookup table for the W25Qxx family

| Chip | Label | = bytes | = 2^n | **FlashSize** (`n-1`) |
|---|---|---|---|---|
| W25Q16 | 16 Mbit | 2 MB | 2^21 | **20** |
| W25Q32 | 32 Mbit | 4 MB | 2^22 | **21** |
| **W25Q64** | 64 Mbit | **8 MB** | 2^23 | **22** ← this board |
| W25Q128 | 128 Mbit | 16 MB | 2^24 | **23** |
| W25Q256 | 256 Mbit | 32 MB | 2^25 | **24** |

Note the mapping: **64 → 22**, not 64 → 26.

### ❌ The common error: entering 26 for a W25Q64

Many tutorials (including earlier revisions of this document) state `FlashSize = 26`,
reasoning that *"64 Mbit = 2^26"*. That arithmetic is correct — but only for **bits**,
whereas ST's formula needs **bytes**:

```
2^26 bits  =  2^23 bytes     (dividing by 8 = dividing by 2^3)
              ↑ this is the value the formula expects
```

Entering 26 declares `2^27 = 128 MB` — **16× larger** than the real capacity.

### What actually goes wrong

```
FlashSize = 22 (correct):
  0x90000000 ─────────────► 0x907FFFFF        8 MB, matches the chip
  Read past the end → QSPI raises an error → you find out immediately

FlashSize = 26 (wrong):
  0x90000000 ─────────────────────────────────► 0x97FFFFFF     128 MB
              │← real chip 8 MB →│← 120 MB THAT DOESN'T EXIST →│
```

The dangerous part is that **those 120 MB of phantom space raise no error at all.** In
24-bit addressing the QSPI sends three address bytes (A23–A0), but the W25Q64 only uses
up to A22 — A23 is ignored. So reading `0x90800000` returns exactly the data at
`0x90000000`: a **silent wrap-around**. This class of bug is hard to track down because
the data looks valid; it simply belongs to a different address range.

**Severity depends on which mode you use:**

| Mode | Effect of an oversized FSIZE | Notes |
|---|---|---|
| Indirect mode | Not broken yet | Accesses inside the first 8 MB still work; you only lose out-of-range protection |
| **Memory-mapped / XIP** | **Silent bug** | The surplus range wraps back to the start of the chip with no error reported |

So if your driver still runs in indirect mode, `FSIZE = 26` produces no visible symptom —
but it must be corrected before you enable XIP.

### How to remember it

> Take the **Mbit** number on the label → **divide by 8** to get MB → find the
> **power of 2** → **subtract 1**.

```c
/* W25Q64: 64 Mbit → 8 MB → 2^23 → FlashSize = 22 */
hqspi.Init.FlashSize = 22;
```

> **Where to change it:** this value is generated by CubeMX, so edit it in CubeMX
> (Connectivity → QUADSPI → Flash Size) or in the `.ioc` file. Editing only
> `Core/Src/quadspi.c` will be overwritten on the next **Generate Code**.

---

## Key Points Summary

✅ **What QSPI is:**
- 4-line SPI interface (4x faster than single-line SPI)
- Memory-mapped into CPU address space
- Enables Execute-In-Place (XIP)

✅ **How XIP works:**
- Flash appears at address 0x90000000 in CPU address space
- CPU can execute code directly from QSPI
- No copy to RAM needed
- Transparent to application code

✅ **Advantages for STM32H750 bootloader:**
- Full 7 MB application size (not limited by RAM)
- Fast boot (no copy overhead)
- All RAM available for heap/stack
- Industry-standard approach

✅ **W25Q64 specifics:**
- 8 MB capacity (64 Mbit ÷ 8)
- Geometry: 128 blocks × 64 KB = 2048 sectors × 4 KB = 32768 pages × 256 B
- Smallest erase = one 4 KB sector (`0x20`); fast firmware erase = 64 KB block (`0xD8`)
- Supports quad mode (4 data lines)
- Requires proper initialization
- JEDEC ID: 0xEF 0x40 0x17

⚠️ **Three things that are frequently gotten wrong:**
- `FlashSize = 22` (NOT 26) — it is *address bits − 1*, derived from **bytes**, not bits
- A Winbond "sector" is **4 KB** and a "block" is **64 KB** — never call the 64 KB unit a sector
- `0x38` is **Enter QPI Mode**, not "enable quad read" — for `0xEB` you set the **QE bit** via `0x31`

📖 **Command opcodes** (`0xD8`, `0x20`, `0x9F`, …) come from the **Winbond datasheet**, not
from ST's HAL. See *Where Do These Command Bytes Come From?* above for the full instruction
set table, the SFDP runtime-discovery alternative, and the JEDEC-ID capacity cross-check.

---

## Next Steps

1. **Stage 3:** Implement QSPI driver to read/write W25Q64
2. **Stage 4:** Initialize QSPI memory-mapped mode
3. **Jump:** Boot application directly from 0x90000000
4. **Application:** Build with linker script targeting 0x90000000

---

**Reference:** STM32H750 Reference Manual, W25Q64 Datasheet, QSPI XIP Best Practices

---

## Reading the JEDEC ID in `QSPI_Flash_ReadID()`

`QSPI_Flash_ReadID()` performs an **indirect-mode** transaction. The STM32 sends the `0x9F` JEDEC Read ID command and then receives the three-byte response from the W25Q64.

```text
STM32                              W25Q64
  | ---- 0x9F (Read JEDEC ID) ----> |
  | <--- 0xEF 0x40 0x17 ---------- |
```

```c
cmd.Instruction = 0x9F;
cmd.InstructionMode = QSPI_INSTRUCTION_1_LINE;
cmd.AddressMode = QSPI_ADDRESS_NONE;
cmd.AlternateByteMode = QSPI_ALTERNATE_BYTES_NONE;
cmd.DataMode = QSPI_DATA_1_LINE;
cmd.NbData = 3;
cmd.DummyCycles = 0;
cmd.DdrMode = QSPI_DDR_MODE_DISABLE;
cmd.DdrHoldHalfCycle = QSPI_DDR_HHC_HALF_CLK_DELAY;
cmd.SIOOMode = QSPI_SIOO_INST_EVERY_CMD;
```

| Setting | Explanation |
|---|---|
| `Instruction = 0x9F` | JEDEC **Read ID** opcode; the flash returns manufacturer, memory-type, and capacity bytes. |
| `InstructionMode = QSPI_INSTRUCTION_1_LINE` | Sends `0x9F` on IO0/MOSI; this identification command uses standard single-line SPI signalling. |
| `AddressMode = QSPI_ADDRESS_NONE` | No address is needed because `0x9F` identifies the device rather than reading stored data. |
| `AlternateByteMode = QSPI_ALTERNATE_BYTES_NONE` | This command requires no mode/alternate byte. |
| `DataMode = QSPI_DATA_1_LINE` | Receives the ID bytes on one data line, normally IO1/MISO. |
| `NbData = 3` | Receives exactly `0xEF`, `0x40`, and `0x17`. |
| `DummyCycles = 0` | The ID is available immediately after the instruction, so no delay clocks are needed. |
| `DdrMode = QSPI_DDR_MODE_DISABLE` | Uses SDR, sampling once per clock period; this command does not use DDR. |
| `DdrHoldHalfCycle = QSPI_DDR_HHC_HALF_CLK_DELAY` | A DDR timing setting with no practical effect while DDR is disabled, but the HAL structure requires a valid value. |
| `SIOOMode = QSPI_SIOO_INST_EVERY_CMD` | Resends the opcode for every independent transaction, the safe choice for ID reads. |

The HAL calls execute the two phases:

```c
HAL_QSPI_Command(&hqspi, &cmd, 100);  // Instruction phase: send 0x9F
HAL_QSPI_Receive(&hqspi, id, 100);    // Data phase: receive three bytes
```

`id` points to the caller's three-byte array. After `memset(id, 0, 3)`, `HAL_QSPI_Receive()` writes directly to that array:

```c
id[0] = 0xEF;
id[1] = 0x40;
id[2] = 0x17;
```

This transaction has only an instruction phase and a data phase. An XIP memory read normally uses `0xEB`, a flash address, optional mode bytes, dummy cycles, and four IO lines.

---

## Enabling the Quad Enable (QE) bit

The Quad Enable (QE) bit allows the W25Q64 to use all four data lines (`IO0` to
`IO3`) for Quad commands. QE is bit 1 of Status Register-2 (SR2).

```text
SR2:  bit7 ... bit2  bit1  bit0
                     QE

QE = 0: Quad commands are not enabled.
QE = 1: Quad data transfers are enabled.
```

QE must be set before using commands such as `0x32` (Quad Input Page Program)
or Quad read commands such as `0xEB`. It is not the same as QPI mode:

| Action | Set QE bit | Enter QPI mode (`0x38`) |
|---|---|---|
| How it is enabled | Write SR2 bit 1 using `0x31` | Send `0x38` |
| Instruction transfer | 1 line | 4 lines after entry |
| Intended use here | Standard Quad SPI operation | Not needed |

Do **not** use `0x38` to enable normal Quad transfers. The recommended sequence
is to set QE while keeping instructions on one line.

### Required command sequence

```text
1. Read SR2 with 0x35.
2. If SR2 bit 1 is already 1, stop: QE is enabled.
3. Send Write Enable (0x06).
4. Write SR2 with 0x31 and data: old_SR2 | 0x02.
5. Poll Status Register-1 (0x05) until BUSY bit 0 clears.
6. Read SR2 with 0x35 again and verify QE is 1.
```

Preserve every existing SR2 bit when setting QE. For example, use:

```c
uint8_t new_sr2 = old_sr2 | 0x02;
```

Do not write a hard-coded `0x02`, because that could clear other configuration
bits.

### Driver definitions

Add these definitions to `external_flash.h`:

```c
#define CMD_READ_STATUS_REG2     0x35
#define CMD_WRITE_STATUS_REG2    0x31
#define STATUS_REG2_QE           0x02
```

### Read Status Register-2

```c
static int8_t QSPI_Flash_ReadStatusReg2(uint8_t *sr2)
{
    if (sr2 == NULL)
        return -1;

    QSPI_CommandTypeDef cmd = {0};

    cmd.Instruction = CMD_READ_STATUS_REG2;
    cmd.InstructionMode = QSPI_INSTRUCTION_1_LINE;
    cmd.AddressMode = QSPI_ADDRESS_NONE;
    cmd.AlternateByteMode = QSPI_ALTERNATE_BYTES_NONE;
    cmd.DataMode = QSPI_DATA_1_LINE;
    cmd.NbData = 1;
    cmd.DummyCycles = 0;
    cmd.DdrMode = QSPI_DDR_MODE_DISABLE;
    cmd.DdrHoldHalfCycle = QSPI_DDR_HHC_HALF_CLK_DELAY;
    cmd.SIOOMode = QSPI_SIOO_INST_EVERY_CMD;

    if (HAL_QSPI_Command(&hqspi, &cmd, 100) != HAL_OK)
        return -1;

    return (HAL_QSPI_Receive(&hqspi, sr2, 100) == HAL_OK) ? 0 : -1;
}
```

### Wait for the flash to become ready

```c
static int8_t QSPI_Flash_WaitReady(uint32_t timeout_ms)
{
    uint32_t start = HAL_GetTick();

    while ((HAL_GetTick() - start) < timeout_ms) {
        int8_t busy = QSPI_Flash_IsBusy();

        if (busy < 0)
            return -1;
        if (busy == 0)
            return 0;
    }

    return -1;
}
```

### Set and verify QE

```c
static int8_t QSPI_Flash_EnableQuadMode(void)
{
    uint8_t sr2;
    QSPI_CommandTypeDef cmd = {0};

    if (QSPI_Flash_ReadStatusReg2(&sr2) != 0)
        return -1;

    /* Already configured: avoid an unnecessary non-volatile write. */
    if ((sr2 & STATUS_REG2_QE) != 0)
        return 0;

    /* Write Enable (0x06) */
    cmd.Instruction = CMD_WRITE_ENABLE;
    cmd.InstructionMode = QSPI_INSTRUCTION_1_LINE;
    cmd.AddressMode = QSPI_ADDRESS_NONE;
    cmd.AlternateByteMode = QSPI_ALTERNATE_BYTES_NONE;
    cmd.DataMode = QSPI_DATA_NONE;
    cmd.NbData = 0;

    if (HAL_QSPI_Command(&hqspi, &cmd, 100) != HAL_OK)
        return -1;

    /* Write Status Register-2 (0x31), setting QE only. */
    sr2 |= STATUS_REG2_QE;
    cmd.Instruction = CMD_WRITE_STATUS_REG2;
    cmd.DataMode = QSPI_DATA_1_LINE;
    cmd.NbData = 1;

    if (HAL_QSPI_Command(&hqspi, &cmd, 100) != HAL_OK)
        return -1;
    if (HAL_QSPI_Transmit(&hqspi, &sr2, 100) != HAL_OK)
        return -1;
    if (QSPI_Flash_WaitReady(100) != 0)
        return -1;

    /* Verify that the non-volatile QE bit was written. */
    if (QSPI_Flash_ReadStatusReg2(&sr2) != 0)
        return -1;

    return ((sr2 & STATUS_REG2_QE) != 0) ? 0 : -1;
}
```

Call `QSPI_Flash_EnableQuadMode()` from `QSPI_Flash_Init()` after validating
the JEDEC ID and before entering memory-mapped (XIP) mode:

```c
if (QSPI_Flash_EnableQuadMode() != 0)
    return -1;
```

On many W25Q64 variants QE is non-volatile, so it normally survives reset and
power loss. The driver should still read it at boot and write it only when it
is not already set.

---

## Writing arbitrary buffers without crossing page boundaries

`Bootloader_WriteExternalFlash()` accepts a buffer of any size and divides it
into Page Program operations. A W25Q64 Page Program command can transfer at
most 256 bytes, and every command must remain inside one physical 256-byte
page.

If a command starts partway through a page and reaches its end, the flash does
not automatically continue into the next page. Extra bytes can wrap to the
beginning of the current page and overwrite existing data.

```text
Start address: 100
Length:        256 bytes
Current page:  0 ... 255
Requested:     100 ... 355   <-- crosses the boundary at 256
```

Only 156 bytes fit in the first page. The remaining 100 bytes require another
Page Program command starting at address 256.

### Page-aware implementation

```c
int8_t Bootloader_WriteExternalFlash(uint32_t address,
                                     uint8_t *data,
                                     uint32_t size)
{
    if (data == NULL || size == 0)
        return -1;

    /* Validate the range without allowing address + size to overflow. */
    if (address < QSPI_APP_ADDR ||
        address >= (QSPI_APP_ADDR + QSPI_APP_MAX_SIZE) ||
        size > (QSPI_APP_ADDR + QSPI_APP_MAX_SIZE - address))
        return -1;

    uint32_t offset = 0;
    while (offset < size)
    {
        uint32_t current_address = address + offset;
        uint32_t page_offset = current_address % QSPI_FLASH_PAGE_SIZE;
        uint32_t page_remaining = QSPI_FLASH_PAGE_SIZE - page_offset;
        uint32_t data_remaining = size - offset;
        uint32_t write_size = (data_remaining < page_remaining)
                            ? data_remaining : page_remaining;

        if (QSPI_Flash_WritePage(current_address,
                                 &data[offset], write_size) != 0)
            return -1;

        offset += write_size;
    }

    return 0;
}
```

For a 300-byte buffer starting at address 100:

```text
QSPI_Flash_WritePage(100, &data[0],   156)
QSPI_Flash_WritePage(256, &data[156], 144)
```

This matters for UART firmware chunks because their lengths can be arbitrary;
the next chunk does not necessarily begin on a page boundary.

### Defensive check in the low-level driver

`QSPI_Flash_WritePage()` should also reject requests that cross a page boundary:

```c
if ((address % QSPI_FLASH_PAGE_SIZE) + length >
    QSPI_FLASH_PAGE_SIZE)
    return -1;
```

The target must already be erased, QE must be enabled when using Quad Page
Program `0x32`, and memory-mapped mode must be exited before indirect erase or
program commands are issued.

---

## Making `Bootloader_ReceiveAndWrite()` robust

The UART protocol sends length-prefixed firmware chunks:

```text
[length: 2 bytes, little-endian][data: length bytes]
...
[0x00 0x00]  End-of-transfer marker
```

A 1024-byte length (`0x0400`) is sent as `00 04`. Only the zero-length marker
means success. A UART timeout must be an error; otherwise a partial firmware
image could be accepted as complete.

The improved receive loop should:

1. Decode the two-byte little-endian header explicitly.
2. Accept `0x0000` as the only valid end marker.
3. Reject chunks larger than `CHUNK_SIZE` or remaining flash capacity.
4. Treat both header and data timeouts as errors.
5. Advance counters only after a successful flash write.
6. Require a valid end marker and at least one received byte.

```c
uint8_t chunk_header[2];

if (HAL_UART_Receive(&huart1, chunk_header,
                     sizeof(chunk_header), 5000) != HAL_OK)
    return -1;

uint16_t chunk_len = (uint16_t)chunk_header[0]
                   | ((uint16_t)chunk_header[1] << 8);

if (chunk_len == 0) {
    transfer_complete = 1;
    break;
}

if (chunk_len > CHUNK_SIZE ||
    chunk_len > (QSPI_APP_MAX_SIZE - fw_received))
    return -1;

if (HAL_UART_Receive(&huart1, fw_chunk,
                     chunk_len, 5000) != HAL_OK)
    return -1;

if (Bootloader_WriteExternalFlash(fw_write_addr,
                                  fw_chunk, chunk_len) != 0)
    return -1;

fw_write_addr += chunk_len;
fw_received   += chunk_len;
```

After the loop, distinguish a valid end marker from another termination:

```c
if (!transfer_complete || fw_received == 0)
    return -1;

fw_total_size = fw_received;
```

For progress, compare with the next threshold instead of using modulo, since a
chunk can jump across an exact 256-KB boundary:

```c
while (fw_received >= next_progress) {
    /* Print next_progress here. */
    next_progress += 256U * 1024U;
}
```

This protocol still cannot detect corrupted data. A stronger version should
start with a header containing a magic value, expected firmware size, and
firmware CRC32. Reboot only after the received size and CRC32 of the image in
flash both match the header.

---

## Safely jumping to the QSPI application

`Bootloader_JumpToApplication()` transfers execution from internal flash to an
application mapped at `0x90000000`. QSPI must already be memory-mapped and must
remain enabled because the CPU fetches application instructions from it.

The handoff must validate the initial MSP and Reset Handler, clean bootloader
interrupt state, relocate `VTOR`, load the new MSP, and branch to Reset Handler.

### Validate the application vector table

Check each real STM32H750 RAM region separately. The upper boundary is
inclusive because an initial MSP normally points immediately above the stack.

```c
static uint8_t Bootloader_IsValidStackPointer(uint32_t sp)
{
    uint8_t in_dtcm =
        (sp >= 0x20000000U) && (sp <= 0x20020000U);
    uint8_t in_axi =
        (sp >= 0x24000000U) && (sp <= 0x24080000U);
    uint8_t in_d2 =
        (sp >= 0x30000000U) && (sp <= 0x30048000U);
    uint8_t in_d3 =
        (sp >= 0x38000000U) && (sp <= 0x38010000U);
    return in_dtcm || in_axi || in_d2 || in_d3;
}
```

Read and validate the initial MSP and Reset Handler:

```c
uint32_t app_sp =
    *(volatile uint32_t *)(QSPI_BASE_ADDR + 0x00U);
uint32_t app_reset_handler =
    *(volatile uint32_t *)(QSPI_BASE_ADDR + 0x04U);

if (app_sp == 0U || app_sp == 0xFFFFFFFFU ||
    app_reset_handler == 0U || app_reset_handler == 0xFFFFFFFFU)
    return;

if ((app_sp & 0x7U) != 0U ||
    !Bootloader_IsValidStackPointer(app_sp))
    return;

if ((app_reset_handler & 1U) == 0U)
    return;

uint32_t reset_address = app_reset_handler & ~1U;
if (reset_address < QSPI_BASE_ADDR ||
    reset_address >= (QSPI_BASE_ADDR + QSPI_FLASH_MAX_SIZE))
    return;
```

### Clean the bootloader execution state

Finish all debug output before disabling UART and interrupts. Then stop SysTick,
clear pending system exceptions, and disable and clear every NVIC interrupt:

```c
HAL_UART_DeInit(&huart1);
__disable_irq();

SysTick->CTRL = 0U;
SysTick->LOAD = 0U;
SysTick->VAL  = 0U;
SCB->ICSR = SCB_ICSR_PENDSTCLR_Msk |
            SCB_ICSR_PENDSVCLR_Msk;

for (uint32_t i = 0;
     i < (sizeof(NVIC->ICER) / sizeof(NVIC->ICER[0])); i++)
{
    NVIC->ICER[i] = 0xFFFFFFFFU;
    NVIC->ICPR[i] = 0xFFFFFFFFU;
}

/* Match the privileged, MSP-based state expected after reset. */
__set_CONTROL(0U);
__ISB();

SCB->VTOR = QSPI_BASE_ADDR;
__DSB();
__ISB();
```

Clearing SysTick, PendSV, and NVIC pending bits prevents old bootloader events
from firing during application startup. `VTOR` selects the application vector
table, while `__DSB()` and `__ISB()` make that change effective before branch.

### Final branch

After changing MSP, do not call ordinary C functions because generated code may
still access the bootloader stack frame. Use one inline-assembly sequence to load
the application MSP, re-enable interrupts, and branch to its Reset Handler.

Do not deinitialize QSPI, reset HAL, or disable the QSPI clock before branching.
Doing so removes the mapping while the CPU is about to execute at `0x90000000`.
Since SysTick is disabled, use an infinite `__NOP()` loop rather than
`HAL_Delay()` for an unreachable safety trap.

### QSPI state after entering the application

A direct branch to the application's Reset Handler does not reset the MCU.
QSPI therefore remains in memory-mapped mode while the application runs:

```text
Bootloader enables memory-mapped mode
                 -> branches to application
                 -> QSPI remains mapped at 0x90000000
                 -> CPU continues fetching application instructions
```

This differs from `NVIC_SystemReset()`. A system reset resets the QSPI
peripheral and removes memory-mapped mode. Execution then returns to the
internal-flash bootloader, which must initialize QSPI and enter memory-mapped
mode again before jumping to the external application.

An XIP application must not destroy the inherited QSPI state during
`Reset_Handler`, `SystemInit()`, or `main()`. It must not:

- Call `HAL_QSPI_DeInit()` or reset the QSPI peripheral.
- Disable the QSPI clock or reconfigure its GPIO pins.
- Call an `MX_QUADSPI_Init()` implementation that resets the active mapping.
- Change the clock tree so that the current QSPI timing becomes invalid.
- Exit memory-mapped mode while executing code from `0x90000000`.

`HAL_Init()` does not normally deinitialize QSPI by itself, but the
application's `HAL_MspInit()`, clock setup, and generated peripheral
initialization must be checked for QSPI-related side effects.

If QSPI must be reconfigured, the complete reconfiguration routine and all of
its dependencies must execute from internal flash or RAM. Code cannot safely
turn off the memory interface from which it is fetching instructions.

```text
Direct branch to application -> memory-mapped mode is preserved
System reset                 -> memory-mapped mode is lost
```
