import serial
import struct
import time
import sys
import zlib

# ======== CONFIG ========
PORT = "COM8"  # ← THAY BẰNG COM PORT CỦA BẠN
BAUD = 115200
BIN_FILE = r"C:\Users\dinhtuan.cao\STM32CubeIDE\cortexm\STM32H750_Application\Debug\STM32H750_Application.bin"
CHUNK_SIZE = 1024
MAX_FIRMWARE_SIZE = 8 * 1024 * 1024
WAIT_FOR_CHUNK_ACK = True
CHUNK_DELAY_SECONDS = 0.02
BOOT_TIMEOUT_SECONDS = 30
ERASE_TIMEOUT_SECONDS = 120
CRC_TIMEOUT_SECONDS = 120
REBOOT_TIMEOUT_SECONDS = 30

def build_firmware_header(firmware):
    firmware_crc = zlib.crc32(firmware) & 0xFFFFFFFF
    header = b"FWUP" + struct.pack("<I", len(firmware))
    header += struct.pack("<I", firmware_crc)
    return header, firmware_crc

def wait_for_message(ser, expected, timeout):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        raw_line = ser.readline()
        if not raw_line:
            continue
        line = raw_line.decode("utf-8", errors="replace").strip()
        if line:
            print(f"[STM32] {line}")
        if expected in line:
            return
        if "ERROR:" in line:
            raise RuntimeError(f"Bootloader error: {line}")
    raise TimeoutError(f"Timeout waiting for {expected!r}")

def send_bytes(ser, data):
    if ser.write(data) != len(data):
        raise IOError("Serial short write")
    ser.flush()

try:
    # Read .bin file
    print(f"[*] Reading firmware from: {BIN_FILE}")
    with open(BIN_FILE, 'rb') as f:
        fw_data = f.read()

    if not fw_data:
        raise ValueError("Firmware file is empty")
    if len(fw_data) > MAX_FIRMWARE_SIZE:
        raise ValueError("Firmware exceeds W25Q64 capacity")

    metadata, expected_crc = build_firmware_header(fw_data)
    total_chunks = (len(fw_data) + CHUNK_SIZE - 1) // CHUNK_SIZE

    print(f"[OK] Firmware size: {len(fw_data)} bytes")
    print(f"[OK] Firmware CRC32: 0x{expected_crc:08X}")
    print(f"[OK] Number of chunks: {total_chunks}")
    
    # Connect to UART
    print(f"[*] Connecting to {PORT} @ {BAUD} baud...")
    ser = serial.Serial(PORT, BAUD, timeout=0.25, write_timeout=5)
    ser.reset_input_buffer()
    ser.reset_output_buffer()

    print("[*] Press RESET on the STM32 board now")
    print("[*] Waiting for the bootloader update window...")
    wait_for_message(
        ser,
        "[OK] W25Q64 detected",
        BOOT_TIMEOUT_SECONDS,
    )

    print("[*] Sending UPDATE command...")
    send_bytes(ser, b"UPDATE")
    wait_for_message(ser, "=== FIRMWARE UPDATE MODE ===", 5)

    print("[*] Sending FWUP metadata...")
    send_bytes(ser, metadata)

    print("[*] Waiting for external flash erase...")
    wait_for_message(
        ser,
        "Waiting for firmware upload",
        ERASE_TIMEOUT_SECONDS,
    )
    wait_for_message(
        ser,
        "Send binary data",
        5,
    )
    time.sleep(0.1)

    print("[*] Starting firmware upload...")
    
    # Send firmware in chunks
    bytes_sent = 0
    for i in range(0, len(fw_data), CHUNK_SIZE):
        chunk = fw_data[i:i + CHUNK_SIZE]
        chunk_len = len(chunk)
        
        # Packet = LE16 chunk length followed by raw firmware bytes.
        packet = struct.pack('<H', chunk_len) + chunk
        send_bytes(ser, packet)

        if WAIT_FOR_CHUNK_ACK:
            wait_for_message(ser, "ACK", 10)
        else:
            # Temporary fallback until bootloader sends ACK per chunk.
            time.sleep(CHUNK_DELAY_SECONDS)
        
        bytes_sent += chunk_len
        print(f"[+] Sent {bytes_sent} / {len(fw_data)} bytes ({100 * bytes_sent / len(fw_data):.1f}%)")
        
    
    # Send end marker (0x00 0x00)
    print("[*] Sending end marker...")
    send_bytes(ser, struct.pack('<H', 0))

    print("[*] Waiting for CRC verification...")
    wait_for_message(
        ser,
        "[OK] UART firmware CRC verified",
        CRC_TIMEOUT_SECONDS,
    )
    wait_for_message(
        ser,
        "[OK] External flash CRC verified",
        CRC_TIMEOUT_SECONDS,
    )

    print("[OK] Firmware upload and CRC verification complete!")
    print(f"[OK] Verified CRC32: 0x{expected_crc:08X}")

    print("[*] Waiting for STM32 reboot confirmation...")
    deadline = time.monotonic() + REBOOT_TIMEOUT_SECONDS
    update_ok = False
    while time.monotonic() < deadline:
        raw_line = ser.readline()
        if not raw_line:
            continue
        line = raw_line.decode("utf-8", errors="replace").strip()
        if line:
            print(f"[STM32] {line}")
        if "Update successful, rebooting" in line:
            update_ok = True
            break
        if "Update failed" in line:
            raise RuntimeError(f"Bootloader reported update failure: {line}")
        if "ERROR:" in line:
            raise RuntimeError(f"Bootloader error: {line}")
    if not update_ok:
        raise TimeoutError("Timeout waiting for 'Update successful, rebooting'")

    ser.close()
    print("[SUCCESS] Done!")

except FileNotFoundError:
    print(f"[ERROR] .bin file not found: {BIN_FILE}")
    sys.exit(1)
except serial.SerialException as e:
    print(f"[ERROR] Serial port error: {e}")
    print(f"[TIP] Check Device Manager for correct COM port")
    sys.exit(1)
except (ValueError, RuntimeError, TimeoutError, OSError) as e:
    print(f"[ERROR] {e}")
    sys.exit(1)
except Exception as e:
    print(f"[ERROR] {e}")
    sys.exit(1)
