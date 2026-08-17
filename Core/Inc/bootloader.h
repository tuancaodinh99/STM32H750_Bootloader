/*
 * bootloader.h
 *
 *  Created on: Aug 10, 2026
 *      Author: dinhtuan.cao
 */

#ifndef INC_BOOTLOADER_H_
#define INC_BOOTLOADER_H_

#include "external_flash.h"
#include <stdio.h>

#define FW_HEADER_SIZE  12U

/* --- Design choice: erase firmware in 64 KB blocks for speed --- */
#define QSPI_FW_ERASE_GRANULARITY  (QSPI_FLASH_BLOCK_SIZE)

#define QSPI_APP_ADDR            (0x00000000)       /* App at flash start */
#define QSPI_APP_MAX_SIZE        (8 * 1024 * 1024)  /* Full 8 MB used for app */
#define CHUNK_SIZE               (1024)             /* Receive buffer size */

#define BOOT_TEST                (0) /* 1 -> Enable ; 0 -> Disable  */

typedef struct
{
    uint32_t size;
    uint32_t crc32;
} FirmwareInfo;

// UART
void UART_SendString(UART_HandleTypeDef *huart, const char *str);
void UART_SendLine(UART_HandleTypeDef *huart, const char *str);

// Bootloader update
int8_t Bootloader_UpdateFirmware(void);

/* Jump to application in QSPI */
void Bootloader_JumpToApplication(void);

#endif /* INC_BOOTLOADER_H_ */
