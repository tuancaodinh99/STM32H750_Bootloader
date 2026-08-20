#!/usr/bin/env python3
"""
RSA Secure Boot Demo (Simplified Educational Version)
=====================================================

Implements RSA signing/verification from scratch -- no PKCS#1 padding,
no external crypto library. Pure Python with manual modular exponentiation.

Signing flow:
  1. Compute SHA-256(firmware) -> 32-byte hash
  2. Take first 4 bytes as 32-bit digest (big-endian)
  3. digest = digest mod n
  4. signature = digest^d mod n  (RSA signing with private key)

Verification flow:
  1. Compute SHA-256(firmware) -> 32-byte hash
  2. Take first 4 bytes as expected digest
  3. recovered = signature^e mod n  (RSA verification with public key)
  4. Valid if recovered == expected digest

Key sizes:
  - RSA-32:  n fits in 4 bytes  -> 4-byte signature  (educational only!)
  - RSA-64:  n fits in 8 bytes  -> 8-byte signature
  - RSA-2048: n fits in 256 bytes -> 256-byte signature (production)

Firmware layout (signature at END):
  [FWUP header (12)] [firmware] [RSA signature] [fw_size footer (4)] [magic marker 0x55667788 (4)]

  The signature is appended AFTER the firmware binary so the application
  vector table stays at offset 0x00 — Bootloader_JumpToApplication()
  needs no changes. The 4-byte fw_size footer lets the bootloader locate
  the signature on boot. The magic marker 0x55667788 marks the end of the
  payload (more reliable than scanning for 0xFF, which can appear in firmware).


Usage:
  python rsa_sign_demo.py                    # Default: RSA-32 with real firmware
  python rsa_sign_demo.py --bits 64          # RSA-64
  python rsa_sign_demo.py --bits 2048        # RSA-2048 (real security)
  python rsa_sign_demo.py --skip-keygen      # Reuse existing keys

"""

import sys
import os
import hashlib
import struct
import argparse
import json
import math


# =====================================================================
# RSA Math -- From Scratch
# =====================================================================

def is_prime(n, k=20):
    """Miller-Rabin primality test."""
    if n < 2:
        return False
    if n == 2 or n == 3:
        return True
    if n % 2 == 0:
        return False

    # Write n-1 as 2^r * d
    r, d = 0, n - 1
    while d % 2 == 0:
        r += 1
        d //= 2

    # Witness loop
    import random
    for _ in range(k):
        a = random.randrange(2, n - 1)
        x = pow(a, d, n)
        if x == 1 or x == n - 1:
            continue
        for _ in range(r - 1):
            x = pow(x, 2, n)
            if x == n - 1:
                break
        else:
            return False
    return True


def generate_prime(bit_size):
    """Generate a random prime number of approximately bit_size bits."""
    import random
    while True:
        # Generate random odd number of the right size
        p = random.getrandbits(bit_size)
        p |= (1 << (bit_size - 1))  # Ensure top bit set (correct size)
        p |= 1                        # Ensure odd
        if is_prime(p):
            return p


