/*
 * bootloader.c
 *
 *  Created on: Aug 10, 2026
 *      Author: dinhtuan.cao
 */

#include "bootloader.h"
#include "usart.h"
#include "crc32.h"

uint8_t  fw_chunk[CHUNK_SIZE];
uint32_t fw_total_size = 0;
uint32_t fw_received   = 0;
uint32_t fw_write_addr = QSPI_APP_ADDR;

typedef void (*pFunction)(void);

static void Bootloader_SendCmd(uint8_t cmd);
static int8_t Bootloader_EraseExternalFlash(void);
static int8_t Bootloader_WriteExternalFlash(uint32_t address, uint8_t *data, uint32_t size);
static int8_t Bootloader_ReceiveFirmwareInfo(FirmwareInfo *info);
static int8_t Bootloader_ReceiveAndWrite(uint32_t expected_size, uint32_t *received_crc);

static uint8_t Bootloader_IsValidStackPointer(uint32_t sp);
static uint32_t ReadLE32(const uint8_t *data);

static uint32_t modpow32(uint32_t base, uint32_t exp, uint32_t mod);
static uint32_t Bootloader_FindPayloadEnd(void);


/*
 * Modular exponentiation: base^exp mod mod
 * For RSA-32: all values fit in uint32_t.
 * Uses uint64_t for intermediate products to prevent overflow.
 *
 * Algorithm: Right-to-left binary method (square-and-multiply)
 *   result = 1
 *   for each bit of exp (LSB first):
 *     if bit is 1: result = result * base mod mod
 *     base = base * base mod mod
 *     exp >>= 1
*/
static uint32_t modpow32(uint32_t base, uint32_t exp, uint32_t mod)
{
    uint32_t result = 1;
    base = base % mod;

    while (exp > 0)
    {
        if (exp & 1U)
            result = (uint32_t)(((uint64_t)result * base) % mod);

        exp >>= 1;
        base = (uint32_t)(((uint64_t)base * base) % mod);
    }

    return result;
}

// ========== UART HELPER FUNCTIONS ==========
// Send a string via UART
void UART_SendString(UART_HandleTypeDef *huart, const char *str)
{
	uint16_t len = strlen(str);
	HAL_UART_Transmit(huart, (uint8_t *)str, len, 1000);
}

// Send a line (string + newline)
void UART_SendLine(UART_HandleTypeDef *huart, const char *str)
{
	UART_SendString(huart, str);
	UART_SendString(huart, "\r\n");
}

/* Send a 1-byte protocol command code to the host */
static void Bootloader_SendCmd(uint8_t cmd)
{
    HAL_UART_Transmit(&huart1, &cmd, 1, 100);
}

/* Send an error message (text debug) followed by a binary error code.*/
void Bootloader_SendErrorCmd(const char *msg, uint8_t error_code)
{
    UART_SendLine(&huart1, msg);
    Bootloader_SendCmd(error_code);
}

/**
 * Erase external flash firmware region (8 MB) in 64KB blocks
 * W25Q64: Use block erase (0xD8) for speed
 * Returns: 0 = OK, -1 = Error
 */
static int8_t Bootloader_EraseExternalFlash(void)
{
    UART_SendLine(&huart1, "Erasing external Flash (8 MB)...");

    uint32_t blocks = QSPI_APP_MAX_SIZE / QSPI_FLASH_BLOCK_SIZE;

    for (uint32_t i = 0; i < blocks; i++)
    {
        uint32_t addr = QSPI_APP_ADDR + (i * QSPI_FLASH_BLOCK_SIZE);

        if (QSPI_Flash_EraseBlock(addr) != 0)
        {
            Bootloader_SendErrorCmd("ERROR!", ERROR_FLASH);
            return -1;
        }

        // Print progress every 16 blocks (1 MB)
        if ((i + 1) % 16 == 0)
        {
            char msg[64];
            uint32_t mb = ((i + 1) * QSPI_FLASH_BLOCK_SIZE) / (1024 * 1024);
            snprintf(msg, sizeof(msg), "  Erased %lu MB", mb);
            UART_SendLine(&huart1, msg);
        }
    }

    UART_SendLine(&huart1, "Erase complete!");
    return 0;
}

