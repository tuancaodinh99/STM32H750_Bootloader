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

static int8_t Bootloader_EraseExternalFlash(void);
static int8_t Bootloader_WriteExternalFlash(uint32_t address, uint8_t *data, uint32_t size);
static int8_t Bootloader_ReceiveFirmwareInfo(FirmwareInfo *info);
static int8_t Bootloader_ReceiveAndWrite(uint32_t expected_size, uint32_t *received_crc);

static uint8_t Bootloader_IsValidStackPointer(uint32_t sp);
static uint32_t ReadLE32(const uint8_t *data);

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
            UART_SendLine(&huart1, "ERROR: Erase failed!");
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
        UART_SendLine(&huart1, "ERROR: Firmware header timeout");
        return -1;
    }

    if (header[0] != 'F' || header[1] != 'W' ||
        header[2] != 'U' || header[3] != 'P')
    {
        UART_SendLine(&huart1, "ERROR: Invalid firmware magic");
        return -1;
    }

    info->size  = ReadLE32(&header[4]);
    info->crc32 = ReadLE32(&header[8]);

    if (info->size == 0U || info->size > QSPI_APP_MAX_SIZE)
    {
        UART_SendLine(&huart1, "ERROR: Invalid firmware size");
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

    // Ready notification
    UART_SendLine(&huart1, "Waiting for firmware upload...");
    UART_SendLine(&huart1, "Send binary data (max 8 MB)");

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
#if (BOOT_TEST == 1)
            UART_SendLine(&huart1, "ERROR: Timeout receiving chunk header");
#endif
            return -1;
        }

        chunk_len = (uint16_t)chunk_header[0] | ((uint16_t)chunk_header[1] << 8);

        /* A zero-length chunk is the only valid end marker. */
        if (chunk_len == 0U)
        {
            UART_SendLine(&huart1, "ERROR: Firmware ended prematurely");
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
            UART_SendLine(&huart1, "ERROR: Invalid chunk length");
            return -1;
        }

        if (HAL_UART_Receive(&huart1, fw_chunk, chunk_len, 5000) != HAL_OK)
        {
#if (BOOT_TEST == 1) 
            UART_SendLine(&huart1, "ERROR: Timeout receiving chunk data");
#endif
            return -1;
        }

        running_crc = CRC32_Update(running_crc, fw_chunk, chunk_len);

        // Write to external flash
        if (Bootloader_WriteExternalFlash(fw_write_addr, fw_chunk, chunk_len) != 0)
        {
#if (BOOT_TEST == 1) 
            UART_SendLine(&huart1, "ERROR: External flash write failed");
#endif
            return -1;  /* Write error */
        }

        fw_write_addr += chunk_len;
        fw_received += chunk_len;

        /* Send ACK so the host knows this chunk was processed
         * and the next one can be transmitted safely. */
        UART_SendLine(&huart1, "ACK");
    }


    /*
    * At this point, exactly expected_size bytes have been received.
    * The next two bytes must be the zero-length end marker.
    */
    uint8_t end_header[2];

    if (HAL_UART_Receive(&huart1, end_header, sizeof(end_header), 5000) != HAL_OK)
    {
#if (BOOT_TEST == 1)
        UART_SendLine(&huart1, "ERROR: Timeout receiving end marker");
#endif
        return -1;
    }

    uint16_t end_marker = (uint16_t)end_header[0] | ((uint16_t)end_header[1] << 8);

    if (end_marker != 0U)
    {
#if (BOOT_TEST == 1)
        UART_SendLine(&huart1, "ERROR: Invalid end-of-transfer marker");
#endif
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

    UART_SendLine(&huart1, "=== FIRMWARE UPDATE MODE ===");

    /* Receive size and expected CRC before erasing the old app.*/

    // Step 0: Check firmware infomation
    if (Bootloader_ReceiveFirmwareInfo(&firmware) != 0)
        return -1;
    
    // Step 1: Erase external flash
    if (Bootloader_EraseExternalFlash() != 0)
        return -1;

    // Step 2: Receive and write firmware
    if (Bootloader_ReceiveAndWrite(firmware.size, &received_crc) != 0)
        return -1;

    // Step 3: CRC check
    if (received_crc != firmware.crc32)
    {
        char msg[96];

        snprintf(msg, sizeof(msg),
                    "ERROR: CRC mismatch: "
                    "expected=%08lX received=%08lX",
                    (unsigned long)firmware.crc32,
                    (unsigned long)received_crc);

        UART_SendLine(&huart1, msg);
        return -1;
    }

    UART_SendLine(&huart1, "[OK] UART firmware CRC verified");

    /* Continue with QSPI read-back CRC verification... */
    if (QSPI_Flash_EnterMemoryMappedMode() != 0)
        return -1;

    flash_crc = CRC32_Calculate((const uint8_t *)QSPI_BASE_ADDR, firmware.size);

    if (flash_crc != firmware.crc32)
    {
        UART_SendLine(&huart1, "ERROR: Flash read-back CRC mismatch");
        return -1;
    }

    UART_SendLine(&huart1, "[OK] External flash CRC verified");

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