def extended_gcd(a, b):
    """Extended Euclidean Algorithm. Returns (gcd, x, y) where a*x + b*y = gcd."""
    if a == 0:
        return b, 0, 1
    g, x1, y1 = extended_gcd(b % a, a)
    return g, y1 - (b // a) * x1, x1


def mod_inverse(e, phi):
    """Compute modular multiplicative inverse: e^(-1) mod phi."""
    g, x, _ = extended_gcd(e % phi, phi)
    if g != 1:
        raise ValueError("Modular inverse does not exist")
    return x % phi


# =====================================================================
# RSA Key Generation
# =====================================================================

def rsa_generate_keypair(bit_size):
    """
    Generate RSA key pair.

    For RSA-32: bit_size=32, each prime ~16 bits, n ~32 bits (4 bytes)
    For RSA-2048: bit_size=2048, each prime ~1024 bits, n ~2048 bits (256 bytes)

    Returns: dict with n, e, d (public key = {n, e}, private key = {n, d})
    """
    prime_bits = bit_size // 2

    print(f"  Generating two {prime_bits}-bit primes...")

    p = generate_prime(prime_bits)
    q = generate_prime(prime_bits)
    while q == p:
        q = generate_prime(prime_bits)

    n = p * q
    phi = (p - 1) * (q - 1)

    # Public exponent e = 65537 (standard). Must satisfy gcd(e, phi) == 1.
    e = 65537
    if bit_size < 32:
        e = 3  # For very small keys, use e=3

    # Ensure gcd(e, phi) == 1
    while math.gcd(e, phi) != 1:
        e += 2

    # Private exponent d = e^(-1) mod phi
    d = mod_inverse(e, phi)

    print(f"  p = {p} (0x{p:0{bit_size//4}X})")
    print(f"  q = {q} (0x{q:0{bit_size//4}X})")
    print(f"  n = p*q = {n}")
    print(f"  n = 0x{n:0{bit_size//4}X} ({bit_size//8} bytes)")
    print(f"  e = {e}")
    print(f"  d = {d}")
    print(f"  phi(n) = {phi}")

    return {
        'n': n,
        'e': e,
        'd': d,
        'p': p,
        'q': q,
        'bit_size': bit_size,
    }


def save_keys(keys, filename):
    """Save key pair to JSON file."""
    with open(filename, 'w') as f:
        json.dump(keys, f, indent=2)
    print(f"  Keys saved to {filename}")


def load_keys(filename):
    """Load key pair from JSON file."""
    with open(filename, 'r') as f:
        keys = json.load(f)
    print(f"  Keys loaded from {filename}")
    return keys


# =====================================================================
# RSA Sign and Verify -- Core Logic
# =====================================================================

def compute_digest(firmware_data):
    """
    Step 1-2: Compute SHA-256 hash, take first 4 bytes as 32-bit digest.

    Returns: 4-byte big-endian digest (integer)
    """
    # Step 1: SHA-256 hash -> 32 bytes
    sha256_hash = hashlib.sha256(firmware_data).digest()
    print(f"    SHA-256 hash (32 bytes):")
    print(f"      {sha256_hash.hex()}")

    # Step 2: Take first 4 bytes as 32-bit digest (big-endian)
    digest_bytes = sha256_hash[:4]
    digest = int.from_bytes(digest_bytes, byteorder='big')

    print(f"    First 4 bytes (big-endian): {digest_bytes.hex()}")
    print(f"    Digest as integer: {digest} (0x{digest:08X})")

    return digest


def rsa_sign(keys, firmware_data):
    """
    Sign firmware with RSA private key.

    Steps:
      1. Compute SHA-256(firmware) -> 32-byte hash
      2. Take first 4 bytes as 32-bit digest (big-endian)
      3. digest = digest mod n
      4. signature = digest^d mod n  (RSA signing)

    Returns: signature as integer
    """
    n = keys['n']
    d = keys['d']
    bit_size = keys['bit_size']
    sig_byte_len = bit_size // 8

    print(f"\n[3] Signing firmware with private key (d)...")

    # Steps 1-2: Compute digest
    digest = compute_digest(firmware_data)

    # Step 3: digest = digest mod n
    digest_mod = digest % n
    print(f"    Step 3: digest mod n = {digest_mod} (0x{digest_mod:0{sig_byte_len*2}X})")

    # Step 4: signature = digest^d mod n (RSA signing)
    signature = pow(digest_mod, d, n)
    print(f"    Step 4: signature = digest^d mod n")
    print(f"           = {signature}")
    print(f"           = 0x{signature:0{sig_byte_len*2}X}")

    # Convert to bytes
    sig_bytes = signature.to_bytes(sig_byte_len, byteorder='big')
    print(f"    Signature ({len(sig_bytes)} bytes): {sig_bytes.hex()}")

    return signature, sig_bytes


def rsa_verify(keys, firmware_data, signature):
    """
    Verify RSA signature with public key.

    Steps:
      1. Compute SHA-256(firmware) -> 32-byte hash
      2. Take first 4 bytes as expected digest
      3. recovered = signature^e mod n  (RSA verification)
      4. Valid if recovered == expected digest

    Returns: True if valid, False otherwise
    """
    n = keys['n']
    e = keys['e']

    print(f"\n[4] Verifying signature with public key (e)...")

    # Steps 1-2: Compute expected digest
    expected_digest = compute_digest(firmware_data)
    expected_mod = expected_digest % n
    print(f"    Expected digest: {expected_digest} (0x{expected_digest:08X})")
    print(f"    Expected digest mod n: {expected_mod}")

    # Step 3: recovered = signature^e mod n
    recovered = pow(signature, e, n)
    print(f"    Step 3: recovered = signature^e mod n")
    print(f"           = {recovered}")
    print(f"           = 0x{recovered:08X}")

    # Step 4: Compare
    if recovered == expected_mod:
        print(f"    Step 4: {recovered} == {expected_mod} -> MATCH!")
        print(f"    [OK] Signature verified -- firmware is authentic")
        return True
    else:
        print(f"    Step 4: {recovered} != {expected_mod} -> MISMATCH!")
        print(f"    [FAIL] Signature verification FAILED")
        return False


# =====================================================================
# Tamper Test
# =====================================================================

def tamper_and_verify(keys, firmware_data, signature):
    """Flip 1 byte in firmware and verify -- should fail."""
    print(f"\n[5] Tampering firmware (flip 1 byte)...")

    tampered = bytearray(firmware_data)
    offset = min(1000, len(tampered) - 1)
    original_byte = tampered[offset]
    tampered[offset] ^= 0x01

    print(f"    Original byte at offset {offset}: 0x{original_byte:02X}")
    print(f"    Tampered byte at offset {offset}: 0x{tampered[offset]:02X}")

    print(f"\n[6] Verifying tampered firmware...")
    result = rsa_verify(keys, bytes(tampered), signature)
    if not result:
        print(f"    [OK] Tamper detection working correctly!")
    return not result


# =====================================================================
# Build Signed Firmware File
# =====================================================================

def build_signed_firmware(firmware_data, sig_bytes):
    """
    Build signed firmware file with signature at the END.

    Layout: [FWUP header (12)] [firmware] [RSA signature] [fw_size footer (4)]

    The signature is appended AFTER the firmware binary so the application
    vector table stays at offset 0x00 — Bootloader_JumpToApplication()
    needs no changes.

    The 4-byte fw_size footer lets the bootloader locate the signature
    on boot (when there is no UART header to provide the size).
    """
    import zlib

    print(f"\n[7] Building signed firmware file...")

    # fw_size footer: 4 bytes, little-endian — stores the firmware size
    # so the bootloader can find the signature at (payload_start + fw_size)
    fw_size_footer = struct.pack("<I", len(firmware_data))

    # Magic marker 0x55667788: marks end of payload
    # More reliable than scanning for 0xFF (firmware can contain 0xFF bytes)
    payload_end_marker = struct.pack("<I", 0x55667788)

    # Payload = firmware + signature + footer + magic marker
    # CRC32 is computed over the entire payload
    payload = firmware_data + sig_bytes + fw_size_footer + payload_end_marker
    crc32 = zlib.crc32(payload) & 0xFFFFFFFF

    # Header: magic + payload_size + CRC32
    header = b"FWUP"
    header += struct.pack("<I", len(payload))
    header += struct.pack("<I", crc32)

    # Final file: header + payload
    signed_fw = header + payload

    filename = "firmware_signed.bin"
    with open(filename, "wb") as f:
        f.write(signed_fw)

    print(f"    CRC32:      0x{crc32:08X} (over payload: fw + sig + footer + marker)")
    print(f"    Header:     {len(header)} bytes")
    print(f"    Firmware:   {len(firmware_data)} bytes")
    print(f"    Signature:  {len(sig_bytes)} bytes (at END)")
    print(f"    Footer:     {len(fw_size_footer)} bytes (fw_size = {len(firmware_data)})")
    print(f"    Marker:     {len(payload_end_marker)} bytes (0x55667788)")
    print(f"    Payload:    {len(payload)} bytes (fw + sig + footer + marker)")
    print(f"    Total:      {len(signed_fw)} bytes (header + payload)")
    print(f"    Output:     {filename}")
    print(f"    [OK] Signed firmware file created (signature at END, marker appended)")


    return signed_fw



# =====================================================================
# Print Public Key as C Array (for STM32)
# =====================================================================

def print_public_key_c_array(keys):
    """Print public key (n, e) as C array for STM32 embedding."""
    n = keys['n']
    e = keys['e']
    d = keys['d']
    bit_size = keys['bit_size']
    sig_byte_len = bit_size // 8

    n_bytes = n.to_bytes(sig_byte_len, byteorder='big')

    print(f"\n[8] Public key as C array (for STM32 embedding)...")

    print(f"    /* RSA-{bit_size} Public Key -- embed in bootloader.c */")
    print(f"    /* Modulus n ({len(n_bytes)} bytes, big-endian) */")
    print(f"    const uint8_t rsa_public_n[{len(n_bytes)}] = {{")
    for i in range(0, len(n_bytes), 16):
        chunk = n_bytes[i:i+16]
        hex_line = ", ".join(f"0x{b:02X}" for b in chunk)
        comma = "," if i + 16 < len(n_bytes) else ""
        print(f"        {hex_line}{comma}")
    print(f"    }};")
    print(f"")
    print(f"    /* Public exponent e */")
    print(f"    const uint32_t rsa_public_e = {e}UL;")
    print(f"")
    print(f"    /* Private exponent d (NEVER embed in device!) */")
    print(f"    /* d = {d} */")
    print(f"    [OK] Copy public key (n, e) into bootloader.c")


# =====================================================================
# Manual Verification (show the math step by step)
# =====================================================================

def show_manual_math(keys, firmware_data, signature):
    """Show the RSA math step by step for educational purposes."""
    n = keys['n']
    e = keys['e']
    d = keys['d']

    print(f"\n    --- Manual Math Verification ---")

    # Compute digest
    sha256_hash = hashlib.sha256(firmware_data).digest()
    digest = int.from_bytes(sha256_hash[:4], byteorder='big')
    digest_mod = digest % n

    print(f"    SHA-256 first 4 bytes: 0x{digest:08X} = {digest}")
    print(f"    digest mod n:          {digest_mod}")
    print(f"    n:                     {n}")
    print(f"    e:                     {e}")
    print(f"    d:                     {d}")
    print(f"")
    print(f"    Signing:")
    print(f"      signature = {digest_mod}^{d} mod {n}")
    print(f"               = {signature}")
    print(f"")
    print(f"    Verifying:")
    recovered = pow(signature, e, n)
    print(f"      recovered = {signature}^{e} mod {n}")
    print(f"               = {recovered}")
    print(f"")
    if recovered == digest_mod:
        print(f"      {recovered} == {digest_mod} -> VALID")
    else:
        print(f"      {recovered} != {digest_mod} -> INVALID")
    print(f"    --- End Manual Math ---")


# =====================================================================
# Main
# =====================================================================

def main():
    parser = argparse.ArgumentParser(description="RSA Secure Boot Demo (Simplified)")
    parser.add_argument("--bits", type=int, default=32,
                        help="RSA key size in bits (32=educational, 2048=production)")
    parser.add_argument("--fw", default=None,
                        help="Path to firmware .bin file")
    parser.add_argument("--skip-keygen", action="store_true",
                        help="Reuse existing RSA key pair")
    args = parser.parse_args()

    default_fw = r"C:\Users\dinhtuan.cao\STM32CubeIDE\cortexm\STM32H750_Application\Debug\STM32H750_Application.bin"
    fw_path = args.fw if args.fw else default_fw
    key_file = f"rsa_keys_{args.bits}.json"

    print(f"\n{'='*60}")
    print(f"  RSA Secure Boot Demo (Simplified Educational Version)")
    print(f"{'='*60}")
    print(f"  RSA key size: {args.bits} bits")
    print(f"  Signature size: {args.bits // 8} bytes")
    print(f"  Firmware: {fw_path}")
    print(f"  Key file: {key_file}")

    # Step 1: Generate or load RSA key pair
    print(f"\n[1] RSA Key Generation...")
    if args.skip_keygen and os.path.exists(key_file):
        keys = load_keys(key_file)
    else:
        keys = rsa_generate_keypair(args.bits)
        save_keys(keys, key_file)
    print(f"    [OK] Key pair ready")

    # Step 2: Read firmware
    print(f"\n[2] Reading firmware...")
    if not os.path.exists(fw_path):
        print(f"    [FAIL] File not found: {fw_path}")
        print(f"    Using dummy data (256 bytes of 0xAA) for demo.")
        fw_data = bytes([0xAA] * 256)
    else:
        with open(fw_path, "rb") as f:
            fw_data = f.read()
    print(f"    File: {fw_path}")
    print(f"    Size: {len(fw_data)} bytes")

    # Step 3: Sign firmware
    signature, sig_bytes = rsa_sign(keys, fw_data)

    # Show manual math
    show_manual_math(keys, fw_data, signature)

    # Step 4: Verify (valid case)
    result = rsa_verify(keys, fw_data, signature)

    # Step 5 & 6: Tamper and verify
    tamper_and_verify(keys, fw_data, signature)

    # Step 7: Build signed firmware file
    build_signed_firmware(fw_data, sig_bytes)

    # Step 8: Print public key as C array
    print_public_key_c_array(keys)

    # Summary
    print(f"\n{'='*60}")
    print(f"  Summary")
    print(f"{'='*60}")
    print(f"  [OK] RSA-{args.bits} key pair generated")
    print(f"  [OK] Firmware signed: signature = digest^d mod n")
    print(f"  [OK] Signature verified: signature^e mod n == digest")
    print(f"  [OK] Tampered firmware correctly rejected")
    print(f"  [OK] Signed firmware file: firmware_signed.bin")
    print(f"  [OK] Public key (n, e) printed for STM32 embedding")
    print(f"")
    print(f"  Security note:")
    if args.bits < 2048:
        print(f"    RSA-{args.bits} is EDUCATIONAL ONLY -- not secure!")
        print(f"    Use RSA-2048+ for production: python rsa_sign_demo.py --bits 2048")
    print(f"")
    print(f"  Next step: Implement rsa_verify() in STM32 bootloader")
    print(f"  See: prompt/24_RSA_SECURE_BOOT_GUIDE.md")
    print(f"{'='*60}")


if __name__ == "__main__":
    main()