/**
 * Write firmware page to external flash
 * W25Q64: Write up to 256 bytes per page program (0x02 or 0x32 command)
 * Returns: 0 = OK, -1 = Error
 */
static int8_t Bootloader_WriteExternalFlash(uint32_t address, uint8_t *data, uint32_t size)
{
    if (data == NULL || size == 0)
    return -1;

    /* Check range without integer-overflow risk */
    if ( (address < QSPI_APP_ADDR) 
       ||(address >= (QSPI_APP_ADDR + QSPI_APP_MAX_SIZE)) ||
         (size > (QSPI_APP_ADDR + QSPI_APP_MAX_SIZE - address)))
    {
        return -1;
    }

    uint32_t offset = 0;

    while (offset < size)
    {
        uint32_t current_address = address + offset;

        /* Position of current_address inside its 256-byte page */
        uint32_t page_offset =
            current_address % QSPI_FLASH_PAGE_SIZE;

        /* Number of bytes available before reaching page boundary */
        uint32_t page_remaining =
            QSPI_FLASH_PAGE_SIZE - page_offset;

        uint32_t data_remaining = size - offset;

        /* Never cross a physical flash page boundary */
        uint32_t write_size =
            (data_remaining < page_remaining)
                ? data_remaining
                : page_remaining;

        if (QSPI_Flash_WritePage(current_address,
                                 &data[offset],
                                 write_size) != 0)
        {
            return -1;
        }

        offset += write_size;
    }

    return 0;  /* Success */
}

/*Decode integers explicitly instead of receiving directly into a C structure.*/
static uint32_t ReadLE32(const uint8_t *data)
{
    return ((uint32_t)data[0])       |
           ((uint32_t)data[1] << 8)  |
           ((uint32_t)data[2] << 16) |
           ((uint32_t)data[3] << 24);
}

/* Receive and validate the metadata.*/
static int8_t Bootloader_ReceiveFirmwareInfo(FirmwareInfo *info)
{
    uint8_t header[FW_HEADER_SIZE];

    if (info == NULL)
        return -1;

    if (HAL_UART_Receive(&huart1, header,
                         sizeof(header), 5000) != HAL_OK)
    {
        Bootloader_SendErrorCmd("ERROR!", ERROR_HEADER);
        return -1;
    }

    if (header[0] != 'F' || header[1] != 'W' ||
        header[2] != 'U' || header[3] != 'P')
    {
        Bootloader_SendErrorCmd("ERROR!", ERROR_HEADER);
        return -1;
    }

    info->size  = ReadLE32(&header[4]);
    info->crc32 = ReadLE32(&header[8]);

    if (info->size == 0U || info->size > QSPI_APP_MAX_SIZE)
    {
        Bootloader_SendErrorCmd("ERROR!", ERROR_HEADER);
        return -1;
    }

    return 0;
}

/*
 * Receive firmware over UART and write to external flash
 * Protocol: Binary data received, written page-by-page to W25Q64
 */
static int8_t Bootloader_ReceiveAndWrite(uint32_t expected_size, uint32_t *received_crc)
{
    if (received_crc == NULL ||
        expected_size == 0U ||
        expected_size > QSPI_APP_MAX_SIZE)
    {
        return -1;
    }

    /* Optional debug text — host does NOT parse this */
    UART_SendLine(&huart1, "Waiting for firmware upload...");
    UART_SendLine(&huart1, "Send binary data (max 8 MB)");

    /* Send binary status: flash ready, can receive firmware data */
    Bootloader_SendCmd(STATUS_UPDATE_READY);

    // Initialize the receive state.
    uint32_t running_crc = CRC32_Init();

    fw_total_size = 0;
    fw_received = 0;
    fw_write_addr = QSPI_APP_ADDR;

    // Receive firmware in chunks
    while (fw_received < expected_size) // QSPI_APP_MAX_SIZE
    {
        uint8_t  chunk_header[2];
        uint16_t chunk_len = 0;

        /*
        * Receive the two-byte little-endian chunk length.
        * Example: 1024 = 0x0400, transmitted as 00 04.
        */

        if (HAL_UART_Receive(&huart1, chunk_header, sizeof(chunk_header), 5000) != HAL_OK)
        {
            Bootloader_SendErrorCmd("ERROR!", ERROR_CHUNK);
            return -1;
        }

        chunk_len = (uint16_t)chunk_header[0] | ((uint16_t)chunk_header[1] << 8);

        /* A zero-length chunk is the only valid end marker. */
        if (chunk_len == 0U)
        {
            Bootloader_SendErrorCmd("ERROR!", ERROR_CHUNK);
            return -1;
        }

        /*
        * Protect the RAM receive buffer.
        * Protect the flash range without calculating
        * fw_received + chunk_len, which could overflow.
        */
        if (chunk_len > CHUNK_SIZE ||
            chunk_len > (expected_size - fw_received))
        {
            Bootloader_SendErrorCmd("ERROR!", ERROR_CHUNK);
            return -1;
        }

        if (HAL_UART_Receive(&huart1, fw_chunk, chunk_len, 5000) != HAL_OK)
        {
            Bootloader_SendErrorCmd("ERROR!", ERROR_CHUNK);
            return -1;
        }

        running_crc = CRC32_Update(running_crc, fw_chunk, chunk_len);

        // Write to external flash
        if (Bootloader_WriteExternalFlash(fw_write_addr, fw_chunk, chunk_len) != 0)
        {
            Bootloader_SendErrorCmd("ERROR!", ERROR_FLASH);
            return -1;  /* Write error */
        }

        fw_write_addr += chunk_len;
        fw_received += chunk_len;

        /* Send binary ACK: chunk received and written successfully */
        Bootloader_SendCmd(STATUS_CHUNK_ACK);
    }

    /*
    * At this point, exactly expected_size bytes have been received.
    * The next two bytes must be the zero-length end marker.
    */
    uint8_t end_header[2];

    if (HAL_UART_Receive(&huart1, end_header, sizeof(end_header), 5000) != HAL_OK)
    {
        Bootloader_SendErrorCmd("ERROR!", ERROR_HEADER);
        return -1;
    }

    uint16_t end_marker = (uint16_t)end_header[0] | ((uint16_t)end_header[1] << 8);

    if (end_marker != 0U)
    {
        Bootloader_SendErrorCmd("ERROR!", ERROR_HEADER);
        return -1;
    }

    fw_total_size = fw_received;

    /*
    * Final XOR converts the running CRC state into the final
    * CRC-32/ISO-HDLC result.
    */
    *received_crc = CRC32_Finalize(running_crc);

#if (BOOT_TEST == 1)
    char msg[96];
    snprintf(msg, sizeof(msg),
             "Received %lu bytes, CRC32=0x%08lX",
             (unsigned long)fw_total_size,
             (unsigned long)*received_crc);

    UART_SendLine(&huart1, msg);
#endif
    return 0;  /* Success */
}

/*
 * Complete firmware update sequence
 */
int8_t Bootloader_UpdateFirmware(void)
{
    FirmwareInfo firmware;
    uint32_t received_crc;
    uint32_t flash_crc;

    /* Step 0: Receive and validate firmware metadata */
    if (Bootloader_ReceiveFirmwareInfo(&firmware) != 0)
        return -1;
    
    /* Step 1: Erase external flash */
    if (Bootloader_EraseExternalFlash() != 0)
        return -1;

    /*
     * Step 2: Receive and write payload (firmware + signature + footer + marker)
     *         to flash. The payload starts at QSPI_APP_ADDR = 0x00.
     *         NO header is stored in flash — only the payload.
     *         firmware.size = fw_size + RSA_SIG_SIZE + FW_SIZE_FOOTER_SIZE
     *                         + PAYLOAD_END_MARKER_SIZE
     */
    if (Bootloader_ReceiveAndWrite(firmware.size, &received_crc) != 0)
        return -1;

    /* Step 3: CRC check (UART receive CRC) */

    if (received_crc != firmware.crc32)
    {
        Bootloader_SendErrorCmd("ERROR!", ERROR_CRC);
        return -1;
    }
    UART_SendLine(&huart1, "[OK] UART firmware CRC verified");

    /* Step 4: Flash read-back CRC */
    if (QSPI_Flash_EnterMemoryMappedMode() != 0)
        return -1;

    flash_crc = CRC32_Calculate((const uint8_t *)QSPI_BASE_ADDR, firmware.size);

    if (flash_crc != firmware.crc32)
    {
        Bootloader_SendErrorCmd("ERROR!", ERROR_CRC);
        return -1;
    }
    UART_SendLine(&huart1, "[OK] External flash CRC verified");

#if (RSA_SECURE_BOOT_ENABLE == 1)
    /* Step 5: RSA signature verification */
    if (Bootloader_VerifyRSASignature() != 0)
    {
        Bootloader_SendErrorCmd("RSA_FAIL", ERROR_RSA);
        return -1;
    }
    UART_SendLine(&huart1, "[OK] RSA signature verified during update");
#endif

    /* All checks passed — notify host */
    Bootloader_SendCmd(STATUS_UPDATE_COMPLETE);

    /*
     * Do NOT reset here.  Return to main() so it can print the
     * "Update successful, rebooting..." message before calling
     * NVIC_SystemReset().  This lets the host script confirm the
     * update completed successfully.
     */
    return 0;
}


static uint8_t Bootloader_IsValidStackPointer(uint32_t sp)
{
    /*
     * Initial MSP normally points to the first address immediately
     * above a RAM region, so the upper boundary is inclusive.
     */
    uint8_t in_dtcm =
        (sp >= 0x20000000U) && (sp <= 0x20020000U);

    uint8_t in_axi_sram =
        (sp >= 0x24000000U) && (sp <= 0x24080000U);

    uint8_t in_d2_sram =
        (sp >= 0x30000000U) && (sp <= 0x30048000U);

    uint8_t in_d3_sram =
        (sp >= 0x38000000U) && (sp <= 0x38010000U);

    return in_dtcm || in_axi_sram || in_d2_sram || in_d3_sram;
}

/*
 * Find the end of payload in flash using binary search.
 *
 * Flash is erased to 0xFF before writing payload. After the payload,
 * the remaining flash is 0xFF. This function finds the first 0xFF byte
 * (the boundary between payload data and erased flash), then verifies
 * the magic marker 0x55667788 is present just before the boundary.
 *
 * Uses binary search: only ~23 byte reads for 8MB flash (log2(8MB) = 23).
 * Returns: offset of the magic marker (= payload_end - MARKER_SIZE),
 *          or 0 if flash empty or marker not found.
 */
static uint32_t Bootloader_FindPayloadEnd(void)
{
    const volatile uint8_t *flash = (const volatile uint8_t *)QSPI_BASE_ADDR;
    uint32_t lo = 0;                       /* flash[lo] is data (!= 0xFF) */
    uint32_t hi = QSPI_APP_MAX_SIZE;       /* flash[hi] is 0xFF (or out of range) */

    /* Edge case: flash is completely empty (first byte is 0xFF) */
    if (flash[0] == 0xFF)
        return 0;

    /* Binary search: narrow down data/0xFF boundary */
    while ((hi - lo) > 1U)
    {
        uint32_t mid = lo + ((hi - lo) / 2U);

        if (flash[mid] != 0xFF)
            lo = mid;      /* data extends at least to mid */
        else
            hi = mid;      /* 0xFF starts at or before mid */
    }

    /*
     * lo = last data byte, hi = first 0xFF byte.
     * Verify the magic marker 0x55667788 is at (hi - PAYLOAD_END_MARKER_SIZE).
     * This confirms we found the real payload boundary, not a 0xFF inside firmware.
     */
    uint32_t marker_offset = hi - PAYLOAD_END_MARKER_SIZE;
    uint32_t marker_val = *(volatile uint32_t *)(QSPI_BASE_ADDR + marker_offset);

    if (marker_val != PAYLOAD_END_MARKER)
    {
        /* Marker not found — boundary detection failed */
        return 0;
    }

    /* Return offset of the marker (start of the marker, not the 0xFF) */
    return marker_offset;
}

/*
 * Verify RSA signature of firmware in external flash.
 *
 * Prerequisites:
 *   - QSPI must be in memory-mapped mode (XIP)
 *   - Firmware must have been written to flash
 *   - Flash must have been fully erased (0xFF) before writing payload
 *
 * Flash layout (at QSPI_BASE_ADDR = 0x90000000):
 *   [0x00]               Firmware binary (fw_size bytes)
 *                        Vector table: SP at 0x00, Reset_Handler at 0x04
 *   [0x00+fw_size]       RSA signature (RSA_SIG_SIZE bytes)
 *   [0x00+fw_size+sig]   fw_size footer (4 bytes, little-endian)
 *   [0x00+fw_size+sig+4] Magic marker 0x55667788 (4 bytes)
 *   [0x00+fw_size+sig+8] 0xFF ... (erased flash)
 *
 * The header (12 bytes) is NOT in flash — only used during UART transfer.
 * On boot, binary search finds the data/0xFF boundary, then verifies the
 * magic marker 0x55667788 is present just before the boundary.
 * fw_size is read from the 4 bytes before the marker.
 *
 * Returns:  0 = signature valid
 *          -1 = signature invalid or error
 */
int8_t Bootloader_VerifyRSASignature(void)
{
    uint8_t  sha256_digest[SHA256_DIGEST_SIZE];
    uint8_t  sig_bytes[RSA_SIG_SIZE];
    uint32_t expected_digest;
    uint32_t recovered;
    uint32_t signature_val;
    uint32_t n_val;
    uint32_t fw_size;
    uint32_t marker_offset;
    uint32_t sig_addr;

    UART_SendLine(&huart1, "[*] Verifying RSA signature...");

    /*
     * Step 1: Find payload end using binary search (data/0xFF boundary).
     *         ~23 byte reads for 8MB flash — instant on QSPI XIP.
     *         Verify magic marker 0x55667788 is present.
     */
    marker_offset = Bootloader_FindPayloadEnd();

    if (marker_offset == 0U)
    {
        UART_SendLine(&huart1, "[!] ERROR: Flash empty or magic marker not found");
        return -1;
    }

    if (marker_offset < (RSA_SIG_SIZE + FW_SIZE_FOOTER_SIZE))
    {
        UART_SendLine(&huart1, "[!] ERROR: Payload too small for signature + footer");
        return -1;
    }

    /*
     * Step 2: Read fw_size from the footer (4 bytes before the marker).
     *         marker_offset points to the start of the magic marker.
     *         footer is at (marker_offset - FW_SIZE_FOOTER_SIZE).
     */
    fw_size = *(volatile uint32_t *)(QSPI_BASE_ADDR + marker_offset - FW_SIZE_FOOTER_SIZE);

    if (fw_size == 0U || fw_size == 0xFFFFFFFFU || fw_size > QSPI_APP_MAX_SIZE)
    {
        char msg[80];
        snprintf(msg, sizeof(msg),
                 "[!] ERROR: Invalid fw_size in footer: 0x%08lX",
                 (unsigned long)fw_size);
        UART_SendLine(&huart1, msg);
        return -1;
    }

    /*
     * Step 3: Read the RSA signature from flash.
     *         Signature is at (QSPI_BASE_ADDR + fw_size), right after the
     *         firmware binary. Firmware starts at QSPI_BASE_ADDR (offset 0x00).
     */
    sig_addr = QSPI_BASE_ADDR + fw_size;
    const uint8_t *sig_src = (const uint8_t *)sig_addr;
    for (uint32_t i = 0; i < RSA_SIG_SIZE; i++)
        sig_bytes[i] = sig_src[i];

    /*
     * Step 4: Compute SHA-256 over the firmware binary only.
     *         Firmware starts at QSPI_BASE_ADDR (offset 0x00 in flash).
     *         SHA-256 covers exactly fw_size bytes (NOT the signature/footer).
     */
    sha256_hash((const uint8_t *)QSPI_BASE_ADDR, fw_size, sha256_digest);

    /*
     * Step 5: Take first 4 bytes of SHA-256 as 32-bit digest (big-endian).
     *         This matches the Python demo: int.from_bytes(hash[:4], 'big')
     */
    expected_digest = ((uint32_t)sha256_digest[0] << 24) |
                       ((uint32_t)sha256_digest[1] << 16) |
                       ((uint32_t)sha256_digest[2] <<  8) |
                       ((uint32_t)sha256_digest[3]);

    /*
     * Step 6: Convert signature bytes to uint32_t (big-endian).
     *         The Python demo stores signature as big-endian bytes.
     */
    signature_val = 0;
    for (uint32_t i = 0; i < RSA_SIG_SIZE; i++)
        signature_val = (signature_val << 8) | sig_bytes[i];

    /*
     * Step 7: Convert modulus n from byte array to uint32_t (big-endian).
     */
    n_val = 0;
    for (uint32_t i = 0; i < RSA_SIG_SIZE; i++)
        n_val = (n_val << 8) | rsa_public_n[i];

    /*
     * Step 8: RSA verify — recovered = signature^e mod n
     *         If valid, recovered should equal (expected_digest mod n).
     */
    recovered = modpow32(signature_val, RSA_PUBLIC_E, n_val);

    uint32_t expected_mod = expected_digest % n_val;

    if (recovered != expected_mod)
    {
        char msg[96];
        snprintf(msg, sizeof(msg),
                 "[!] RSA FAIL: recovered=0x%08lX, expected=0x%08lX",
                 (unsigned long)recovered, (unsigned long)expected_mod);
        UART_SendLine(&huart1, msg);
        UART_SendLine(&huart1, "[!] ERROR: RSA signature verification FAILED");
        return -1;
    }

    UART_SendLine(&huart1, "[OK] RSA signature verified — firmware is authentic");
    return 0;
}


void Bootloader_JumpToApplication(void)
{
    uint32_t app_sp, app_reset_handler, reset_address;
    char debug_msg[96];

    /* 1. Read application's vector table from QSPI
          Since QSPI is memory-mapped at 0x90000000, we can read directly
    */
    app_sp = 
        *(volatile uint32_t *)(QSPI_BASE_ADDR + 0x00);

    app_reset_handler = 
        *(volatile uint32_t *)(QSPI_BASE_ADDR + 0x04);

    /* 2. Debug: Print what we read */
    snprintf(debug_msg, sizeof(debug_msg),
             "[DEBUG] App vector table: SP=0x%08lX, ResetHandler=0x%08lX",
             app_sp,
             app_reset_handler);

    UART_SendLine(&huart1, debug_msg);

    /* Check for an empty or unprogrammed vector table.*/
    if (app_sp == 0x00000000U ||
        app_sp == 0xFFFFFFFFU ||
        app_reset_handler == 0x00000000U ||
        app_reset_handler == 0xFFFFFFFFU)
    {
        UART_SendLine(&huart1,
                      "[!] ERROR: Application vector table is empty");
        return;
    }

    /* ARM AAPCS requires the stack to be 8-byte aligned.*/
    if ((app_sp & 0x7U) != 0U)
    {
        UART_SendLine(&huart1,
                      "[!] ERROR: Application stack is not 8-byte aligned");
        return;
    }

    if (!Bootloader_IsValidStackPointer(app_sp))
    {
        snprintf(debug_msg, sizeof(debug_msg),
                 "[!] ERROR: Invalid application SP: 0x%08lX",
                 (unsigned long)app_sp);

        UART_SendLine(&huart1, debug_msg);
        return;
    }

    /*
     * Cortex-M executes Thumb instructions only.
     * Bit 0 of a function address must therefore be 1.
     */

    if ((app_reset_handler & 1U) == 0U)
    {
        UART_SendLine(&huart1,
                    "[!] ERROR: Reset_Handler Thumb bit is not set");
        return;
    }

    snprintf(debug_msg, sizeof(debug_msg),
             "[OK] Valid app found. Jumping to 0x%08lX...",
             app_reset_handler);
    UART_SendLine(&huart1, debug_msg);
    HAL_Delay(100);

    /* Remove the Thumb-state bit before checking the address range.*/
    reset_address = app_reset_handler & ~1U;

    if (reset_address < QSPI_BASE_ADDR || reset_address >= (QSPI_BASE_ADDR + QSPI_FLASH_MAX_SIZE))
    {
        snprintf(debug_msg, sizeof(debug_msg),
                 "[!] ERROR: Reset_Handler outside QSPI: 0x%08lX",
                 (unsigned long)app_reset_handler);

        UART_SendLine(&huart1, debug_msg);
        return;
    }

    snprintf(debug_msg, sizeof(debug_msg),
            "[OK] Jumping to application at 0x%08lX",
            (unsigned long)app_reset_handler);

    UART_SendLine(&huart1, debug_msg);
    HAL_Delay(100);

    /*
     * UART is no longer needed. Keep QSPI and its clocks active,
     * because the application executes directly from QSPI.
    */
    HAL_UART_DeInit(&huart1);

    /* Prevent interrupts while changing VTOR and stack context.*/
    __disable_irq();

    /*
     * Stop the bootloader SysTick and clear pending system exceptions.
     * Use assignment instead of |= because ICSR contains write-one action bits.
    */
    SysTick->CTRL = 0U;
    SysTick->LOAD = 0U;
    SysTick->VAL  = 0U;
   
    SCB->ICSR = SCB_ICSR_PENDSTCLR_Msk | SCB_ICSR_PENDSVCLR_Msk;

    /* Disable every external interrupt and remove pending requests.*/
    for (uint32_t i = 0; i < (sizeof(NVIC->ICER) / sizeof(NVIC->ICER[0])); i++)
    {
       NVIC->ICER[i] = 0xFFFFFFFFU;
       NVIC->ICPR[i] = 0xFFFFFFFFU;
    }

    /* The application should start privileged and use MSP, as it would after a hardware reset.*/
    __set_CONTROL(0U);
    __ISB();

    /* Make the application's vector table active.*/
    SCB->VTOR = QSPI_BASE_ADDR;
    __DSB();    // Finish previous work
    __ISB();    // Start fetching with the new rules

    /*
     * Do not call ordinary C functions after changing MSP.
     *
     * This sequence:
     *   1. Loads the application's MSP.
     *   2. Re-enables global interrupts.
     *   3. Branches directly to the application's Reset_Handler.
     *
     * All NVIC pending interrupts were cleared above, so there should
     * be no interrupt in the one-instruction window before BX.
    */

    // __set_MSP(app_sp);
    // __enable_irq();
    // jump_to_app();

    __asm volatile (
        "msr msp, %0  \n"
        "cpsie i      \n"
        "bx %1        \n"
        :
        : "r" (app_sp),
        "r" (app_reset_handler)
        : "memory"
    );

    /* The BX instruction above must never return.*/
    for (;;)
    {
        __NOP();
    }
}
